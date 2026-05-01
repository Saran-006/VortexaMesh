/*
 * VortexaMesh — Example 03: Programmatic CLI
 *
 * Demonstrates how to use the built-in MeshTerminal programmatically.
 * This allows you to pass string commands to the engine and capture the
 * output as a String, which is useful for building custom Web UIs,
 * remote dashboards, or Bluetooth-to-Mesh bridges.
 */

#include <MeshFramework.hpp>

mesh::Mesh         meshNode;
mesh::MeshTerminal terminal(meshNode);
unsigned long      lastCheck = 0;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n--- VortexaMesh Programmatic CLI Example ---");

    meshNode.init();
    meshNode.start();

    // The Mesh is running.
    Serial.println("Running automated 'ls' command every 10 seconds...");
}

void loop() {
    meshNode.getEventBus().processOne(0);

    // Every 10 seconds, execute the 'ls' command programmatically
    if (millis() - lastCheck > 10000) {
        lastCheck = millis();

        Serial.println("\n[APP] Requesting node list via Terminal...");
        
        // The 'true' parameter tells it to be quiet (don't echo to Serial automatically)
        // It returns the output so you can send it to a Web UI, BLE, Display, etc.
        String output = terminal.execute("ls", true);
        
        Serial.print("--- Captured Terminal Output ---\n");
        Serial.print(output);
        Serial.print("--------------------------------\n");
    }
}
