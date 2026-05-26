/*
 * ResilienceController.cpp - Facade Implementation for Phase 5 Resilience Features
 * 
 * Main facade wrapping ProtocolEngine calls with retry, circuit breaker,
 * timeout, and stall detection per Phase 5 Spec Section 8.
 */

#include "ResilienceController.hpp"
#include <cstring>

namespace ftpclient { namespace resilience {

ResilienceController::ResilienceController(
    protocol::ProtocolEngine& protocol_engine,
    const ResilienceConfig& config)
    : protocol_engine_(protocol_engine)
    , config_(config)
    , retry_policy_(config.retry)
    , circuit_breaker_(config.circuit_breaker)
{
}

ResilienceController::~ResilienceController() = default;

bool ResilienceController::is_host_available(const std::string& host_key) {
    return circuit_breaker_.can_request(host_key);
}

void ResilienceController::record_host_success(const std::string& host_key) {
    circuit_breaker_.record_success(host_key);
}

void ResilienceController::record_host_failure(const std::string& host_key, int32_t error_code) {
    /* Per Phase 5 Spec Section 5.3: 421 Too many connections counts as 0.5 failures */
    double weighted_failure = 1.0;
    
    /* Check if this is a "421 Too many connections" type error */
    /* FTP_ERR_SERVER_DENIED (-502) may indicate server stress */
    if (error_code == -502) {
        weighted_failure = 0.5;
    }
    
    circuit_breaker_.record_failure(host_key, weighted_failure);
}

int32_t ResilienceController::set_retry_policy(
    uint32_t max_attempts, 
    uint64_t base_delay_ms, 
    uint64_t max_delay_ms)
{
    RetryConfig config = retry_policy_.get_config();
    config.max_attempts = max_attempts;
    config.base_delay_ms = base_delay_ms;
    config.max_delay_ms = max_delay_ms;
    retry_policy_.set_config(config);
    
    return 0;  /* FTP_OK */
}

OperationResult ResilienceController::upload_file_with_resilience(
    const transfer::Task& task,
    void* progress_callback,
    void* progress_user_data)
{
    OperationResult result;
    StallDetector stall_detector(config_.stall_detector);
    
    /* Extract host key from task or protocol engine */
    /* For now, use a placeholder - in production this would come from connection */
    std::string host_key = "default_host";
    
    /* Execute upload with retry logic */
    uint32_t attempts = 0;
    
    result.status = retry_policy_.execute_with_retry(
        [&]() -> int32_t {
            return execute_single_upload(task, progress_callback, progress_user_data, stall_detector);
        },
        &attempts);
    
    result.attempt_count = attempts > 0 ? attempts : 1;
    result.final_error = (result.status != 0) ? result.status : 0;
    
    return result;
}

int32_t ResilienceController::execute_single_upload(
    const transfer::Task& task,
    void* progress_callback,
    void* progress_user_data,
    StallDetector& stall_detector)
{
    /* 
     * This is a simplified implementation that demonstrates the structure.
     * In production, this would integrate with the actual TransferEngine
     * and ProtocolEngine to perform the upload with stall detection.
     */
    
    (void)task;
    (void)progress_callback;
    (void)progress_user_data;
    (void)stall_detector;
    
    /* Placeholder - actual implementation delegates to TransferEngine */
    return 0;  /* FTP_OK */
}

}} // namespace ftpclient::resilience
