/*
 * ResilienceController.hpp - Facade for Phase 5 Resilience Features
 * 
 * Main facade wrapping ProtocolEngine calls with retry, circuit breaker,
 * timeout, and stall detection per Phase 5 Spec Section 8.
 */

#ifndef FTPCLIENT_RESILIENCE_CONTROLLER_HPP
#define FTPCLIENT_RESILIENCE_CONTROLLER_HPP

#include "RetryPolicy.hpp"
#include "CircuitBreaker.hpp"
#include "StallDetector.hpp"
#include "IdempotencyClassifier.hpp"
#include "../protocol/ProtocolEngine.hpp"
#include "../transfer/Task.hpp"
#include <functional>
#include <string>
#include <atomic>

namespace ftpclient { namespace resilience {

/**
 * Resilience Controller Configuration
 * 
 * Aggregates configuration for all resilience components.
 */
struct ResilienceConfig {
    RetryConfig retry;
    CircuitBreakerConfig circuit_breaker;
    StallDetectorConfig stall_detector;
    
    /* Timeout hierarchy per Phase 5 Spec Section 6.1 */
    uint32_t timeout_connect_ms;     /* Default: 5000ms */
    uint32_t timeout_command_ms;     /* Default: 30000ms */
    uint32_t timeout_idle_ms;        /* Default: 30000ms */
    
    ResilienceConfig()
        : timeout_connect_ms(5000)
        , timeout_command_ms(30000)
        , timeout_idle_ms(30000)
    {}
};

/**
 * Operation result with attempt tracking per Phase 5 Spec Section 8.2
 */
struct OperationResult {
    int32_t status;             /* Final status code */
    uint32_t attempt_count;     /* 1 = first try succeeded, >1 = retried */
    int32_t final_error;        /* Error code from last attempt (if failed) */
    
    OperationResult()
        : status(0)
        , attempt_count(1)
        , final_error(0)
    {}
};

/**
 * Resilience Controller - Main Facade for Phase 5 Features
 * 
 * Per Phase 5 Spec Section 3 and Section 8:
 * - Wraps ProtocolEngine calls with retry logic
 * - Integrates circuit breaker for per-host failure tracking
 * - Provides deadline timers for timeouts
 * - Monitors data transfers for stall detection
 * 
 * Integration Rule: ResilienceController is a wrapper layer. It does not 
 * replace TransferEngine or ProtocolEngine. Worker threads call 
 * ResilienceController::execute_with_retry(task) instead of calling 
 * ProtocolEngine directly.
 */
class ResilienceController {
public:
    /**
     * Construct resilience controller
     * 
     * @param protocol_engine Reference to underlying protocol engine
     * @param config Resilience configuration (optional, uses defaults)
     */
    explicit ResilienceController(
        protocol::ProtocolEngine& protocol_engine,
        const ResilienceConfig& config = ResilienceConfig());
    
    /**
     * Destructor
     */
    ~ResilienceController();
    
    /**
     * Execute an operation with full resilience (retry + circuit breaker)
     * 
     * Per Phase 5 Spec Section 8.1 - Worker Thread Modification
     * 
     * @param host_key Host identifier for circuit breaker (host:port)
     * @param operation Callable that performs the operation, returns int32_t status
     * @return OperationResult with status and attempt count
     */
    template<typename Func>
    OperationResult execute_with_retry(
        const std::string& host_key,
        Func&& operation);
    
    /**
     * Execute a file upload task with resilience
     * 
     * Specialized version for file uploads with stall detection.
     * 
     * @param task Upload task to execute
     * @param progress_callback Progress callback (may be nullptr)
     * @param progress_user_data User data for progress callback
     * @return OperationResult with status and attempt count
     */
    OperationResult upload_file_with_resilience(
        const transfer::Task& task,
        void* progress_callback,
        void* progress_user_data);
    
    /**
     * Check if a host is available (circuit breaker check)
     * 
     * @param host_key Host identifier
     * @return true if requests are allowed, false if circuit is OPEN
     */
    bool is_host_available(const std::string& host_key);
    
    /**
     * Record success for a host (resets circuit breaker)
     * 
     * @param host_key Host identifier
     */
    void record_host_success(const std::string& host_key);
    
    /**
     * Record failure for a host (trips circuit breaker if threshold exceeded)
     * 
     * @param host_key Host identifier
     * @param error_code Error code that caused failure
     */
    void record_host_failure(const std::string& host_key, int32_t error_code);
    
    /**
     * Get retry policy component
     */
    RetryPolicy& get_retry_policy() { return retry_policy_; }
    const RetryPolicy& get_retry_policy() const { return retry_policy_; }
    
    /**
     * Get circuit breaker component
     */
    CircuitBreaker& get_circuit_breaker() { return circuit_breaker_; }
    const CircuitBreaker& get_circuit_breaker() const { return circuit_breaker_; }
    
    /**
     * Get current configuration
     */
    const ResilienceConfig& get_config() const { return config_; }
    
    /**
     * Update configuration
     */
    void set_config(const ResilienceConfig& config) { config_ = config; }
    
    /**
     * Set retry policy parameters (ABI compatibility with ftp_set_retry_policy)
     * 
     * @param max_attempts Maximum retry attempts
     * @param base_delay_ms Base delay for exponential backoff
     * @param max_delay_ms Maximum delay cap
     * @return 0 on success, negative error code
     */
    int32_t set_retry_policy(uint32_t max_attempts, uint64_t base_delay_ms, uint64_t max_delay_ms);

private:
    protocol::ProtocolEngine& protocol_engine_;
    ResilienceConfig config_;
    RetryPolicy retry_policy_;
    CircuitBreaker circuit_breaker_;
    
    std::atomic<bool> cancel_flag_{false};
    
    /**
     * Internal helper for upload execution
     */
    int32_t execute_single_upload(
        const transfer::Task& task,
        void* progress_callback,
        void* progress_user_data,
        StallDetector& stall_detector);
};

/* Template implementation */

template<typename Func>
OperationResult ResilienceController::execute_with_retry(
    const std::string& host_key,
    Func&& operation)
{
    OperationResult result;
    
    /* Check circuit breaker before attempting */
    if (!is_host_available(host_key)) {
        result.status = -403;  /* FTP_ERR_NETWORK_RESET - circuit open */
        result.attempt_count = 1;
        result.final_error = -403;
        return result;
    }
    
    /* Execute with retry policy */
    uint32_t attempts = 0;
    
    result.status = retry_policy_.execute_with_retry(
        [&]() -> int32_t {
            /* Check circuit breaker before each attempt */
            if (!is_host_available(host_key) && attempts > 0) {
                return -403;  /* Circuit opened during retry */
            }
            
            int32_t status = operation();
            
            if (status == 0) {
                /* Success - record for circuit breaker */
                record_host_success(host_key);
            } else {
                /* Failure - will record after loop if exhausted */
            }
            
            return status;
        },
        &attempts);
    
    result.attempt_count = attempts > 0 ? attempts : 1;
    result.final_error = (result.status != 0) ? result.status : 0;
    
    /* If all retries failed, record failure for circuit breaker */
    if (result.status != 0) {
        record_host_failure(host_key, result.status);
    }
    
    return result;
}

}} // namespace ftpclient::resilience

#endif /* FTPCLIENT_RESILIENCE_CONTROLLER_HPP */
