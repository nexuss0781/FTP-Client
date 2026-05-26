/*
 * ModeZCompressor.hpp - MODE Z Compression Support
 * 
 * Per Phase 7 Spec Section 6: MODE Z Compression
 * 
 * Implements on-the-fly deflate compression for FTP MODE Z transfers.
 */

#ifndef FTPCLIENT_TRANSFER_MODEZCOMPRESSOR_HPP
#define FTPCLIENT_TRANSFER_MODEZCOMPRESSOR_HPP

#include <cstdint>
#include <cstddef>
#include <vector>

namespace ftpclient {
namespace transfer {

/**
 * MODE Z Compressor for FTP compression negotiation
 * 
 * Thread Safety: Not thread-safe. One instance per transfer thread.
 * 
 * Usage:
 * 1. Call negotiate() after connection to check server support
 * 2. If supported, use compress() to compress data before sending
 */
class ModeZCompressor {
public:
    ModeZCompressor();
    ~ModeZCompressor();
    
    /**
     * Check if MODE Z is available (server advertised support)
     * Note: Actual negotiation happens at protocol layer
     * @return true if compression can be used
     */
    bool isAvailable() const { return available_; }
    
    /**
     * Set availability (called by protocol engine after MODE Z negotiation)
     * @param available Whether server supports MODE Z
     */
    void setAvailable(bool available) { available_ = available; }
    
    /**
     * Compress a buffer
     * @param input Input data pointer
     * @param input_size Input data size in bytes
     * @param output Output buffer (resized as needed)
     * @return Compressed size, or 0 on error
     */
    size_t compress(const void* input, size_t input_size, std::vector<uint8_t>& output);
    
    /**
     * Reset compression state (for new file)
     */
    void reset();
    
    /**
     * Get compression statistics
     */
    uint64_t getBytesIn() const { return bytes_in_; }
    uint64_t getBytesOut() const { return bytes_out_; }
    double getCompressionRatio() const;
    
private:
    struct Impl;
    Impl* impl_;
    bool available_;
    uint64_t bytes_in_;
    uint64_t bytes_out_;
};

} // namespace transfer
} // namespace ftpclient

#endif /* FTPCLIENT_TRANSFER_MODEZCOMPRESSOR_HPP */
