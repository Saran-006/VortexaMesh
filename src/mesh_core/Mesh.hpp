#pragma once

#include "mesh_core/MeshConfig.hpp"
#include <functional>
#include "mesh_core/Node.hpp"
#include "mesh_core/ILocationProvider.hpp"
#include "mesh_core/PeerManager.hpp"
#include "mesh_core/EventBus.hpp"
#include "mesh_core/MeshContext.hpp"
#include "mesh_core/RequestManager.hpp"

#include "mesh_registry/NodeRegistry.hpp"

namespace mesh {

// Forward declarations
class ESPNOWTransport;
class DedupCache;
class DirectionalRouter;
class AckManager;
class FragmentManager;
class Dispatcher;
class RouteCache;
class RequestManager;

class Mesh {
public:
    Mesh();
    ~Mesh();

    // ---- Production API: Configuration & Lifecycle ----
    // Initialize with a custom config object
    void init(const MeshConfig& cfg);
    
    // Initialize with default config
    void init();
    
    void start();

    // Set a custom location provider (GPS, etc)
    void setLocationProvider(ILocationProvider* provider) { locationProvider_ = provider; }

    // ---- Production API: Messaging (Direct & Typed) ----
    // Direct unicast UDP
    bool sendUDP(const uint8_t* destHash, const uint8_t* data, size_t len, bool ackRequired = false);
    
    // Network-wide broadcast UDP
    bool sendBroadcast(const uint8_t* data, size_t len);

    // Direct unicast TCP (blocks for response)
    MeshResponse sendTCP(const uint8_t* destHash, const uint8_t* data, size_t len, int timeoutMs = 10000);

    // Geographic targeted messaging
    bool sendGeo(float lat, float lon, const uint8_t* data, size_t len, bool ackRequired = false);

    // ---- Production API: Event Convenience Layer (Sugar) ----
    using PacketHandler   = std::function<void(const Packet& pkt, const uint8_t senderMac[6])>;
    using SimplePacketHandler = std::function<void(const Packet& pkt)>;
    using NodeHandler     = std::function<void(const Node& node)>;
    using LocationHandler = std::function<void(float lat, float lon)>;
    using VoidHandler     = std::function<void()>;
    using ServiceHandler  = std::function<void(uint8_t service_id)>;
    using ErrorHandler    = std::function<void(int error_code)>;

    void onPacketReceived(PacketHandler handler);
    void onPacketSent(SimplePacketHandler handler);
    void onPacketDropped(SimplePacketHandler handler);
    void onPacketAckReceived(SimplePacketHandler handler);
    void onPacketAckTimeout(SimplePacketHandler handler);

    void onNodeDiscovered(NodeHandler handler);
    void onNodeLost(NodeHandler handler);
    void onNodeUpdated(NodeHandler handler);

    void onLocationUpdated(LocationHandler handler);
    void onLocationLost(VoidHandler handler);

    void onServiceRegistered(ServiceHandler handler);
    void onServiceUnregistered(ServiceHandler handler);

    void onMeshStarted(VoidHandler handler);
    void onMeshStopped(VoidHandler handler);
    void onMeshError(ErrorHandler handler);

    // ---- Peer Lookup Utilities ----
    // Fill outArr with discovered peers. Returns count.
    int getNodes(Node* outArr, int maxOut);

    // Get the hash of a specific peer by index (0 = first discovered).
    // Returns true if index is valid. Use getNodes() count to know bounds.
    bool getNodeHash(int index, uint8_t outHash[16]);

    // ---- Accessors ----
    MeshContext*  getContext()  { return &ctx_; }
    MeshConfig&   getConfig()   { return config_; }
    const Node&   getSelf()     const { return selfNode_; }
    NodeRegistry& getNodeRegistry() { return *nodeRegistry_; }
    EventBus&     getEventBus()  { return eventBus_; }

private:
    MeshConfig         config_;
    Node               selfNode_;
    MeshContext        ctx_;
    EventBus           eventBus_;

    // Default event handling for print-only fallback
    bool               hasUserPacketHandler = false;
    bool               hasUserNodeHandler   = false;

    // Subsystem instances
    ESPNOWTransport*   transport_;
    PeerManager*       peerManager_;
    NodeRegistry*      nodeRegistry_;
    DedupCache*        dedupCache_;
    DirectionalRouter* router_;
    AckManager*        ackManager_;
    FragmentManager*   fragmentManager_;
    Dispatcher*        dispatcher_;
    RouteCache*        routeCache_;
    RequestManager*    requestManager_;

    ILocationProvider* locationProvider_;

    // FreeRTOS task handles
    TaskHandle_t receiverTaskHandle_;
    TaskHandle_t dispatcherTaskHandle_;
    TaskHandle_t senderTaskHandle_;
    TaskHandle_t discoveryTaskHandle_;
    TaskHandle_t locationTaskHandle_;
    TaskHandle_t healthTaskHandle_;

    void createQueues();
    void createTasks();
    void initSelfNode();
    void setupDefaultEventHandlers();
};

} // namespace mesh
