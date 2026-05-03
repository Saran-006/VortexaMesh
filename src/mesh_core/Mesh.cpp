#include "mesh_core/Mesh.hpp"
#include "mesh_core/Logger.hpp"
#include "mesh_core/Security.hpp"
#include "mesh_core/Dispatcher.hpp"
#include "mesh_core/RequestManager.hpp"
#include "mesh_transport/ESPNOWTransport.hpp"
#include "mesh_registry/NodeRegistry.hpp"
#include "mesh_registry/DedupCache.hpp"
#include "mesh_routing/DirectionalRouter.hpp"
#include "mesh_routing/RouteCache.hpp"
#include "mesh_reliability/AckManager.hpp"
#include "mesh_reliability/FragmentManager.hpp"
#include "mesh_tasks/ReceiverTask.hpp"
#include "mesh_tasks/SenderTask.hpp"
#include "mesh_tasks/DiscoveryTask.hpp"
#include "mesh_tasks/LocationTask.hpp"
#include "mesh_tasks/HealthTask.hpp"
#include "mesh_utils/Hash.hpp"
#include "mesh_utils/UUID.hpp"
#include <nvs_flash.h>
#include <cstring>
#include <Arduino.h>

static const char* TAG = "Mesh";

namespace mesh {

Mesh::Mesh()
    : transport_(nullptr), peerManager_(nullptr), nodeRegistry_(nullptr),
      dedupCache_(nullptr), router_(nullptr), ackManager_(nullptr),
      fragmentManager_(nullptr), dispatcher_(nullptr), routeCache_(nullptr),
      requestManager_(nullptr), locationProvider_(nullptr),
      receiverTaskHandle_(nullptr), dispatcherTaskHandle_(nullptr),
      senderTaskHandle_(nullptr), discoveryTaskHandle_(nullptr),
      locationTaskHandle_(nullptr), healthTaskHandle_(nullptr) {
    memset(&ctx_, 0, sizeof(ctx_));
}

Mesh::~Mesh() {
    ctx_.running = false;
    vTaskDelay(pdMS_TO_TICKS(500)); // let tasks exit

    delete transport_;
    delete peerManager_;
    delete nodeRegistry_;
    delete dedupCache_;
    delete router_;
    delete ackManager_;
    delete fragmentManager_;
    delete dispatcher_;
    delete routeCache_;
    delete requestManager_;
}

// ---- Production API: Configuration & Lifecycle ----

void Mesh::init(const MeshConfig& cfg) {
    config_ = cfg;
    init();
}

void Mesh::init() {
    LOG_INFO(TAG, "Initializing mesh (Framework Mode)...");

    // Initialize NVS
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Create subsystems
    transport_      = new ESPNOWTransport();
    peerManager_    = new PeerManager();
    nodeRegistry_   = new NodeRegistry(config_.maxPeers);
    dedupCache_     = new DedupCache(config_.dedupSize);
    router_         = new DirectionalRouter(nodeRegistry_, config_.angleThreshold,
                                            config_.geoProgressThresholdM);
    ackManager_     = new AckManager(config_.maxRetries, config_.retryBackoffMs);
    fragmentManager_ = new FragmentManager();
    routeCache_     = new RouteCache(config_.maxPeers);
    requestManager_ = new RequestManager();

    // Initialize event bus
    eventBus_.init(config_.eventQueueSize);

    // Setup print-fallback handlers BEFORE user can subscribe
    setupDefaultEventHandlers();

    // Create queues
    createQueues();

    // Initialize transport (WiFi STA + ESP-NOW)
    transport_->init(ctx_.rawIncomingQueue);

    // Initialize self node
    initSelfNode();

    // Initialize location provider
    if (locationProvider_) {
        locationProvider_->init();
    }

    // Wire up context
    ctx_.config          = &config_;
    ctx_.selfNode        = &selfNode_;
    ctx_.transport       = transport_;
    ctx_.peerManager     = peerManager_;
    ctx_.nodeRegistry    = nodeRegistry_;
    ctx_.dedupCache      = dedupCache_;
    ctx_.router          = router_;
    ctx_.ackManager      = ackManager_;
    ctx_.fragmentManager = fragmentManager_;
    ctx_.locationProvider = locationProvider_;
    ctx_.eventBus        = &eventBus_;
    ctx_.routeCache      = routeCache_;
    ctx_.requestManager  = requestManager_;
    ctx_.running         = false;

    // Create dispatcher (uses context)
    dispatcher_ = new Dispatcher(&ctx_);

    LOG_INFO(TAG, "Mesh initialized with %d peers limit", config_.maxPeers);
}

void Mesh::start() {
    ctx_.running = true;
    createTasks();

    MeshEvent evt;
    evt.type = MeshEventType::MESH_STARTED;
    eventBus_.post(evt);

    LOG_INFO(TAG, "Mesh started — node hash: %02X%02X%02X%02X...",
             selfNode_.node_hash[0], selfNode_.node_hash[1],
             selfNode_.node_hash[2], selfNode_.node_hash[3]);
}

// ---- Production API: Messaging (Direct & Typed) ----

bool Mesh::sendUDP(const uint8_t* destHash, const uint8_t* data, size_t len, bool ackRequired) {
    if (len > MAX_SINGLE_PAYLOAD) {
        Packet* fragments = new Packet[32];
        uint8_t pktId[16];
        UUID::generate(pktId);
        uint8_t flags = FLAG_DATA | FLAG_ROUTE_RECORD;
        if (ackRequired) flags |= FLAG_ACK_REQUIRED;

        float dLat = 0, dLon = 0, sourceDist = 0.0f;
        const Node* n = nodeRegistry_->findByHash(destHash);
        if (n) { 
            dLat = n->lat; dLon = n->lon; 
            if (selfNode_.hasLocation()) {
                sourceDist = DirectionalRouter::distanceM(selfNode_.lat, selfNode_.lon, dLat, dLon);
            }
        }

        bool isBroadcast = true;
        for (int i=0; i<16; i++) { if (destHash[i] != 0) { isBroadcast = false; break; } }

        uint8_t cachedMac[6];
        uint8_t strat = (uint8_t)RoutingStrategy::STRAT_BROADCAST;
        if (!isBroadcast) {
            if (routeCache_->lookupNextHop(destHash, cachedMac)) {
                strat = (uint8_t)RoutingStrategy::STRAT_DIRECT;
            } else if (dLat != 0.0f) {
                strat = (uint8_t)RoutingStrategy::STRAT_GEO_FLOOD;
            }
        }

        int fragCount = fragmentManager_->fragment(selfNode_.node_hash, destHash, pktId, flags, 
                                                  (uint8_t)Priority::PRIO_LOW, strat, dLat, dLon, sourceDist,
                                                  config_.ttlDefault, data, len, fragments, 32);
        if (fragCount <= 0) { delete[] fragments; return false; }
        for (int i = 0; i < fragCount; i++) enqueueOutgoing(&ctx_, fragments[i]);
        delete[] fragments;
        return true;
    }

    Packet pkt;
    memset(&pkt, 0, sizeof(Packet));
    pkt.header.version = 1;
    pkt.header.ttl = config_.ttlDefault;
    pkt.header.flags = FLAG_DATA | FLAG_ROUTE_RECORD;
    if (ackRequired) pkt.header.flags |= FLAG_ACK_REQUIRED;
    pkt.header.priority = (uint8_t)Priority::PRIO_LOW;
    UUID::generate(pkt.header.packet_id);
    memcpy(pkt.header.source_hash, selfNode_.node_hash, 16);
    memcpy(pkt.header.dest_hash, destHash, 16);

    const Node* n = nodeRegistry_->findByHash(destHash);
    if (n) {
        pkt.header.dest_lat = n->lat; pkt.header.dest_lon = n->lon;
        if (selfNode_.hasLocation()) {
            pkt.header.source_dist = DirectionalRouter::distanceM(selfNode_.lat, selfNode_.lon, n->lat, n->lon);
        }
    }

    bool isBroadcast = true;
    for (int i=0; i<16; i++) { if (destHash[i] != 0) { isBroadcast = false; break; } }

    uint8_t cachedMac[6];
    pkt.header.routing_strategy = (uint8_t)RoutingStrategy::STRAT_BROADCAST;
    if (!isBroadcast) {
        if (routeCache_->lookupNextHop(destHash, cachedMac)) {
            pkt.header.routing_strategy = (uint8_t)RoutingStrategy::STRAT_DIRECT;
        } else if (n && n->hasLocation()) {
            pkt.header.routing_strategy = (uint8_t)RoutingStrategy::STRAT_GEO_FLOOD;
        }
    }
    
    memcpy(pkt.payload, data, (len > MAX_SINGLE_PAYLOAD ? MAX_SINGLE_PAYLOAD : len));
    pkt.header.payload_size = len;
    return enqueueOutgoing(&ctx_, pkt);
}

bool Mesh::sendBroadcast(const uint8_t* data, size_t len) {
    uint8_t zeros[16] = {0};
    return sendUDP(zeros, data, len, false);
}

MeshResponse Mesh::sendTCP(const uint8_t* destHash, const uint8_t* data, size_t len, int timeoutMs) {
    MeshResponse fail; fail.success = false;
    if (len > MAX_SINGLE_PAYLOAD - 16) return fail;

    Packet pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.version = 1;
    pkt.header.ttl = config_.ttlDefault;
    pkt.header.flags = FLAG_DATA | FLAG_TCP_REQUEST | FLAG_ACK_REQUIRED | FLAG_ROUTE_RECORD;
    pkt.header.priority = (uint8_t)Priority::PRIO_MEDIUM;
    UUID::generate(pkt.header.packet_id);
    memcpy(pkt.header.source_hash, selfNode_.node_hash, 16);
    memcpy(pkt.header.dest_hash, destHash, 16);

    const Node* n = nodeRegistry_->findByHash(destHash);
    if (n) {
        pkt.header.dest_lat = n->lat; pkt.header.dest_lon = n->lon;
        if (selfNode_.hasLocation()) {
            pkt.header.source_dist = DirectionalRouter::distanceM(selfNode_.lat, selfNode_.lon, n->lat, n->lon);
        }
    }

    bool isBroadcast = true;
    for (int i=0; i<16; i++) { if (destHash[i] != 0) { isBroadcast = false; break; } }

    uint8_t cachedMac[6];
    pkt.header.routing_strategy = (uint8_t)RoutingStrategy::STRAT_BROADCAST;
    if (!isBroadcast) {
        if (routeCache_->lookupNextHop(destHash, cachedMac)) {
            pkt.header.routing_strategy = (uint8_t)RoutingStrategy::STRAT_DIRECT;
        } else if (n && n->hasLocation()) {
            pkt.header.routing_strategy = (uint8_t)RoutingStrategy::STRAT_GEO_FLOOD;
        }
    }

    memcpy(pkt.payload, data, len);
    pkt.header.payload_size = len;

    if (!requestManager_->registerRequest(pkt.header.packet_id)) return fail;
    if (!enqueueOutgoing(&ctx_, pkt)) { requestManager_->cancelRequest(pkt.header.packet_id); return fail; }

    return requestManager_->sendAndWait(pkt.header.packet_id, timeoutMs);
}

bool Mesh::sendGeo(float lat, float lon, const uint8_t* data, size_t len, bool ackRequired) {
    uint8_t zeros[16] = {0};
    Packet pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.version = 1;
    pkt.header.ttl = config_.ttlDefault;
    pkt.header.flags = FLAG_DATA | FLAG_ROUTE_RECORD;
    if (ackRequired) pkt.header.flags |= FLAG_ACK_REQUIRED;
    pkt.header.priority = (uint8_t)Priority::PRIO_LOW;
    UUID::generate(pkt.header.packet_id);
    memcpy(pkt.header.source_hash, selfNode_.node_hash, 16);
    memcpy(pkt.header.dest_hash, zeros, 16);

    pkt.header.dest_lat = lat;
    pkt.header.dest_lon = lon;
    if (selfNode_.hasLocation()) {
        pkt.header.source_dist = DirectionalRouter::distanceM(selfNode_.lat, selfNode_.lon, lat, lon);
    }
    pkt.header.routing_strategy = (uint8_t)RoutingStrategy::STRAT_GEO_FLOOD;

    memcpy(pkt.payload, data, (len > MAX_SINGLE_PAYLOAD ? MAX_SINGLE_PAYLOAD : len));
    pkt.header.payload_size = len;
    return enqueueOutgoing(&ctx_, pkt);
}

// ---- Production API: Event Convenience Layer (Sugar) ----

void Mesh::onPacketReceived(PacketHandler handler) {
    hasUserPacketHandler = true;
    eventBus_.subscribe(MeshEventType::PACKET_RECEIVED, [handler](const MeshEvent& ev, void*) {
        if (ev.packetData.isForUs) {
            handler(ev.packetData.packet, ev.packetData.sender_mac);
        }
    }, nullptr);
}

void Mesh::onNodeDiscovered(NodeHandler handler) {
    hasUserNodeHandler = true;
    eventBus_.subscribe(MeshEventType::NODE_DISCOVERED, [handler](const MeshEvent& ev, void*) {
        handler(ev.nodeData.node);
    }, nullptr);
}

void Mesh::onLocationUpdated(LocationHandler handler) {
    eventBus_.subscribe(MeshEventType::LOCATION_UPDATED, [handler](const MeshEvent& ev, void*) {
        handler(ev.locationData.lat, ev.locationData.lon);
    }, nullptr);
}

void Mesh::onPacketSent(SimplePacketHandler handler) {
    eventBus_.subscribe(MeshEventType::PACKET_SENT, [handler](const MeshEvent& ev, void*) {
        handler(ev.packetData.packet);
    }, nullptr);
}

void Mesh::onPacketDropped(SimplePacketHandler handler) {
    eventBus_.subscribe(MeshEventType::PACKET_DROPPED, [handler](const MeshEvent& ev, void*) {
        handler(ev.packetData.packet);
    }, nullptr);
}

void Mesh::onPacketAckReceived(SimplePacketHandler handler) {
    eventBus_.subscribe(MeshEventType::PACKET_ACK_RECEIVED, [handler](const MeshEvent& ev, void*) {
        handler(ev.packetData.packet);
    }, nullptr);
}

void Mesh::onPacketAckTimeout(SimplePacketHandler handler) {
    eventBus_.subscribe(MeshEventType::PACKET_ACK_TIMEOUT, [handler](const MeshEvent& ev, void*) {
        handler(ev.packetData.packet);
    }, nullptr);
}

void Mesh::onNodeLost(NodeHandler handler) {
    eventBus_.subscribe(MeshEventType::NODE_LOST, [handler](const MeshEvent& ev, void*) {
        handler(ev.nodeData.node);
    }, nullptr);
}

void Mesh::onNodeUpdated(NodeHandler handler) {
    eventBus_.subscribe(MeshEventType::NODE_UPDATED, [handler](const MeshEvent& ev, void*) {
        handler(ev.nodeData.node);
    }, nullptr);
}

void Mesh::onLocationLost(VoidHandler handler) {
    eventBus_.subscribe(MeshEventType::LOCATION_LOST, [handler](const MeshEvent& ev, void*) {
        handler();
    }, nullptr);
}

void Mesh::onServiceRegistered(ServiceHandler handler) {
    eventBus_.subscribe(MeshEventType::SERVICE_REGISTERED, [handler](const MeshEvent& ev, void*) {
        handler(ev.serviceData.service_id);
    }, nullptr);
}

void Mesh::onServiceUnregistered(ServiceHandler handler) {
    eventBus_.subscribe(MeshEventType::SERVICE_UNREGISTERED, [handler](const MeshEvent& ev, void*) {
        handler(ev.serviceData.service_id);
    }, nullptr);
}

void Mesh::onMeshStarted(VoidHandler handler) {
    eventBus_.subscribe(MeshEventType::MESH_STARTED, [handler](const MeshEvent& ev, void*) {
        handler();
    }, nullptr);
}

void Mesh::onMeshStopped(VoidHandler handler) {
    eventBus_.subscribe(MeshEventType::MESH_STOPPED, [handler](const MeshEvent& ev, void*) {
        handler();
    }, nullptr);
}

void Mesh::onMeshError(ErrorHandler handler) {
    eventBus_.subscribe(MeshEventType::MESH_ERROR, [handler](const MeshEvent& ev, void*) {
        handler(ev.errorData.error_code);
    }, nullptr);
}

// ---- Peer Lookup Utilities ----

int Mesh::getNodes(Node* outArr, int maxOut) {
    if (!nodeRegistry_) return 0;
    return nodeRegistry_->getAll(outArr, maxOut);
}

bool Mesh::getNodeHash(int index, uint8_t outHash[16]) {
    if (!nodeRegistry_) return false;
    Node* buf = new Node[config_.maxPeers];
    int cnt = nodeRegistry_->getAll(buf, config_.maxPeers);
    if (index < 0 || index >= cnt) {
        delete[] buf;
        return false;
    }
    memcpy(outHash, buf[index].node_hash, 16);
    delete[] buf;
    return true;
}

void Mesh::setupDefaultEventHandlers() {
    // Default Fallback: If no user handler is set, log received packets to Serial
    eventBus_.subscribe(MeshEventType::PACKET_RECEIVED, [this](const MeshEvent& ev, void*) {
        if (!hasUserPacketHandler && ev.packetData.isForUs && !ev.packetData.isDuplicate) {
            LOG_INFO("DEFAULT", "Packet from %02X:%02X... size=%d", 
                     ev.packetData.sender_mac[0], ev.packetData.sender_mac[1], ev.packetData.packet.header.payload_size);
        }
    }, nullptr);

    eventBus_.subscribe(MeshEventType::NODE_DISCOVERED, [this](const MeshEvent& ev, void*) {
        if (!hasUserNodeHandler) {
            LOG_INFO("DEFAULT", "New Node: %02X%02X...", ev.nodeData.node.node_hash[0], ev.nodeData.node.node_hash[1]);
        }
    }, nullptr);
}

void Mesh::initSelfNode() {
    uint8_t mac[6];
    transport_->getOwnMac(mac);
    memcpy(selfNode_.mac, mac, 6);
    Hash::nodeHashFromMac(mac, selfNode_.node_hash);
    selfNode_.lat = 0; selfNode_.lon = 0;
    selfNode_.last_seen = millis();
    LOG_INFO(TAG, "Self: MAC=%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void Mesh::createQueues() {
    ctx_.rawIncomingQueue = xQueueCreate(config_.eventQueueSize, sizeof(RawIncoming));
    ctx_.packetQueue = xQueueCreate(config_.eventQueueSize, sizeof(Packet));
    ctx_.outgoingQueueHigh = xQueueCreate(config_.outgoingQueueSize / 2 + 1, sizeof(Packet));
    ctx_.outgoingQueueMed = xQueueCreate(config_.outgoingQueueSize / 2 + 1, sizeof(Packet));
    ctx_.outgoingQueueLow = xQueueCreate(config_.outgoingQueueSize, sizeof(Packet));
}

void Mesh::createTasks() {
    xTaskCreatePinnedToCore(receiverTaskFn, "mesh_recv", 4096, &ctx_, 5, &receiverTaskHandle_, 1);
    xTaskCreatePinnedToCore(dispatcherTaskFn, "mesh_disp", 8192, &ctx_, 3, &dispatcherTaskHandle_, 1);
    xTaskCreatePinnedToCore(senderTaskFn, "mesh_send", 8192, &ctx_, 3, &senderTaskHandle_, 1);
    xTaskCreatePinnedToCore(discoveryTaskFn, "mesh_disc", 4096, &ctx_, 2, &discoveryTaskHandle_, 0);
    if (locationProvider_) xTaskCreatePinnedToCore(locationTaskFn, "mesh_loc", 4096, &ctx_, 1, &locationTaskHandle_, 0);
    xTaskCreatePinnedToCore(healthTaskFn, "mesh_health", 4096, &ctx_, 1, &healthTaskHandle_, 0);
}

} // namespace mesh
