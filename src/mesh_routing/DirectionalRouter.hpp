#pragma once

#include "mesh_core/Node.hpp"
#include "mesh_core/Packet.hpp"
#include "mesh_registry/NodeRegistry.hpp"
#include <cstdint>

namespace mesh {

class DirectionalRouter {
public:
    DirectionalRouter(NodeRegistry* registry, float angleThreshold, float progressThreshold = 5.0f);

    // Choose the best next-hop MAC. Returns false if no suitable peer found (fallback flood).
    bool selectNextHop(const Packet& pkt, const Node& self,
                       uint8_t outMac[6]) const;

    // Get all valid forward peers for flooding. Returns count.
    int  getFloodTargets(const Packet& pkt, const Node& self,
                         uint8_t outMacs[][6], int maxOut) const;

    void setAngleThreshold(float deg)      { angleThreshold_ = deg; }
    void setProgressThreshold(float m)     { progressThreshold_ = m; }

    // Haversine distance in meters (public static so callers can reuse it)
    static float distanceM(float lat1, float lon1, float lat2, float lon2);

private:
    NodeRegistry* registry_;
    float         angleThreshold_;
    float         progressThreshold_;  // GPS-noise tolerance for progress check

    // Bearing in degrees [0, 360)
    static float bearing(float lat1, float lon1, float lat2, float lon2);

    // Angle difference [0, 180]
    static float angleDiff(float a, float b);
};

} // namespace mesh
