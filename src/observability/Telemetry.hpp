/*
 * Telemetry.hpp - Observability & Event Callback System
 * 
 * Per Phase 7 Spec Section 7: Observability & Telemetry Hooks
 * 
 * This header defines the telemetry event emission system that allows
 * Python and other consumers to observe library internals without
 * coupling to specific monitoring frameworks.
 */

#ifndef FTPCLIENT_OBSERVABILITY_TELEMETRY_HPP
#define FTPCLIENT_OBSERVABILITY_TELEMETRY_HPP

#include <cstdint>
#include <chrono>
#include <string>
#include <mutex>
#include <atomic>
#include <functional>

namespace ftpclient {
namespace observability {

/**
 * Event types as per Phase 7 Spec Section 7.1
 */
enum class EventType : int32_t {
    CONNECT_START = 0,
    CONNECT_END = 1,
    AUTH_START = 2,
    AUTH_END = 3,
    UPLOAD_START = 4,
    UPLOAD_END = 5,
    UPLOAD_PROGRESS = 6,
    RETRY_TRIGGERED = 7,
    CIRCUIT_BREAKER_TRIP = 8,
    ERROR = 9
};

/**
 * Event structure passed to callbacks
 * Per Phase 7 Spec Section 7.1
 */
struct Event {
    EventType type;
    int64_t timestamp_ns;       /* CLOCK_MONOTONIC nanoseconds */
    const char* file_path;      /* Nullable, borrowed */
    const char* remote_path;    /* Nullable, borrowed */
    int32_t status;             /* Error code if applicable */
    uint64_t bytes_transferred;
    uint64_t duration_ns;
    const char* json_payload;   /* Nullable, extra structured data */
    
    Event() : type(EventType::ERROR), timestamp_ns(0), file_path(nullptr),
              remote_path(nullptr), status(0), bytes_transferred(0),
              duration_ns(0), json_payload(nullptr) {}
};

/**
 * Callback type matching C ABI
 */
using EventCallback = void (*)(const Event* event, void* user_data);

/**
 * Telemetry controller for event emission
 * 
 * Thread Safety: Callback registration is thread-safe.
 * Event emission is lock-free when callback is set.
 */
class TelemetryController {
public:
    TelemetryController();
    ~TelemetryController();
    
    /**
     * Set event callback
     * @param cb Callback function (nullptr to disable)
     * @param user_data Opaque pointer passed to callback
     */
    void setCallback(EventCallback cb, void* user_data);
    
    /**
     * Get current user data
     */
    void* getUserData() const { return user_data_.load(std::memory_order_acquire); }
    
    /**
     * Emit an event
     * @param event Event to emit
     */
    void emit(const Event& event);
    
    /**
     * Helper: Get current monotonic timestamp in nanoseconds
     */
    static int64_t nowNanoseconds();
    
    /**
     * Helper: Create event with current timestamp
     */
    static Event createEvent(EventType type);
    
private:
    std::atomic<EventCallback> callback_;
    std::atomic<void*> user_data_;
    std::mutex mutex_;
};

/**
 * RAII guard for timing events (start/end pairs)
 */
class EventTimer {
public:
    explicit EventTimer(TelemetryController& telemetry, EventType type);
    ~EventTimer();
    
    /**
     * Set additional event fields before end
     */
    void setFilePath(const char* path) { event_.file_path = path; }
    void setRemotePath(const char* path) { event_.remote_path = path; }
    void setStatus(int32_t status) { event_.status = status; }
    void setBytesTransferred(uint64_t bytes) { event_.bytes_transferred = bytes; }
    void setJsonPayload(const char* payload) { event_.json_payload = payload; }
    
    /**
     * End the event early with status
     */
    void end(int32_t status = 0);
    
private:
    TelemetryController& telemetry_;
    Event event_;
    int64_t start_time_;
    bool ended_;
};

} // namespace observability
} // namespace ftpclient

#endif /* FTPCLIENT_OBSERVABILITY_TELEMETRY_HPP */
