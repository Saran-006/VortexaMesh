# VortexaMesh Developer Manual

Welcome to the **VortexaMesh** developer manual. This document covers the architecture, routing logic, data structures, and the three primary ways to interact with the mesh framework.

> **🧠 Quick Mental Model**
> VortexaMesh is an event-driven mesh framework where:
> - You **send** data via the API (`sendUDP`, `sendTCP`, `sendGeo`) or the Serial CLI
> - The **internal engine** autonomously handles routing decisions, reliability retries, and hardware transport
> - You **react** to network activity by attaching event callbacks — the framework calls them when things happen
>
> That's it. You don't manage connections, sockets, or addresses. You send, and you listen.

---

## 📑 Table of Contents
1. [Architecture & Workflow](#1-architecture--workflow)
2. [Routing Decision Logic](#2-routing-decision-logic)
3. [Configuration (MeshConfig)](#3-configuration-meshconfig)
4. [Events Available](#4-events-available)
5. [Core Structures & Enums](#5-core-structures--enums)
6. [Event System & Subscriptions](#6-event-system--subscriptions)
7. [Defining Event Signatures](#7-defining-event-signatures)
8. [Ways of Using Mesh APIs](#8-ways-of-using-mesh-apis)
9. [Error Handling](#9-error-handling)
10. [Self-Healing Mechanism](#10-self-healing-mechanism)
11. [Extending the Framework](#11-extending-the-framework)
12. [Example Code](#12-example-code)

---

## 1. Architecture & Workflow

VortexaMesh operates as a multi-threaded system pinned to both cores of the ESP32.

### System Pipeline (ASCII Diagram)
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
SenderTask  ──── SHA-256 Sign ──── ESP-NOW Transport
                                         │
                                         ▼
                                  (Over the air)
                                         │
                                         ▼
                                   ReceiverTask
                                         │
                                    Verify + Dedup
                                         │
                                         ▼
                                     Dispatcher
                                         │
                                       EventBus
                                     /    |    \
                              Handler1  Handler2  Handler3
                          (multiple handlers per event allowed)
```

### Internal Step-by-Step:
1. **Initiation:** User calls `sendUDP`, `sendTCP`, `sendGeo`, or `sendBroadcast`.
2. **Strategy:** Routing logic determines the forwarding method (see Section 2).
3. **Queueing:** Packet enters the priority queue (`High`, `Med`, or `Low`).
4. **Processing:** `SenderTask` (Core 1) adds SHA-256 signature and transmits via raw ESP-NOW.
5. **Reception:** Remote node's `ReceiverTask` (Core 1) picks up the raw frame.
6. **Dispatch:** Frame is verified, deduplicated, and passed to the `Dispatcher`.
7. **EventBus:** `Dispatcher` posts a typed `MeshEvent` to the `EventBus`.
8. **Handlers:** All subscribed callbacks for that event type are invoked. If no handler is registered, the built-in default logger prints to Serial.

---

## 2. Routing Decision Logic

> **This is the core innovation of VortexaMesh.** Every packet transmission follows a smart, layered decision tree rather than blindly flooding the network.

### Decision Flow:
```
Send Packet
     │
     ▼
Is there a cached direct path to destination?
     │
    YES ──► STRAT_DIRECT  (point-to-point to specific MAC)
     │
     NO
     │
     ▼
Does the destination node have known GPS coordinates?
     │
    YES ──► STRAT_GEO_FLOOD  (directional flood toward coordinates)
     │                │
     │                └── Progress-Based Filter:
     │                    Only relay nodes that are physically closer
     │                    to the destination than the current node.
     │                    (Eliminates "border waste" at flood edges)
     │
     NO
     │
     ▼
STRAT_BROADCAST  (blind flood — used for discovery only)
```

### How the Dispatcher re-evaluates on each hop:
Every intermediate relay node independently re-runs this logic so that the route can dynamically switch to a direct path if a route is cached from a previous TCP response.

> **Why this matters:** This progress-based approach significantly reduces redundant transmissions compared to traditional broadcast flooding — improving overall bandwidth efficiency, reducing edge-node congestion, and making VortexaMesh practical for dense environments. It is also a key differentiator from standard mesh protocols like ESP-Mesh or Zigbee flooding.

### Relevant Source Files:
| File | Responsibility |
| :--- | :--- |
| `src/mesh_core/Mesh.cpp` | Initial strategy assignment on send |
| `src/mesh_core/Dispatcher.cpp` | Per-hop re-evaluation and forwarding |
| `src/mesh_routing/DirectionalRouter.cpp` | Progress-based geographic flood filtering |
| `src/mesh_routing/RouteCache.cpp` | Storing and looking up cached direct routes |

---

## 3. Configuration (MeshConfig)

You can initialize the mesh with default settings or provide a custom `MeshConfig` object:

```cpp
mesh::MeshConfig cfg;
cfg.maxPeers      = 32;
cfg.networkKeyLen = 16;
memcpy(cfg.networkKey, "MY_SECURE_KEY_12", 16);

meshNode.init(cfg);
```

| Property | Default | Description |
| :--- | :--- | :--- |
| `maxPeers` | 20 | Capacity of the peer registry. |
| `maxRetries` | 3 | Retries before triggering self-healing. |
| `networkKey` | (Empty) | Key for SHA-256 integrity signatures. |
| `ttlDefault` | 10 | Maximum hops allowed. |
| `angleThreshold` | 90.0f | Flooding cone angle in degrees. |
| `distanceTolerance` | 500.0f | Minimum progress (meters) to be a valid relay. |
| `nodeTimeoutMs` | 30000 | Silence timeout before a node is pruned. |

---

## 4. Events Available

The framework dispatches all events through the `EventBus`. Every event has a convenient sugar method — **no need to ever touch the raw EventBus** unless you want to.

> **💡 Key Rule:** Sugar methods automatically unpack the event for you with clean, typed parameters. Use them unless you specifically need raw access.

| Triggering Condition | Event Enum | Sugar Method | Data Source |
| :--- | :--- | :--- | :--- |
| Data packet arrives for this node | `PACKET_RECEIVED` | `onPacketReceived` | `ev.packetData` |
| A packet was sent successfully | `PACKET_SENT` | `onPacketSent` | `ev.packetData` |
| A packet was dropped (queue full / TTL) | `PACKET_DROPPED` | `onPacketDropped` | `ev.packetData` |
| An ACK was received for a sent packet | `PACKET_ACK_RECEIVED` | `onPacketAckReceived` | `ev.packetData` |
| A sent packet timed out waiting for ACK | `PACKET_ACK_TIMEOUT` | `onPacketAckTimeout` | `ev.packetData` |
| A new peer node was discovered | `NODE_DISCOVERED` | `onNodeDiscovered` | `ev.nodeData` |
| A peer node timed out and was pruned | `NODE_LOST` | `onNodeLost` | `ev.nodeData` |
| A peer node's info was refreshed | `NODE_UPDATED` | `onNodeUpdated` | `ev.nodeData` |
| Local GPS coordinates changed | `LOCATION_UPDATED` | `onLocationUpdated` | `ev.locationData` |
| GPS signal was lost | `LOCATION_LOST` | `onLocationLost` | `ev.locationData` |
| A service was registered | `SERVICE_REGISTERED` | `onServiceRegistered` | `ev.serviceData` |
| A service was removed | `SERVICE_UNREGISTERED` | `onServiceUnregistered` | `ev.serviceData` |
| Mesh finished initializing | `MESH_STARTED` | `onMeshStarted` | *(none)* |
| Mesh was stopped | `MESH_STOPPED` | `onMeshStopped` | *(none)* |
| An internal error occurred | `MESH_ERROR` | `onMeshError` | `ev.errorData` |

---

## 5. Core Structures & Enums

### `Node` Struct
```cpp
// File: src/mesh_core/Node.hpp
struct Node {
    uint8_t  node_hash[16]; // Derived from MAC, used as a stable address
    uint8_t  mac[6];        // Physical hardware MAC
    float    lat, lon;      // GPS coordinates (0,0 if unknown)
    int64_t  last_seen;     // millis() timestamp of last contact
};
```

### `PacketHeader` Struct
```cpp
// File: src/mesh_core/Packet.hpp
struct PacketHeader {
    uint8_t  version, ttl, flags, priority;
    uint8_t  routing_strategy; // See RoutingStrategy enum
    uint16_t payload_size;
    uint8_t  packet_id[16];
    uint8_t  source_hash[16], dest_hash[16];
    uint8_t  last_hop_mac[6];
    float    dest_lat, dest_lon; // Physical destination for geo-routing
};
```

### `RoutingStrategy` Enum
```cpp
// File: src/mesh_core/Packet.hpp
enum class RoutingStrategy : uint8_t {
    STRAT_DIRECT    = 0,  // Point-to-point via cached MAC
    STRAT_GEO_FLOOD = 1,  // Progress-filtered directional flood
    STRAT_BROADCAST = 2   // Blind flood (discovery/fallback only)
};
```

---

## 6. Event System & Subscriptions

VortexaMesh provides two distinct ways to subscribe to events:

### 1) Sugar Calls — Recommended
The framework automatically unpacks the event union and calls your handler with clean, typed parameters.

```cpp
meshNode.onPacketReceived([](const mesh::Packet& pkt, const uint8_t senderMac[6]) {
    Serial.printf("Packet! Size: %d bytes\n", pkt.header.payload_size);
});

meshNode.onNodeDiscovered([](const mesh::Node& node) {
    Serial.printf("New node: %02X:%02X...\n", node.mac[0], node.mac[1]);
});

meshNode.onNodeLost([](const mesh::Node& node) {
    Serial.println("Lost a node — self-healing triggered.");
});

meshNode.onMeshError([](int error_code) {
    Serial.printf("Mesh error: %d\n", error_code);
});
```

### 2) Direct EventBus — Power Users
Gives raw access to the `MeshEvent` struct. Useful for events not covered by sugar methods or for advanced filtering.

```cpp
meshNode.getEventBus().subscribe(mesh::MeshEventType::PACKET_SENT,
    [](const mesh::MeshEvent& ev, void* ctx) {
        Serial.printf("Sent packet size: %d\n",
            ev.packetData.packet.header.payload_size);
    }, nullptr);
```

---

## 7. Defining Event Signatures

When using the direct `EventBus`, you must manually unpack the `MeshEvent` union.

### The Callback Signature
```cpp
void myHandler(const mesh::MeshEvent& ev, void* context) { }
```

### ⚠️ Union Access Guide — Read the Correct Field
> Access the wrong union field and you get garbage data. Always match `ev.type` to the correct union member.

| Event Group | Union Field to Read | Available Fields |
| :--- | :--- | :--- |
| `PACKET_*` | `ev.packetData` | `.packet`, `.sender_mac`, `.isDuplicate`, `.isForUs` |
| `NODE_*` | `ev.nodeData` | `.node` (full Node struct) |
| `LOCATION_*` | `ev.locationData` | `.lat`, `.lon`, `.valid` |
| `SERVICE_*` | `ev.serviceData` | `.service_id` |
| `MESH_ERROR` | `ev.errorData` | `.error_code` |

**Example — safe unpacking:**
```cpp
meshNode.getEventBus().subscribe(mesh::MeshEventType::NODE_LOST,
    [](const mesh::MeshEvent& ev, void*) {
        const mesh::Node& lost = ev.nodeData.node; // ← use nodeData for NODE_*
        Serial.printf("Lost node at %.4f, %.4f\n", lost.lat, lost.lon);
    }, nullptr);
```

---

## 8. Ways of Using Mesh APIs

### Mode 1: Manual Serial CLI
Connect via Serial (115200 baud). Useful for live debugging.
- `help` — Print all available commands.
- `ls` — List all currently discovered nodes.
- `msg <hash> <text>` — Send a UDP message.
- `tcp <hash> <text>` — Send a reliable TCP request.
- `geo <lat> <lon> <text>` — Send a geo-targeted message.

### Mode 2: Programmatic Command Execution
Invoke the CLI engine from code and capture output as a `String`. Useful for custom UIs or dashboards.
```cpp
mesh::MeshTerminal terminal(meshNode);
String output = terminal.execute("ls");
Serial.print(output); // Returns formatted node list as a string
```

### Mode 3: Direct API Function Calls (Recommended)
The fastest, most efficient method. All functions return typed statuses.

> **How to get `targetHash`:** Use `getNodeHash(index, outHash)` to look up any peer by index from the live registry — same data the `ls` command uses internally:
> ```cpp
> uint8_t targetHash[16];
> if (meshNode.getNodeHash(0, targetHash)) {
>     // index 0 = first discovered peer
>     meshNode.sendUDP(targetHash, (uint8_t*)"Hello", 5);
> }
>
> // Or iterate all known peers:
> mesh::Node peers[64];
> int count = meshNode.getNodes(peers, 64);
> for (int i = 0; i < count; i++) {
>     // peers[i].node_hash, .mac, .lat, .lon, .last_seen
> }
> ```

```cpp
// 1. Broadcast to all nodes in range
//    Returns: bool (true = packet queued)
bool ok = meshNode.sendBroadcast((uint8_t*)"Hello", 5);

// 2. Unicast UDP to a specific node (use hash captured above)
//    Returns: bool (true = packet queued)
bool ok = meshNode.sendUDP(targetHash, (uint8_t*)"Hello", 5);

// 3. Reliable TCP — blocks until response or timeout
//    Returns: MeshResponse { bool success; uint8_t* payload; size_t payloadLen; }
mesh::MeshResponse res = meshNode.sendTCP(targetHash, (uint8_t*)"Ping", 4);
if (res.success) {
    Serial.printf("TCP Reply: %d bytes\n", res.payloadLen);
}

// 4. Geographic flood toward a coordinate
//    Returns: bool (true = packet queued)
bool ok = meshNode.sendGeo(12.9716, 77.5946, (uint8_t*)"Alert", 5);
```

---

## 9. Error Handling

Every API call can fail silently unless you check the return value. Here is what each failure means:

| Return | Method | Failure Reason |
| :--- | :--- | :--- |
| `false` | `sendUDP`, `sendBroadcast`, `sendGeo` | **Queue Full** — outgoing queue is saturated. Reduce send rate. |
| `false` | `sendUDP` | **Fragmentation Failed** — payload too large even after splitting. |
| `res.success == false` | `sendTCP` | **Timeout** — destination did not respond within `timeoutMs`. |
| `res.success == false` | `sendTCP` | **No Route** — after all retries, no path reached the destination. |
| Event `PACKET_DROPPED` | any | **TTL Exceeded** — packet exhausted its hop count before arriving. |
| Event `PACKET_ACK_TIMEOUT` | any with `ackRequired` | **ACK Lost** — destination received but reply was lost, or node is dead. |

**Best practice:**
```cpp
if (!meshNode.sendUDP(hash, data, len)) {
    Serial.println("Send failed: queue full or fragmentation error.");
}

mesh::MeshResponse res = meshNode.sendTCP(hash, data, len);
if (!res.success) {
    Serial.println("TCP failed: node unreachable or timeout.");
}

meshNode.onPacketDropped([](const mesh::Packet& pkt) {
    Serial.printf("Packet dropped — TTL: %d\n", pkt.header.ttl);
});
```

---

## 10. Self-Healing Mechanism

VortexaMesh actively monitors network health. If a direct transmission fails after `maxRetries` attempts:
1. The destination is **removed** from `NodeRegistry`.
2. All cached paths in `RouteCache` using that node as a hop are **invalidated**.
3. The next packet automatically falls back to `STRAT_GEO_FLOOD` to find an alternative path.

> **Relevant files:** `src/mesh_tasks/HealthTask.cpp`, `src/mesh_registry/NodeRegistry.cpp`, `src/mesh_routing/RouteCache.cpp`

---

## 11. Extending the Framework

VortexaMesh is designed to be extended without modifying its core. Here are the primary extension points:

**Custom Event Reactions:** Use `getEventBus().subscribe(...)` to hook into any internal event — including those without sugar wrappers — and inject your own logic.

**Custom Routing Strategies:** Add new strategy values to the `RoutingStrategy` enum and implement the forwarding logic in `Dispatcher.cpp`. The `SenderTask` will automatically respect the strategy field in each packet header.

**Custom Services:** Use `SERVICE_REGISTERED` and `SERVICE_UNREGISTERED` events to build discoverable application-level services on top of the transport layer — similar to mDNS but over mesh.

**Custom Location Providers:** Implement the `ILocationProvider` interface (`src/mesh_core/ILocationProvider.hpp`) and pass it to `init(cfg)`. The framework will automatically use your provider for geo-routing, whether it is a GPS module, a BLE beacon, or a hardcoded coordinate.

> **`src/mesh_core/ILocationProvider.hpp`** — Start here to plug in any spatial awareness source.

---

## 12. Example Code

The framework includes 4 progressive examples covering all integration modes. You can find them by navigating to `File → Examples → MeshFramework` in the Arduino IDE, or locally in the `./examples/` directory:

1. **`01_minimal_node`** — The absolute bare minimum code to join a network. Zero config.
2. **`02_direct_api`** — How to use the high-performance programmatic API directly (`sendUDP`, `sendTCP`, `getNodeHash`).
3. **`03_programmatic_cli`** — How to pass commands to the built-in terminal from code and capture the `String` output (useful for Web UIs/dashboards).
4. **`04_serial_cli`** — A fully interactive mesh terminal showcasing remote control (LED toggling) and the modern, sugar-coated event API.

---

© 2026 VortexaMesh Framework

