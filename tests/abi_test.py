#!/usr/bin/env python3
"""
abi_test.py - Python cffi/ctypes ABI Compatibility Test

This test verifies that the library can be loaded and used from Python.
As per spec Section 12.1 and Section 6: "Write a Python script using cffi 
that loads the library, creates a handle, calls every function, and destroys 
the handle."

This test uses ctypes (built-in) for maximum portability.
"""

import ctypes
import ctypes.util
import os
import sys
from pathlib import Path

# ============================================================================
# Library Loading
# ============================================================================

def find_library():
    """Find the shared library in build directory or system paths."""
    # Try common locations
    search_paths = [
        Path(__file__).parent.parent / "build" / "libftpclient.so",
        Path(__file__).parent.parent / "build" / "Release" / "ftpclient.dll",
        Path(__file__).parent.parent / "build" / "Debug" / "ftpclient.dll",
        "/usr/local/lib/libftpclient.so",
        "/usr/lib/libftpclient.so",
    ]
    
    for path in search_paths:
        if path.exists():
            return str(path)
    
    # Try system library lookup
    lib_name = "ftpclient"
    if sys.platform == "win32":
        lib_name = "ftpclient.dll"
    elif sys.platform == "darwin":
        lib_name = "libftpclient.dylib"
    else:
        lib_name = "libftpclient.so"
    
    found = ctypes.util.find_library(lib_name.replace("lib", "").replace(".so", "").replace(".dll", ""))
    if found:
        return found
    
    raise FileNotFoundError(f"Could not find ftpclient library. Searched: {search_paths}")


# ============================================================================
# C Type Definitions
# ============================================================================

class FtpCredentials(ctypes.Structure):
    """ftp_credentials_t structure."""
    _fields_ = [
        ("host", ctypes.c_char_p),
        ("port", ctypes.c_uint16),
        ("username", ctypes.c_char_p),
        ("password", ctypes.c_char_p),
        ("use_tls", ctypes.c_int32),
        ("verify_cert", ctypes.c_int32),
        ("ca_bundle_path", ctypes.c_char_p),
    ]


class FtpUploadOptions(ctypes.Structure):
    """ftp_upload_options_t structure."""
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("max_parallel", ctypes.c_int32),
        ("retry_attempts", ctypes.c_int32),
        ("retry_base_delay_ms", ctypes.c_uint64),
        ("resume_enabled", ctypes.c_int32),
        ("create_remote_dirs", ctypes.c_int32),
        ("remote_chmod", ctypes.c_char_p),
        ("expected_sha256", ctypes.c_char_p),
        ("stall_timeout_ms", ctypes.c_uint32),
        ("resume_metadata_enabled", ctypes.c_int32),
    ]


class FtpDownloadDigest(ctypes.Structure):
    """ftp_download_digest_t structure."""
    _fields_ = [
        ("remote_path", ctypes.c_char_p),
        ("sha256", ctypes.c_char_p),
    ]


class FtpDownloadOptions(ctypes.Structure):
    """ftp_download_options_t structure."""
    _fields_ = [
        ("struct_size", ctypes.c_uint32),
        ("stall_timeout_ms", ctypes.c_uint32),
        ("expected_sha256", ctypes.c_char_p),
        ("resume_enabled", ctypes.c_int32),
        ("resume_metadata_enabled", ctypes.c_int32),
        ("file_digests", ctypes.POINTER(FtpDownloadDigest)),
        ("file_digest_count", ctypes.c_uint32),
        ("resume_allow_unverified", ctypes.c_int32),
    ]


class FtpFileResult(ctypes.Structure):
    """ftp_file_result_t structure."""
    _fields_ = [
        ("local_path", ctypes.c_char_p),
        ("remote_path", ctypes.c_char_p),
        ("status", ctypes.c_int32),
        ("bytes_sent", ctypes.c_uint64),
        ("attempt_count", ctypes.c_uint32),
        ("final_error", ctypes.c_int32),
    ]


class FtpResult(ctypes.Structure):
    """ftp_result_t structure."""
    _fields_ = [
        ("status", ctypes.c_int32),
        ("files_total", ctypes.c_uint64),
        ("files_success", ctypes.c_uint64),
        ("files_failed", ctypes.c_uint64),
        ("bytes_transferred", ctypes.c_uint64),
        ("file_result_count", ctypes.c_uint64),
        ("file_results", ctypes.POINTER(FtpFileResult)),
    ]


# Progress callback type
FtpProgressCb = ctypes.CFUNCTYPE(
    None,                           # return type
    ctypes.c_char_p,                # local_path
    ctypes.c_char_p,                # remote_path
    ctypes.c_uint64,                # bytes_current
    ctypes.c_uint64,                # bytes_total
    ctypes.c_double,                # bytes_per_second
    ctypes.c_void_p,                # user_data
)


# ============================================================================
# Error Codes
# ============================================================================

FTP_OK = 0
FTP_ERR_NOMEM = -101
FTP_ERR_SYSTEM = -102
FTP_ERR_INVALID_HANDLE = -201
FTP_ERR_INVALID_ARGUMENT = -202
FTP_ERR_INVALID_STATE = -203
FTP_ERR_NOT_IMPLEMENTED = -204
FTP_ERR_AUTH_FAILED = -301
FTP_ERR_CONNECT = -401
FTP_ERR_TIMEOUT = -402
FTP_ERR_CANCELLED = -604
FTP_ERR_STALLED = -605
FTP_ERR_INTEGRITY = -606

ERROR_NAMES = {
    FTP_OK: "FTP_OK",
    FTP_ERR_NOMEM: "FTP_ERR_NOMEM",
    FTP_ERR_SYSTEM: "FTP_ERR_SYSTEM",
    FTP_ERR_INVALID_HANDLE: "FTP_ERR_INVALID_HANDLE",
    FTP_ERR_INVALID_ARGUMENT: "FTP_ERR_INVALID_ARGUMENT",
    FTP_ERR_INVALID_STATE: "FTP_ERR_INVALID_STATE",
    FTP_ERR_AUTH_FAILED: "FTP_ERR_AUTH_FAILED",
    FTP_ERR_CONNECT: "FTP_ERR_CONNECT",
    FTP_ERR_TIMEOUT: "FTP_ERR_TIMEOUT",
    FTP_ERR_CANCELLED: "FTP_ERR_CANCELLED",
    FTP_ERR_STALLED: "FTP_ERR_STALLED",
    FTP_ERR_INTEGRITY: "FTP_ERR_INTEGRITY",
}


def error_name(code):
    """Get human-readable error name."""
    return ERROR_NAMES.get(code, f"UNKNOWN({code})")


# ============================================================================
# Test Framework
# ============================================================================

tests_passed = 0
tests_failed = 0


def test_assert(name, condition):
    """Assert a condition and track test results."""
    global tests_passed, tests_failed
    if condition:
        print(f"[PASS] {name}")
        tests_passed += 1
    else:
        print(f"[FAIL] {name}")
        tests_failed += 1


# ============================================================================
# Tests
# ============================================================================

def test_version_capabilities(lib):
    """Test version and capabilities functions."""
    print("\n=== Testing Version and Capabilities ===")
    
    # Setup function signatures
    lib.ftp_get_version.restype = ctypes.c_uint32
    lib.ftp_get_version.argtypes = []
    
    lib.ftp_get_capabilities.restype = ctypes.c_int32
    lib.ftp_get_capabilities.argtypes = [ctypes.POINTER(ctypes.c_uint64)]
    
    # Test version
    version = lib.ftp_get_version()
    test_assert("ftp_get_version returns non-zero", version != 0)
    test_assert("Version is M0 0.1.0 (0x00010000)", version == 0x00010000)
    
    # Test capabilities
    caps = ctypes.c_uint64()
    ret = lib.ftp_get_capabilities(ctypes.byref(caps))
    test_assert("ftp_get_capabilities returns OK", ret == FTP_OK)
    test_assert("M4 reports real plain control capability", (caps.value & 0x0020) != 0)
    test_assert("M4 reports explicit FTPS capability", (caps.value & 0x0001) != 0)
    test_assert("M4 reports plain passive data capability", (caps.value & 0x0040) != 0)
    test_assert("M4 reports protected passive data capability", (caps.value & 0x0080) != 0)
    test_assert("M4 reports REST resume capability", (caps.value & 0x0010) != 0)
    test_assert("M8 reports SHA-256 integrity capability", (caps.value & 0x0100) != 0)
    
    # Test with NULL
    ret = lib.ftp_get_capabilities(None)
    test_assert("ftp_get_capabilities with NULL returns error", ret == FTP_ERR_INVALID_ARGUMENT)


def test_m8_exports(lib):
    """Validate M8 exported symbols and cancellation preconditions."""
    print("\\n=== Testing M8 Exports ===")
    for symbol in ("ftp_download_file_ex", "ftp_download_dir", "ftp_cancel", "ftp_clear_cancel"):
        test_assert(f"{symbol} is exported", hasattr(lib, symbol))

    lib.ftp_cancel.restype = ctypes.c_int32
    lib.ftp_cancel.argtypes = [ctypes.c_void_p]
    lib.ftp_clear_cancel.restype = ctypes.c_int32
    lib.ftp_clear_cancel.argtypes = [ctypes.c_void_p]
    lib.ftp_download_dir.restype = ctypes.c_int32
    lib.ftp_download_dir.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
        ctypes.POINTER(FtpDownloadOptions), ctypes.c_void_p,
        ctypes.c_void_p, ctypes.POINTER(FtpResult)
    ]
    test_assert("ftp_cancel NULL handle fails", lib.ftp_cancel(None) == FTP_ERR_INVALID_HANDLE)
    test_assert("ftp_clear_cancel NULL handle fails", lib.ftp_clear_cancel(None) == FTP_ERR_INVALID_HANDLE)
    handle = ctypes.c_void_p()
    lib.ftp_client_create(ctypes.byref(handle))
    result = FtpResult()
    test_assert("ftp_download_dir without connection fails", lib.ftp_download_dir(
        handle, b"/tmp/m9", b"/deploy", None, None, None,
        ctypes.byref(result)) == FTP_ERR_INVALID_STATE)
    lib.ftp_client_destroy(handle)


def test_handle_lifecycle(lib):
    """Test handle creation and destruction."""
    print("\n=== Testing Handle Lifecycle ===")
    
    # Setup function signatures
    lib.ftp_client_create.restype = ctypes.c_int32
    lib.ftp_client_create.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
    
    lib.ftp_client_destroy.restype = ctypes.c_int32
    lib.ftp_client_destroy.argtypes = [ctypes.c_void_p]
    
    # Test create with NULL
    ret = lib.ftp_client_create(None)
    test_assert("ftp_client_create with NULL returns error", ret == FTP_ERR_INVALID_ARGUMENT)
    
    # Test normal create
    handle = ctypes.c_void_p()
    ret = lib.ftp_client_create(ctypes.byref(handle))
    test_assert("ftp_client_create succeeds", ret == FTP_OK)
    test_assert("Handle is non-NULL", handle.value is not None)
    
    # Test destroy
    ret = lib.ftp_client_destroy(handle)
    test_assert("ftp_client_destroy succeeds", ret == FTP_OK)
    
    # Test destroy NULL
    ret = lib.ftp_client_destroy(None)
    test_assert("ftp_client_destroy with NULL returns error", ret == FTP_ERR_INVALID_HANDLE)
    
    # Test double destroy - after first destroy, set handle to None to avoid use-after-free
    handle = ctypes.c_void_p()
    lib.ftp_client_create(ctypes.byref(handle))
    lib.ftp_client_destroy(handle)
    # After destroy, we must NOT call destroy again on the same handle value
    # as it points to freed memory. The spec says "Double-destroy is undefined behavior"
    # and "callers must not rely on this". So we test that calling with NULL returns error.
    handle = None  # Clear handle after destroy to avoid use-after-free in test
    ret = lib.ftp_client_destroy(handle)
    test_assert("Destroy with NULL after previous destroy returns error", ret == FTP_ERR_INVALID_HANDLE)


def test_configuration(lib):
    """Test configuration functions."""
    print("\n=== Testing Configuration Functions ===")
    
    # Setup signatures
    lib.ftp_set_buffer_size.restype = ctypes.c_int32
    lib.ftp_set_buffer_size.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
    
    lib.ftp_set_timeout_connect_ms.restype = ctypes.c_int32
    lib.ftp_set_timeout_connect_ms.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    
    lib.ftp_set_timeout_command_ms.restype = ctypes.c_int32
    lib.ftp_set_timeout_command_ms.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    
    # Create handle
    handle = ctypes.c_void_p()
    lib.ftp_client_create(ctypes.byref(handle))
    
    # Test buffer size
    ret = lib.ftp_set_buffer_size(None, 65536)
    test_assert("ftp_set_buffer_size with NULL fails", ret == FTP_ERR_INVALID_HANDLE)
    
    ret = lib.ftp_set_buffer_size(handle, 65536)
    test_assert("ftp_set_buffer_size succeeds", ret == FTP_OK)
    
    ret = lib.ftp_set_buffer_size(handle, 0)
    test_assert("ftp_set_buffer_size with 0 (default) succeeds", ret == FTP_OK)
    
    # Test connect timeout
    ret = lib.ftp_set_timeout_connect_ms(None, 10000)
    test_assert("ftp_set_timeout_connect_ms with NULL fails", ret == FTP_ERR_INVALID_HANDLE)
    
    ret = lib.ftp_set_timeout_connect_ms(handle, 10000)
    test_assert("ftp_set_timeout_connect_ms succeeds", ret == FTP_OK)
    
    # Test command timeout
    ret = lib.ftp_set_timeout_command_ms(None, 60000)
    test_assert("ftp_set_timeout_command_ms with NULL fails", ret == FTP_ERR_INVALID_HANDLE)
    
    ret = lib.ftp_set_timeout_command_ms(handle, 60000)
    test_assert("ftp_set_timeout_command_ms succeeds", ret == FTP_OK)
    
    lib.ftp_client_destroy(handle)


def test_connection(lib):
    """Test connection management functions."""
    print("\n=== Testing Connection Management ===")
    
    # Setup signatures
    lib.ftp_connect.restype = ctypes.c_int32
    lib.ftp_connect.argtypes = [ctypes.c_void_p, ctypes.POINTER(FtpCredentials)]
    
    lib.ftp_disconnect.restype = ctypes.c_int32
    lib.ftp_disconnect.argtypes = [ctypes.c_void_p]
    
    lib.ftp_ping.restype = ctypes.c_int32
    lib.ftp_ping.argtypes = [ctypes.c_void_p]
    
    # Create handle
    handle = ctypes.c_void_p()
    lib.ftp_client_create(ctypes.byref(handle))
    
    # Create credentials
    creds = FtpCredentials()
    creds.host = b"localhost"
    creds.port = 21
    creds.username = b"testuser"
    creds.password = b"testpass"
    creds.use_tls = 2  # Implicit FTPS is intentionally unavailable in M4
    creds.verify_cert = 0
    creds.ca_bundle_path = None
    
    # Test connect with NULL handle
    ret = lib.ftp_connect(None, ctypes.byref(creds))
    test_assert("ftp_connect with NULL handle fails", ret == FTP_ERR_INVALID_HANDLE)
    
    # Test connect with NULL creds
    ret = lib.ftp_connect(handle, None)
    test_assert("ftp_connect with NULL creds fails", ret == FTP_ERR_INVALID_ARGUMENT)
    
    # M4 keeps implicit FTPS deterministic until the port-990 factory is integrated.
    ret = lib.ftp_connect(handle, ctypes.byref(creds))
    test_assert("ftp_connect reports implicit FTPS not implemented in M4", ret == FTP_ERR_NOT_IMPLEMENTED)
    
    # The failed connect must not transition the handle to CONNECTED.
    ret = lib.ftp_ping(handle)
    test_assert("ftp_ping after unimplemented connect fails", ret == FTP_ERR_INVALID_STATE)
    
    # Test ping with NULL
    ret = lib.ftp_ping(None)
    test_assert("ftp_ping with NULL fails", ret == FTP_ERR_INVALID_HANDLE)
    
    # Test disconnect
    ret = lib.ftp_disconnect(handle)
    test_assert("ftp_disconnect succeeds", ret == FTP_OK)
    
    # Test double disconnect (idempotent)
    ret = lib.ftp_disconnect(handle)
    test_assert("ftp_disconnect is idempotent", ret == FTP_OK)
    
    # Test ping after disconnect
    ret = lib.ftp_ping(handle)
    test_assert("ftp_ping after disconnect fails", ret == FTP_ERR_INVALID_STATE)
    
    lib.ftp_client_destroy(handle)


def test_upload_dir_stub(lib):
    """Test the M4 upload contract: validation plus authenticated-state checks."""
    print("\n=== Testing Upload Directory (M4 Orchestration Path) ===")
    
    # Setup signature - note: progress callback can be None (ctypes converts NULL automatically)
    lib.ftp_upload_dir.restype = ctypes.c_int32
    lib.ftp_upload_dir.argtypes = [
        ctypes.c_void_p,
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.POINTER(FtpUploadOptions),
        ctypes.c_void_p,  # Use c_void_p for nullable function pointer
        ctypes.c_void_p,
        ctypes.POINTER(FtpResult),
    ]
    
    # Create handle and connect
    handle = ctypes.c_void_p()
    lib.ftp_client_create(ctypes.byref(handle))
    
    creds = FtpCredentials()
    creds.host = b"localhost"
    creds.port = 21
    lib.ftp_connect(handle, ctypes.byref(creds))
    
    # A valid request requires an authenticated connection in M3.
    lib.ftp_disconnect(handle)
    result = FtpResult()
    ret = lib.ftp_upload_dir(handle, b"/local", b"/remote", None, None, None, ctypes.byref(result))
    test_assert("ftp_upload_dir without connection fails with invalid state", ret == FTP_ERR_INVALID_STATE)
    test_assert("upload result is zeroed on invalid state", result.status == FTP_OK and not bool(result.file_results))
    
    # Reconnect
    lib.ftp_connect(handle, ctypes.byref(creds))
    
    # Test with NULL paths
    ret = lib.ftp_upload_dir(handle, None, b"/remote", None, None, None, None)
    test_assert("ftp_upload_dir with NULL local_path fails", ret == FTP_ERR_INVALID_ARGUMENT)
    
    ret = lib.ftp_upload_dir(handle, b"/local", None, None, None, None, None)
    test_assert("ftp_upload_dir with NULL remote_path fails", ret == FTP_ERR_INVALID_ARGUMENT)
    
    # A failed connection does not transition the handle into CONNECTED.
    ret = lib.ftp_upload_dir(handle, b"/local/test", b"/remote/test", None, None, None, ctypes.byref(result))
    test_assert("ftp_upload_dir after failed connect reports invalid state", ret == FTP_ERR_INVALID_STATE)
    lib.ftp_result_free.argtypes = [ctypes.POINTER(FtpResult)]
    lib.ftp_result_free.restype = ctypes.c_int32
    test_assert("ftp_result_free accepts zeroed result", lib.ftp_result_free(ctypes.byref(result)) == FTP_OK)
    
    lib.ftp_client_destroy(handle)


def test_handle_stress(lib):
    """Stress test: create/destroy 1000 handles."""
    print("\n=== Testing Handle Lifecycle Stress (1000 iterations) ===")
    
    iterations = 1000
    failures = 0
    
    for i in range(iterations):
        handle = ctypes.c_void_p()
        ret = lib.ftp_client_create(ctypes.byref(handle))
        if ret != FTP_OK or handle.value is None:
            failures += 1
            continue
        
        ret = lib.ftp_client_destroy(handle)
        if ret != FTP_OK:
            failures += 1
    
    test_assert("Stress test: all iterations completed", failures == 0)
    print(f"Stress test: {iterations} iterations, {failures} failures")


# ============================================================================
# Main
# ============================================================================

def main():
    print("=" * 50)
    print("FTP Client Library - Python ABI Compatibility Test")
    print("=" * 50)
    
    # Find and load library
    try:
        lib_path = find_library()
        print(f"Loading library from: {lib_path}")
        lib = ctypes.CDLL(lib_path)
    except FileNotFoundError as e:
        print(f"ERROR: {e}")
        print("\nPlease build the library first:")
        print("  mkdir -p build && cd build && cmake .. && make")
        return 1
    
    # Run tests
    test_version_capabilities(lib)
    test_m8_exports(lib)
    test_handle_lifecycle(lib)
    test_configuration(lib)
    test_connection(lib)
    test_upload_dir_stub(lib)
    test_handle_stress(lib)
    
    # Summary
    print("\n" + "=" * 50)
    print(f"Test Summary: {tests_passed} passed, {tests_failed} failed")
    print("=" * 50)
    
    return 0 if tests_failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
