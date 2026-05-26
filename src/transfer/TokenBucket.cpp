/*
 * TokenBucket.cpp - Bandwidth Throttling Rate Limiter Implementation
 * 
 * Per Phase 7 Spec Section 5: Bandwidth Throttling (Rate Limiting)
 */

#include "TokenBucket.hpp"
#include <thread>
#include <algorithm>

namespace ftpclient {
namespace transfer {

TokenBucket::TokenBucket(uint64_t bytes_per_second, uint64_t burst_bytes)
    : rate_(bytes_per_second)
    , burst_(burst_bytes > 0 ? burst_bytes : bytes_per_second) /* Default to 1 second burst */
    , tokens_(burst_ * TOKEN_UNIT) /* Start full */
    , last_update_(std::chrono::steady_clock::now()) {
}

void TokenBucket::consume(uint64_t bytes) {
    if (rate_ == 0) {
        return; /* Unlimited - no throttling */
    }
    
    const uint64_t bytes_needed = bytes * TOKEN_UNIT;
    
    std::unique_lock<std::mutex> lock(mutex_);
    
    while (true) {
        refill();
        
        if (tokens_ >= bytes_needed) {
            tokens_ -= bytes_needed;
            return;
        }
        
        /* Calculate wait time for needed tokens */
        uint64_t tokens_deficit = bytes_needed - tokens_;
        double seconds_to_wait = static_cast<double>(tokens_deficit) / 
                                 (static_cast<double>(rate_) * TOKEN_UNIT);
        
        /* Release lock while sleeping */
        lock.unlock();
        
        /* Sleep with minimum granularity of 1ms */
        auto sleep_ms = static_cast<int64_t>(seconds_to_wait * 1000.0);
        if (sleep_ms < 1) sleep_ms = 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        
        lock.lock();
    }
}

void TokenBucket::setRate(uint64_t bytes_per_second) {
    std::lock_guard<std::mutex> lock(mutex_);
    rate_ = bytes_per_second;
    if (bytes_per_second == 0) {
        burst_ = 0;
        tokens_ = 0;
    }
}

void TokenBucket::setBurst(uint64_t burst_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    burst_ = burst_bytes > 0 ? burst_bytes : rate_;
}

void TokenBucket::refill() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double>(now - last_update_).count();
    
    if (elapsed <= 0.0) {
        return;
    }
    
    /* Add tokens based on elapsed time */
    uint64_t tokens_to_add = static_cast<uint64_t>(
        elapsed * static_cast<double>(rate_) * TOKEN_UNIT
    );
    
    tokens_ = std::min(tokens_ + tokens_to_add, burst_ * TOKEN_UNIT);
    last_update_ = now;
}

} // namespace transfer
} // namespace ftpclient
