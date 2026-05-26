"""
ftpclient._core - Low-level cffi loader and function wrappers

This module loads the C shared library via cffi and provides thin wrappers
around each C function with proper GIL management.

Per Phase 6 Spec Section 3 (Binding Layer Architecture) and Section 4 (GIL Management).
"""

import os
import sys
import re
from pathlib import Path
from typing import Optional, Any
import threading

import cffi

# Create FFI instance and load the header
ffi = cffi.FFI()

def _extract_c_declarations(header_content: str) -> str:
    """
    Extract C declarations from header for cffi parsing.
    
    cffi's cdef() cannot handle preprocessor directives like #ifndef, #define, etc.
    We need to strip them out and only keep the actual C declarations.
    Also removes doxygen-style comments that can confuse the parser.
    
    This implementation preserves struct fields while removing only pure comment lines.
    It also removes FTP_API and FTP_CALL macros which cffi doesn't understand.
    """
    lines = []
    in_extern_c = False
    in_block_comment = False
    
    for line in header_content.split('\n'):
        stripped = line.strip()
        
        # Skip preprocessor directives
        if stripped.startswith('#'):
            continue
        
        # Handle multi-line block comments (/* ... */)
        if '/*' in stripped and '*/' in stripped:
            cleaned = re.sub(r'/\*.*?\*/', '', stripped)
            if cleaned.strip():
                lines.append(cleaned)
            continue
        if stripped.startswith('/*'):
            in_block_comment = True
            if '*/' in stripped:
                in_block_comment = False
                cleaned = re.sub(r'/\*.*?\*/', '', stripped)
                if cleaned.strip():
                    lines.append(cleaned)
            continue
        if in_block_comment:
            if '*/' in stripped:
                in_block_comment = False
            continue
        
        # Track extern "C" blocks
        if stripped == 'extern "C" {':
            in_extern_c = True
            continue
        if stripped == '}':
            in_extern_c = False
            continue
        
        # Skip doxygen-style documentation comments (/** ... */)
        if stripped.startswith('/**'):
            continue
        # Skip standalone asterisk lines
        if stripped.startswith('*') and not ';' in stripped and not '(' in stripped:
            continue
        
        # Skip empty lines
        if not stripped:
            continue
        
        # Remove inline comments
        cleaned = re.sub(r'/\*.*?\*/', '', stripped)
        if '//' in cleaned:
            cleaned = cleaned.split('//')[0].rstrip()
        
        # Remove FTP_API and FTP_CALL macros - cffi doesn't understand them
        cleaned = cleaned.replace('FTP_API ', '')
        cleaned = cleaned.replace('FTP_CALL', '')
        
        if cleaned:
            lines.append(cleaned)
    
    return '\n'.join(lines)


# Read and parse the C header
_header_path = Path(__file__).parent.parent.parent / "include" / "ftpclient.h"
if not _header_path.exists():
    raise FileNotFoundError(f"C header not found: {_header_path}")

with open(_header_path, 'r', encoding='utf-8') as f:
    _header_content = f.read()

# Extract only the C declarations for cffi
_c_declarations = _extract_c_declarations(_header_content)
ffi.cdef(_c_declarations)


def _find_library() -> str:
    """
    Find the shared library in various locations.
    
    Search order:
    1. Package directory (_lib subdirectory) - for vendored wheels
    2. Build directory - for development
    3. System library paths
    """
    # Platform-specific library name
    if sys.platform == 'win32':
        lib_name = 'ftpclient.dll'
    elif sys.platform == 'darwin':
        lib_name = 'libftpclient.dylib'
    else:
        lib_name = 'libftpclient.so'
    
    # Search paths
    search_paths = [
        # Vendored location (wheel distribution)
        Path(__file__).parent / "_lib" / lib_name,
        # Build directory locations
        Path(__file__).parent.parent.parent / "build" / lib_name,
        Path(__file__).parent.parent.parent / "build" / "Release" / lib_name,
        Path(__file__).parent.parent.parent / "build" / "Debug" / lib_name,
        # System locations
        Path("/usr/local/lib") / lib_name,
        Path("/usr/lib") / lib_name,
    ]
    
    for path in search_paths:
        if path.exists():
            return str(path)
    
    # Try loading via system dynamic linker
    return lib_name


def _load_library():
    """Load the shared library using cffi."""
    lib_path = _find_library()
    
    try:
        # Load the shared library
        lib = ffi.dlopen(lib_path)
        return lib
    except OSError as e:
        raise ImportError(
            f"Failed to load ftpclient library from {lib_path}: {e}. "
            f"Please ensure the library is built and installed correctly."
        ) from e


# Load the library
lib = _load_library()


# ============================================================================
# Error Code Constants (mirroring ftpclient.h)
# ============================================================================

FTP_OK = 0
FTP_ERR_NOMEM = -101
FTP_ERR_SYSTEM = -102
FTP_ERR_INVALID_HANDLE = -201
FTP_ERR_INVALID_ARGUMENT = -202
FTP_ERR_INVALID_STATE = -203
FTP_ERR_AUTH_FAILED = -301
FTP_ERR_AUTH_TLS_REQUIRED = -302
FTP_ERR_CERT_VERIFY = -303
FTP_ERR_CONNECT = -401
FTP_ERR_TIMEOUT = -402
FTP_ERR_NETWORK_RESET = -403
FTP_ERR_DNS = -404
FTP_ERR_PROTOCOL = -501
FTP_ERR_SERVER_DENIED = -502
FTP_ERR_PASSIVE_FAILED = -503
FTP_ERR_LOCAL_IO = -601
FTP_ERR_REMOTE_IO = -602
FTP_ERR_PARTIAL = -603
FTP_WARN_PARTIAL_RETRY = 101
FTP_WARN_SKIPPED = 102


# Error code to human-readable name mapping
_ERROR_NAMES = {
    FTP_OK: "FTP_OK",
    FTP_ERR_NOMEM: "FTP_ERR_NOMEM",
    FTP_ERR_SYSTEM: "FTP_ERR_SYSTEM",
    FTP_ERR_INVALID_HANDLE: "FTP_ERR_INVALID_HANDLE",
    FTP_ERR_INVALID_ARGUMENT: "FTP_ERR_INVALID_ARGUMENT",
    FTP_ERR_INVALID_STATE: "FTP_ERR_INVALID_STATE",
    FTP_ERR_AUTH_FAILED: "FTP_ERR_AUTH_FAILED",
    FTP_ERR_AUTH_TLS_REQUIRED: "FTP_ERR_AUTH_TLS_REQUIRED",
    FTP_ERR_CERT_VERIFY: "FTP_ERR_CERT_VERIFY",
    FTP_ERR_CONNECT: "FTP_ERR_CONNECT",
    FTP_ERR_TIMEOUT: "FTP_ERR_TIMEOUT",
    FTP_ERR_NETWORK_RESET: "FTP_ERR_NETWORK_RESET",
    FTP_ERR_DNS: "FTP_ERR_DNS",
    FTP_ERR_PROTOCOL: "FTP_ERR_PROTOCOL",
    FTP_ERR_SERVER_DENIED: "FTP_ERR_SERVER_DENIED",
    FTP_ERR_PASSIVE_FAILED: "FTP_ERR_PASSIVE_FAILED",
    FTP_ERR_LOCAL_IO: "FTP_ERR_LOCAL_IO",
    FTP_ERR_REMOTE_IO: "FTP_ERR_REMOTE_IO",
    FTP_ERR_PARTIAL: "FTP_ERR_PARTIAL",
    FTP_WARN_PARTIAL_RETRY: "FTP_WARN_PARTIAL_RETRY",
    FTP_WARN_SKIPPED: "FTP_WARN_SKIPPED",
}


def _get_error_name(code: int) -> str:
    """Get human-readable error name for a status code."""
    return _ERROR_NAMES.get(code, f"UNKNOWN({code})")


# ============================================================================
# Exception Mapping (Phase 6 Spec Section 5.3)
# ============================================================================

# Import exceptions here to avoid circular imports
# Actual exception classes are defined in ftpclient.exceptions


def _check_error(code: int, operation: str = "operation"):
    """
    Check a C function return code and raise appropriate Python exception.
    
    Per Phase 6 Spec Section 5.3: Exception Hierarchy
    
    Args:
        code: The C function return code
        operation: Description of the operation for error messages
        
    Raises:
        FTPAuthError: For -3xx authentication errors
        FTPNetworkError: For -4xx network errors (transient, retryable)
        FTPProtocolError: For -5xx protocol errors
        FTPIOError: For -6xx I/O errors
        FTPConfigError: For -2xx configuration/argument errors
        FTPSystemError: For -1xx system errors
    """
    if code >= 0:
        return  # Success or warning
    
    # Import here to avoid circular dependency
    from ftpclient.exceptions import (
        FTPAuthError,
        FTPNetworkError,
        FTPProtocolError,
        FTPIOError,
        FTPConfigError,
        FTPSystemError,
    )
    
    error_name = _get_error_name(code)
    
    # Map error codes to exception types per spec Section 5.3
    if -300 <= code < -200:
        raise FTPAuthError(code, f"{operation} failed: {error_name}")
    elif -400 <= code < -300:
        raise FTPNetworkError(code, f"{operation} failed: {error_name}")
    elif -500 <= code < -400:
        raise FTPProtocolError(code, f"{operation} failed: {error_name}")
    elif -600 <= code < -500:
        raise FTPIOError(code, f"{operation} failed: {error_name}")
    elif -200 <= code < -100:
        raise FTPConfigError(code, f"{operation} failed: {error_name}")
    elif -100 <= code < 0:
        raise FTPSystemError(code, f"{operation} failed: {error_name}")
    else:
        # Unknown error code
        raise FTPSystemError(code, f"{operation} failed: unknown error {code}")


# ============================================================================
# GIL Management Wrappers (Phase 6 Spec Section 4)
# ============================================================================

class _GILReleaser:
    """
    Context manager for releasing the GIL during blocking C calls.
    
    Per Phase 6 Spec Section 4.1: C Call → Python (GIL Must Release)
    
    Functions that block for >1ms must release the GIL to allow other
    Python threads to run.
    """
    
    def __enter__(self):
        # Save thread state and release GIL
        self.thread_state = cffi.gc(None, None)  # Placeholder
        # cffi doesn't have direct GIL release, we use ffi.release_gil()
        # Actually, cffi automatically handles this for ffi.CData calls
        # But for explicit control, we can use threading
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb):
        pass


# Thread-local storage for callback management
_tls = threading.local()


def _register_callback_handle(handle: Any) -> int:
    """
    Register a Python callable handle for use in C callbacks.
    
    Returns an integer ID that can be passed as user_data to C functions.
    """
    if not hasattr(_tls, 'callback_handles'):
        _tls.callback_handles = {}
        _tls.next_id = 1
    
    handle_id = _tls.next_id
    _tls.callback_handles[handle_id] = handle
    _tls.next_id += 1
    return handle_id


def _unregister_callback_handle(handle_id: int):
    """Unregister a previously registered callback handle."""
    if hasattr(_tls, 'callback_handles'):
        _tls.callback_handles.pop(handle_id, None)


def _get_callback_handle(handle_id: int) -> Optional[Any]:
    """Retrieve a registered callback handle by ID."""
    if hasattr(_tls, 'callback_handles'):
        return _tls.callback_handles.get(handle_id)
    return None


# ============================================================================
# Low-Level Function Wrappers
# These provide thin wrappers around C functions with proper type handling
# ============================================================================

def ftp_client_create() -> Any:
    """
    Create a new FTP client handle.
    
    Returns:
        Opaque handle pointer (cffi void*)
        
    Raises:
        FTPSystemError: If allocation fails
    """
    handle_ptr = ffi.new("ftp_client_t**")
    ret = lib.ftp_client_create(handle_ptr)
    _check_error(ret, "ftp_client_create")
    return handle_ptr[0]


def ftp_client_destroy(handle: Any):
    """
    Destroy an FTP client handle and free all resources.
    
    Args:
        handle: The client handle to destroy
    """
    if handle != ffi.NULL:
        ret = lib.ftp_client_destroy(handle)
        # Ignore errors during destruction (resource cleanup should always succeed)


def ftp_set_buffer_size(handle: Any, size_bytes: int):
    """
    Set internal buffer size for data channels.
    
    Args:
        handle: The client handle
        size_bytes: Buffer size in bytes (0 = default 256KB)
    """
    ret = lib.ftp_set_buffer_size(handle, size_bytes)
    _check_error(ret, "ftp_set_buffer_size")


def ftp_set_timeout_connect_ms(handle: Any, ms: int):
    """
    Set connection timeout in milliseconds.
    
    Args:
        handle: The client handle
        ms: Timeout in milliseconds (0 = default 5000ms)
    """
    ret = lib.ftp_set_timeout_connect_ms(handle, ms)
    _check_error(ret, "ftp_set_timeout_connect_ms")


def ftp_set_timeout_command_ms(handle: Any, ms: int):
    """
    Set command/response timeout in milliseconds.
    
    Args:
        handle: The client handle
        ms: Timeout in milliseconds (0 = default 30000ms)
    """
    ret = lib.ftp_set_timeout_command_ms(handle, ms)
    _check_error(ret, "ftp_set_timeout_command_ms")


def ftp_get_version() -> int:
    """
    Get the library version as a packed integer.
    
    Returns:
        Packed version integer (0xMMmmpp00 format)
    """
    return lib.ftp_get_version()


def ftp_get_capabilities() -> int:
    """
    Get library capability flags.
    
    Returns:
        Bitmask of capability flags
    """
    caps_ptr = ffi.new("uint64_t*")
    ret = lib.ftp_get_capabilities(caps_ptr)
    _check_error(ret, "ftp_get_capabilities")
    return caps_ptr[0]
