/*
 * StallDetector.cpp - Adaptive Throughput-Based Stall Detection Implementation for Phase 5
 * 
 * Implements adaptive stall detection per Phase 5 Spec Section 6.2.
 */

#include "StallDetector.hpp"
#include <algorithm>

namespace ftpclient { namespace resilience {

StallDetector::StallDetector(const StallDetectorConfig& config)
    : config_(config)
    , file_size_(0)
    , total_bytes_transferred_(0)
    , observed_bps_(0.0)
    , started_(false)
{
}

StallDetector::~StallDetector() = default;

void StallDetector::start(uint64_t file_size_bytes) {
    file_size_ = file_size_bytes;
    total_bytes_transferred_ = 0;
    observed_bps_ = 0.0;
    started_ = true;
    
    auto now = std::chrono::steady_clock::now();
    start_time_ = now;
    last_progress_time_ = now;
}

void StallDetector::report_progress(uint64_t bytes_sent_since_last_check) {
    if (!started_) {
        return;
    }
    
    auto now = std::chrono::steady_clock::now();
    
    /* Update total bytes */
    total_bytes_transferred_ += bytes_sent_since_last_check;
    
    /* Calculate time elapsed since last progress report */
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_progress_time_).count();
    
    if (elapsed > 0) {
        /* Calculate instantaneous bandwidth */
        double instant_bps = static_cast<double>(bytes_sent_since_last_check) * 1000.0 
                            / static_cast<double>(elapsed);
        
        /* Update EMA of throughput: observed_bps = 0.7 * observed_bps + 0.3 * instant_bps */
        /* Per Phase 5 Spec Section 6.2 Algorithm Step 2 */
        if (observed_bps_ > 0.0) {
            observed_bps_ = (0.7 * observed_bps_) + (0.3 * instant_bps);
        } else {
            /* First reading - use instant value */
            observed_bps_ = instant_bps;
        }
    }
    
    /* Update last progress time */
    last_progress_time_ = now;
}

bool StallDetector::is_stalled() {
    if (!started_) {
        return false;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed_since_progress = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_progress_time_).count();
    
    double elapsed_seconds = static_cast<double>(elapsed_since_progress) / 1000.0;
    
    /* Calculate stall threshold per Phase 5 Spec Section 6.2 Algorithm Steps 3-5 */
    double stall_threshold = calculate_stall_threshold_seconds();
    
    /* Step 5: If (now - last_progress_time) > stall_threshold: return STALLED */
    return elapsed_seconds > stall_threshold;
}

double StallDetector::calculate_stall_threshold_seconds() const {
    /* 
     * Per Phase 5 Spec Section 6.2 Algorithm:
     * Step 3: Expected time for next buffer = buffer_size / max(observed_bps, MINIMUM_BPS)
     * Step 4: Stall threshold = max(expected_time * 3, 5.0) seconds
     */
    
    /* Use minimum BPS floor if observed is too low */
    double effective_bps = std::max(observed_bps_, config_.minimum_bps);
    
    /* For simplicity, we use a fixed buffer size estimate of 256KB */
    /* In practice, this would be the actual buffer size used */
    constexpr double DEFAULT_BUFFER_SIZE = 256.0 * 1024.0;
    
    /* Expected time to transfer one buffer at current throughput */
    double expected_time = DEFAULT_BUFFER_SIZE / effective_bps;
    
    /* Apply stall multiplier (3x expected time) */
    double stall_threshold = expected_time * config_.stall_multiplier;
    
    /* Apply minimum stall threshold */
    stall_threshold = std::max(stall_threshold, static_cast<double>(config_.min_stall_seconds));
    
    return stall_threshold;
}

void StallDetector::reset() {
    file_size_ = 0;
    total_bytes_transferred_ = 0;
    observed_bps_ = 0.0;
    started_ = false;
}

}} // namespace ftpclient::resilience
