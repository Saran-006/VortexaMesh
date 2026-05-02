#include "mesh_tasks/DiscoveryTask.hpp"
#include "mesh_core/Logger.hpp"
#include "mesh_core/Packet.hpp"
#include "mesh_utils/UUID.hpp"
#include <cstring>
#include <Arduino.h>

static const char* TAG = "DiscTask";

namespace mesh {

void discoveryTaskFn(void* param) {
    MeshContext* ctx = static_cast<MeshContext*>(param);

    LOG_INFO(TAG, "Discovery task started");

    while (ctx->running) {
        // Build discovery beacon
        Packet beacon;
        memset(&beacon, 0, sizeof(beacon));

        beacon.header.version = 1;
        beacon.header.ttl = 1;   // single hop for beacons
        beacon.header.flags = FLAG_CONTROL | FLAG_ROUTE_RECORD;
        beacon.header.priority = static_cast<uint8_t>(Priority::PRIO_HIGH);

        UUID::generate(beacon.header.packet_id);
        memcpy(beacon.header.source_hash, ctx->selfNode->node_hash, 16);
        // dest_hash = all zeros → broadcast
        memset(beacon.header.dest_hash, 0, 16);
        beacon.header.routing_strategy = static_cast<uint8_t>(RoutingStrategy::STRAT_BROADCAST);

        // Payload: [CTRL_DISCOVERY_BEACON(1)] [lat(4)] [lon(4)]
        beacon.payload[0] = CTRL_DISCOVERY_BEACON;
        float lat = ctx->selfNode->lat;
        float lon = ctx->selfNode->lon;
        memcpy(beacon.payload + 1, &lat, 4);
        memcpy(beacon.payload + 5, &lon, 4);
        beacon.header.payload_size = 9;

        enqueueOutgoing(ctx, beacon);
        LOG_INFO(TAG, "Discovery beacon sent (lat=%.6f lon=%.6f)", lat, lon);

        vTaskDelay(pdMS_TO_TICKS(ctx->config->discoveryIntervalMs));
    }

    LOG_INFO(TAG, "Discovery task stopped");
    vTaskDelete(nullptr);
}

} // namespace mesh
