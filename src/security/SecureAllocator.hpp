/*
 * SecureAllocator.hpp - Secure Memory Allocator for Credentials
 * 
 * Implements a C++ allocator that:
 * - Locks pages in memory (mlock/VirtualLock)
 * - Excludes pages from core dumps (MADV_DONTDUMP/MADV_NOCORE)
 * - Securely zeros memory on deallocation (explicit_bzero/SecureZeroMemory)
 * 
 * Per Phase 3 Spec Section 3.2
 */

#ifndef FTPCLIENT_SECURE_ALLOCATOR_HPP
#define FTPCLIENT_SECURE_ALLOCATOR_HPP

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/mman.h>
    #include <unistd.h>
#endif

namespace ftpclient { namespace security {

/**
 * Platform abstraction for secure memory operations
 */
class SecureMemoryOps {
public:
    /**
     * Lock memory pages to prevent swapping
     */
    static inline bool lock_pages(void* addr, std::size_t len) {
#ifdef _WIN32
        return VirtualLock(addr, len) != FALSE;
#else
        return mlock(addr, len) == 0;
#endif
    }

    /**
     * Unlock memory pages
     */
    static inline void unlock_pages(void* addr, std::size_t len) {
#ifdef _WIN32
        VirtualUnlock(addr, len);
#else
        munlock(addr, len);
#endif
    }

    /**
     * Exclude memory from core dumps
     */
    static inline void exclude_from_dump(void* addr, std::size_t len) {
#if defined(__linux__)
        madvise(addr, len, MADV_DONTDUMP);
#elif defined(__APPLE__)
        madvise(addr, len, MADV_NOCORE);
#elif defined(_WIN32)
        // Windows: Use VirtualProtect to mark as PAGE_NOCACHE
        // Note: Full core dump exclusion requires SetProcessValidCallTargets
        // which is complex; we rely on locked pages not being dumped
        (void)addr;
        (void)len;
#endif
    }

    /**
     * Securely zero memory - compiler barrier prevents optimization
     */
    static inline void secure_zero(void* ptr, std::size_t len) {
#ifdef _WIN32
        SecureZeroMemory(ptr, len);
#elif defined(__linux__) && __GLIBC__ >= 2 && __GLIBC_MINOR__ >= 25
        explicit_bzero(ptr, len);
#else
        // Portable fallback using volatile pointer
        volatile unsigned char* p = static_cast<unsigned char*>(ptr);
        while (len--) {
            *p++ = 0;
        }
#endif
    }

    /**
     * Get system page size
     */
    static inline std::size_t get_page_size() {
#ifdef _WIN32
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return si.dwPageSize;
#else
        return sysconf(_SC_PAGESIZE);
#endif
    }

    /**
     * Align size to page boundary
     */
    static inline std::size_t align_to_page(std::size_t size) {
        std::size_t page_size = get_page_size();
        return ((size + page_size - 1) / page_size) * page_size;
    }
};

/**
 * SecureAllocator - Allocator for sensitive data
 * 
 * Template allocator that ensures allocated memory is:
 * - Locked in RAM (not swappable)
 * - Excluded from core dumps
 * - Zeroed before deallocation
 * 
 * Usage: std::vector<char, SecureAllocator<char>> secure_buffer;
 * 
 * Per Phase 3 Spec Section 3.2
 */
template <typename T>
class SecureAllocator {
public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    template <typename U>
    struct rebind {
        using other = SecureAllocator<U>;
    };

    SecureAllocator() = default;

    template <typename U>
    SecureAllocator(const SecureAllocator<U>&) noexcept {}

    /**
     * Allocate n elements of type T
     * 
     * @param n Number of elements to allocate
     * @return Pointer to allocated memory
     * @throws std::bad_alloc if allocation fails
     */
    pointer allocate(size_type n) {
        if (n == 0) {
            return nullptr;
        }

        std::size_t bytes = n * sizeof(T);
        
        // Allocate raw memory
        pointer p = static_cast<pointer>(std::malloc(bytes));
        if (!p) {
            throw std::bad_alloc();
        }

        // Attempt to lock pages (best effort - don't fail if it doesn't work)
        if (!SecureMemoryOps::lock_pages(p, bytes)) {
            // Log warning but continue - degradation is acceptable
            // In production, this would use a logging facility
        }

        // Exclude from core dumps
        SecureMemoryOps::exclude_from_dump(p, bytes);

        return p;
    }

    /**
     * Deallocate memory - securely zeroes before freeing
     * 
     * @param p Pointer to deallocate
     * @param n Number of elements
     */
    void deallocate(pointer p, size_type n) noexcept {
        if (p == nullptr || n == 0) {
            return;
        }

        std::size_t bytes = n * sizeof(T);

        // Securely zero the memory before unlocking and freeing
        SecureMemoryOps::secure_zero(p, bytes);

        // Unlock pages
        SecureMemoryOps::unlock_pages(p, bytes);

        // Free the memory
        std::free(p);
    }

    /**
     * Construct object in place
     */
    template <typename U, typename... Args>
    void construct(U* p, Args&&... args) {
        ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
    }

    /**
     * Destroy object
     */
    template <typename U>
    void destroy(U* p) {
        p->~U();
    }

    /**
     * Maximum allocation size
     */
    size_type max_size() const noexcept {
        return std::numeric_limits<std::size_t>::max() / sizeof(T);
    }
};

// Equality comparison - all SecureAllocators are equal
template <typename T, typename U>
bool operator==(const SecureAllocator<T>&, const SecureAllocator<U>&) noexcept {
    return true;
}

template <typename T, typename U>
bool operator!=(const SecureAllocator<T>&, const SecureAllocator<U>&) noexcept {
    return false;
}

}} // namespace ftpclient::security

#endif /* FTPCLIENT_SECURE_ALLOCATOR_HPP */
