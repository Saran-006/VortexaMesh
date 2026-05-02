#include "mesh_reliability/FragmentManager.hpp"
#include "mesh_core/Logger.hpp"
#include "mesh_utils/UUID.hpp"
#include <cstring>
#include <Arduino.h>

static const char* TAG = "FragMgr";

namespace mesh {

FragmentManager::FragmentManager() {
    for (int i = 0; i < MAX_ASSEMBLY_SLOTS; i++) {
        slots_[i].active = false;
    }
    mutex_ = xSemaphoreCreateMutex();
}

FragmentManager::~FragmentManager() {
    if (mutex_) vSemaphoreDelete(mutex_);
}

int FragmentManager::fragment(const uint8_t* sourceHash, const uint8_t* destHash,
                              const uint8_t* packetId, uint8_t flags, uint8_t priority,
                              uint8_t strategy, float destLat, float destLon, float sourceDist,
                              uint8_t ttl, const uint8_t* payload, size_t payloadLen,
                              Packet* outPkts, int maxOut) {
    size_t maxChunk = MAX_SINGLE_PAYLOAD;
    int totalFragments = (payloadLen + maxChunk - 1) / maxChunk;

    if (totalFragments > MAX_FRAGMENTS_PER_PACKET) {
        LOG_ERROR(TAG, "Payload too large: %d bytes, max fragments: %d",
                  (int)payloadLen, MAX_FRAGMENTS_PER_PACKET);
        return 0;
    }

    if (totalFragments > maxOut) {
        LOG_ERROR(TAG, "Not enough output slots: need %d, have %d", totalFragments, maxOut);
        return 0;
    }

    for (int i = 0; i < totalFragments; i++) {
        memset(&outPkts[i], 0, sizeof(Packet));

        outPkts[i].header.version = 1;
        outPkts[i].header.ttl = ttl;
        outPkts[i].header.flags = flags | FLAG_FRAGMENTED;
        outPkts[i].header.priority = priority;
        outPkts[i].header.routing_strategy = strategy;

        memcpy(outPkts[i].header.packet_id, packetId, 16);
        memcpy(outPkts[i].header.source_hash, sourceHash, 16);
        memcpy(outPkts[i].header.dest_hash, destHash, 16);

        outPkts[i].header.fragment_index = i;
        outPkts[i].header.total_fragments = totalFragments;
        outPkts[i].header.dest_lat = destLat;
        outPkts[i].header.dest_lon = destLon;
        outPkts[i].header.source_dist = sourceDist;

        size_t offset = i * maxChunk;
        size_t chunkLen = payloadLen - offset;
        if (chunkLen > maxChunk) chunkLen = maxChunk;

        memcpy(outPkts[i].payload, payload + offset, chunkLen);
        outPkts[i].header.payload_size = chunkLen;
    }

    LOG_INFO(TAG, "Fragmented %d bytes into %d fragments (Strategy: %d)", (int)payloadLen, totalFragments, strategy);
    return totalFragments;
}

FragmentManager::AssemblySlot* FragmentManager::findSlot(const uint8_t packetId[16]) {
    for (int i = 0; i < MAX_ASSEMBLY_SLOTS; i++) {
        if (slots_[i].active && memcmp(slots_[i].packetId, packetId, 16) == 0) {
            return &slots_[i];
        }
    }
    return nullptr;
}

FragmentManager::AssemblySlot* FragmentManager::allocSlot(const uint8_t packetId[16]) {
    // Find free slot
    for (int i = 0; i < MAX_ASSEMBLY_SLOTS; i++) {
        if (!slots_[i].active) {
            memset(&slots_[i], 0, sizeof(AssemblySlot));
            memcpy(slots_[i].packetId, packetId, 16);
            slots_[i].active = true;
            slots_[i].startedAtMs = millis();
            return &slots_[i];
        }
    }

    // Evict oldest
    int oldest = 0;
    int64_t oldestTime = slots_[0].startedAtMs;
    for (int i = 1; i < MAX_ASSEMBLY_SLOTS; i++) {
        if (slots_[i].active && slots_[i].startedAtMs < oldestTime) {
            oldest = i;
            oldestTime = slots_[i].startedAtMs;
        }
    }
    LOG_WARN(TAG, "Assembly slots full, evicting oldest");
    memset(&slots_[oldest], 0, sizeof(AssemblySlot));
    memcpy(slots_[oldest].packetId, packetId, 16);
    slots_[oldest].active = true;
    slots_[oldest].startedAtMs = millis();
    return &slots_[oldest];
}

bool FragmentManager::addFragment(const Packet& pkt, uint8_t* outPayload,
                                   size_t outBufSize, size_t* outLen) {
    xSemaphoreTake(mutex_, portMAX_DELAY);

    AssemblySlot* slot = findSlot(pkt.header.packet_id);
    if (!slot) {
        slot = allocSlot(pkt.header.packet_id);
    }

    uint8_t idx = pkt.header.fragment_index;
    if (idx >= MAX_FRAGMENTS_PER_PACKET) {
        LOG_WARN(TAG, "Fragment index %d out of range", idx);
        xSemaphoreGive(mutex_);
        return false;
    }

    slot->totalFragments = pkt.header.total_fragments;

    if (!slot->received[idx]) {
        memcpy(slot->fragments[idx], pkt.payload, pkt.header.payload_size);
        slot->fragSizes[idx] = pkt.header.payload_size;
        slot->received[idx] = true;
        slot->receivedCount++;
    }

    LOG_INFO(TAG, "Fragment %d/%d received (%d/%d done)",
             idx + 1, slot->totalFragments, slot->receivedCount, slot->totalFragments);

    if (slot->receivedCount >= slot->totalFragments) {
        // Reassemble
        size_t totalLen = 0;
        for (int i = 0; i < slot->totalFragments; i++) {
            totalLen += slot->fragSizes[i];
        }

        if (totalLen > outBufSize) {
            LOG_ERROR(TAG, "Reassembly buffer too small: %d > %d", (int)totalLen, (int)outBufSize);
            slot->active = false;
            xSemaphoreGive(mutex_);
            return false;
        }

        size_t offset = 0;
        for (int i = 0; i < slot->totalFragments; i++) {
            memcpy(outPayload + offset, slot->fragments[i], slot->fragSizes[i]);
            offset += slot->fragSizes[i];
        }
        *outLen = totalLen;
        slot->active = false;

        xSemaphoreGive(mutex_);
        return true;
    }

    xSemaphoreGive(mutex_);
    return false;
}

void FragmentManager::pruneStale(int64_t nowMs) {
    xSemaphoreTake(mutex_, portMAX_DELAY);

    for (int i = 0; i < MAX_ASSEMBLY_SLOTS; i++) {
        if (slots_[i].active &&
            (nowMs - slots_[i].startedAtMs) > ASSEMBLY_TIMEOUT_MS) {
            LOG_WARN(TAG, "Discarding timed-out assembly slot %d (%d/%d fragments)",
                     i, slots_[i].receivedCount, slots_[i].totalFragments);
            slots_[i].active = false;
        }
    }

    xSemaphoreGive(mutex_);
}

} // namespace mesh
