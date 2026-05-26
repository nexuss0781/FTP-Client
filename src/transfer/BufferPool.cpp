/*
 * BufferPool.cpp - Reusable Buffer Pool Implementation
 * 
 * Implements thread-safe buffer pool for Phase 4 transfer engine.
 * Per Phase 4 spec Section 7.1.
 */

#include "BufferPool.hpp"
#include <cstring>
#include <new>

namespace ftpclient { namespace transfer {

BufferPool::BufferPool(const BufferPoolConfig& config)
    : config_(config)
{
    /* Pre-allocate initial buffers */
    pool_.reserve(config_.initial_count);
    for (uint32_t i = 0; i < config_.initial_count; ++i) {
        char* buf = allocate_buffer();
        if (buf) {
            pool_.push_back(buf);
        }
    }
}

BufferPool::~BufferPool() {
    /* Free all buffers in pool */
    std::lock_guard<std::mutex> lock(mutex_);
    for (char* buf : pool_) {
        free_buffer(buf);
    }
    pool_.clear();
}

char* BufferPool::allocate_buffer() {
    try {
        /* Allocate with padding for alignment safety */
        char* buf = new(std::nothrow) char[config_.buffer_size];
        if (buf) {
            std::memset(buf, 0, config_.buffer_size);
        }
        return buf;
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void BufferPool::free_buffer(char* buf) {
    if (buf) {
        /* Secure zero before freeing */
        std::memset(buf, 0, config_.buffer_size);
        delete[] buf;
    }
}

char* BufferPool::acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!pool_.empty()) {
        char* buf = pool_.back();
        pool_.pop_back();
        return buf;
    }
    
    /* Pool exhausted - allocate fallback buffer */
    /* Per spec Section 7.1: fallback to new char[BUFFER_SIZE] */
    return allocate_buffer();
}

void BufferPool::release(char* buf) {
    if (!buf) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    /* Return to pool for reuse */
    pool_.push_back(buf);
}

size_t BufferPool::get_pool_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pool_.size();
}

}} // namespace ftpclient::transfer
