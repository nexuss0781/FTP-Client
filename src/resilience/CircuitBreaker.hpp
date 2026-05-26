/*
 * CircuitBreaker.hpp - Per-Host Circuit Breaker for Phase 5
 * 
 * Implements circuit breaker pattern per Phase 5 Spec Section 5.
 * Prevents hammering dead/unhealthy servers.
 */

#ifndef FTPCLIENT_CIRCUIT_BREAKER_HPP
#define FTPCLIENT_CIRCUIT_BREAKER_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <chrono>
#include <atomic>

namespace ftpclient { namespace resilience {

/**
 * Circuit Breaker Configuration per Phase 5 Spec Section 5.2
 */
struct CircuitBreakerConfig {
    uint32_t failure_threshold;     /* Default: 5 - Failures to trip OPEN */
    uint64_t window_ms;             /* Default: 60000ms - Rolling window for failure count */
    uint64_t open_duration_ms;      /* Default: 60000ms - Time in OPEN before HALF_OPEN */
    uint32_t half_open_max;         /* Default: 1 - Parallel probes allowed in HALF_OPEN */
    
    CircuitBreakerConfig()
        : failure_threshold(5)
        , window_ms(60000)
        , open_duration_ms(60000)
        , half_open_max(1)
    {}
};

/**
 * Circuit Breaker States per Phase 5 Spec Section 5.1
 */
enum class CircuitState {
    CLOSED,     /* Normal operation. All requests pass. */
    OPEN,       /* Fast-fail all requests for this host. */
    HALF_OPEN   /* Allow one probe request. */
};

/**
 * Host state tracking for circuit breaker
 */
struct HostState {
    CircuitState state;
    uint32_t failure_count;
    std::chrono::steady_clock::time_point last_failure_time;
    std::chrono::steady_clock::time_point open_since;
    uint32_t half_open_probes;
    
    HostState()
        : state(CircuitState::CLOSED)
        , failure_count(0)
        , half_open_probes(0)
    {
        auto now = std::chrono::steady_clock::now();
        last_failure_time = now;
        open_since = now;
    }
};

/**
 * Circuit Breaker - Per-Host State Machine
 * 
 * Thread-safe implementation using shared_mutex for read-heavy workload.
 * Per Phase 5 Spec Section 5.
 */
class CircuitBreaker {
public:
    /**
     * Construct circuit breaker with configuration
     * 
     * @param config Circuit breaker configuration parameters
     */
    explicit CircuitBreaker(const CircuitBreakerConfig& config = CircuitBreakerConfig());
    
    /**
     * Destructor
     */
    ~CircuitBreaker();
    
    /**
     * Check if a host is available (circuit allows request)
     * 
     * @param host_key Host identifier (host:port string)
     * @return true if request should proceed, false if fast-fail
     */
    bool can_request(const std::string& host_key);
    
    /**
     * Record a successful operation for a host
     * 
     * @param host_key Host identifier
     */
    void record_success(const std::string& host_key);
    
    /**
     * Record a failed operation for a host
     * 
     * @param host_key Host identifier
     * @param weighted_failure Failure weight (1.0 = full, 0.5 = partial like 421)
     */
    void record_failure(const std::string& host_key, double weighted_failure = 1.0);
    
    /**
     * Get current state for a host (for debugging/telemetry)
     * 
     * @param host_key Host identifier
     * @return Current circuit state
     */
    CircuitState get_state(const std::string& host_key);
    
    /**
     * Get failure count for a host (for telemetry)
     * 
     * @param host_key Host identifier
     * @return Current failure count
     */
    uint32_t get_failure_count(const std::string& host_key);
    
    /**
     * Reset circuit breaker for a specific host
     * 
     * @param host_key Host identifier
     */
    void reset_host(const std::string& host_key);
    
    /**
     * Clear all host states
     */
    void clear_all();
    
    /**
     * Get current configuration
     */
    const CircuitBreakerConfig& get_config() const { return config_; }
    
    /**
     * Update configuration
     */
    void set_config(const CircuitBreakerConfig& config) { config_ = config; }

private:
    CircuitBreakerConfig config_;
    std::unordered_map<std::string, HostState> host_states_;
    mutable std::shared_mutex mutex_;  /* Read-heavy: many workers check, few updates */
    
    /**
     * Get or create host state (must hold write lock)
     */
    HostState& get_or_create_host_state(const std::string& host_key);
    
    /**
     * Check if host state should transition from OPEN to HALF_OPEN
     */
    bool should_try_half_open(const HostState& state);
    
    /**
     * Calculate current failure count within rolling window
     */
    uint32_t get_weighted_failures_in_window(const HostState& state);
};

}} // namespace ftpclient::resilience

#endif /* FTPCLIENT_CIRCUIT_BREAKER_HPP */
