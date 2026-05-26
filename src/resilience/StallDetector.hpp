/*
 * StallDetector.hpp - Adaptive Throughput-Based Stall Detection for Phase 5
 * 
 * Implements adaptive stall detection per Phase 5 Spec Section 6.2.
 * Detects stalled transfers based on observed bandwidth rather than fixed timeouts.
 */

#ifndef FTPCLIENT_STALL_DETECTOR_HPP
#define FTPCLIENT_STALL_DETECTOR_HPP

#include <cstdint>
#include <chrono>

namespace ftpclient { namespace resilience {

/**
 * Stall Detector Configuration per Phase 5 Spec Section 6.2
 */
struct StallDetectorConfig {
    double minimum_bps;           /* Default: 1024.0 - 1KB/s floor */
    double stall_multiplier;      /* Default: 3.0 - 3x expected time threshold */
    uint64_t min_stall_seconds;   /* Default: 5 - Minimum stall threshold in seconds */
    
    StallDetectorConfig()
        : minimum_bps(1024.0)
        , stall_multiplier(3.0)
        , min_stall_seconds(5)
    {}
};

/**
 * Stall Detector - Adaptive Throughput-Based Stall Detection
 * 
 * Per Phase 5 Spec Section 6.2:
 * - Tracks observed bandwidth using exponential moving average (EMA)
 * - Calculates expected transfer time based on observed throughput
 * - Triggers stall if no progress for > 3x expected time (minimum 5s)
 * 
 * Thread Safety: Not thread-safe. Must be called from single transfer thread.
 */
class StallDetector {
public:
    /**
     * Construct stall detector with configuration
     * 
     * @param config Stall detector configuration parameters
     */
    explicit StallDetector(const StallDetectorConfig& config = StallDetectorConfig());
    
    /**
     * Destructor
     */
    ~StallDetector();
    
    /**
     * Start monitoring a file transfer
     * 
     * @param file_size_bytes Total file size in bytes
     */
    void start(uint64_t file_size_bytes);
    
    /**
     * Report progress - call after each buffer write
     * 
     * @param bytes_sent_since_last_check Bytes transferred since last report
     */
    void report_progress(uint64_t bytes_sent_since_last_check);
    
    /**
     * Check if transfer is stalled
     * 
     * @return true if stall detected, false otherwise
     */
    bool is_stalled();
    
    /**
     * Get observed bandwidth in bytes per second
     * 
     * @return Current observed bandwidth (EMA)
     */
    double get_observed_bps() const { return observed_bps_; }
    
    /**
     * Get total bytes transferred since start
     * 
     * @return Total bytes transferred
     */
    uint64_t get_total_bytes_transferred() const { return total_bytes_transferred_; }
    
    /**
     * Reset the stall detector for reuse
     */
    void reset();
    
    /**
     * Get current configuration
     */
    const StallDetectorConfig& get_config() const { return config_; }
    
    /**
     * Update configuration
     */
    void set_config(const StallDetectorConfig& config) { config_ = config; }

private:
    StallDetectorConfig config_;
    
    uint64_t file_size_;
    uint64_t total_bytes_transferred_;
    
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_progress_time_;
    
    double observed_bps_;  /* EMA of throughput */
    
    bool started_;
    
    /**
     * Calculate stall threshold in seconds based on current throughput
     * 
     * @return Stall threshold in seconds
     */
    double calculate_stall_threshold_seconds() const;
};

}} // namespace ftpclient::resilience

#endif /* FTPCLIENT_STALL_DETECTOR_HPP */
