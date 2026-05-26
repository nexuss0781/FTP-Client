/*
 * RetryPolicy.cpp - Exponential Backoff with Full Jitter Implementation for Phase 5
 * 
 * Implements retry policy engine per Phase 5 Spec Section 4.
 */

#include "RetryPolicy.hpp"
#include <algorithm>
#include <thread>

namespace ftpclient { namespace resilience {

RetryPolicy::RetryPolicy(const RetryConfig& config)
    : config_(config)
    , rng_(std::random_device{}())
    , uniform_dist_(0.0, 1.0)
{
}

RetryPolicy::~RetryPolicy() = default;

ErrorCategory RetryPolicy::classify_error(int32_t error_code) {
    /* Per Phase 5 Spec Section 4.3 - Retryable vs. Non-Retryable Errors */
    
    /* Transient Network Errors (-4xx) */
    if (error_code == -401 ||  /* FTP_ERR_CONNECT */
        error_code == -402 ||  /* FTP_ERR_TIMEOUT */
        error_code == -403 ||  /* FTP_ERR_NETWORK_RESET */
        error_code == -404 ||  /* FTP_ERR_DNS */
        error_code == -503) {  /* FTP_ERR_PASSIVE_FAILED */
        return ErrorCategory::TRANSIENT_NETWORK;
    }
    
    /* Transient Server Errors (-5xx specific cases) */
    if (error_code == -502) {  /* FTP_ERR_SERVER_DENIED - could be temporary */
        return ErrorCategory::TRANSIENT_SERVER;
    }
    
    /* Permanent Auth Errors (-3xx) */
    if (error_code == -301 ||  /* FTP_ERR_AUTH_FAILED */
        error_code == -302 ||  /* FTP_ERR_AUTH_TLS_REQUIRED */
        error_code == -303) {  /* FTP_ERR_CERT_VERIFY */
        return ErrorCategory::PERMANENT_AUTH;
    }
    
    /* Permanent Protocol Errors (-5xx) */
    if (error_code == -501) {  /* FTP_ERR_PROTOCOL */
        return ErrorCategory::PERMANENT_PROTOCOL;
    }
    
    /* Permanent Local Errors (-6xx) */
    if (error_code == -601) {  /* FTP_ERR_LOCAL_IO */
        return ErrorCategory::PERMANENT_LOCAL;
    }
    
    /* Ambiguous Errors */
    if (error_code == -602 ||  /* FTP_ERR_REMOTE_IO (451) */
        error_code == -603) {  /* FTP_ERR_PARTIAL */
        return ErrorCategory::AMBIGUOUS;
    }
    
    /* System errors */
    if (error_code == -101 ||  /* FTP_ERR_NOMEM */
        error_code == -102) {  /* FTP_ERR_SYSTEM */
        return ErrorCategory::TRANSIENT_NETWORK;
    }
    
    /* Invalid state/argument - not retryable */
    if (error_code == -201 ||  /* FTP_ERR_INVALID_HANDLE */
        error_code == -202 ||  /* FTP_ERR_INVALID_ARGUMENT */
        error_code == -203) {  /* FTP_ERR_INVALID_STATE */
        return ErrorCategory::PERMANENT_LOCAL;
    }
    
    /* Default: treat as transient for safety */
    return ErrorCategory::TRANSIENT_NETWORK;
}

bool RetryPolicy::is_retryable(int32_t error_code, bool retry_all) {
    if (retry_all) {
        /* Hail Mary mode - retry everything */
        return error_code != 0;
    }
    
    ErrorCategory category = classify_error(error_code);
    
    switch (category) {
        case ErrorCategory::TRANSIENT_NETWORK:
        case ErrorCategory::TRANSIENT_SERVER:
            return true;
        
        case ErrorCategory::AMBIGUOUS:
            /* Ambiguous errors retry once then fail - handled by caller logic */
            return true;
        
        case ErrorCategory::PERMANENT_AUTH:
        case ErrorCategory::PERMANENT_PROTOCOL:
        case ErrorCategory::PERMANENT_LOCAL:
            return false;
        
        default:
            return false;
    }
}

uint64_t RetryPolicy::calculate_delay(uint32_t attempt) {
    /* 
     * Per Phase 5 Spec Section 4.2 - Full Jitter Algorithm:
     * delay(attempt) = random_uniform(0, min(base * 2^attempt, max_delay))
     */
    
    /* Calculate exponential base: base * 2^attempt */
    uint64_t exponential_delay = config_.base_delay_ms;
    
    /* Prevent overflow for large attempt numbers */
    for (uint32_t i = 0; i < attempt && exponential_delay < config_.max_delay_ms; ++i) {
        exponential_delay *= 2;
    }
    
    /* Cap at max_delay */
    uint64_t capped_delay = std::min(exponential_delay, config_.max_delay_ms);
    
    /* Apply full jitter: random(0, capped_delay) */
    double jitter = uniform_dist_(rng_);
    uint64_t delay = static_cast<uint64_t>(jitter * static_cast<double>(capped_delay));
    
    return delay;
}

}} // namespace ftpclient::resilience
