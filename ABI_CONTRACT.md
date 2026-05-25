# ABI_CONTRACT.md - FTP Client Library C ABI Contract

## Overview

This document summarizes the ABI (Application Binary Interface) guarantees for the FTP Client Library. This contract ensures binary compatibility between the C++17 implementation and foreign-language consumers (primarily Python via `cffi`/`ctypes`).

## ABI Version: 1.0

**Status:** FROZEN - No modifications to existing function signatures or struct layouts permitted.

## Core Guarantees

### 1. Calling Convention

| Platform | Convention |
|----------|------------|
| Linux x86_64 | System V AMD64 ABI (cdecl) |
| macOS x86_64/arm64 | System V / AAPCS64 |
| Windows x64 | Microsoft x64 calling convention |

### 2. Symbol Visibility

- All exported functions are wrapped in `extern "C"` to prevent name mangling
- Default symbol visibility is `hidden`
- Only functions prefixed with `ftp_` are exported
- No C++ `std::` symbols are visible in the dynamic symbol table

### 3. Type Stability

- All public structs use fixed-width integer types (`int32_t`, `uint64_t`, etc.)
- No `bool` type (uses `int32_t` with 0=false, 1=true)
- No `enum` in public headers (uses `#define` constants)
- Pointer fields are `void*` or `char*` only

### 4. Handle Lifecycle

```
UNINITIALIZED → ALLOCATED → CONNECTED → DISCONNECTED → DESTROYED
                         ↘_______________↗
```

**Rules:**
- `ftp_client_t*` is an opaque pointer to internal C++ state
- Null pointer is never valid except as return value for allocation failure
- Double-destroy is undefined behavior (returns error)
- Handles are NOT thread-safe

### 5. Memory Ownership

| Parameter | Allocation Owner | Deallocation Owner |
|-----------|------------------|-------------------|
| `out_handle` from `ftp_client_create` | Implementation | Caller via `ftp_client_destroy` |
| `ftp_credentials_t` fields | Caller | Caller (deep-copied internally) |
| `local_path`, `remote_path` | Caller | Caller (borrowed) |
| `out_result` struct | Caller | Caller |

**Critical:** Cross-boundary memory transfers use value semantics or handle-mediated copying. C++ never calls `free()` on Python-allocated memory and vice versa.

### 6. String Encoding

- **Mandatory:** UTF-8 for all `char*` crossing the ABI
- File paths converted to native encoding internally
- Locale-independent string operations (no dependency on global C locale)

### 7. Error Handling

All functions return `int32_t`:
- `0` = Success (`FTP_OK`)
- Negative = Error codes (see below)
- Positive = Warnings

#### Error Code Taxonomy

| Range | Category | Examples |
|-------|----------|----------|
| `-1xx` | System/Resource | `FTP_ERR_NOMEM`, `FTP_ERR_SYSTEM` |
| `-2xx` | Argument/Precondition | `FTP_ERR_INVALID_HANDLE`, `FTP_ERR_INVALID_ARGUMENT` |
| `-3xx` | Authentication | `FTP_ERR_AUTH_FAILED`, `FTP_ERR_CERT_VERIFY` |
| `-4xx` | Network/Transport | `FTP_ERR_CONNECT`, `FTP_ERR_TIMEOUT` |
| `-5xx` | Protocol/Server | `FTP_ERR_PROTOCOL`, `FTP_ERR_SERVER_DENIED` |
| `-6xx` | Transfer/I/O | `FTP_ERR_LOCAL_IO`, `FTP_ERR_PARTIAL` |
| `+1xx` | Warnings | `FTP_WARN_PARTIAL_RETRY`, `FTP_WARN_SKIPPED` |

### 8. Threading Model

- **Handles are NOT thread-safe:** Concurrent calls on same handle from multiple threads = undefined behavior
- **Exception:** `ftp_client_destroy()` is safe if no other operations in flight
- Progress callbacks may be invoked from internal worker threads
- GIL management is the Python binding's responsibility

## Exported Function Reference

### Lifecycle Functions

```c
int32_t ftp_client_create(ftp_client_t** out_handle);
int32_t ftp_client_destroy(ftp_client_t* handle);
```

### Configuration Functions

```c
int32_t ftp_set_buffer_size(ftp_client_t* handle, uint64_t size_bytes);
int32_t ftp_set_timeout_connect_ms(ftp_client_t* handle, uint32_t ms);
int32_t ftp_set_timeout_command_ms(ftp_client_t* handle, uint32_t ms);
```

### Connection Management

```c
int32_t ftp_connect(ftp_client_t* handle, const ftp_credentials_t* creds);
int32_t ftp_disconnect(ftp_client_t* handle);
int32_t ftp_ping(ftp_client_t* handle);
```

### Transfer Operations

```c
int32_t ftp_upload_dir(
    ftp_client_t* handle,
    const char* local_path,
    const char* remote_path,
    const ftp_upload_options_t* options,
    ftp_progress_cb_t progress_cb,
    void* user_data,
    ftp_result_t* out_result
);
```

### Introspection

```c
uint32_t ftp_get_version(void);
int32_t ftp_get_capabilities(uint64_t* out_caps);
```

## Capability Flags

| Flag | Value | Description |
|------|-------|-------------|
| `FTP_CAP_TLS` | `0x0001` | TLS/FTPS support |
| `FTP_CAP_SENDFILE` | `0x0002` | Zero-copy sendfile available |
| `FTP_CAP_COMPRESSION` | `0x0004` | MODE Z compression |
| `FTP_CAP_IPV6` | `0x0008` | IPv6 support |
| `FTP_CAP_RESUME` | `0x0010` | Resume/REST support |

## Extensibility Mechanism

All option structs include `struct_size` as first member:

```c
typedef struct {
    uint32_t    struct_size;    /* sizeof(this struct) */
    int32_t     max_parallel;
    // ... more fields
} ftp_upload_options_t;
```

This allows the implementation to detect struct version and handle older/newer callers gracefully.

Future extensions use the reserved extension structure:

```c
typedef struct {
    uint32_t    struct_size;
    uint32_t    flags;
    void*       reserved1;
    void*       reserved2;
} ftp_context_ext_t;
```

## Quality Gates

### 1. ABI Compatibility Test
- Plain C program loads library and exercises all functions
- Must pass on Linux, Windows, macOS without modification

### 2. Symbol Visibility Test
```bash
nm -D libftpclient.so | grep -E ' T '
```
Must list only `ftp_` prefixed symbols.

### 3. Handle Lifecycle Stress Test
- Loop 100,000 times: create → destroy
- No memory growth, no handle leaks

### 4. Header Purity Test
```bash
gcc -x c -std=c99 -pedantic -Wall -Werror -c ftpclient.h
```
Zero errors, zero warnings.

### 5. Cross-Platform Load Test
- Build artifacts on CI for all platforms
- Load from plain C and Python cffi/ctypes

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0.0 | Phase 1 | Initial frozen ABI |

## Contact

For ABI-related questions or to report compatibility issues, refer to the project maintainers.

---

**This document is part of the Phase 1 deliverables. The `ftpclient.h` header is the authoritative source for the ABI specification.**
