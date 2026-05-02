#pragma once

#include "mesh_core/MeshEvent.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <cstdint>
#include <functional>

namespace mesh {

// Maximum registered event handlers
static constexpr int MAX_EVENT_HANDLERS = 16;

using EventHandler = std::function<void(const MeshEvent& event, void* userCtx)>;

class EventBus {
public:
    EventBus();
    ~EventBus();

    bool init(int queueSize);

    // Register a handler for a specific event type
    bool subscribe(MeshEventType type, EventHandler handler, void* userCtx = nullptr);

    // Post an event to the bus (from any task/ISR)
    bool post(const MeshEvent& event, TickType_t timeout = 0);

    // Process one event from the queue (call from a task loop)
    bool processOne(TickType_t timeout = portMAX_DELAY);

private:
    QueueHandle_t queue_;

    struct Subscription {
        MeshEventType type;
        EventHandler  handler;
        void*         userCtx;
        bool          active;
    };
    Subscription    subs_[MAX_EVENT_HANDLERS];
    int             subCount_;
    SemaphoreHandle_t subMutex_;
};

} // namespace mesh
