/*
 * BufferPool.hpp - Reusable Buffer Pool for Phase 4
 * 
 * Manages a pool of 256KB buffers for transfer operations.
 * Per Phase 4 spec Section 7.1.
 */

#ifndef FTPCLIENT_TRANSFER_BUFFER_POOL_HPP
#define FTPCLIENT_TRANSFER_BUFFER_POOL_HPP

#include <vector>
#include <mutex>
#include <cstdint>
#include <new>

namespace ftpclient { namespace transfer {

/**
 * Buffer Pool Configuration
 * Per spec Section 7.1
 */
struct BufferPoolConfig {
    uint32_t buffer_size;       /* Default: 256KB */
    uint32_t initial_count;     /* Number of buffers to pre-allocate */
    
    BufferPoolConfig()
        : buffer_size(256 * 1024)
        , initial_count(8)
    {}
    
    explicit BufferPoolConfig(uint32_t buf_size, uint32_t count)
        : buffer_size(buf_size)
        , initial_count(count)
    {}
};

/**
 * Buffer Pool
 * 
 * Thread-safe pool of reusable buffers for data transfers.
 * Reduces allocator contention by reusing buffers across transfers.
 * Per spec Section 7.1.
 */
class BufferPool {
public:
    /**
     * Construct buffer pool with given configuration
     * 
     * @param config Pool configuration
     */
    explicit BufferPool(const BufferPoolConfig& config = BufferPoolConfig());
    
    /**
     * Destructor - frees all allocated buffers
     */
    ~BufferPool();
    
    /* Disable copying */
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;
    
    /**
     * Acquire a buffer from the pool
     * 
     * @return Pointer to buffer (BUFFER_SIZE bytes). Caller must release().
     *         Returns nullptr only if allocation fails.
     */
    char* acquire();
    
    /**
     * Release a buffer back to the pool
     * 
     * @param buf Buffer pointer returned by acquire()
     */
    void release(char* buf);
    
    /**
     * Get buffer size
     */
    uint32_t get_buffer_size() const { return config_.buffer_size; }
    
    /**
     * Get current pool size (available buffers)
     */
    size_t get_pool_size() const;

private:
    BufferPoolConfig config_;
    std::vector<char*> pool_;
    mutable std::mutex mutex_;
    
    /**
     * Allocate a new buffer
     */
    char* allocate_buffer();
    
    /**
     * Free a buffer
     */
    void free_buffer(char* buf);
};

}} // namespace ftpclient::transfer

#endif /* FTPCLIENT_TRANSFER_BUFFER_POOL_HPP */
