/*
 * VortexaMesh — Example 01: Minimal Node
 *
 * The absolute bare minimum code required to join a VortexaMesh network.
 * Flash this onto two or more ESP32s, open the Serial Monitor, and watch
 * them autonomously discover each other without any manual configuration.
 */

#include <MeshFramework.hpp>

mesh::Mesh meshNode;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n--- VortexaMesh Minimal Node ---");

    // Option A: Initialize with default settings (zero config required)
    meshNode.init();

    // Hook into discovery event to prove the network is forming
    meshNode.onNodeDiscovered([](const mesh::Node& node) {
        Serial.printf("[MESH] Discovered new node! Hash: %02X%02X...\n", 
            node.node_hash[0], node.node_hash[1]);
    });

    // Start background network tasks
    meshNode.start();
    Serial.println("Mesh started. Waiting for peers...");
}

void loop() {
    // Required: Drives the internal event callbacks
    meshNode.getEventBus().processOne(0);
}
