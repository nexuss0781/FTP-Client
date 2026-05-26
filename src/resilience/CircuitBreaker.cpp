/*
 * CircuitBreaker.cpp - Per-Host Circuit Breaker Implementation for Phase 5
 * 
 * Implements circuit breaker pattern per Phase 5 Spec Section 5.
 */

#include "CircuitBreaker.hpp"
#include <algorithm>
#include <mutex>

namespace ftpclient { namespace resilience {

CircuitBreaker::CircuitBreaker(const CircuitBreakerConfig& config)
    : config_(config)
{
}

CircuitBreaker::~CircuitBreaker() = default;

bool CircuitBreaker::should_try_half_open(const HostState& state) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - state.open_since).count();
    
    return static_cast<uint64_t>(elapsed) >= config_.open_duration_ms;
}

uint32_t CircuitBreaker::get_weighted_failures_in_window(const HostState& state) {
    /* For simplicity, we track cumulative weighted failures */
    /* A production implementation would use a sliding window with timestamps */
    return static_cast<uint32_t>(state.failure_count);
}

HostState& CircuitBreaker::get_or_create_host_state(const std::string& host_key) {
    /* Caller must hold write lock */
    auto it = host_states_.find(host_key);
    if (it == host_states_.end()) {
        host_states_.emplace(host_key, HostState{});
        return host_states_[host_key];
    }
    return it->second;
}

bool CircuitBreaker::can_request(const std::string& host_key) {
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    
    auto it = host_states_.find(host_key);
    if (it == host_states_.end()) {
        return true;  /* No history, allow request */
    }
    
    const HostState& state = it->second;
    
    switch (state.state) {
        case CircuitState::CLOSED:
            return true;  /* Normal operation */
        
        case CircuitState::OPEN:
            /* Check if we should transition to HALF_OPEN */
            if (should_try_half_open(state)) {
                /* Transition will happen in record_success/failure */
                return true;  /* Allow probe request */
            }
            return false;  /* Fast-fail */
        
        case CircuitState::HALF_OPEN:
            /* Allow limited probes */
            return state.half_open_probes < config_.half_open_max;
        
        default:
            return false;
    }
}

void CircuitBreaker::record_success(const std::string& host_key) {
    std::unique_lock<std::shared_mutex> write_lock(mutex_);
    
    HostState& state = get_or_create_host_state(host_key);
    
    /* Success resets failure count and transitions to CLOSED */
    state.failure_count = 0;
    state.state = CircuitState::CLOSED;
    state.half_open_probes = 0;
}

void CircuitBreaker::record_failure(const std::string& host_key, double weighted_failure) {
    std::unique_lock<std::shared_mutex> write_lock(mutex_);
    
    HostState& state = get_or_create_host_state(host_key);
    
    auto now = std::chrono::steady_clock::now();
    state.last_failure_time = now;
    
    /* Add weighted failure */
    state.failure_count += static_cast<uint32_t>(weighted_failure * 100);  /* Store as integer *100 */
    
    switch (state.state) {
        case CircuitState::CLOSED:
            /* Check if we should trip to OPEN */
            if (state.failure_count >= config_.failure_threshold * 100) {
                state.state = CircuitState::OPEN;
                state.open_since = now;
                state.half_open_probes = 0;
            }
            break;
        
        case CircuitState::HALF_OPEN:
            /* Probe failed, go back to OPEN */
            state.state = CircuitState::OPEN;
            state.open_since = now;
            state.half_open_probes = 0;
            break;
        
        case CircuitState::OPEN:
            /* Already open, extend timeout */
            state.open_since = now;
            break;
        
        default:
            break;
    }
}

CircuitState CircuitBreaker::get_state(const std::string& host_key) {
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    
    auto it = host_states_.find(host_key);
    if (it == host_states_.end()) {
        return CircuitState::CLOSED;
    }
    
    const HostState& state = it->second;
    
    /* Check for automatic state transition */
    if (state.state == CircuitState::OPEN && should_try_half_open(state)) {
        return CircuitState::HALF_OPEN;
    }
    
    return state.state;
}

uint32_t CircuitBreaker::get_failure_count(const std::string& host_key) {
    std::shared_lock<std::shared_mutex> read_lock(mutex_);
    
    auto it = host_states_.find(host_key);
    if (it == host_states_.end()) {
        return 0;
    }
    
    return it->second.failure_count / 100;  /* Convert back from *100 */
}

void CircuitBreaker::reset_host(const std::string& host_key) {
    std::unique_lock<std::shared_mutex> write_lock(mutex_);
    
    auto it = host_states_.find(host_key);
    if (it != host_states_.end()) {
        host_states_.erase(it);
    }
}

void CircuitBreaker::clear_all() {
    std::unique_lock<std::shared_mutex> write_lock(mutex_);
    host_states_.clear();
}

}} // namespace ftpclient::resilience
