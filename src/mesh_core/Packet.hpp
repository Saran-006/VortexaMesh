#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>

namespace mesh {

// ---- Flags ----
constexpr uint8_t FLAG_ACK_REQUIRED  = 0x01;
constexpr uint8_t FLAG_ACK           = 0x02;
constexpr uint8_t FLAG_FRAGMENTED    = 0x04;
constexpr uint8_t FLAG_CONTROL       = 0x08;
constexpr uint8_t FLAG_DATA          = 0x10;
constexpr uint8_t FLAG_ROUTE_RECORD  = 0x20;
constexpr uint8_t FLAG_TCP_REQUEST   = 0x40;
constexpr uint8_t FLAG_TCP_RESPONSE  = 0x80;

// ---- Routing Strategy ----
enum class RoutingStrategy : uint8_t {
    STRAT_DIRECT    = 0,  // Use specific next hop (RouteCache)
    STRAT_GEO_FLOOD = 1,  // Multi-target directional flood
    STRAT_BROADCAST = 2   // Universal flood (blind broadcast)
};

// ---- Priority ----
enum class Priority : uint8_t {
    PRIO_LOW    = 0,
    PRIO_MEDIUM = 1,
    PRIO_HIGH   = 2
};

// ---- Packed Packet Header ----
struct __attribute__((packed)) PacketHeader {
    uint8_t  version;
    uint8_t  ttl;
    uint8_t  flags;
    uint8_t  priority;
    uint8_t  routing_strategy;
    uint16_t payload_size;

    uint8_t  packet_id[16];
    uint8_t  source_hash[16];
    uint8_t  dest_hash[16];

    uint8_t  last_hop_mac[6];

    uint8_t  fragment_index;
    uint8_t  total_fragments;

    float    dest_lat;
    float    dest_lon;
    float    source_dist; // Distance from original sender to destination
};

// Signature appended after payload
constexpr size_t SIGNATURE_SIZE = 16;

// Maximum raw ESP-NOW frame
constexpr size_t ESPNOW_MAX_DATA_LEN = 250;

// Maximum payload that fits in a single ESP-NOW frame
constexpr size_t MAX_SINGLE_PAYLOAD = ESPNOW_MAX_DATA_LEN - sizeof(PacketHeader) - SIGNATURE_SIZE;

// ---- Control sub-types (first byte of payload for control packets) ----
constexpr uint8_t CTRL_DISCOVERY_BEACON   = 0x01;
constexpr uint8_t CTRL_DISCOVERY_RESPONSE = 0x02;
constexpr uint8_t CTRL_LOCATION_UPDATE    = 0x03;
constexpr uint8_t CTRL_HEALTH_PING        = 0x04;
constexpr uint8_t CTRL_HEALTH_PONG        = 0x05;

// ---- Full Packet ----
struct Packet {
    PacketHeader header;
    uint8_t      payload[MAX_SINGLE_PAYLOAD];
    uint8_t      signature[SIGNATURE_SIZE];

    size_t wireSize() const {
        return sizeof(PacketHeader) + header.payload_size + SIGNATURE_SIZE;
    }

    size_t serialize(uint8_t* buf, size_t bufLen) const {
        size_t total = wireSize();
        if (bufLen < total) return 0;
        memcpy(buf, &header, sizeof(PacketHeader));
        if (header.payload_size > 0) {
            memcpy(buf + sizeof(PacketHeader), payload, header.payload_size);
        }
        memcpy(buf + sizeof(PacketHeader) + header.payload_size, signature, SIGNATURE_SIZE);
        return total;
    }

    static bool deserialize(const uint8_t* buf, size_t len, Packet& out) {
        if (len < sizeof(PacketHeader) + SIGNATURE_SIZE) return false;
        memcpy(&out.header, buf, sizeof(PacketHeader));
        size_t expected = sizeof(PacketHeader) + out.header.payload_size + SIGNATURE_SIZE;
        if (len < expected) return false;
        if (out.header.payload_size > MAX_SINGLE_PAYLOAD) return false;
        if (out.header.payload_size > 0) {
            memcpy(out.payload, buf + sizeof(PacketHeader), out.header.payload_size);
        }
        memcpy(out.signature, buf + sizeof(PacketHeader) + out.header.payload_size, SIGNATURE_SIZE);
        return true;
    }

    bool isAckRequired() const { return (header.flags & FLAG_ACK_REQUIRED) != 0; }
    bool isAck()         const { return (header.flags & FLAG_ACK) != 0; }
    bool isFragmented()  const { return (header.flags & FLAG_FRAGMENTED) != 0; }
    bool isControl()     const { return (header.flags & FLAG_CONTROL) != 0; }
    bool isData()        const { return (header.flags & FLAG_DATA) != 0; }
};

// ---- Raw data received from ESP-NOW callback ----
struct RawIncoming {
    uint8_t senderMac[6];
    uint8_t data[ESPNOW_MAX_DATA_LEN];
    int     length;
};

} // namespace mesh
