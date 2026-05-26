# Phase 6 Specification: Python Binding & Ergonomics

**Document Version:** 6.0-DRAFT  
**Depends On:** Phase 1 (Frozen ABI), Phase 2–5 (Complete Core)  
**Scope:** `cffi`-based Python module, GIL management, exception hierarchy, type-safe API, packaging, and distribution. No C++ code changes.

---

## 1. Purpose & Scope

Phase 6 is a **pure binding and packaging layer**. It does not modify the C++ shared library or the C ABI. Its sole purpose is to expose the frozen C ABI to Python developers with idiomatic, zero-friction ergonomics while preserving the performance characteristics engineered in Phases 2–5.

**In Scope:**
- `cffi` binding layer: struct layout verification, function loading, callback trampolines
- GIL discipline: release on long C calls, acquire on C-to-Python callbacks
- Pythonic API: context managers, dataclasses, `pathlib` support, iterator-based progress
- Exception hierarchy: C error codes → typed Python exceptions
- Credential provider integration: Python callables passed through C callback mechanism
- Wheel packaging: platform-specific binary distribution (`manylinux`, `macosx`, `win_amd64`)
- Type stubs (`.pyi`) for IDE completion and static analysis

**Explicitly Out of Scope:**
- Pure-Python fallback (if the shared library fails to load, we fail fast)
- `asyncio` native integration (async/await wrapper is a thin thread-pool facade, not true async I/O)
- Upload *download* operations (deferred to Phase 7)
- CLI tool / command-line interface (separate product decision)

---

## 2. Architectural Decisions & Trade-offs

| Decision | Selected Approach | Rejected Alternative | Rationale |
|----------|------------------|---------------------|-----------|
| **Binding Technology** | `cffi` (ABI mode, out-of-line) | `ctypes`, `pybind11`, Cython, `ctypesgen` | `cffi` parses C headers directly (no manual struct layout), supports callbacks with automatic GIL handling, and produces stable wheels without requiring a C compiler at install time. `pybind11` would break Phase 1's C ABI freeze by exposing C++ symbols. |
| **GIL Release** | Explicit `libffi`/`cffi` `release()` + manual `Py_BEGIN_ALLOW_THREADS` equivalent | Trust `cffi` defaults | `cffi` does not automatically release the GIL for C calls. We must wrap every blocking C function in a `with nogil:` block (CPython C-API) or use `cffi`'s `release()` context. |
| **Callback GIL** | `cffi` callback with `lock=True` (acquires GIL before Python code runs) | Custom C trampoline that calls `PyGILState_Ensure` | `cffi` handles `PyGILState_Ensure/Ensure` internally when `lock=True`. Less error-prone than hand-written C trampoline. |
| **API Style** | Synchronous blocking with optional `concurrent.futures` wrapper | Native `asyncio` protocol implementation | True asyncio would require rewriting the C++ engine around an event loop, violating Phase 2's blocking-I/O architecture. A thread-pool wrapper gives Python users async ergonomics without core changes. |
| **String Encoding** | UTF-8 strict; `surrogateescape` for filesystem paths | Latin-1 fallback | FTP protocol is UTF-8 (RFC 2640). Filesystem paths use `os.fsencode()` / `os.fsdecode()` with `surrogateescape` to handle non-UTF-8 local filenames robustly. |
| **Packaging** | `auditwheel` / `delvewheel` / `delocate` to vendor shared library into wheel | Require user to install system package | Enterprise Python deployment expects `pip install`. Vendoring the `.so`/`.dll` ensures hermetic builds and version pinning. |
| **Type Hints** | Inline `typing` + separate `.pyi` stub files | `mypy` plugin only | Stubs allow IDEs and `mypy` to validate user code without executing the binary extension. Critical for developer experience. |

---

## 3. Binding Layer Architecture (`cffi`)

### 3.1 Build-Time vs. Run-Time
`cffi` operates in two phases:

1. **Build-time (wheel construction):** A Python script (`build_ffi.py`) reads `ftpclient.h` and generates `_ftpclient.c` + compiled extension module `_ftpclient*.so`.
2. **Run-time (user machine):** The pure-Python `ftpclient` package imports `_ftpclient` and calls its loaded functions.

**Advantage:** The end user never needs a C compiler. The wheel contains a pre-built extension that dynamically links (or embeds) the core `libftpclient.so`.

### 3.2 `ftpclient.h` Ingestion
`cffi` must consume the **exact** `ftpclient.h` from Phase 1/3/4/5 without modification. The build script:

```python
# build_ffi.py (conceptual)
from cffi import FFI
ffi = FFI()
with open("include/ftpclient.h") as f:
    ffi.cdef(f.read())  # Parse C declarations
ffi.set_source("_ftpclient", ...)
```

**Constraint:** `ftpclient.h` must remain C-parsable (no C++ features, no macros with logic). Phase 1 guarantees this via the header purity gate.

### 3.3 Shared Library Loading Strategy
The core `libftpclient.so` (Linux), `ftpclient.dll` (Windows), or `libftpclient.dylib` (macOS) must be discoverable at runtime.

| Platform | Strategy |
|----------|----------|
| Linux | RPATH `$ORIGIN` set during wheel build; `.so` placed in same package directory as `_ftpclient*.so` |
| Windows | `.dll` placed next to `_ftpclient*.pyd`; Windows DLL search path resolves it |
| macOS | `@loader_path` install_name; `.dylib` placed in same package directory |

**Packaging Layout:**
```
ftpclient/
    __init__.py          # Public API
    _core.py             # cffi loader + low-level wrappers
    types.py             # Dataclasses / enums
    exceptions.py        # Exception hierarchy
    _lib/
        libftpclient.so  # Vendored C++ shared library
        __init__.py      # Empty marker
```

---

## 4. GIL Management Specification

### 4.1 C Call → Python (Inbound to C, GIL Must Release)
Every C function that blocks for >1ms must be called with the GIL released.

| C Function | Typical Duration | GIL Action |
|------------|---------------|------------|
| `ftp_connect()` | 100ms–5s | **RELEASE** |
| `ftp_upload_dir()` | Seconds–hours | **RELEASE** |
| `ftp_disconnect()` | 10ms–1s | **RELEASE** |
| `ftp_ping()` | 50ms–500ms | **RELEASE** |
| `ftp_client_create()` | <1ms | Keep (negligible) |
| `ftp_client_destroy()` | <10ms | Keep (negligible) |
| `ftp_set_buffer_size()` | <1ms | Keep |
| `ftp_get_version()` | <1ms | Keep |

**Implementation Pattern:**
```python
# In _core.py
def connect(handle, creds):
    # cffi automatically handles this if we use ffi.new() and pass pointers
    # For GIL release, cffi supports:
    with _lib.gil_release():
        result = _lib.ftp_connect(handle, creds)
    return result
```

**Critical:** If the GIL is not released during `ftp_upload_dir()`, a single upload blocks all other Python threads in the process, defeating the purpose of the C++ thread pool.

### 4.2 C Callback → Python (Outbound from C, GIL Must Acquire)
The progress callback and credential provider callback are invoked from C++ worker threads.

**`cffi` Callback Declaration:**
```python
@ffi.callback("int32_t(ftp_credentials_t*, void*, int32_t)", lock=True)
def _credential_provider_trampoline(out_creds, user_data, attempt):
    # lock=True ensures PyGILState_Ensure is called before this body runs
    py_callable = ffi.from_handle(user_data)  # Recover Python callable
    # ... populate out_creds from py_callable ...
    return 0
```

**`lock=True` Behavior:**
- C++ thread calls the callback.
- `cffi` acquires GIL.
- Python code executes.
- GIL released.
- Return value passed back to C++.

**Performance Warning:** If the Python callback performs heavy work (e.g., keyring database lookup), it holds the GIL and stalls the C++ worker. Documentation must state: "Callbacks should complete in <10ms. Offload heavy work to a background thread if necessary."

---

## 5. Pythonic Public API

### 5.1 High-Level Class: `FTPClient`
```python
from ftpclient import FTPClient, Credentials, UploadOptions

with FTPClient() as client:
    client.connect(Credentials(
        host="ftp.example.com",
        port=21,
        username="user",
        password="pass",
        use_tls=1,
        verify_cert=2
    ))
    
    result = client.upload_directory(
        local_path="/data/backup",
        remote_path="/backups/2024",
        options=UploadOptions(max_parallel=4, retry_attempts=3),
        progress=lambda f, done, total, bps: print(f"{f}: {done}/{total} @ {bps/1024:.1f}KB/s")
    )
    
    print(f"Uploaded {result.files_success}/{result.files_total} files")
```

### 5.2 Dataclasses (Type-Safe, Immutable)

```python
@dataclass(frozen=True)
class Credentials:
    host: str
    port: int = 21
    username: Optional[str] = None
    password: Optional[str] = None
    use_tls: int = 1          # 0=plain, 1=explicit, 2=implicit
    verify_cert: int = 2      # 0=none, 1=peer, 2=host
    ca_bundle_path: Optional[Path] = None

@dataclass(frozen=True)
class UploadOptions:
    max_parallel: int = 0      # 0 = auto (hardware concurrency)
    retry_attempts: int = 3
    retry_base_delay_ms: int = 1000
    resume_enabled: bool = False
    create_remote_dirs: bool = True
    remote_chmod: Optional[str] = None

@dataclass(frozen=True)
class FileResult:
    local_path: Path
    remote_path: str
    status: int
    bytes_sent: int
    attempt_count: int
    final_error: Optional[int]

@dataclass(frozen=True)
class UploadResult:
    status: int
    files_total: int
    files_success: int
    files_failed: int
    bytes_transferred: int
    file_results: Tuple[FileResult, ...]
```

### 5.3 Exception Hierarchy
C error codes map to typed Python exceptions:

```
FTPError (base)
├── FTPAuthError          # -3xx codes
├── FTPNetworkError       # -4xx codes (transient, retryable)
├── FTPProtocolError      # -5xx codes (server misbehavior)
├── FTPIOError            # -6xx codes (local or remote I/O)
├── FTPConfigError        # -2xx codes (invalid arguments)
└── FTPSystemError        # -1xx codes (OOM, system failure)
```

**Mapping Rule:**
```python
_ERROR_MAP = {
    -301: FTPAuthError, -302: FTPAuthError, -303: FTPAuthError,
    -401: FTPNetworkError, -402: FTPNetworkError, ...
}
```

**Behavior:** `ftp_upload_dir()` returning a non-zero code does **not** raise by default. The `UploadResult` contains the code. However, `client.connect()` **does** raise on failure because a failed connection leaves the client unusable. This distinction is documented.

---

## 6. Credential Provider Integration

### 6.1 Python-Native Secret Injection
Phase 3's C callback mechanism is wrapped so Python users pass a plain callable:

```python
def get_creds(attempt: int) -> Credentials:
    return Credentials(
        host="ftp.example.com",
        username="admin",
        password=keyring.get_password("ftp", "admin")
    )

client = FTPClient()
client.set_credential_provider(get_creds)
client.connect()  # No static creds passed; provider invoked
```

### 6.2 Implementation Mechanics
1. Python callable is wrapped in a `ctypes.py_object` or `cffi` handle.
2. A C-level `ftp_credential_provider_cb_t` trampoline (defined via `cffi` callback) receives the handle as `user_data`.
3. Trampoline calls the Python function, populates the C struct, and returns.
4. The C++ `CredentialVault` deep-copies the strings (Phase 3), so the Python strings are safe to GC immediately.

**Lifetime Management:**
- The Python callable reference is held by the `FTPClient` instance.
- If the callable is a bound method or closure, the reference keeps its `self` / captured variables alive.
- On `client.disconnect()` or `client.__del__()`, the provider callback is cleared in C via `ftp_clear_credential_provider()`.

---

## 7. Progress Callback Design

### 7.1 Pythonic Signature
```python
def progress_callback(
    local_path: Path,
    remote_path: str,
    bytes_current: int,
    bytes_total: int,
    bytes_per_second: float
) -> None:
    ...
```

### 7.2 Throttling Guarantee
The C++ engine throttles to 10Hz (Phase 4). The Python binding adds a second layer: if the Python callable is slow, the C++ callback may block the worker. The binding logs a warning if callback duration exceeds 50ms.

### 7.3 Thread Safety
The callback is invoked from C++ worker threads. The Python user must ensure their callback is thread-safe (e.g., use `threading.Lock` if updating shared GUI state). The binding provides a convenience `ProgressBar` adapter using `tqdm`:

```python
from ftpclient.utils import TqdmProgressAdapter

result = client.upload_directory(
    ...,
    progress=TqdmProgressAdapter(desc="Uploading")
)
```

---

## 8. Async Wrapper (Optional Facade)

### 8.1 `asyncio` Integration
Since the core is blocking C++ threads, the async wrapper uses `concurrent.futures.ThreadPoolExecutor`:

```python
import asyncio
from ftpclient import AsyncFTPClient

async def main():
    async with AsyncFTPClient() as client:
        await client.connect(creds)
        result = await client.upload_directory(
            local_path="/data",
            remote_path="/remote"
        )
        # Internally: loop.run_in_executor(None, sync_upload)

asyncio.run(main())
```

**Design Constraint:** This is **not** true async I/O. It is a thread-pool facade. The benefit is non-blocking the Python `asyncio` event loop. The cost is one OS thread per concurrent `upload_directory()` call. This is acceptable for batch scripts and moderate concurrency.

---

## 9. Packaging & Distribution

### 9.1 Wheel Build Pipeline
| Step | Tool | Platform |
|------|------|----------|
| Build C++ core | CMake | All |
| Build cffi extension | `python setup.py build_ext` | All |
| Repair wheel (vendor shared lib) | `auditwheel repair` | Linux |
| Repair wheel | `delocate` | macOS |
| Repair wheel | `delvewheel repair` | Windows |
| Test wheel | `twine check` + `pytest` | All |

### 9.2 `manylinux` Compliance
- Build inside `quay.io/pypa/manylinux2014_x86_64` (or `_2_28`) Docker image.
- Statically link OpenSSL, libcurl (if any), and C++ standard library into `libftpclient.so` to avoid external dependencies.
- `auditwheel` bundles the `.so` and adjusts RPATH.

### 9.3 Versioning
Python package version tracks C++ core version:
- C++ core `1.0.0` → Python `1.0.0`
- ABI compatibility: Python `1.x.y` works with C++ core `1.x.*`
- The Python package embeds the core version and verifies it at import time via `ftp_get_version()`.

---

## 10. Quality Gates & Acceptance Criteria

### 10.1 `cffi` Header Parity
**Test:** `build_ffi.py` successfully parses `ftpclient.h` without errors or warnings.  
**Pass Criteria:** `cffi` `cdef()` executes cleanly. All structs, function pointers, and constants are recognized.

### 10.2 GIL Release Verification
**Test:** Python script with two threads:
- Thread A: calls `ftp_upload_dir()` with a 10s sleep injected in C++ (mock slow server).
- Thread B: runs a tight loop incrementing a counter.

**Pass Criteria:** Thread B's counter increments during Thread A's blocked call. If GIL is not released, Thread B stalls completely.

### 10.3 Callback Invocation Integrity
**Test:** Upload 100 files with a progress callback that appends to a `list`.  
**Pass Criteria:** List contains exactly 100 final-completion entries. No duplicate entries for the same file. No missing entries. ThreadSanitizer or `threading.Lock` audit confirms no race.

### 10.4 Exception Translation Accuracy
**Test:** Attempt connections producing every C error code (-101 through -603).  
**Pass Criteria:** Each maps to the correct Python exception subclass. `str(e)` contains a human-readable message. `e.error_code` attribute exposes the raw integer.

### 10.5 Wheel Portability
**Test:** Build wheel on CI (Linux x86_64, Windows x64, macOS x86_64+arm64 universal2). Install in clean virtualenv on target platform. Run `import ftpclient; client = ftpclient.FTPClient()`.  
**Pass Criteria:** Import succeeds without system dependency errors. `nm -D` / `dumpbin` audit confirms no unexpected external shared library dependencies.

### 10.6 Memory Safety from Python
**Test:** 1,000 iterations of:
```python
client = FTPClient()
client.connect(creds)
result = client.upload_dir(...)
# Explicitly delete or let reference drop
del client
gc.collect()
```
**Pass Criteria:** Valgrind/ASan on the Python process shows no leaks attributable to `_ftpclient` or `libftpclient.so`. No dangling handles.

### 10.7 Type Stub Completeness
**Test:** `mypy --strict` on a test script using every public API.  
**Pass Criteria:** Zero `mypy` errors. All function parameters, return types, and attributes are typed.

---

## 11. Deliverables

| File | Purpose |
|------|---------|
| `python/ftpclient/__init__.py` | Public API exports |
| `python/ftpclient/_core.py` | `cffi` loader, low-level function wrappers |
| `python/ftpclient/types.py` | Dataclasses (`Credentials`, `UploadOptions`, etc.) |
| `python/ftpclient/exceptions.py` | Exception hierarchy |
| `python/ftpclient/utils.py` | `TqdmProgressAdapter`, helpers |
| `python/ftpclient/_async.py` | `AsyncFTPClient` thread-pool facade |
| `python/ftpclient/py.typed` | PEP 561 marker for typed package |
| `python/ftpclient/__init__.pyi` | Stub file for IDE completion |
| `python/build_ffi.py` | `cffi` out-of-line build script |
| `python/setup.py` / `pyproject.toml` | Build configuration |
| `python/tests/test_gil_release.py` | GIL verification test |
| `python/tests/test_callback_integrity.py` | Progress callback accuracy |
| `python/tests/test_exceptions.py` | Exception mapping coverage |
| `python/tests/test_wheel.py` | Import and smoke test in clean env |
| `python/tests/test_memory.py` | Handle lifecycle + GC stress |
| `python/tests/test_types.py` | `mypy` validation script |

---

## 12. Transition Criteria to Phase 7

Phase 6 is **ratified** when:

1. All 7 Quality Gates (Section 10) pass on CI for CPython 3.8–3.12 on Linux, Windows, and macOS.
2. `pip install ftpclient` in a clean virtualenv succeeds and passes `import ftpclient; ftpclient.FTPClient()` without system library errors.
3. The GIL release test demonstrates that `ftp_upload_dir()` does not block other Python threads.
4. `mypy --strict` passes on a comprehensive test script.
5. A public test server (e.g., `test.rebex.net`) upload completes end-to-end using only the Python API (no direct C calls).
6. **No C++ source code modified** during Phase 6. Any bugs discovered in the C++ core are fixed in a patch release of Phases 1–5, then Phase 6 rebases.

**Upon ratification:** The product is a complete, installable Python package. Phase 7 (Optimization & Observability) will add telemetry, platform zero-copy, and advanced features without altering the public Python API.

---

**End of Phase 6 Specification**
