/*
 * phase5_test.cpp - Phase 5 Resilience Component Tests
 * 
 * Validates all Phase 5 resilience features per SPEC/Phase-5.md Section 9 Quality Gates:
 * - Retry exhaustion with exponential backoff and full jitter
 * - Circuit breaker trip and recovery
 * - Stall detection accuracy
 * - Idempotency classification
 */

#include <iostream>
#include <cassert>
#include <vector>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>

#include "resilience/RetryPolicy.hpp"
#include "resilience/CircuitBreaker.hpp"
#include "resilience/StallDetector.hpp"
#include "resilience/IdempotencyClassifier.hpp"
#include "resilience/ResilienceController.hpp"

using namespace ftpclient::resilience;

#define TEST_PASS(name) std::cout << "[PASS] " << name << std::endl
#define TEST_FAIL(name, reason) do { \
    std::cout << "[FAIL] " << name << ": " << reason << std::endl; \
    failures++; \
} while(0)

static int failures = 0;

/* ============================================================================
 * Test 1: Retry Policy - Error Classification (Spec Section 4.3)
 * ============================================================================ */
void test_retry_policy_error_classification() {
    std::cout << "\n=== Testing Retry Policy: Error Classification ===" << std::endl;
    
    /* Transient Network Errors should be retryable */
    assert(RetryPolicy::is_retryable(-401) == true);  /* FTP_ERR_CONNECT */
    TEST_PASS("FTP_ERR_CONNECT (-401) is retryable");
    
    assert(RetryPolicy::is_retryable(-402) == true);  /* FTP_ERR_TIMEOUT */
    TEST_PASS("FTP_ERR_TIMEOUT (-402) is retryable");
    
    assert(RetryPolicy::is_retryable(-403) == true);  /* FTP_ERR_NETWORK_RESET */
    TEST_PASS("FTP_ERR_NETWORK_RESET (-403) is retryable");
    
    assert(RetryPolicy::is_retryable(-503) == true);  /* FTP_ERR_PASSIVE_FAILED */
    TEST_PASS("FTP_ERR_PASSIVE_FAILED (-503) is retryable");
    
    /* Permanent Auth Errors should NOT be retryable */
    assert(RetryPolicy::is_retryable(-301) == false);  /* FTP_ERR_AUTH_FAILED */
    TEST_PASS("FTP_ERR_AUTH_FAILED (-301) is NOT retryable");
    
    assert(RetryPolicy::is_retryable(-303) == false);  /* FTP_ERR_CERT_VERIFY */
    TEST_PASS("FTP_ERR_CERT_VERIFY (-303) is NOT retryable");
    
    /* Permanent Protocol Errors should NOT be retryable */
    assert(RetryPolicy::is_retryable(-501) == false);  /* FTP_ERR_PROTOCOL */
    TEST_PASS("FTP_ERR_PROTOCOL (-501) is NOT retryable");
    
    /* Permanent Local Errors should NOT be retryable */
    assert(RetryPolicy::is_retryable(-601) == false);  /* FTP_ERR_LOCAL_IO */
    TEST_PASS("FTP_ERR_LOCAL_IO (-601) is NOT retryable");
    
    /* Ambiguous errors are retryable */
    assert(RetryPolicy::is_retryable(-602) == true);  /* FTP_ERR_REMOTE_IO */
    TEST_PASS("FTP_ERR_REMOTE_IO (-602) is retryable (ambiguous)");
}

/* ============================================================================
 * Test 2: Exponential Backoff with Full Jitter (Spec Section 4.2)
 * ============================================================================ */
void test_exponential_backoff_jitter() {
    std::cout << "\n=== Testing Exponential Backoff with Full Jitter ===" << std::endl;
    
    RetryConfig config;
    config.base_delay_ms = 1000;
    config.max_delay_ms = 30000;
    config.jitter_factor = 1.0;
    config.max_attempts = 5;
    
    RetryPolicy policy(config);
    
    /* Test delay calculation for various attempts */
    /* Attempt 0: delay in [0, 1000) */
    uint64_t delay0 = policy.calculate_delay(0);
    assert(delay0 < 1000);
    TEST_PASS("Attempt 0 delay < 1000ms (base)");
    
    /* Attempt 1: delay in [0, 2000) */
    uint64_t delay1 = policy.calculate_delay(1);
    assert(delay1 < 2000);
    TEST_PASS("Attempt 1 delay < 2000ms (2x base)");
    
    /* Attempt 2: delay in [0, 4000) */
    uint64_t delay2 = policy.calculate_delay(2);
    assert(delay2 < 4000);
    TEST_PASS("Attempt 2 delay < 4000ms (4x base)");
    
    /* Attempt 5: delay capped at max_delay (30000) */
    uint64_t delay5 = policy.calculate_delay(5);
    assert(delay5 <= 30000);
    TEST_PASS("Attempt 5 delay <= 30000ms (max cap)");
    
    /* Test jitter distribution - collect samples */
    std::vector<uint64_t> samples;
    for (int i = 0; i < 100; i++) {
        samples.push_back(policy.calculate_delay(0));
    }
    
    /* Verify jitter produces variance (not always same value) */
    bool has_variance = false;
    for (size_t i = 1; i < samples.size(); i++) {
        if (samples[i] != samples[0]) {
            has_variance = true;
            break;
        }
    }
    assert(has_variance);
    TEST_PASS("Jitter produces variance in delays");
    
    /* Verify no clustering at boundaries */
    bool has_mid_range = false;
    for (auto s : samples) {
        if (s > 200 && s < 800) {
            has_mid_range = true;
            break;
        }
    }
    assert(has_mid_range);
    TEST_PASS("Jitter distribution includes mid-range values");
}

/* ============================================================================
 * Test 3: Retry Execution with Success (Spec Section 4)
 * ============================================================================ */
void test_retry_execution_success() {
    std::cout << "\n=== Testing Retry Execution: Success Case ===" << std::endl;
    
    RetryConfig config;
    config.max_attempts = 3;
    config.base_delay_ms = 10;  /* Fast for testing */
    config.max_delay_ms = 50;
    
    RetryPolicy policy(config);
    
    /* Function that succeeds on first try */
    int call_count = 0;
    auto success_func = [&]() -> int32_t {
        call_count++;
        return 0;  /* FTP_OK */
    };
    
    uint32_t attempts = 0;
    int32_t result = policy.execute_with_retry(success_func, &attempts);
    
    assert(result == 0);
    TEST_PASS("Success on first attempt returns FTP_OK");
    
    assert(attempts == 1);
    TEST_PASS("Attempt count is 1 for immediate success");
    
    assert(call_count == 1);
    TEST_PASS("Function called exactly once");
}

/* ============================================================================
 * Test 4: Retry Execution with Failure then Success (Spec Section 4)
 * ============================================================================ */
void test_retry_execution_recovery() {
    std::cout << "\n=== Testing Retry Execution: Recovery after Failures ===" << std::endl;
    
    RetryConfig config;
    config.max_attempts = 3;
    config.base_delay_ms = 10;  /* Fast for testing */
    config.max_delay_ms = 50;
    
    RetryPolicy policy(config);
    
    /* Function that fails twice then succeeds */
    int call_count = 0;
    auto flaky_func = [&]() -> int32_t {
        call_count++;
        if (call_count <= 2) {
            return -403;  /* FTP_ERR_NETWORK_RESET - retryable */
        }
        return 0;  /* FTP_OK */
    };
    
    uint32_t attempts = 0;
    int32_t result = policy.execute_with_retry(flaky_func, &attempts);
    
    assert(result == 0);
    TEST_PASS("Recovery after transient failures returns FTP_OK");
    
    assert(attempts == 3);
    TEST_PASS("Attempt count is 3 (2 failures + 1 success)");
    
    assert(call_count == 3);
    TEST_PASS("Function called 3 times");
}

/* ============================================================================
 * Test 5: Retry Exhaustion (Spec Section 9.1)
 * ============================================================================ */
void test_retry_exhaustion() {
    std::cout << "\n=== Testing Retry Exhaustion ===" << std::endl;
    
    RetryConfig config;
    config.max_attempts = 3;
    config.base_delay_ms = 10;
    config.max_delay_ms = 50;
    
    RetryPolicy policy(config);
    
    /* Function that always fails */
    int call_count = 0;
    auto failing_func = [&]() -> int32_t {
        call_count++;
        return -403;  /* FTP_ERR_NETWORK_RESET */
    };
    
    uint32_t attempts = 0;
    int32_t result = policy.execute_with_retry(failing_func, &attempts);
    
    assert(result == -403);
    TEST_PASS("Exhausted retries return last error code");
    
    assert(attempts == 4);  /* 1 initial + 3 retries */
    TEST_PASS("Attempt count is 4 (1 initial + 3 retries)");
    
    assert(call_count == 4);
    TEST_PASS("Function called 4 times before giving up");
}

/* ============================================================================
 * Test 6: Non-Retryable Error Fast-Fail (Spec Section 4.3)
 * ============================================================================ */
void test_non_retryable_fast_fail() {
    std::cout << "\n=== Testing Non-Retryable Error Fast-Fail ===" << std::endl;
    
    RetryConfig config;
    config.max_attempts = 5;
    config.base_delay_ms = 10;
    
    RetryPolicy policy(config);
    
    /* Function that fails with non-retryable error */
    int call_count = 0;
    auto auth_fail_func = [&]() -> int32_t {
        call_count++;
        return -301;  /* FTP_ERR_AUTH_FAILED - NOT retryable */
    };
    
    uint32_t attempts = 0;
    int32_t result = policy.execute_with_retry(auth_fail_func, &attempts);
    
    assert(result == -301);
    TEST_PASS("Non-retryable error returns immediately");
    
    assert(attempts == 1);
    TEST_PASS("Only 1 attempt for non-retryable error (fast-fail)");
    
    assert(call_count == 1);
    TEST_PASS("Function called only once");
}

/* ============================================================================
 * Test 7: Circuit Breaker - Basic State Transitions (Spec Section 5.1)
 * ============================================================================ */
void test_circuit_breaker_states() {
    std::cout << "\n=== Testing Circuit Breaker: State Transitions ===" << std::endl;
    
    CircuitBreakerConfig config;
    config.failure_threshold = 3;
    config.window_ms = 60000;
    config.open_duration_ms = 100;  /* Fast for testing */
    config.half_open_max = 1;
    
    CircuitBreaker cb(config);
    
    std::string host = "test.example.com:21";
    
    /* Initial state should be CLOSED */
    assert(cb.get_state(host) == CircuitState::CLOSED);
    TEST_PASS("Initial circuit state is CLOSED");
    
    assert(cb.can_request(host) == true);
    TEST_PASS("Requests allowed in CLOSED state");
    
    /* Record failures to trip circuit */
    cb.record_failure(host, 1.0);
    cb.record_failure(host, 1.0);
    cb.record_failure(host, 1.0);
    
    /* Should now be OPEN */
    assert(cb.get_state(host) == CircuitState::OPEN);
    TEST_PASS("Circuit trips to OPEN after threshold failures");
    
    assert(cb.can_request(host) == false);
    TEST_PASS("Requests blocked in OPEN state");
}

/* ============================================================================
 * Test 8: Circuit Breaker - HALF_OPEN Recovery (Spec Section 5.1)
 * ============================================================================ */
void test_circuit_breaker_half_open_recovery() {
    std::cout << "\n=== Testing Circuit Breaker: HALF_OPEN Recovery ===" << std::endl;
    
    CircuitBreakerConfig config;
    config.failure_threshold = 2;
    config.window_ms = 60000;
    config.open_duration_ms = 100;  /* 100ms for fast test */
    config.half_open_max = 1;
    
    CircuitBreaker cb(config);
    
    std::string host = "recovery.example.com:21";
    
    /* Trip the circuit */
    cb.record_failure(host, 1.0);
    cb.record_failure(host, 1.0);
    
    assert(cb.get_state(host) == CircuitState::OPEN);
    TEST_PASS("Circuit is OPEN after failures");
    
    /* Wait for open_duration to expire */
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    
    /* Should transition to HALF_OPEN */
    assert(cb.get_state(host) == CircuitState::HALF_OPEN);
    TEST_PASS("Circuit transitions to HALF_OPEN after timeout");
    
    /* Allow probe request */
    assert(cb.can_request(host) == true);
    TEST_PASS("Probe request allowed in HALF_OPEN");
    
    /* Record success - should go back to CLOSED */
    cb.record_success(host);
    
    assert(cb.get_state(host) == CircuitState::CLOSED);
    TEST_PASS("Success in HALF_OPEN transitions to CLOSED");
    
    assert(cb.can_request(host) == true);
    TEST_PASS("Requests allowed after recovery to CLOSED");
}

/* ============================================================================
 * Test 9: Circuit Breaker - Weighted Failures (Spec Section 5.3)
 * ============================================================================ */
void test_circuit_breaker_weighted_failures() {
    std::cout << "\n=== Testing Circuit Breaker: Weighted Failures ===" << std::endl;
    
    CircuitBreakerConfig config;
    config.failure_threshold = 3;
    config.window_ms = 60000;
    config.open_duration_ms = 60000;
    
    CircuitBreaker cb(config);
    
    std::string host = "stress.example.com:21";
    
    /* Record weighted failures (like 421 Too many connections) */
    cb.record_failure(host, 0.5);  /* 0.5 failure */
    cb.record_failure(host, 0.5);  /* 0.5 failure */
    cb.record_failure(host, 0.5);  /* 0.5 failure */
    
    /* Should not trip yet (1.5 < 3.0) */
    assert(cb.get_state(host) == CircuitState::CLOSED);
    TEST_PASS("Circuit remains CLOSED with weighted failures below threshold");
    
    /* Add more failures to trip */
    cb.record_failure(host, 0.5);
    cb.record_failure(host, 0.5);
    cb.record_failure(host, 0.5);
    
    /* Now should trip (3.0 >= 3.0) */
    assert(cb.get_state(host) == CircuitState::OPEN);
    TEST_PASS("Circuit trips when weighted failures reach threshold");
}

/* ============================================================================
 * Test 10: Stall Detector - Basic Operation (Spec Section 6.2)
 * ============================================================================ */
void test_stall_detector_basic() {
    std::cout << "\n=== Testing Stall Detector: Basic Operation ===" << std::endl;
    
    StallDetectorConfig config;
    config.minimum_bps = 1024.0;  /* 1KB/s floor */
    config.stall_multiplier = 3.0;
    config.min_stall_seconds = 1;  /* Fast for testing */
    
    StallDetector detector(config);
    
    /* Start monitoring a 1MB file */
    detector.start(1024 * 1024);
    
    /* Initially not stalled */
    assert(detector.is_stalled() == false);
    TEST_PASS("Not stalled immediately after start");
    
    /* Report some progress */
    detector.report_progress(64 * 1024);  /* 64KB */
    
    assert(detector.is_stalled() == false);
    TEST_PASS("Not stalled after progress report");
    
    assert(detector.get_total_bytes_transferred() == 64 * 1024);
    TEST_PASS("Total bytes tracked correctly");
}

/* ============================================================================
 * Test 11: Stall Detector - Stall Detection (Spec Section 6.2)
 * ============================================================================ */
void test_stall_detector_stall() {
    std::cout << "\n=== Testing Stall Detector: Stall Detection ===" << std::endl;
    
    StallDetectorConfig config;
    config.minimum_bps = 1024.0;
    config.stall_multiplier = 3.0;
    config.min_stall_seconds = 1;  /* 1 second minimum stall */
    
    StallDetector detector(config);
    
    /* Start monitoring */
    detector.start(1024 * 1024);
    
    /* Report initial progress */
    detector.report_progress(64 * 1024);
    
    /* Wait for stall threshold */
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    
    /* Should detect stall */
    assert(detector.is_stalled() == true);
    TEST_PASS("Stall detected after no progress for threshold duration");
}

/* ============================================================================
 * Test 12: Idempotency Classifier - Command Classification (Spec Section 7.1)
 * ============================================================================ */
void test_idempotency_classifier() {
    std::cout << "\n=== Testing Idempotency Classifier: Command Classification ===" << std::endl;
    
    /* SAFE commands */
    assert(IdempotencyClassifier::classify_command("USER") == IdempotencyClass::SAFE);
    TEST_PASS("USER classified as SAFE");
    
    assert(IdempotencyClassifier::classify_command("PASS") == IdempotencyClass::SAFE);
    TEST_PASS("PASS classified as SAFE");
    
    assert(IdempotencyClassifier::classify_command("PASV") == IdempotencyClass::SAFE);
    TEST_PASS("PASV classified as SAFE");
    
    assert(IdempotencyClassifier::classify_command("NOOP") == IdempotencyClass::SAFE);
    TEST_PASS("NOOP classified as SAFE");
    
    assert(IdempotencyClassifier::classify_command("PWD") == IdempotencyClass::SAFE);
    TEST_PASS("PWD classified as SAFE");
    
    /* CONDITIONAL commands */
    assert(IdempotencyClassifier::classify_command("MKD") == IdempotencyClass::CONDITIONAL);
    TEST_PASS("MKD classified as CONDITIONAL");
    
    /* UNSAFE commands */
    assert(IdempotencyClassifier::classify_command("STOR") == IdempotencyClass::UNSAFE);
    TEST_PASS("STOR classified as UNSAFE");
    
    assert(IdempotencyClassifier::classify_command("DELE") == IdempotencyClass::UNSAFE);
    TEST_PASS("DELE classified as UNSAFE");
    
    assert(IdempotencyClassifier::classify_command("RNFR") == IdempotencyClass::UNSAFE);
    TEST_PASS("RNFR classified as UNSAFE");
    
    assert(IdempotencyClassifier::classify_command("RNTO") == IdempotencyClass::UNSAFE);
    TEST_PASS("RNTO classified as UNSAFE");
}

/* ============================================================================
 * Test 13: STOR Retry Semantics (Spec Section 7.2)
 * ============================================================================ */
void test_stor_retry_semantics() {
    std::cout << "\n=== Testing STOR Retry Semantics ===" << std::endl;
    
    /* Without resume enabled, should restart from 0 */
    uint64_t offset_no_resume = IdempotencyClassifier::get_stor_restart_offset(5000, false);
    assert(offset_no_resume == 0);
    TEST_PASS("STOR restart offset is 0 without resume");
    
    /* With resume enabled, should restart from confirmed bytes */
    uint64_t offset_with_resume = IdempotencyClassifier::get_stor_restart_offset(5000, true);
    assert(offset_with_resume == 5000);
    TEST_PASS("STOR restart offset uses confirmed bytes with resume");
}

/* ============================================================================
 * Test 14: MKD Retry Logic (Spec Section 7.1)
 * ============================================================================ */
void test_mkd_retry_logic() {
    std::cout << "\n=== Testing MKD Retry Logic ===" << std::endl;
    
    /* MKD with server denied (550 - already exists) should NOT retry */
    assert(IdempotencyClassifier::should_retry_mkd(-502) == false);
    TEST_PASS("MKD does not retry on SERVER_DENIED (already exists)");
    
    /* Other errors also don't retry */
    assert(IdempotencyClassifier::should_retry_mkd(-401) == false);
    TEST_PASS("MKD does not retry on other errors");
}

/* ============================================================================
 * Test 15: Resilience Controller Integration (Spec Section 8)
 * ============================================================================ */
void test_resilience_controller_integration() {
    std::cout << "\n=== Testing Resilience Controller Integration ===" << std::endl;
    
    /* Test RetryPolicy directly since we can't easily mock ProtocolEngine */
    RetryConfig retry_config;
    retry_config.max_attempts = 3;
    retry_config.base_delay_ms = 10;
    
    RetryPolicy policy(retry_config);
    
    /* Update configuration using set_config method */
    RetryConfig new_config;
    new_config.max_attempts = 5;
    new_config.base_delay_ms = 2000;
    new_config.max_delay_ms = 60000;
    policy.set_config(new_config);
    TEST_PASS("RetryPolicy configuration methods accessible");
    
    const auto& cfg = policy.get_config();
    assert(cfg.max_attempts == 5);
    TEST_PASS("Retry policy updated: max_attempts = 5");
    
    assert(cfg.base_delay_ms == 2000);
    TEST_PASS("Retry policy updated: base_delay_ms = 2000");
    
    assert(cfg.max_delay_ms == 60000);
    TEST_PASS("Retry policy updated: max_delay_ms = 60000");
}

/* ============================================================================
 * Main Test Runner
 * ============================================================================ */
int main() {
    std::cout << "==============================================" << std::endl;
    std::cout << "FTP Client Library - Phase 5 Resilience Tests" << std::endl;
    std::cout << "==============================================" << std::endl;
    
    /* Run all tests */
    test_retry_policy_error_classification();
    test_exponential_backoff_jitter();
    test_retry_execution_success();
    test_retry_execution_recovery();
    test_retry_exhaustion();
    test_non_retryable_fast_fail();
    test_circuit_breaker_states();
    test_circuit_breaker_half_open_recovery();
    test_circuit_breaker_weighted_failures();
    test_stall_detector_basic();
    test_stall_detector_stall();
    test_idempotency_classifier();
    test_stor_retry_semantics();
    test_mkd_retry_logic();
    test_resilience_controller_integration();
    
    /* Summary */
    std::cout << "\n==============================================" << std::endl;
    if (failures == 0) {
        std::cout << "Test Summary: All tests passed!" << std::endl;
    } else {
        std::cout << "Test Summary: " << failures << " test(s) failed" << std::endl;
    }
    std::cout << "==============================================" << std::endl;
    
    return failures > 0 ? 1 : 0;
}
