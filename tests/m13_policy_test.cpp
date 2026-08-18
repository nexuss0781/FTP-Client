#include "resilience/RetryPolicy.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

using ftpclient::resilience::ErrorCategory;
using ftpclient::resilience::RetryConfig;
using ftpclient::resilience::RetryPolicy;
using ftpclient::resilience::kRetryCategoryNetwork;
using ftpclient::resilience::kRetryCategoryServer;

int main() {
    RetryConfig config;
    config.max_attempts = 2;
    config.base_delay_ms = 0;
    config.max_delay_ms = 0;
    config.jitter_factor = 0.0;
    config.retry_categories = kRetryCategoryNetwork;
    RetryPolicy policy(config);

    uint32_t attempts = 0;
    int call_count = 0;
    const int32_t recovered = policy.execute_with_retry([&]() {
        ++call_count;
        return call_count < 3 ? -401 : 0;
    }, &attempts);
    assert(recovered == 0);
    assert(call_count == 3);
    assert(attempts == 3);

    attempts = 0;
    call_count = 0;
    const int32_t category_filtered = policy.execute_with_retry([&]() {
        ++call_count;
        return -502;
    }, &attempts);
    assert(category_filtered == -502);
    assert(call_count == 1);
    assert(attempts == 1);

    config.retry_categories = kRetryCategoryServer;
    config.retry_all_errors = 1;
    RetryPolicy retry_all_policy(config);
    attempts = 0;
    call_count = 0;
    const int32_t retry_all_result = retry_all_policy.execute_with_retry([&]() {
        ++call_count;
        return -301;
    }, &attempts);
    assert(retry_all_result == -301);
    assert(call_count == 3);
    assert(attempts == 3);

    assert(RetryPolicy::classify_error(-401) == ErrorCategory::TRANSIENT_NETWORK);
    assert(RetryPolicy::classify_error(-502) == ErrorCategory::TRANSIENT_SERVER);
    assert(RetryPolicy::category_mask(ErrorCategory::TRANSIENT_NETWORK) == kRetryCategoryNetwork);
    assert(RetryPolicy::is_retryable(-401, false, kRetryCategoryNetwork));
    assert(!RetryPolicy::is_retryable(-401, false, kRetryCategoryServer));
    assert(RetryPolicy::is_retryable(-301, true, kRetryCategoryServer));

    RetryConfig budget_config;
    budget_config.max_attempts = 20;
    budget_config.base_delay_ms = 1;
    budget_config.max_delay_ms = 1;
    budget_config.max_elapsed_ms = 2;
    budget_config.jitter_factor = 0.0;
    RetryPolicy budget_policy(budget_config);
    attempts = 0;
    const int32_t budget_result = budget_policy.execute_with_retry(
        []() { return static_cast<int32_t>(-401); }, &attempts);
    assert(budget_result == -401);
    assert(attempts >= 1 && attempts <= 21);

    std::cout << "M13 retry policy controls passed" << std::endl;
    return 0;
}
