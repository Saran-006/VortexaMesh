/*
 * VortexaMesh — Example 04: Interactive Serial CLI
 *
 * An interactive "Mesh Terminal" for the ESP32.
 * This sketch demonstrates how to initialize the mesh and hand control 
 * over to the interactive CLI (Serial Monitor).
 *
 * AVAILABLE COMMANDS:
 *   - ls              : List all nearby nodes
 *   - msg <hash> <txt>: Send an unacknowledged UDP message
 *   - tcp <hash> <txt>: Perform a reliable TCP handshake
 *   - geo <lat> <lon> <txt>: Route a message geographically
 *   - broadcast <txt> : Send to every node in range
 */

#include <MeshFramework.hpp>

// ---- Hardware Pins (ESP32) ----
#define GPS_RX_PIN        16
#define GPS_TX_PIN        17
#define INTERNAL_LED      2   // Built-in LED for traffic heartbeat
#define EXTERNAL_LED      4   // For remote control showcase
// WARNING: Do NOT use GPIO 1 or GPIO 3 — they are Serial TX/RX!

// ---- Global instances ----
mesh::Mesh         meshNode;
mesh::MeshTerminal terminal(meshNode);
mesh::GPSLocationProvider gps(GPS_RX_PIN, GPS_TX_PIN, 9600, 2);

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    pinMode(INTERNAL_LED, OUTPUT);
    digitalWrite(INTERNAL_LED, LOW);
    
    pinMode(EXTERNAL_LED, OUTPUT);
    digitalWrite(EXTERNAL_LED, LOW);

    Serial.println("\n========================================");
    Serial.println("  VortexaMesh Framework — CLI Terminal   ");
    Serial.println("========================================\n");
    Serial.println("Type 'ls' to see nodes or 'help' for commands.\n");

    // Initialize with a custom network key and GPS
    mesh::MeshConfig cfg;
    memcpy(cfg.networkKey, "MySecretMeshKey!", 16);
    cfg.networkKeyLen = 16;
    
    meshNode.init(cfg);
    meshNode.setLocationProvider(&gps);

    // ---- Event: New Node Discovered ----
    meshNode.onNodeDiscovered([](const mesh::Node& node) {
        Serial.printf("\n[APP] *** NEW NODE FOUND: %02X%02X... ***\n> ", 
            node.node_hash[0], node.node_hash[1]);
    });

    // ---- Event: Data Packet Received ----
    meshNode.onPacketReceived([](const mesh::Packet& pkt, const uint8_t mac[6]) {
        // Blink internal LED for any traffic
        digitalWrite(INTERNAL_LED, HIGH);

        // TCP responses have a 16-byte request ID prefix, standard UDP has none
        size_t offset = (pkt.header.flags & mesh::FLAG_TCP_RESPONSE) ? 16 : 0;
        
        if (pkt.header.payload_size > offset) {
            String data = String((const char*)(pkt.payload + offset), pkt.header.payload_size - offset);
            
            // Extract a short ID for logging
            char idStr[5];
            snprintf(idStr, 5, "%02X%02X", pkt.header.packet_id[0], pkt.header.packet_id[1]);

            Serial.printf("\n[RX] %s : %s\n> ", idStr, data.c_str());

            // REMOTE CONTROL: Toggle External LED if commanded
            if (data == "LED_ON") {
                digitalWrite(EXTERNAL_LED, HIGH);
                Serial.println("\n[APP] Command Executed: LED -> ON\n> ");
            } 
            else if (data == "LED_OFF") {
                digitalWrite(EXTERNAL_LED, LOW);
                Serial.println("\n[APP] Command Executed: LED -> OFF\n> ");
            }
        }

        delay(20); 
        digitalWrite(INTERNAL_LED, LOW); 
    });

    // Start background tasks
    meshNode.start();

    Serial.println("[OK] Mesh active. Type 'help' for commands.\n");
}

void loop() {
    // 1. Process background events
    meshNode.getEventBus().processOne(0);
    
    // 2. Read input from the Arduino IDE Serial Monitor
    terminal.processSerial();
}
