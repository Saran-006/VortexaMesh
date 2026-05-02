# VortexaMesh — Decentralized ESP32 Mesh Framework

A robust, FreeRTOS-based mesh networking framework for ESP32 devices utilizing the raw ESP-NOW protocol. VortexaMesh goes beyond simple packet delivery — it provides intelligent geographic routing, automatic self-healing, hardware-accelerated security, and a clean event-driven API that lets you build complex mesh applications with minimal boilerplate.

> **Quick Mental Model:** You send via the API. The internal engine handles routing, reliability, and transport. You react to network activity using event callbacks. That's it.

---

## Features

- **"Throw-and-Play" Self-Organization**: Nodes automatically discover each other on boot using beacon broadcasts and build a live peer registry — no manual configuration needed.
- **Progress-Based Geographic Routing**: Integrates GPS data (via `GPSLocationProvider`) to route packets directionally. Only nodes that are physically closer to the destination forward the packet, eliminating the "border waste" found in traditional flooding.
- **Symmetric Reverse Routing**: Automatically caches the exact path a TCP response traveled and reuses it for all subsequent messages, drastically reducing network flood volume.
- **Active Self-Healing**: A background `HealthTask` monitors node responsiveness. Dead nodes are pruned from the registry and route cache, and the framework automatically reroutes traffic via geo-flooding.
- **Dual-Core Task Segregation**: Radio-intensive tasks (Receiver, Dispatcher, Sender) are pinned to Core 1; logic tasks (Discovery, Health, Location) run on Core 0 — preventing watchdog panics.
- **Packet Fragmentation**: Automatically fragments and reassembles large payloads, overcoming ESP-NOW's 250-byte hardware limit. The routing strategy is preserved across all fragments.
- **Reliability Layers**: Supports fire-and-forget UDP-style and blocking TCP-style request/response with `AckManager` retries.
- **Hardware Limit Bypassing**: ESP-NOW natively limits devices to 20 hardware peers. VortexaMesh uses an automatic LRU cache to seamlessly swap MAC addresses in and out of the radio hardware, allowing networks to scale to hundreds of nodes.
- **Hardware-Accelerated Security**: SHA-256 HMAC packet signing via `mbedtls` on every frame.
- **Full Event System**: A typed `EventBus` with sugar-coated convenience methods (`onPacketReceived`, `onNodeLost`, etc.) for all 15 network events.
- **Three Integration Modes**: Use the Serial CLI for debugging, call `terminal.execute()` for programmatic control, or call the direct API (`sendUDP`, `sendTCP`, `sendGeo`, `sendBroadcast`) for maximum performance.

---

## Architecture

```
User Code
   │
   ▼
[Direct API]  ──or──  [CLI Terminal]
   │
   ▼
 Queue  (High / Med / Low priority)
   │
   ▼
SenderTask (Core 1) ── SHA-256 Sign ── ESP-NOW
                                          │
                                    (Over the air)
                                          │
                                    ReceiverTask (Core 1)
                                          │
                                     Dispatcher
                                          │
                                       EventBus
                                     /    |    \
                              Handler1  Handler2  Handler3
```

### Routing Decision Logic
```
Send Packet
     │
     ▼
Cached direct path? ──YES──► STRAT_DIRECT
     │
     NO
     ▼
Destination coordinates known? ──YES──► STRAT_GEO_FLOOD (progress-filtered)
     │
     NO
     ▼
STRAT_BROADCAST (discovery fallback only)
```

---

## Quick Start

```cpp
#include <MeshFramework.hpp>

mesh::Mesh meshNode;
// GPS is optional — remove if not using geographic routing
mesh::GPSLocationProvider gps(16, 17, 9600, 2);

void setup() {
    Serial.begin(115200);

    // 1. Bind GPS to the engine (must happen before init)
    meshNode.setLocationProvider(&gps);

    // 2. Option A: Default config — just works, no setup needed
    meshNode.init();

    // Option B: Custom config
    // mesh::MeshConfig cfg;
    // cfg.maxPeers = 32;
    // memcpy(cfg.networkKey, "MySecretMeshKey!", 16);
    // cfg.networkKeyLen = 16;
    // meshNode.init(cfg);

    // React to incoming data
    meshNode.onPacketReceived([](const mesh::Packet& pkt, const uint8_t mac[6]) {
        Serial.printf("Got %d bytes!\n", pkt.header.payload_size);
    });

    meshNode.onNodeDiscovered([](const mesh::Node& node) {
        Serial.printf("Peer found: %02X%02X... at (%.4f, %.4f)\n",
            node.node_hash[0], node.node_hash[1], node.lat, node.lon);
    });

    meshNode.start();
}

void loop() {
    // Required: drives all event callbacks
    meshNode.getEventBus().processOne(0);
}
```

---

## Three Ways to Use VortexaMesh

### Mode 1 — Direct API Calls (Recommended)
Call functions directly from your code for maximum performance. All functions return typed statuses.

| Method | Returns | Description |
| :--- | :--- | :--- |
| `sendBroadcast(data, len)` | `bool` | Sends to all nodes in range. |
| `sendUDP(hash, data, len)` | `bool` | Unicast, fire-and-forget. |
| `sendTCP(hash, data, len)` | `MeshResponse` | Blocking request-response. |
| `sendGeo(lat, lon, data, len)` | `bool` | Geographic target flood. |

```cpp
// Step 1: Get a peer's hash by index (call this after nodes have had time to discover)
//         getNodeHash(index, outHash) — same data the 'ls' command shows
uint8_t targetHash[16];
if (meshNode.getNodeHash(0, targetHash)) {        // index 0 = first peer

    // UDP unicast — fire and forget
    bool ok = meshNode.sendUDP(targetHash, (uint8_t*)"Hello", 5);

    // TCP — blocks until response or timeout
    auto res = meshNode.sendTCP(targetHash, (uint8_t*)"Ping", 4);
    if (res.success) Serial.printf("Reply: %d bytes\n", res.payloadLen);
}

// Broadcast to everyone — no hash needed
bool ok = meshNode.sendBroadcast((uint8_t*)"Hello All", 9);

// Geographic flood — no hash needed, targets a physical location
meshNode.sendGeo(12.9716, 77.5946, (uint8_t*)"Alert", 5);

// Iterate all known peers — returns full Node structs with all data
mesh::Node peers[64];
int count = meshNode.getNodes(peers, 64);
for (int i = 0; i < count; i++) {
    // Full 16-byte hash (copy-paste into msg/tcp commands)
    char hashStr[33] = {0};
    for (int j = 0; j < 16; j++) sprintf(&hashStr[j*2], "%02X", peers[i].node_hash[j]);
    Serial.printf("Peer %d: HASH:%s | POS:(%.6f, %.6f) | SEEN:%lus\n",
        i, hashStr, peers[i].lat, peers[i].lon,
        (millis() - peers[i].last_seen) / 1000);
}
```

---

### Config Reference

| Property | Default | Description |
| :--- | :--- | :--- |
| `maxPeers` | 20 | Capacity of the peer registry. If > 20, automatically utilizes LRU hardware-eviction to bypass ESP-NOW's physical 20-peer limitation. Memory dynamically scales based on this value. |
| `ttlDefault` | 10 | Max hops before packet is dropped. |
| `angleThreshold` | 90.0f | Directional cone width (degrees). Primary filter for geo-flood — only peers within this angle toward the destination are considered. Set to `180.0f` for omnidirectional (progress-only) mode. |
| `geoProgressThresholdM` | 50.0f | How much further from the destination a relay peer can be compared to the **original sender** (`peerDist <= originalSenderDist + threshold`). Positive (e.g. `50.0f`) = allows 50m of backward routing to navigate around rivers or buildings (use with `angleThreshold = 270°`). **⚠️ No GPS Fallback:** If the original sender didn't have a GPS lock, the framework safely ignores this threshold and falls back to strict forward progress relative to each hop. |
| `geoDestinationRadiusM` | 20.0f | Radius (meters) within which a geo-packet is accepted as "arrived" and delivered to the app. Nodes outside silently continue forwarding. |
| `nodeTimeoutMs` | 30000 | Silence timeout before a node is pruned (ms). |
| `discoveryIntervalMs` | 10000 | How often beacons are sent (ms). |
| `networkKey` | (empty) | 1–32 byte key for SHA-256 packet signing. |

> **💡 Deployment Best Practices:**
> * **Know your terrain:** The defaults (`angleThreshold=270`, `geoProgressThresholdM=50`) are designed for extreme robustness to route around voids (buildings/lakes). If you have a clear line-of-sight, drop the angle for higher efficiency.
> * **Node Spacing:** Always place nodes at roughly **70% of max radio range** for optimal efficiency and minimal collision overlap.
> * **Topology:** A vertex/hexagonal geometric placement is the mathematical ideal. But in emergency or disaster zones, simply "throwing them in range" will work fine.
> * **Memory:** Set `maxPeers = 10` for better RAM stability in dense networks unless you specifically need to track a massive neighbor table.



### Mode 2 — Programmatic Command Pass
Call the terminal engine from your code and capture the output as a `String`. Useful for dashboards, remote monitoring, or custom UIs.

```cpp
mesh::MeshTerminal terminal(meshNode);

// Pass a command string and get the result back as a String
String nodeList = terminal.execute("ls");
Serial.print(nodeList);

String result = terminal.execute("msg AABBCCDD Hello");
// result contains success/error response as a string
```

---

### Mode 3 — Serial CLI
Type commands directly into the Arduino IDE Serial Monitor (115200 baud) for live debugging and manual control.

```cpp
mesh::MeshTerminal terminal(meshNode);

void loop() {
    // Both lines are required when using the Serial CLI:
    meshNode.getEventBus().processOne(0); // drives event callbacks
    terminal.processSerial();             // reads and executes Serial input
}
```

| Command | Description |
| :--- | :--- |
| `help` | Show all available commands. |
| `ls` | List all discovered nodes. Shows **full 16-byte hash**, MAC, GPS (6 decimal places), and age. Copy the hash directly into `msg`/`tcp` commands. |
| `msg <hash> <text>` | Send a UDP message. |
| `tcp <hash> <text>` | Send a reliable TCP request. |
| `geo <lat> <lon> <text>` | Send a geographically routed message. |
| `broadcast <text>` | Broadcast to all nodes. |

---


## Documentation

See **[dev-manual.md](./dev-manual.md)** for the full developer reference, including:
- Detailed routing decision logic and source file map
- All 15 event types with sugar methods and data sources
- Error handling guide
- Event signature unpacking reference
- Framework extension points

---

## Hardware

- **MCU:** ESP32 Dev Module (or compatible)
- **GPS:** NEO-6M or any UART GPS module
- **Power:** LM2596S buck converter for stable off-grid operation

---

## Dependencies

- ESP32 Arduino Core
- `mbedtls` (bundled with ESP32 Arduino Core)

---

## License

MIT License
