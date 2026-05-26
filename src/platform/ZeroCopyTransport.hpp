/*
 * ZeroCopyTransport.hpp - Platform Zero-Copy I/O
 * 
 * Per Phase 7 Spec Section 3: Zero-Copy I/O Subsystem
 * 
 * Provides platform-specific zero-copy sendfile implementations:
 * - Linux: sendfile() syscall
 * - Windows: TransmitFile
 * - macOS: mmap + write fallback
 */

#ifndef FTPCLIENT_PLATFORM_ZEROCOPYTRANSPORT_HPP
#define FTPCLIENT_PLATFORM_ZEROCOPYTRANSPORT_HPP

#include <cstdint>
#include <string>

namespace ftpclient {
namespace platform {

/**
 * Zero-copy transport capabilities
 */
enum class ZeroCopyCapability {
    NONE = 0,           /* No zero-copy available */
    SENDFILE = 1,       /* Linux sendfile() */
    TRANSMITFILE = 2,   /* Windows TransmitFile */
    MMAP = 3            /* macOS mmap fallback */
};

/**
 * Query zero-copy capability for current platform
 */
ZeroCopyCapability queryZeroCopyCapability();

/**
 * Check if kernel/version supports zero-copy
 */
bool isZeroCopyAvailable();

/**
 * Send file using zero-copy where available
 * 
 * @param socket_fd Destination socket file descriptor
 * @param file_fd Source file file descriptor  
 * @param offset Offset in file to start from
 * @param count Number of bytes to send
 * @return Bytes sent on success, negative error code on failure,
 *         -ENOSYS if zero-copy not available (caller should fallback)
 */
int32_t sendFileZeroCopy(int socket_fd, int file_fd, uint64_t offset, uint64_t count);

/**
 * Get human-readable name of zero-copy method used
 */
const char* getZeroCopyMethodName();

} // namespace platform
} // namespace ftpclient

#endif /* FTPCLIENT_PLATFORM_ZEROCOPYTRANSPORT_HPP */
