/*
 * RetryPolicy.hpp - Exponential Backoff with Full Jitter for Phase 5
 * 
 * Implements retry policy engine per Phase 5 Spec Section 4.
 * Provides exponential backoff with full jitter algorithm.
 */

#ifndef FTPCLIENT_RETRY_POLICY_HPP
#define FTPCLIENT_RETRY_POLICY_HPP

#include <cstdint>
#include <random>
#include <chrono>
#include <thread>
#include <functional>

namespace ftpclient { namespace resilience {

/**
 * Retry Configuration per Phase 5 Spec Section 4.1
 */
struct RetryConfig {
    uint32_t max_attempts;        /* Default: 3 (1 = initial only, no retry) */
    uint64_t base_delay_ms;       /* Default: 1000ms - Exponential backoff base */
    uint64_t max_delay_ms;        /* Default: 30000ms - Cap backoff at 30s */
    double   jitter_factor;       /* Default: 1.0 - 1.0 = full jitter, 0.0 = no jitter */
    int32_t  retry_all_errors;    /* Default: 0 - 0 = retry only transient, 1 = retry all */
    
    RetryConfig()
        : max_attempts(3)
        , base_delay_ms(1000)
        , max_delay_ms(30000)
        , jitter_factor(1.0)
        , retry_all_errors(0)
    {}
};

/**
 * Error classification for retry decisions per Phase 5 Spec Section 4.3
 */
enum class ErrorCategory {
    TRANSIENT_NETWORK,    /* Retry: Yes - e.g., NETWORK_RESET, TIMEOUT, CONNECT, PASSIVE_FAILED */
    TRANSIENT_SERVER,     /* Retry: Yes - e.g., SERVER_DENIED (550 temp full), 421 Too many connections */
    PERMANENT_AUTH,       /* Retry: No - e.g., AUTH_FAILED, CERT_VERIFY */
    PERMANENT_PROTOCOL,   /* Retry: No - e.g., PROTOCOL (server violates RFC) */
    PERMANENT_LOCAL,      /* Retry: No - e.g., LOCAL_IO (disk error, permission denied) */
    AMBIGUOUS             /* Retry: Conditional - e.g., REMOTE_IO (451) - retry once, then fail */
};

/**
 * Retry Policy Engine
 * 
 * Implements exponential backoff with full jitter per Phase 5 Spec Section 4.2.
 * Thread-safe: Can be called from multiple worker threads concurrently.
 */
class RetryPolicy {
public:
    /**
     * Construct retry policy with configuration
     * 
     * @param config Retry configuration parameters
     */
    explicit RetryPolicy(const RetryConfig& config = RetryConfig());
    
    /**
     * Destructor
     */
    ~RetryPolicy();
    
    /**
     * Classify an error code into a retry category per Phase 5 Spec Section 4.3
     * 
     * @param error_code FTP error code (from ftpclient.h error taxonomy)
     * @return ErrorCategory for retry decision
     */
    static ErrorCategory classify_error(int32_t error_code);
    
    /**
     * Check if an error is retryable per Phase 5 Spec Section 4.3
     * 
     * @param error_code FTP error code
     * @param retry_all If true, even permanent errors are considered retryable
     * @return true if error should be retried, false otherwise
     */
    static bool is_retryable(int32_t error_code, bool retry_all = false);
    
    /**
     * Calculate delay for a given attempt using full jitter algorithm
     * 
     * Per Phase 5 Spec Section 4.2:
     * delay(attempt) = random_uniform(0, min(base * 2^attempt, max_delay))
     * 
     * @param attempt Attempt number (0-indexed: 0 = first retry after initial failure)
     * @return Delay in milliseconds
     */
    uint64_t calculate_delay(uint32_t attempt);
    
    /**
     * Execute a function with retry logic
     * 
     * Wraps a callable with retry policy, exponential backoff, and jitter.
     * 
     * @param func Callable that returns int32_t (FTP status code)
     * @param attempt_count Output parameter: number of attempts made (1 = first try succeeded)
     * @return Final result code (FTP_OK or last error)
     */
    template<typename Func>
    int32_t execute_with_retry(Func&& func, uint32_t* attempt_count = nullptr);
    
    /**
     * Get current configuration
     */
    const RetryConfig& get_config() const { return config_; }
    
    /**
     * Update configuration
     */
    void set_config(const RetryConfig& config) { config_ = config; }

private:
    RetryConfig config_;
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uniform_dist_;
};

/* Template implementation */

template<typename Func>
int32_t RetryPolicy::execute_with_retry(Func&& func, uint32_t* attempt_count) {
    uint32_t attempts = 0;
    int32_t last_result = 0;
    
    while (attempts < config_.max_attempts + 1) {  /* +1 for initial attempt */
        attempts++;
        last_result = func();
        
        if (last_result == 0) {  /* FTP_OK */
            break;  /* Success */
        }
        
        /* Check if retryable */
        if (!is_retryable(last_result, config_.retry_all_errors != 0)) {
            break;  /* Non-retryable error, fail fast */
        }
        
        /* If this was the last attempt, don't sleep */
        if (attempts >= config_.max_attempts + 1) {
            break;
        }
        
        /* Calculate and apply backoff delay */
        uint64_t delay_ms = calculate_delay(attempts - 1);  /* 0-indexed */
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    
    if (attempt_count) {
        *attempt_count = attempts;
    }
    
    return last_result;
}

}} // namespace ftpclient::resilience

#endif /* FTPCLIENT_RETRY_POLICY_HPP */
