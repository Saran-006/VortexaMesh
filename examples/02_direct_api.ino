/*
 * VortexaMesh — Example 02: Direct API Calls
 *
 * Demonstrates how to use the high-performance programmatic API directly,
 * bypassing the CLI. Shows how to broadcast, fetch node hashes, send UDP,
 * and handle reliable TCP responses.
 */

#include <MeshFramework.hpp>

mesh::Mesh meshNode;
unsigned long lastSendTime = 0;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n--- VortexaMesh Direct API Example ---");

    meshNode.init();

    // Listen for incoming packets targeted at us
    meshNode.onPacketReceived([](const mesh::Packet& pkt, const uint8_t mac[6]) {
        // TCP payloads have a 16-byte request ID prefix, UDP does not
        size_t offset = (pkt.header.flags & mesh::FLAG_TCP_RESPONSE) ? 16 : 0;
        if (pkt.header.payload_size > offset) {
            String data = String((const char*)(pkt.payload + offset), pkt.header.payload_size - offset);
            Serial.printf("[RX] Received from %02X:%02X: %s\n", mac[0], mac[1], data.c_str());
        }
    });

    meshNode.start();
    Serial.println("Mesh started. Will send automated test packets every 5 seconds.");
}

void loop() {
    meshNode.getEventBus().processOne(0);

    // Send a burst of different packet types every 5 seconds
    if (millis() - lastSendTime > 5000) {
        lastSendTime = millis();

        // 1. Broadcast (discovery/blind flood)
        meshNode.sendBroadcast((uint8_t*)"Ping All", 8);
        Serial.println("\n[TX] Sent Broadcast to all nearby nodes.");

        // 2. Fetch a specific node to send a targeted message
        uint8_t targetHash[16];
        if (meshNode.getNodeHash(0, targetHash)) { // Get 1st discovered node
            
            // Send UDP Unicast (Fire-and-forget)
            meshNode.sendUDP(targetHash, (uint8_t*)"Hello UDP", 9);
            Serial.println("[TX] Sent UDP Unicast to Node 0");

            // Send TCP Unicast (Blocks, waits for symmetric reverse ACK)
            Serial.println("[TX] Sending TCP Request (waiting for reply...)");
            auto res = meshNode.sendTCP(targetHash, (uint8_t*)"Hello TCP", 9);
            
            if (res.success) {
                // Read response payload (offset by 16 bytes for request ID)
                String reply = String((const char*)(res.responsePacket.payload + 16), res.payloadLen);
                Serial.printf("[TX] TCP Success! Reply: %s\n", reply.c_str());
            } else {
                Serial.println("[TX] TCP Failed: Timeout or Unreachable.");
            }
        } else {
            Serial.println("[TX] Waiting for at least 1 node to join before sending Unicasts...");
        }
    }
}
