/*
 * ZeroCopyTransport.cpp - Platform Zero-Copy I/O Implementation
 * 
 * Per Phase 7 Spec Section 3: Zero-Copy I/O Subsystem
 */

#include "ZeroCopyTransport.hpp"
#include <cerrno>

#if defined(__linux__)
#include <sys/sendfile.h>
#include <unistd.h>
#include <sys/stat.h>

namespace ftpclient {
namespace platform {

ZeroCopyCapability queryZeroCopyCapability() {
    return ZeroCopyCapability::SENDFILE;
}

bool isZeroCopyAvailable() {
    /* Linux sendfile available on all modern kernels */
    return true;
}

int32_t sendFileZeroCopy(int socket_fd, int file_fd, uint64_t offset, uint64_t count) {
    off_t off = static_cast<off_t>(offset);
    
    ssize_t sent = ::sendfile(socket_fd, file_fd, &off, static_cast<size_t>(count));
    
    if (sent < 0) {
        if (errno == EINVAL || errno == ENOSYS || errno == ENOTSOCK) {
            /* Kernel or filesystem doesn't support sendfile for this combo */
            return -ENOSYS;
        }
        return -errno;
    }
    
    return static_cast<int32_t>(sent);
}

const char* getZeroCopyMethodName() {
    return "sendfile";
}

} // namespace platform
} // namespace ftpclient

#elif defined(_WIN32)
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>

namespace ftpclient {
namespace platform {

ZeroCopyCapability queryZeroCopyCapability() {
    return ZeroCopyCapability::TRANSMITFILE;
}

bool isZeroCopyAvailable() {
    /* TransmitFile available on all Windows versions */
    return true;
}

int32_t sendFileZeroCopy(int socket_fd, int file_fd, uint64_t offset, uint64_t count) {
    /* Get file handle from fd */
    HANDLE hFile = reinterpret_cast<HANDLE>(_get_osfhandle(file_fd));
    if (hFile == INVALID_HANDLE_VALUE) {
        return -EBADF;
    }
    
    /* Seek to offset */
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(offset);
    li.LowPart = SetFilePointer(hFile, li.LowPart, &li.HighPart, FILE_BEGIN);
    if (li.LowPart == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) {
        return -EIO;
    }
    
    /* Use TransmitFile */
    BOOL ok = TransmitFile(
        static_cast<SOCKET>(socket_fd),
        hFile,
        static_cast<DWORD>(count),
        0,
        NULL,
        NULL,
        TF_USE_KERNEL_APC
    );
    
    if (!ok) {
        DWORD err = GetLastError();
        if (err == WSAEINVAL || err == ERROR_INVALID_FUNCTION) {
            return -ENOSYS;
        }
        return -static_cast<int32_t>(err);
    }
    
    return static_cast<int32_t>(count);
}

const char* getZeroCopyMethodName() {
    return "TransmitFile";
}

} // namespace platform
} // namespace ftpclient

#elif defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>

namespace ftpclient {
namespace platform {

ZeroCopyCapability queryZeroCopyCapability() {
    return ZeroCopyCapability::MMAP;
}

bool isZeroCopyAvailable() {
    /* mmap available on macOS */
    return true;
}

int32_t sendFileZeroCopy(int socket_fd, int file_fd, uint64_t offset, uint64_t count) {
    /* macOS lacks sendfile for socket-to-socket, use mmap + write */
    void* mapped = mmap(nullptr, static_cast<size_t>(count), PROT_READ, 
                        MAP_PRIVATE, file_fd, static_cast<off_t>(offset));
    
    if (mapped == MAP_FAILED) {
        return -errno;
    }
    
    ssize_t written = ::write(socket_fd, mapped, static_cast<size_t>(count));
    munmap(mapped, static_cast<size_t>(count));
    
    if (written < 0) {
        return -errno;
    }
    
    return static_cast<int32_t>(written);
}

const char* getZeroCopyMethodName() {
    return "mmap+write";
}

} // namespace platform
} // namespace ftpclient

#else
/* Fallback for unsupported platforms */

namespace ftpclient {
namespace platform {

ZeroCopyCapability queryZeroCopyCapability() {
    return ZeroCopyCapability::NONE;
}

bool isZeroCopyAvailable() {
    return false;
}

int32_t sendFileZeroCopy(int socket_fd, int file_fd, uint64_t offset, uint64_t count) {
    (void)socket_fd;
    (void)file_fd;
    (void)offset;
    (void)count;
    return -ENOSYS;
}

const char* getZeroCopyMethodName() {
    return "none";
}

} // namespace platform
} // namespace ftpclient

#endif
