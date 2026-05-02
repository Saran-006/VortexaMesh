#include "mesh_registry/NodeRegistry.hpp"
#include "mesh_core/Logger.hpp"
#include <cstring>
#include <Arduino.h>

static const char* TAG = "Registry";

namespace mesh {

NodeRegistry::NodeRegistry(int maxNodes) : maxNodes_(maxNodes), count_(0) {
    nodes_ = new Node[maxNodes_];
    for (int i = 0; i < maxNodes_; i++) {
        memset(&nodes_[i], 0, sizeof(Node));
    }
    mutex_ = xSemaphoreCreateMutex();
}

NodeRegistry::~NodeRegistry() {
    if (mutex_) vSemaphoreDelete(mutex_);
    delete[] nodes_;
}

bool NodeRegistry::upsert(const Node& node) {
    xSemaphoreTake(mutex_, portMAX_DELAY);

    // Check if already exists (by hash)
    for (int i = 0; i < maxNodes_; i++) {
        if (nodes_[i].last_seen > 0 && nodes_[i].hashEquals(node.node_hash)) {
            // Update existing
            memcpy(nodes_[i].mac, node.mac, 6);
            nodes_[i].lat = node.lat;
            nodes_[i].lon = node.lon;
            nodes_[i].last_seen = node.last_seen;
            xSemaphoreGive(mutex_);
            return false; // not new
        }
    }

    // Insert new
    if (count_ >= maxNodes_) {
        // Evict oldest
        int oldest = 0;
        int64_t oldestTime = nodes_[0].last_seen;
        for (int i = 1; i < maxNodes_; i++) {
            if (nodes_[i].last_seen < oldestTime && nodes_[i].last_seen > 0) {
                oldest = i;
                oldestTime = nodes_[i].last_seen;
            }
        }
        LOG_WARN(TAG, "Registry full, evicting oldest node");
        memset(&nodes_[oldest], 0, sizeof(Node));
        count_--;
        nodes_[oldest] = node;
        count_++;
        xSemaphoreGive(mutex_);
        return true;
    }

    for (int i = 0; i < maxNodes_; i++) {
        if (nodes_[i].last_seen == 0) {
            nodes_[i] = node;
            count_++;
            LOG_INFO(TAG, "New node registered (total: %d)", count_);
            xSemaphoreGive(mutex_);
            return true;
        }
    }

    xSemaphoreGive(mutex_);
    return false;
}

const Node* NodeRegistry::findByHash(const uint8_t hash[16]) const {
    for (int i = 0; i < maxNodes_; i++) {
        if (nodes_[i].last_seen > 0 && nodes_[i].hashEquals(hash)) {
            return &nodes_[i];
        }
    }
    return nullptr;
}

const Node* NodeRegistry::findByMac(const uint8_t mac[6]) const {
    for (int i = 0; i < maxNodes_; i++) {
        if (nodes_[i].last_seen > 0 && nodes_[i].macEquals(mac)) {
            return &nodes_[i];
        }
    }
    return nullptr;
}

int NodeRegistry::pruneStale(int64_t nowMs, int timeoutMs) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    int removed = 0;

    for (int i = 0; i < maxNodes_; i++) {
        if (nodes_[i].last_seen > 0 && (nowMs - nodes_[i].last_seen) > timeoutMs) {
            LOG_INFO(TAG, "Pruning stale node");
            memset(&nodes_[i], 0, sizeof(Node));
            count_--;
            removed++;
        }
    }

    xSemaphoreGive(mutex_);
    return removed;
}

void NodeRegistry::removeByMac(const uint8_t mac[6]) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    for (int i = 0; i < maxNodes_; i++) {
        if (nodes_[i].last_seen > 0 && memcmp(nodes_[i].mac, mac, 6) == 0) {
            memset(&nodes_[i], 0, sizeof(Node));
            count_--;
        }
    }
    xSemaphoreGive(mutex_);
}

int NodeRegistry::getAll(Node* outArr, int maxOut) const {
    xSemaphoreTake(mutex_, portMAX_DELAY);
    int idx = 0;
    for (int i = 0; i < maxNodes_ && idx < maxOut; i++) {
        if (nodes_[i].last_seen > 0) {
            outArr[idx++] = nodes_[i];
        }
    }
    xSemaphoreGive(mutex_);
    return idx;
}

int NodeRegistry::capacity() const {
    return maxNodes_;
}

} // namespace mesh
