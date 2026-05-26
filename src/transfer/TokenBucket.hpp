/*
 * TokenBucket.hpp - Bandwidth Throttling Rate Limiter
 * 
 * Per Phase 7 Spec Section 5: Bandwidth Throttling (Rate Limiting)
 * 
 * Implements token bucket algorithm for rate limiting data transfers.
 */

#ifndef FTPCLIENT_TRANSFER_TOKENBUCKET_HPP
#define FTPCLIENT_TRANSFER_TOKENBUCKET_HPP

#include <cstdint>
#include <chrono>
#include <mutex>

namespace ftpclient {
namespace transfer {

/**
 * Token Bucket Rate Limiter
 * 
 * Thread Safety: Thread-safe via internal mutex.
 * 
 * Algorithm: Token bucket with configurable rate and burst capacity.
 * Tokens are replenished at `rate_` bytes per second, up to `burst_` maximum.
 * Each consume() call blocks until sufficient tokens are available.
 */
class TokenBucket {
public:
    /**
     * Construct token bucket
     * @param bytes_per_second Maximum sustained rate (0 = unlimited)
     * @param burst_bytes Burst capacity (0 = defaults to 1 second of tokens)
     */
    explicit TokenBucket(uint64_t bytes_per_second = 0, uint64_t burst_bytes = 0);
    
    /**
     * Consume bytes from the bucket
     * Blocks (sleeps) until tokens are available, then deducts.
     * @param bytes Number of bytes to consume
     */
    void consume(uint64_t bytes);
    
    /**
     * Set new rate limit
     * @param bytes_per_second New rate (0 = unlimited)
     */
    void setRate(uint64_t bytes_per_second);
    
    /**
     * Set new burst capacity
     * @param burst_bytes New burst capacity
     */
    void setBurst(uint64_t burst_bytes);
    
    /**
     * Check if rate limiting is active
     */
    bool isActive() const { return rate_ > 0; }
    
private:
    /**
     * Refill tokens based on elapsed time
     */
    void refill();
    
    uint64_t rate_;           /* Bytes per second */
    uint64_t burst_;          /* Maximum token capacity */
    uint64_t tokens_;         /* Current available tokens (fixed point) */
    std::chrono::steady_clock::time_point last_update_;
    mutable std::mutex mutex_;
    
    /* Fixed point precision for fractional tokens */
    static constexpr int TOKEN_SHIFT = 20;
    static constexpr uint64_t TOKEN_UNIT = uint64_t(1) << TOKEN_SHIFT;
};

} // namespace transfer
} // namespace ftpclient

#endif /* FTPCLIENT_TRANSFER_TOKENBUCKET_HPP */
