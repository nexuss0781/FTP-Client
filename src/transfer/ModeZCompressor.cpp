/*
 * ModeZCompressor.cpp - MODE Z Compression Support Implementation
 * 
 * Per Phase 7 Spec Section 6: MODE Z Compression
 */

#include "ModeZCompressor.hpp"
#include <cstring>

/* zlib include - optional, only if compression available */
#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

namespace ftpclient {
namespace transfer {

struct ModeZCompressor::Impl {
#ifdef HAVE_ZLIB
    z_stream stream;
    bool initialized;
#endif
    
    Impl() {
#ifdef HAVE_ZLIB
        std::memset(&stream, 0, sizeof(stream));
        initialized = false;
#endif
    }
    
    ~Impl() {
#ifdef HAVE_ZLIB
        if (initialized) {
            deflateEnd(&stream);
        }
#endif
    }
};

ModeZCompressor::ModeZCompressor()
    : impl_(new Impl())
    , available_(false)
    , bytes_in_(0)
    , bytes_out_(0) {
}

ModeZCompressor::~ModeZCompressor() {
    delete impl_;
}

size_t ModeZCompressor::compress(const void* input, size_t input_size, std::vector<uint8_t>& output) {
#ifdef HAVE_ZLIB
    if (!available_) {
        /* Compression not available, return copy of input */
        output.assign(static_cast<const uint8_t*>(input), 
                      static_cast<const uint8_t*>(input) + input_size);
        return input_size;
    }
    
    /* Initialize deflate stream if needed */
    if (!impl_->initialized) {
        impl_->stream.zalloc = Z_NULL;
        impl_->stream.zfree = Z_NULL;
        impl_->stream.opaque = Z_NULL;
        
        /* Default compression level 6 - good balance of speed/ratio */
        int ret = deflateInit2(&impl_->stream, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
        if (ret != Z_OK) {
            return 0; /* Error */
        }
        impl_->initialized = true;
    }
    
    /* Set up input */
    impl_->stream.next_in = static_cast<Bytef*>(const_cast<void*>(input));
    impl_->stream.avail_in = static_cast<uInt>(input_size);
    
    /* Prepare output buffer */
    output.resize(input_size + 1024); /* Initial size with room for headers */
    
    size_t total_out = 0;
    
    while (impl_->stream.avail_in > 0) {
        impl_->stream.next_out = output.data() + total_out;
        impl_->stream.avail_out = static_cast<uInt>(output.size() - total_out);
        
        int ret = deflate(&impl_->stream, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            return 0; /* Error */
        }
        
        total_out = output.size() - impl_->stream.avail_out;
        
        /* Expand output buffer if needed */
        if (impl_->stream.avail_out == 0) {
            output.resize(output.size() * 2);
        }
    }
    
    output.resize(total_out);
    bytes_in_ += input_size;
    bytes_out_ += total_out;
    
    return total_out;
#else
    /* No zlib - just copy input to output */
    (void)input;
    (void)input_size;
    output.assign(static_cast<const uint8_t*>(input), 
                  static_cast<const uint8_t*>(input) + input_size);
    bytes_in_ += input_size;
    bytes_out_ += input_size;
    return input_size;
#endif
}

void ModeZCompressor::reset() {
#ifdef HAVE_ZLIB
    if (impl_->initialized) {
        deflateReset(&impl_->stream);
    }
#endif
    bytes_in_ = 0;
    bytes_out_ = 0;
}

double ModeZCompressor::getCompressionRatio() const {
    if (bytes_in_ == 0) {
        return 1.0;
    }
    return static_cast<double>(bytes_out_) / static_cast<double>(bytes_in_);
}

} // namespace transfer
} // namespace ftpclient
