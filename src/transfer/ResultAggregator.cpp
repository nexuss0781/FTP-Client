/*
 * ResultAggregator.cpp - Per-File Result Tracking Implementation
 * 
 * Implements thread-safe result aggregation for Phase 4.
 * Per Phase 4 spec Section 9.
 */

#include "ResultAggregator.hpp"
#include <algorithm>

namespace ftpclient { namespace transfer {

ResultAggregator::ResultAggregator() = default;

void ResultAggregator::record_result(const std::string& local_path,
                                      const std::string& remote_path,
                                      int32_t status,
                                      uint64_t bytes_sent) {
    /* Update counters atomically */
    if (status == 0) {  /* FTP_OK */
        files_success_.fetch_add(1, std::memory_order_relaxed);
    } else {
        files_failed_.fetch_add(1, std::memory_order_relaxed);
        
        /* Update worst status (more negative = worse) */
        int32_t current_worst = worst_status_.load(std::memory_order_acquire);
        while (status < current_worst) {
            if (worst_status_.compare_exchange_weak(current_worst, status,
                                                     std::memory_order_release,
                                                     std::memory_order_relaxed)) {
                break;
            }
        }
    }
    
    /* Store per-file result */
    {
        std::lock_guard<std::mutex> lock(mutex_);
        FileResult result;
        result.local_path = local_path;
        result.remote_path = remote_path;
        result.status = status;
        result.bytes_sent = bytes_sent;
        results_.push_back(std::move(result));
    }
}

void ResultAggregator::add_bytes_transferred(uint64_t bytes) {
    bytes_transferred_.fetch_add(bytes, std::memory_order_relaxed);
}

std::vector<FileResult> ResultAggregator::get_results() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return results_;
}

void ResultAggregator::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    results_.clear();
    files_total_.store(0, std::memory_order_release);
    files_success_.store(0, std::memory_order_release);
    files_failed_.store(0, std::memory_order_release);
    bytes_transferred_.store(0, std::memory_order_release);
    worst_status_.store(0, std::memory_order_release);
}

}} // namespace ftpclient::transfer
