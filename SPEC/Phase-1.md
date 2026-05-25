# Phase 1 Specification: Foundation & C ABI Contract

**Document Version:** 1.0-DRAFT  
**Status:** Specification Ready for Review  
**Scope:** Immutable binary interface boundary, build artifacts, and state handle architecture. No protocol logic, no transfer implementation.

---

## 1. Document Purpose

This specification defines the **unchangeable binary contract** between the C++17 implementation core and all foreign-language consumers (primarily Python via `cffi`/`ctypes`). Once ratified, this ABI remains frozen across all future versions. Internal C++ refactoring, algorithm changes, or data structure modifications must not alter this boundary.

**Constraint:** No C++ type, exception, or name-mangled symbol shall cross this boundary. The shared library must be loadable by a plain C program without C++ runtime dependencies.

---

## 2. ABI Stability Charter

### 2.1 Calling Convention
- **Linux/macOS:** System V AMD64 ABI (`cdecl`). All functions use default C calling convention.
- **Windows x64:** Microsoft x64 calling convention. No `__stdcall` on x64 (obsolete); use standard x64 convention.
- **ARM64 (Linux/macOS):** AAPCS64 standard calling convention.
- **Return values:** Scalar types only (`int32_t`, `uint64_t`, `void*`). No struct returns by value across the ABI.

### 2.2 Symbol Visibility
- **Global default:** `hidden` (GCC/Clang) / `dllexport` controlled explicitly (MSVC).
- **Exported symbols:** Only functions listed in Section 6 are exported. All internal C++ symbols must be stripped from dynamic symbol table.
- **Name mangling prohibition:** All exported functions wrapped in `extern "C"` unconditionally.

### 2.3 Binary Layout Stability
- All public C structs use fixed-width integer types from `<stdint.h>` (`stdint.h` / `cstdint`).
- No `bool` (implementation-defined size). Use `int32_t` with `0 = false`, `1 = true`.
- No `enum` in public headers (C++11 `enum class` and C `enum` have incompatible sizing rules). Use `#define` constants with `int32_t` fields, or documented integer constants.
- Struct packing: Default compiler alignment. No `#pragma pack` unless explicitly justified and documented.
- Pointer size: All pointer fields must be `void*` or `char*` (for UTF-8 strings), never function pointers exposed directly to maintain vtable flexibility.

---

## 3. Opaque Handle Architecture

### 3.1 Handle Definition
```c
/* Opaque forward declaration. Implementation is a C++ object behind void*. */
typedef struct ftp_client_internal ftp_client_t;
```

**Rules:**
- `ftp_client_t*` is the sole object reference crossing the ABI. It is an opaque pointer to an internal C++ class (e.g., `class FtpClientImpl`).
- Python/`cffi` sees this as an opaque pointer type. No field access permitted.
- Null pointer (`NULL`) is never a valid handle except as return value indicating fatal allocation failure.

### 3.2 Handle Lifecycle State Machine
| State | Valid Operations | Transition Trigger |
|-------|-----------------|-------------------|
| `UNINITIALIZED` | None | — |
| `ALLOCATED` | `ftp_set_option_*`, `ftp_connect` | `ftp_client_create()` returns |
| `CONNECTED` | `ftp_upload_dir`, `ftp_disconnect` | Successful `ftp_connect()` |
| `DISCONNECTED` | `ftp_destroy` only | `ftp_disconnect()` or connection loss |
| `DESTROYED` | None (UB to use) | `ftp_client_destroy()` returns |

**Critical Rule:** Double-destroy is undefined behavior. The implementation may set internal pointer to NULL after destroy, but callers must not rely on this. Python binding must implement context manager `__del__` safety with a `closed` boolean flag.

---

## 4. Type System & Data Structures

### 4.1 Credential Structure
```c
typedef struct {
    const char* host;           /* UTF-8, null-terminated. IP or hostname. */
    uint16_t    port;           /* Network byte order or host byte order? SPEC DECISION: Host byte order. */
    const char* username;       /* UTF-8, null-terminated. Nullable (NULL = anonymous). */
    const char* password;       /* UTF-8, null-terminated. Nullable. */
    int32_t     use_tls;        /* 0 = Plain FTP, 1 = FTPS Explicit, 2 = FTPS Implicit */
    int32_t     verify_cert;    /* 0 = No verify, 1 = Verify peer (default), 2 = Verify + hostname match */
    const char* ca_bundle_path; /* UTF-8, null-terminated. Nullable. Path to PEM bundle. */
} ftp_credentials_t;
```

**Ownership Semantics:**
- All `const char*` fields are **borrowed references** during the call to `ftp_connect()`. The implementation must deep-copy into secure internal storage before returning.
- The caller may free/alter credential strings immediately after `ftp_connect()` returns.

### 4.2 Upload Options Structure
```c
typedef struct {
    int32_t     max_parallel;       /* Max concurrent file uploads. 0 = library default (4). */
    int32_t     retry_attempts;     /* Per-file retry count. 0 = library default (3). */
    uint64_t    retry_base_delay_ms; /* Exponential backoff base. 0 = default (1000ms). */
    int32_t     resume_enabled;     /* 0 = overwrite, 1 = resume partial */
    int32_t     create_remote_dirs; /* 0 = fail if missing, 1 = auto-create */
    const char* remote_chmod;       /* Nullable. e.g., "0644". Applied after upload. */
} ftp_upload_options_t;
```

### 4.3 Result Structure (for future directory upload completion)
```c
typedef struct {
    int32_t     status;             /* Overall operation status code */
    uint64_t    files_total;
    uint64_t    files_success;
    uint64_t    files_failed;
    uint64_t    bytes_transferred;
    /* Per-file details omitted from Phase 1 spec; will be added in Phase 4 via extensible callback table */
} ftp_result_t;
```

### 4.4 Progress Callback Type (Function Pointer)
```c
typedef void (*ftp_progress_cb_t)(
    const char* local_path,     /* UTF-8, borrowed string valid only during callback invocation */
    const char* remote_path,      /* UTF-8, borrowed */
    uint64_t    bytes_current,
    uint64_t    bytes_total,
    double      bytes_per_second,
    void*       user_data         /* Opaque pointer passed through from caller */
);
```

**Thread Safety Note:** This callback will be invoked from internal C++ worker threads. The Python binding must acquire the GIL before calling Python callables. The C++ implementation must document that callback duration should be minimal (under 1ms) to avoid stalling the transfer pipeline.

---

## 5. Error Code Taxonomy (Hierarchical `int32_t`)

All functions return `int32_t` status codes. Zero is success. Negative codes are errors. Positive codes are warnings (non-fatal).

```c
#define FTP_OK                          0

/* System / Resource Errors (-1xx) */
#define FTP_ERR_NOMEM                   -101   /* malloc/mmap failure */
#define FTP_ERR_SYSTEM                  -102   /* OS-level failure (errno/GetLastError) */

/* Argument / Precondition Errors (-2xx) */
#define FTP_ERR_INVALID_HANDLE          -201   /* NULL or stale handle */
#define FTP_ERR_INVALID_ARGUMENT        -202   /* Malformed parameter (e.g., port 0) */
#define FTP_ERR_INVALID_STATE           -203   /* Operation in wrong lifecycle state */

/* Authentication Errors (-3xx) */
#define FTP_ERR_AUTH_FAILED             -301   /* USER/PASS rejected */
#define FTP_ERR_AUTH_TLS_REQUIRED       -302   /* Server mandates TLS, plain requested */
#define FTP_ERR_CERT_VERIFY             -303   /* TLS certificate validation failure */

/* Network / Transport Errors (-4xx) */
#define FTP_ERR_CONNECT                 -401   /* TCP connection establishment failure */
#define FTP_ERR_TIMEOUT                 -402   /* Operation exceeded deadline */
#define FTP_ERR_NETWORK_RESET           -403   /* TCP RST or unexpected close */
#define FTP_ERR_DNS                     -404   /* Hostname resolution failure */

/* Protocol / Server Errors (-5xx) */
#define FTP_ERR_PROTOCOL                -501   /* Server response violates RFC */
#define FTP_ERR_SERVER_DENIED           -502   /* Server returned 4xx/5xx response */
#define FTP_ERR_PASSIVE_FAILED          -503   /* PASV/EPSV negotiation failure */

/* Transfer / I/O Errors (-6xx) */
#define FTP_ERR_LOCAL_IO                -601   /* Local file read failure */
#define FTP_ERR_REMOTE_IO               -602   /* Server aborted transfer */
#define FTP_ERR_PARTIAL                 -603   /* Completed but verification/hash mismatch */

/* Warnings (+1xx) */
#define FTP_WARN_PARTIAL_RETRY          101    /* Succeeded after N retries */
#define FTP_WARN_SKIPPED                102    /* File skipped (already exists, no overwrite) */
```

**Error Propagation Rule:** If an internal C++ exception is thrown, it must be caught at the ABI boundary and mapped to the most specific code above. The implementation may log the original exception `what()` internally, but this string must **not** cross the ABI (avoids string lifetime complexity). A future Phase 7 spec may add a `ftp_get_last_error_string()` function.

---

## 6. Exported Function Surface API

### 6.1 Lifecycle Functions
```c
/* Allocates internal state. Does NOT connect. */
int32_t ftp_client_create(ftp_client_t** out_handle);

/* Releases all resources. Idempotent only if called once. */
int32_t ftp_client_destroy(ftp_client_t* handle);
```

### 6.2 Configuration Functions (Pre-Connect)
```c
/* Set internal buffer size for data channels. 0 = default (256KB). */
int32_t ftp_set_buffer_size(ftp_client_t* handle, uint64_t size_bytes);

/* Set connect timeout in milliseconds. 0 = default (5000ms). */
int32_t ftp_set_timeout_connect_ms(ftp_client_t* handle, uint32_t ms);

/* Set command/response timeout. 0 = default (30000ms). */
int32_t ftp_set_timeout_command_ms(ftp_client_t* handle, uint32_t ms);
```

### 6.3 Connection Management
```c
/* Establish control connection and authenticate. */
int32_t ftp_connect(ftp_client_t* handle, const ftp_credentials_t* creds);
int32_t ftp_disconnect(ftp_client_t* handle);

/* Connection health check. Returns FTP_OK if idle and responsive. */
int32_t ftp_ping(ftp_client_t* handle);
```

### 6.4 Transfer Operations (Stubs for Phase 1 — signatures only)
```c
/* Upload directory tree. Actual implementation deferred to Phase 4. */
int32_t ftp_upload_dir(
    ftp_client_t*           handle,
    const char*             local_path,     /* UTF-8, null-terminated */
    const char*             remote_path,    /* UTF-8, null-terminated */
    const ftp_upload_options_t* options,    /* Nullable (uses defaults if NULL) */
    ftp_progress_cb_t       progress_cb,    /* Nullable */
    void*                   user_data,      /* Passed to progress_cb */
    ftp_result_t*           out_result      /* Nullable. Caller-allocated. */
);
```

### 6.5 Version / Capability Introspection
```c
/* Returns library version as packed integer: 0xMMmmpp00 (Major, Minor, Patch). */
uint32_t ftp_get_version(void);

/* Fills capability flags. Allows caller to detect optional features compiled in. */
int32_t ftp_get_capabilities(uint64_t* out_caps);
```

**Capability Flags (bitmask):**
- `0x0001` — TLS/FTPS support compiled in
- `0x0002` — `sendfile()` zero-copy available (Linux)
- `0x0004` — Compression (MODE Z) support
- `0x0008` — IPv6 support
- `0x0010` — Resume/REST support

---

## 7. Memory Ownership Rules

| Parameter Direction | Allocation Owner | Deallocation Owner | Rule |
|---------------------|------------------|-------------------|------|
| `out_handle` from `ftp_client_create` | Implementation | Caller via `ftp_client_destroy` | Must be NULL on input |
| `ftp_credentials_t` fields | Caller | Caller | Deep-copied by implementation inside `ftp_connect` |
| `local_path`, `remote_path` in `ftp_upload_dir` | Caller | Caller | Borrowed for duration of call only |
| `out_result` struct | Caller | Caller | Caller allocates stack/heap struct; implementation fills fields |
| Strings inside `out_result` (future phases) | Implementation | Caller via `ftp_result_free()` | Phase 4 will add `ftp_result_free()` |

**Critical:** The C++ implementation must never call `free()` on a pointer allocated by Python (which may use `pymalloc`). Conversely, Python must never call `free()` on a pointer allocated by C++ (which may use `jemalloc` or custom allocator). All cross-boundary transfers use value semantics or handle-mediated copying.

---

## 8. Threading & Concurrency Model at Boundary

- **Handle thread-safety:** `ftp_client_t*` handles are **NOT thread-safe** by default. Concurrent calls on the same handle from multiple Python threads result in undefined behavior.
- **Exception:** `ftp_client_destroy()` must be safe to call if all other threads have already returned from API calls (i.e., no concurrent operations in flight).
- **Internal concurrency:** The C++ implementation may use internal thread pools, but this is invisible at the ABI boundary. The progress callback (Section 4.4) is the only re-entrant point.
- **GIL Policy:** The C++ library knows nothing about Python GIL. It is the Python binding's responsibility to release the GIL before calling long-running C functions and re-acquire it in the progress callback.

---

## 9. Build System Specification

### 9.1 Supported Targets
| Platform | Minimum Toolchain | Architecture | Artifact |
|----------|------------------|--------------|----------|
| Linux | GCC 11 / Clang 14 | x86_64, aarch64 | `libftpclient.so` |
| Windows | MSVC 2019 (v142) | x64 | `ftpclient.dll` + `ftpclient.lib` import lib |
| macOS | Xcode 14 / Clang 14 | x86_64, arm64 (universal binary) | `libftpclient.dylib` |

### 9.2 CMake Requirements
- Minimum CMake 3.20.
- Preset files (`CMakePresets.json`) for Release, Debug, and RelWithDebInfo.
- Position Independent Code (`-fPIC`) enabled unconditionally for shared library.
- C++17 standard required. C++20 permitted if available, but no C++20 features in public headers.

### 9.3 Symbol Control
- **GCC/Clang:** Compile with `-fvisibility=hidden`. Explicit `__attribute__((visibility("default")))` on exported functions.
- **MSVC:** Module definition file (`.def`) listing exports, OR `__declspec(dllexport)` on exported functions. Prefer `.def` file to avoid C++ attribute pollution in headers.
- **Post-build verification:** `nm -D libftpclient.so | grep -E ' T '` must list **only** symbols from Section 6.

### 9.4 Dependencies
- **Phase 1 allowed dependencies:** None. The shared library must link only to system C/C++ runtime and system networking libraries (`libc`, `libpthread`, `ws2_32.lib` on Windows).
- **Future dependencies** (libcurl, OpenSSL) will be statically linked or privately bundled to avoid DLL hell on the Python side. This will be specified in Phase 2.

---

## 10. Versioning & Forward Compatibility

### 10.1 API Versioning
- `ftp_get_version()` returns compile-time version.
- The `ftp_client_create()` function accepts an optional version parameter in a future extension via a `ftp_client_create2()` function, preserving the original symbol indefinitely.

### 10.2 Extensibility Mechanism (Future-Proofing)
A reserved extension point is mandated:
```c
/* Reserved for future options without breaking ABI. */
typedef struct {
    uint32_t    struct_size;    /* sizeof(this struct) at compile time */
    uint32_t    flags;
    void*       reserved1;
    void*       reserved2;
} ftp_context_ext_t;
```

All option structs (`ftp_upload_options_t`, etc.) must include a `struct_size` field as the first member. This allows the C++ implementation to detect the struct version from the caller and handle older/newer callers gracefully.

---

## 11. String Encoding & Locale

- **Mandatory encoding:** UTF-8 for all `char*` crossing the ABI.
- **Path handling:** File paths are passed as byte sequences (UTF-8). The C++ implementation converts to native filesystem encoding internally (e.g., UTF-16 on Windows via `MultiByteToWideChar`).
- **Locale independence:** The library must call `setlocale(LC_ALL, "C")` internally or use locale-independent string functions. No dependency on global C locale.

---

## 12. Quality Gates & Acceptance Criteria

### 12.1 ABI Compatibility Test
**Test Name:** `abi_stability_dummy`  
**Procedure:** Compile a minimal C++ shared library implementing exactly the Phase 1 ABI. Write a Python script using `ctypes` that loads the library, creates a handle, calls every function, and destroys the handle.  
**Pass Criteria:** Script runs successfully on Linux, Windows, and macOS without modification.

### 12.2 Symbol Leakage Test
**Test Name:** `symbol_visibility`  
**Procedure:** Build Release artifact. Run `nm -D` (Linux), `dumpbin /EXPORTS` (Windows), or `nm -gU` (macOS).  
**Pass Criteria:** Zero exported symbols except those prefixed with `ftp_` and `FTP_` constants. No C++ `std::` symbols visible.

### 12.3 Handle Lifecycle Stress Test
**Test Name:** `handle_lifecycle`  
**Procedure:** C program loops 100,000 times: `create` → `destroy`. Measure memory via valgrind (`massif`) or Windows UMDH.  
**Pass Criteria:** No memory growth. No handle leaks. No double-free crashes.

### 12.4 Cross-Platform Load Test
**Test Name:** `cross_platform_load`  
**Procedure:** Build artifacts on CI (GitHub Actions or equivalent) for all three platforms. Load from plain C and from Python `cffi`.  
**Pass Criteria:** All loads succeed. `ftp_get_version()` returns expected value.

### 12.5 C Header Purity Test
**Test Name:** `header_c_purity`  
**Procedure:** Compile the public header (`ftpclient.h`) with `gcc -x c -std=c99 -pedantic -Wall -Werror` (forcing C mode, not C++).  
**Pass Criteria:** Zero errors, zero warnings. This proves the header is consumable by pure C and Python `cffi` without C++ compiler.

---

## 13. Deliverables for Phase 1

1. **`ftpclient.h`** — The single public C header file containing all types, constants, and function declarations from this spec.
2. **`CMakeLists.txt`** — Build configuration producing the shared library artifact with correct symbol visibility.
3. **`abi_test.c`** — Plain C test harness verifying all exported functions can be called and return expected codes.
4. **`abi_test.py`** — Python `cffi` test script loading the library and exercising the handle lifecycle.
5. **`ABI_CONTRACT.md`** — Human-readable summary of the ABI guarantees for binding authors.

---

## 14. Open Decisions Requiring Stakeholder Input

| Decision | Options | Recommendation |
|----------|---------|----------------|
| **Struct size field endianness** | Host vs network | Host. These are in-process structs, not wire protocol. |
| **TLS default in credentials** | Plain vs Explicit FTPS | Explicit FTPS (port 21 + AUTH TLS) as default for security. Plain FTP requires explicit `use_tls=0`. |
| **Progress callback frequency** | Fixed interval vs per-buffer | Fixed interval (10Hz max) with internal throttling to prevent GIL storms. |
| **Result struct ownership** | Caller-allocates vs impl-allocates | Caller-allocates on stack for Phase 1. Simpler memory semantics. |

---

## 15. Transition Criteria to Phase 2

Phase 1 is considered **complete and frozen** when:
1. All deliverables are committed and reviewed.
2. All five Quality Gates (Section 12) pass on CI for all three target platforms.
3. The Python `cffi` test successfully creates and destroys 10,000 handles with zero memory growth.
4. The header compiles in pure C99 mode with `-Werror`.

**Upon ratification:** The `ftpclient.h` header receives a version stamp (`#define FTP_ABI_VERSION 1`) and enters **lockdown**. No modifications to existing function signatures or struct layouts permitted. Future additions must use new function names or the `ftp_context_ext_t` extension mechanism.

---

**End of Phase 1 Specification**
