#pragma once

#include "mesh_core/Packet.hpp"
#include "mesh_core/MeshConfig.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdint>

namespace mesh {

static constexpr int MAX_ASSEMBLY_SLOTS = 8;
static constexpr int MAX_FRAGMENTS_PER_PACKET = 32;
static constexpr int ASSEMBLY_TIMEOUT_MS = 10000;

class FragmentManager {
public:
    FragmentManager();
    ~FragmentManager();

    // Fragment a large payload into multiple packets.
    // Fills outPkts array, returns count of fragments created.
    int fragment(const uint8_t* sourceHash, const uint8_t* destHash,
                 const uint8_t* packetId, uint8_t flags, uint8_t priority,
                 uint8_t strategy, float destLat, float destLon, float sourceDist,
                 uint8_t ttl, const uint8_t* payload, size_t payloadLen,
                 Packet* outPkts, int maxOut);

    // Add a received fragment. Returns true if reassembly is complete.
    // If complete, fills outPayload and outLen.
    bool addFragment(const Packet& pkt, uint8_t* outPayload, size_t outBufSize,
                     size_t* outLen);

    // Discard timed-out partial assemblies
    void pruneStale(int64_t nowMs);

private:
    struct AssemblySlot {
        uint8_t packetId[16];
        uint8_t fragments[MAX_FRAGMENTS_PER_PACKET][MAX_SINGLE_PAYLOAD];
        uint16_t fragSizes[MAX_FRAGMENTS_PER_PACKET];
        bool    received[MAX_FRAGMENTS_PER_PACKET];
        uint8_t totalFragments;
        uint8_t receivedCount;
        int64_t startedAtMs;
        bool    active;
    };
    AssemblySlot      slots_[MAX_ASSEMBLY_SLOTS];
    SemaphoreHandle_t mutex_;

    AssemblySlot* findSlot(const uint8_t packetId[16]);
    AssemblySlot* allocSlot(const uint8_t packetId[16]);
};

} // namespace mesh
