#pragma once

#include "mesh_core/Node.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstdint>

namespace mesh {

class NodeRegistry {
public:
    explicit NodeRegistry(int maxNodes);
    ~NodeRegistry();

    // Add or update a node. Returns true if new.
    bool upsert(const Node& node);

    // Find a node by hash. Returns pointer or nullptr.
    const Node* findByHash(const uint8_t hash[16]) const;

    // Find a node by MAC. Returns pointer or nullptr.
    const Node* findByMac(const uint8_t mac[6]) const;

    // Remove nodes not seen within timeoutMs. Returns count removed.
    int pruneStale(int64_t nowMs, int timeoutMs);

    // Get number of known nodes
    int count() const;

    // Get max node capacity (config_.maxPeers)
    int capacity() const;

    // Remove a node by MAC address (Self-Healing)
    void removeByMac(const uint8_t mac[6]);

    // Iterate: fills outArr up to maxOut, returns count
    int getAll(Node* outArr, int maxOut) const;

private:
    Node*             nodes_;
    int               maxNodes_;
    int               count_;
    SemaphoreHandle_t mutex_;
};

} // namespace mesh
