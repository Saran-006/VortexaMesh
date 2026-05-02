#pragma once

#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace mesh {

static constexpr int PEER_TABLE_MAX = 20;

class PeerManager {
public:
    PeerManager();
    ~PeerManager();

    bool addPeer(const uint8_t mac[6]);
    bool removePeer(const uint8_t mac[6]);
    bool hasPeer(const uint8_t mac[6]) const;
    int  peerCount() const;

private:
    struct PeerEntry {
        uint8_t mac[6];
        bool    active;
    };
    PeerEntry          peers_[PEER_TABLE_MAX];
    int                count_;
    int                evictIndex_;
    SemaphoreHandle_t  mutex_;
};

} // namespace mesh
