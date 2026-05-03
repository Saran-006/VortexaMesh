#include "mesh_core/Dispatcher.hpp"
#include "mesh_core/Security.hpp"
#include "mesh_core/Logger.hpp"
#include "mesh_core/RequestManager.hpp"
#include "mesh_registry/NodeRegistry.hpp"
#include "mesh_registry/DedupCache.hpp"
#include "mesh_routing/DirectionalRouter.hpp"
#include "mesh_routing/RouteCache.hpp"
#include "mesh_reliability/AckManager.hpp"
#include "mesh_reliability/FragmentManager.hpp"
#include "mesh_transport/ESPNOWTransport.hpp"
#include "mesh_events/PacketEvents.hpp"
#include "mesh_events/NodeEvents.hpp"
#include "mesh_utils/UUID.hpp"
#include <cstring>
#include <Arduino.h>

static const char* TAG = "Dispatch";

namespace mesh {

Dispatcher::Dispatcher(MeshContext* ctx) : ctx_(ctx) {}

void dispatcherTaskFn(void* param) {
    MeshContext* ctx = static_cast<MeshContext*>(param);
    Dispatcher dispatcher(ctx);

    LOG_INFO(TAG, "Dispatcher task started");

    while (ctx->running) {
        Packet pkt;
        if (xQueueReceive(ctx->packetQueue, &pkt, pdMS_TO_TICKS(100)) == pdTRUE) {
            // 1. Verify signature
            if (ctx->config->networkKeyLen > 0) {
                if (!Security::verifyPacket(pkt, ctx->config->networkKey,
                                            ctx->config->networkKeyLen)) {
                    LOG_WARN(TAG, "Signature verification failed, dropping");
                    onPacketDropped(ctx, pkt, "bad signature");
                    continue;
                }
            }

            // 2. Check dedup
            bool isDuplicate = ctx->dedupCache->checkAndInsert(pkt.header.packet_id);

            // 3. Record reverse route: source_hash came from last_hop_mac
            if (!isDuplicate && (pkt.header.flags & FLAG_ROUTE_RECORD)) {
                ctx->routeCache->recordRoute(pkt.header.source_hash,
                                             pkt.header.last_hop_mac);
            }

            // 4. Process packet
            dispatcher.processPacket(pkt, isDuplicate);
        }
    }

    LOG_INFO(TAG, "Dispatcher task stopped");
    vTaskDelete(nullptr);
}

void Dispatcher::processPacket(Packet& pkt, bool isDuplicate) {
    // 0. Filter out our own packets (loopback prevention)
    if (memcmp(pkt.header.source_hash, ctx_->selfNode->node_hash, 16) == 0) {
        return;
    }

    // --- ACK packets: always consume, never forward ---
    if (pkt.isAck()) {
        if (!isDuplicate) handleAck(pkt);
        return;
    }

    // --- TCP response: deliver to RequestManager ---
    if (pkt.header.flags & FLAG_TCP_RESPONSE) {
        if (isForUs(pkt)) {
            // Deliver even if duplicate (for visibility), 
            // but RequestManager will naturally ignore duplicate requestIds.
            handleTcpResponse(pkt, isDuplicate);
            
            // Also send ACK if required (only once)
            if (!isDuplicate && pkt.isAckRequired()) {
                sendAckFor(pkt);
            }
            return;
        } else if (!isDuplicate) {
            forwardPacket(pkt);
            return;
        }
        return;
    }

    // --- Fragmented packets ---
    if (pkt.isFragmented()) {
        if (!isDuplicate) handleFragment(pkt);
        return;
    }

    // --- Packet is for us ---
    bool forUs = isForUs(pkt);
    bool isBroadcast = false;
    uint8_t zeros[16] = {};
    if (memcmp(pkt.header.dest_hash, zeros, 16) == 0 && pkt.header.routing_strategy == (uint8_t)RoutingStrategy::STRAT_BROADCAST) {
        isBroadcast = true;
    }

    if (forUs) {
        if (pkt.isControl()) {
            handleControl(pkt, isDuplicate);
        } else {
            handleData(pkt, isDuplicate);
        }

        // Send ACK if required (only once)
        if (!isDuplicate && pkt.isAckRequired()) {
            sendAckFor(pkt);
        }

        // If it's uniquely for us (not a broadcast, not geo-flood continuing past us), we stop and don't forward.
        if (!isBroadcast && pkt.header.routing_strategy != (uint8_t)RoutingStrategy::STRAT_GEO_FLOOD) {
            return;
        }
    }

    // --- Not exclusively for us, or it's a broadcast/geo-flood: forward only if NOT a duplicate AND NOT a control packet ---
    if (!isDuplicate && !pkt.isControl()) {
        forwardPacket(pkt);
    }
}

void Dispatcher::sendAckFor(const Packet& pkt) {
    Packet ackPkt;
    memset(&ackPkt, 0, sizeof(ackPkt));
    ackPkt.header.version = 1;
    ackPkt.header.ttl = ctx_->config->ttlDefault;
    ackPkt.header.flags = FLAG_ACK | FLAG_CONTROL | FLAG_ROUTE_RECORD;
    ackPkt.header.priority = static_cast<uint8_t>(Priority::PRIO_HIGH);

    UUID::generate(ackPkt.header.packet_id);
    memcpy(ackPkt.header.source_hash, ctx_->selfNode->node_hash, 16);
    memcpy(ackPkt.header.dest_hash, pkt.header.source_hash, 16);

    // Put original packet_id in payload so sender can match
    memcpy(ackPkt.payload, pkt.header.packet_id, 16);
    ackPkt.header.payload_size = 16;

    enqueueOutgoing(ctx_, ackPkt);
}

bool Dispatcher::isForUs(const Packet& pkt) const {
    uint8_t zeros[16] = {};
    if (memcmp(pkt.header.dest_hash, zeros, 16) == 0) {
        // If it's a strict broadcast, it's for everyone
        if (pkt.header.routing_strategy == (uint8_t)RoutingStrategy::STRAT_BROADCAST) return true;
        
        // If it's GEO_FLOOD, it's for us ONLY if we are within the destination radius
        if (pkt.header.routing_strategy == (uint8_t)RoutingStrategy::STRAT_GEO_FLOOD) {
            if (ctx_->selfNode->hasLocation()) {
                float dist = DirectionalRouter::distanceM(ctx_->selfNode->lat, ctx_->selfNode->lon, 
                                                          pkt.header.dest_lat, pkt.header.dest_lon);
                if (dist <= ctx_->config->geoDestinationRadiusM) return true;
            }
            return false;
        }
        
        // Fallback for other zero-hash cases: only accept if it's a broadcast or we are in the flood zone.
        // If it's none of the above, it's not for us.
        return false; 
    }
    return memcmp(pkt.header.dest_hash, ctx_->selfNode->node_hash, 16) == 0;
}

void Dispatcher::handleAck(Packet& pkt) {
    if (pkt.header.payload_size >= 16) {
        ctx_->ackManager->onAckReceived(pkt.payload);
        LOG_INFO(TAG, "ACK received");

        MeshEvent evt;
        evt.type = MeshEventType::PACKET_ACK_RECEIVED;
        memcpy(&evt.packetData.packet, &pkt, sizeof(Packet));
        ctx_->eventBus->post(evt);
    }
}

void Dispatcher::handleTcpResponse(Packet& pkt, bool isDuplicate) {
    if (pkt.header.payload_size >= 16) {
        uint8_t origRequestId[16];
        memcpy(origRequestId, pkt.payload, 16);

        // Build a "clean" response packet with just the response data
        Packet respForCaller;
        memcpy(&respForCaller, &pkt, sizeof(Packet));
        if (pkt.header.payload_size > 16) {
            memmove(respForCaller.payload, pkt.payload + 16,
                    pkt.header.payload_size - 16);
            respForCaller.header.payload_size = pkt.header.payload_size - 16;
        } else {
            respForCaller.header.payload_size = 0;
        }

        // Send to RequestManager (it will ignore duplicates internally based on ID)
        ctx_->requestManager->onResponseReceived(origRequestId, respForCaller);

        // Also fire event for visibility
        MeshEvent evt;
        evt.type = MeshEventType::PACKET_RECEIVED;
        memcpy(&evt.packetData.packet, &pkt, sizeof(Packet));
        memcpy(evt.packetData.sender_mac, pkt.header.last_hop_mac, 6);
        evt.packetData.isDuplicate = isDuplicate;
        evt.packetData.isForUs = true;
        ctx_->eventBus->post(evt);
    }
}

void Dispatcher::handleControl(Packet& pkt, bool isDuplicate) {
    if (isDuplicate) {
        // Just post event for duplicate control packet visibility
        MeshEvent evt;
        evt.type = MeshEventType::PACKET_RECEIVED; // Or specialized CONTROL_RECEIVED if needed
        memcpy(&evt.packetData.packet, &pkt, sizeof(Packet));
        memcpy(evt.packetData.sender_mac, pkt.header.last_hop_mac, 6);
        evt.packetData.isDuplicate = true;
        ctx_->eventBus->post(evt);
        return;
    }
    if (pkt.header.payload_size < 1) return;
    uint8_t ctrlType = pkt.payload[0];

    switch (ctrlType) {
        case CTRL_DISCOVERY_BEACON:
        case CTRL_DISCOVERY_RESPONSE: {
            if (pkt.header.payload_size >= 9) {
                Node discovered;
                memcpy(discovered.node_hash, pkt.header.source_hash, 16);
                memcpy(discovered.mac, pkt.header.last_hop_mac, 6);
                float lat, lon;
                memcpy(&lat, pkt.payload + 1, 4);
                memcpy(&lon, pkt.payload + 5, 4);
                discovered.lat = lat;
                discovered.lon = lon;
                discovered.last_seen = millis();

                bool isNew = ctx_->nodeRegistry->upsert(discovered);
                ctx_->peerManager->addPeer(discovered.mac);

                if (isNew) {
                    onNodeDiscovered(ctx_, discovered);
                } else {
                    onNodeUpdated(ctx_, discovered);
                }

                // Reply to beacons
                if (ctrlType == CTRL_DISCOVERY_BEACON) {
                    Packet resp;
                    memset(&resp, 0, sizeof(resp));
                    resp.header.version = 1;
                    resp.header.ttl = 1;
                    resp.header.flags = FLAG_CONTROL | FLAG_ROUTE_RECORD;
                    resp.header.priority = static_cast<uint8_t>(Priority::PRIO_HIGH);
                    UUID::generate(resp.header.packet_id);
                    memcpy(resp.header.source_hash, ctx_->selfNode->node_hash, 16);
                    memcpy(resp.header.dest_hash, pkt.header.source_hash, 16);
                    resp.payload[0] = CTRL_DISCOVERY_RESPONSE;
                    float sLat = ctx_->selfNode->lat;
                    float sLon = ctx_->selfNode->lon;
                    memcpy(resp.payload + 1, &sLat, 4);
                    memcpy(resp.payload + 5, &sLon, 4);
                    resp.header.payload_size = 9;
                    enqueueOutgoing(ctx_, resp);
                }
            }
            break;
        }
        case CTRL_LOCATION_UPDATE: {
            if (pkt.header.payload_size >= 9) {
                float lat, lon;
                memcpy(&lat, pkt.payload + 1, 4);
                memcpy(&lon, pkt.payload + 5, 4);
                const Node* existing = ctx_->nodeRegistry->findByHash(pkt.header.source_hash);
                if (existing) {
                    Node updated = *existing;
                    updated.lat = lat;
                    updated.lon = lon;
                    updated.last_seen = millis();
                    ctx_->nodeRegistry->upsert(updated);
                    onNodeUpdated(ctx_, updated);
                }
            }
            break;
        }
        case CTRL_HEALTH_PING: {
            Packet pong;
            memset(&pong, 0, sizeof(pong));
            pong.header.version = 1;
            pong.header.ttl = 1;
            pong.header.flags = FLAG_CONTROL;
            pong.header.priority = static_cast<uint8_t>(Priority::PRIO_HIGH);
            UUID::generate(pong.header.packet_id);
            memcpy(pong.header.source_hash, ctx_->selfNode->node_hash, 16);
            memcpy(pong.header.dest_hash, pkt.header.source_hash, 16);
            pong.payload[0] = CTRL_HEALTH_PONG;
            pong.header.payload_size = 1;
            enqueueOutgoing(ctx_, pong);
            break;
        }
        case CTRL_HEALTH_PONG: {
            const Node* n = ctx_->nodeRegistry->findByHash(pkt.header.source_hash);
            if (n) {
                Node updated = *n;
                updated.last_seen = millis();
                ctx_->nodeRegistry->upsert(updated);
            }
            break;
        }
        default:
            LOG_WARN(TAG, "Unknown control type: 0x%02X", ctrlType);
            break;
    }
}

void Dispatcher::handleFragment(Packet& pkt) {
    static uint8_t reassemblyBuf[4096];
    size_t reassembledLen = 0;

    bool complete = ctx_->fragmentManager->addFragment(
        pkt, reassemblyBuf, sizeof(reassemblyBuf), &reassembledLen);

    if (complete) {
        LOG_INFO(TAG, "Reassembly complete, %d bytes", (int)reassembledLen);
        Packet reassembled;
        memcpy(&reassembled.header, &pkt.header, sizeof(PacketHeader));
        reassembled.header.flags &= ~FLAG_FRAGMENTED;
        reassembled.header.fragment_index = 0;
        reassembled.header.total_fragments = 1;

        if (reassembledLen <= MAX_SINGLE_PAYLOAD) {
            memcpy(reassembled.payload, reassemblyBuf, reassembledLen);
            reassembled.header.payload_size = reassembledLen;
            if (isForUs(reassembled)) {
                handleData(reassembled, false); // Reassembled is not a duplicate
            } else {
                forwardPacket(reassembled);
            }
        } else {
            MeshEvent evt;
            evt.type = MeshEventType::PACKET_RECEIVED;
            memcpy(&evt.packetData.packet.header, &reassembled.header, sizeof(PacketHeader));
            size_t copyLen = reassembledLen < MAX_SINGLE_PAYLOAD ? reassembledLen : MAX_SINGLE_PAYLOAD;
            memcpy(evt.packetData.packet.payload, reassemblyBuf, copyLen);
            evt.packetData.packet.header.payload_size = copyLen;
            ctx_->eventBus->post(evt);
        }
    }
}

void Dispatcher::handleData(Packet& pkt, bool isDuplicate) {
    LOG_INFO(TAG, "Data packet for us, %d bytes%s", 
             pkt.header.payload_size, isDuplicate ? " (DUPLICATE)" : "");
    
    if (!isDuplicate) {
        onPacketReceived(ctx_, pkt, pkt.header.last_hop_mac);
    }

    MeshEvent evt;
    evt.type = MeshEventType::PACKET_RECEIVED;
    memcpy(&evt.packetData.packet, &pkt, sizeof(Packet));
    memcpy(evt.packetData.sender_mac, pkt.header.last_hop_mac, 6);
    evt.packetData.isDuplicate = isDuplicate;
    evt.packetData.isForUs = true;
    ctx_->eventBus->post(evt);
}

void Dispatcher::forwardPacket(Packet& pkt) {
    if (pkt.header.ttl == 0) {
        LOG_WARN(TAG, "TTL expired, dropping");
        onPacketDropped(ctx_, pkt, "TTL expired");
        return;
    }
    // FIRE EVENT: Notify application of through-traffic
    MeshEvent evt;
    evt.type = MeshEventType::PACKET_RECEIVED;
    memcpy(&evt.packetData.packet, &pkt, sizeof(Packet));
    memcpy(evt.packetData.sender_mac, pkt.header.last_hop_mac, 6);
    evt.packetData.isDuplicate = false; 
    evt.packetData.isForUs = false;
    ctx_->eventBus->post(evt);

    pkt.header.ttl--;

    // Update last_hop_mac to us
    uint8_t ourMac[6];
    ctx_->transport->getOwnMac(ourMac);
    memcpy(pkt.header.last_hop_mac, ourMac, 6);

    // --- DECIDE ROUTING STRATEGY ---
    uint8_t cachedMac[6];
    if (ctx_->routeCache->lookupNextHop(pkt.header.dest_hash, cachedMac)) {
        // We have a direct path back!
        pkt.header.routing_strategy = (uint8_t)RoutingStrategy::STRAT_DIRECT;
        LOG_INFO(TAG, "Forwarding via DIRECT strategy (cached)");
    } else if (pkt.header.dest_lat != 0.0f || pkt.header.dest_lon != 0.0f) {
        // No direct path, but we have coordinates
        pkt.header.routing_strategy = (uint8_t)RoutingStrategy::STRAT_GEO_FLOOD;
        LOG_INFO(TAG, "Forwarding via GEO_FLOOD strategy");
    } else {
        // Fallback to blind broadcast
        pkt.header.routing_strategy = (uint8_t)RoutingStrategy::STRAT_BROADCAST;
        LOG_INFO(TAG, "Forwarding via BROADCAST strategy");
    }

    if (ctx_->config->networkKeyLen > 0) {
        Security::signPacket(pkt, ctx_->config->networkKey, ctx_->config->networkKeyLen);
    }
    
    enqueueOutgoing(ctx_, pkt);
}

} // namespace mesh
