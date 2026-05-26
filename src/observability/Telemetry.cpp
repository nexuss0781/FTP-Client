/*
 * Telemetry.cpp - Observability & Event Callback System Implementation
 * 
 * Per Phase 7 Spec Section 7: Observability & Telemetry Hooks
 */

#include "Telemetry.hpp"
#include <cstring>

namespace ftpclient {
namespace observability {

TelemetryController::TelemetryController() 
    : callback_(nullptr)
    , user_data_(nullptr) {
}

TelemetryController::~TelemetryController() {
    // Callback may still be invoked during destruction
    // Caller must ensure proper synchronization
}

void TelemetryController::setCallback(EventCallback cb, void* user_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    user_data_.store(user_data, std::memory_order_release);
    callback_.store(cb, std::memory_order_release);
}

void TelemetryController::emit(const Event& event) {
    EventCallback cb = callback_.load(std::memory_order_acquire);
    if (cb != nullptr) {
        void* userdata = user_data_.load(std::memory_order_acquire);
        cb(&event, userdata);
    }
}

int64_t TelemetryController::nowNanoseconds() {
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
}

Event TelemetryController::createEvent(EventType type) {
    Event event;
    event.type = type;
    event.timestamp_ns = nowNanoseconds();
    return event;
}

// EventTimer implementation

EventTimer::EventTimer(TelemetryController& telemetry, EventType type)
    : telemetry_(telemetry)
    , start_time_(TelemetryController::nowNanoseconds())
    , ended_(false) {
    event_ = TelemetryController::createEvent(type);
}

EventTimer::~EventTimer() {
    if (!ended_) {
        end(event_.status);
    }
}

void EventTimer::end(int32_t status) {
    if (ended_) return;
    
    int64_t end_time = TelemetryController::nowNanoseconds();
    event_.duration_ns = static_cast<uint64_t>(end_time - start_time_);
    event_.status = status;
    
    telemetry_.emit(event_);
    ended_ = true;
}

} // namespace observability
} // namespace ftpclient
