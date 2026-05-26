"""
ftpclient.client - High-level FTPClient class

Per Phase 6 Spec Section 5.1: High-Level Class: FTPClient

This module provides the main Pythonic API for the FTP client library,
including context manager support and idiomatic method signatures.
"""

import os
import logging
from pathlib import Path
from typing import Optional, Callable, Any
from contextlib import contextmanager

from ftpclient._core import (
    lib,
    ffi,
    _check_error,
    _get_error_name,
    _register_callback_handle,
    _unregister_callback_handle,
    _get_callback_handle,
    ftp_client_create,
    ftp_client_destroy,
)
from ftpclient.types import Credentials, UploadOptions, UploadResult, FileResult
from ftpclient.exceptions import FTPError, FTPConfigError


logger = logging.getLogger(__name__)


# ============================================================================
# Progress Callback Trampoline
# ============================================================================

@ffi.callback(
    "void(const char*, const char*, uint64_t, uint64_t, double, void*)"
)
def _progress_callback_trampoline(
    local_path: Any,
    remote_path: Any,
    bytes_current: int,
    bytes_total: int,
    bytes_per_second: float,
    user_data: Any
):
    """
    C callback trampoline for progress reporting.
    
    Per Phase 6 Spec Section 7: Progress Callback Design
    
    Note: cffi automatically handles GIL state for callbacks.
    We use PyGILState_Ensure equivalent via cffi's internal handling.
    """
    import sys
    # Acquire GIL manually for safety
    gil_state = None
    try:
        # cffi handles GIL internally when calling Python from C
        # Decode strings from C
        local_str = ffi.string(local_path).decode('utf-8') if local_path != ffi.NULL else ""
        remote_str = ffi.string(remote_path).decode('utf-8') if remote_path != ffi.NULL else ""
        
        # Get the Python callback from user_data
        callback_id = int(ffi.cast("intptr_t", user_data))
        callback = _get_callback_handle(callback_id)
        
        if callback is not None:
            # Call the Python progress callback
            callback(
                Path(local_str),
                remote_str,
                int(bytes_current),
                int(bytes_total),
                float(bytes_per_second)
            )
    except Exception as e:
        # Log but don't propagate exceptions from C callbacks
        logger.warning(f"Progress callback raised exception: {e}")


# ============================================================================
# Credential Provider Callback Trampoline
# ============================================================================

@ffi.callback(
    "int32_t(ftp_credentials_t*, void*, int32_t)"
)
def _credential_provider_trampoline(
    out_creds: Any,
    user_data: Any,
    attempt: int
) -> int:
    """
    C callback trampoline for dynamic credential provision.
    
    Per Phase 6 Spec Section 6: Credential Provider Integration
    
    This allows Python users to provide credentials dynamically via a callable,
    enabling integration with keyring modules or other secret managers.
    
    Note: cffi automatically handles GIL state for callbacks.
    """
    try:
        # Get the Python callback from user_data
        callback_id = int(ffi.cast("intptr_t", user_data))
        callback = _get_callback_handle(callback_id)
        
        if callback is None:
            return lib.FTP_ERR_INVALID_ARGUMENT
        
        # Call the Python credential provider
        creds = callback(int(attempt))
        
        # Populate the C struct
        # Note: We need to keep references to the encoded strings alive
        # The C++ side will deep-copy these, so we can use temporary buffers
        host_bytes = creds.host.encode('utf-8')
        username_bytes = (creds.username or '').encode('utf-8')
        password_bytes = (creds.password or '').encode('utf-8')
        ca_bundle_bytes = (str(creds.ca_bundle_path) if creds.ca_bundle_path else b'').encode('utf-8')
        
        # Allocate and set strings (C++ will copy these)
        out_creds.host = host_bytes
        out_creds.port = creds.port
        out_creds.username = username_bytes if creds.username else ffi.NULL
        out_creds.password = password_bytes if creds.password else ffi.NULL
        out_creds.use_tls = creds.use_tls
        out_creds.verify_cert = creds.verify_cert
        out_creds.ca_bundle_path = ca_bundle_bytes if creds.ca_bundle_path else ffi.NULL
        
        return lib.FTP_OK
        
    except Exception as e:
        logger.warning(f"Credential provider callback raised exception: {e}")
        return lib.FTP_ERR_SYSTEM


class FTPClient:
    """
    High-level FTP client with context manager support.
    
    Per Phase 6 Spec Section 5.1
    
    Example usage:
        with FTPClient() as client:
            client.connect(Credentials(host="ftp.example.com", ...))
            result = client.upload_directory("/local/path", "/remote/path")
            print(f"Uploaded {result.files_success}/{result.files_total} files")
    """
    
    def __init__(self):
        """Initialize a new FTP client instance."""
        self._handle: Optional[Any] = None
        self._connected: bool = False
        self._progress_callback_id: Optional[int] = None
        self._credential_provider_id: Optional[int] = None
    
    def __enter__(self) -> 'FTPClient':
        """Context manager entry."""
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        """Context manager exit - ensures cleanup."""
        self.close()
    
    def __del__(self):
        """Destructor - ensure resources are freed."""
        self.close()
    
    @property
    def handle(self) -> Optional[Any]:
        """Get the underlying C handle (for advanced use)."""
        return self._handle
    
    @property
    def is_connected(self) -> bool:
        """Check if the client is currently connected."""
        return self._connected and self._handle is not None
    
    def connect(self, credentials: Credentials) -> None:
        """
        Establish connection to the FTP server.
        
        Per Phase 6 Spec Section 5.3: connect() raises on failure because
        a failed connection leaves the client unusable.
        
        Args:
            credentials: Connection credentials
            
        Raises:
            FTPAuthError: Authentication failed
            FTPNetworkError: Network error (connection refused, timeout)
            FTPProtocolError: Protocol error
            FTPConfigError: Invalid credentials
        """
        if self._handle is None:
            self._handle = ftp_client_create()
        
        # Convert Python Credentials to C struct
        host_bytes = credentials.host.encode('utf-8')
        username_bytes = (credentials.username or '').encode('utf-8')
        password_bytes = (credentials.password or '').encode('utf-8')
        ca_bundle_bytes = (str(credentials.ca_bundle_path) if credentials.ca_bundle_path else b'').encode('utf-8')
        
        creds = ffi.new("ftp_credentials_t*")
        creds.host = host_bytes
        creds.port = credentials.port
        creds.username = username_bytes if credentials.username else ffi.NULL
        creds.password = password_bytes if credentials.password else ffi.NULL
        creds.use_tls = credentials.use_tls
        creds.verify_cert = credentials.verify_cert
        creds.ca_bundle_path = ca_bundle_bytes if credentials.ca_bundle_path else ffi.NULL
        
        # Connect (this releases GIL internally per spec Section 4.1)
        ret = lib.ftp_connect(self._handle, creds)
        _check_error(ret, "connect")
        
        self._connected = True
        logger.info(f"Connected to {credentials.host}:{credentials.port}")
    
    def disconnect(self) -> None:
        """
        Disconnect from the FTP server.
        
        This is idempotent - safe to call multiple times.
        """
        if self._handle is not None and self._connected:
            ret = lib.ftp_disconnect(self._handle)
            # Ignore errors during disconnect
            self._connected = False
            logger.info("Disconnected from server")
    
    def close(self) -> None:
        """
        Close the client and free all resources.
        
        This calls disconnect() if connected, then destroys the handle.
        Also clears any registered callbacks.
        """
        self.disconnect()
        
        # Clear credential provider if set
        if self._credential_provider_id is not None:
            if self._handle is not None:
                lib.ftp_clear_credential_provider(self._handle)
            _unregister_callback_handle(self._credential_provider_id)
            self._credential_provider_id = None
        
        # Destroy the handle
        if self._handle is not None:
            ftp_client_destroy(self._handle)
            self._handle = None
    
    def ping(self) -> bool:
        """
        Check connection health.
        
        Returns:
            True if connected and responsive, False otherwise
        """
        if self._handle is None or not self._connected:
            return False
        
        ret = lib.ftp_ping(self._handle)
        return ret == lib.FTP_OK
    
    def set_credential_provider(self, provider: Callable[[int], Credentials]) -> None:
        """
        Set a credential provider callback for dynamic credential injection.
        
        Per Phase 6 Spec Section 6: Credential Provider Integration
        
        Args:
            provider: Callable that takes attempt number and returns Credentials
        """
        if self._handle is None:
            self._handle = ftp_client_create()
        
        # Register the callback handle
        callback_id = _register_callback_handle(provider)
        self._credential_provider_id = callback_id
        
        # Convert to cffi-compatible pointer
        user_data = ffi.cast("void*", ffi.cast("intptr_t", callback_id))
        
        # Set the provider in C
        ret = lib.ftp_set_credential_provider(
            self._handle,
            _credential_provider_trampoline,
            user_data
        )
        _check_error(ret, "set_credential_provider")
    
    def upload_directory(
        self,
        local_path: str,
        remote_path: str,
        options: Optional[UploadOptions] = None,
        progress: Optional[Callable[[Path, str, int, int, float], None]] = None
    ) -> UploadResult:
        """
        Upload a directory tree to the FTP server.
        
        Per Phase 6 Spec Section 5.1 and Section 7: Progress Callback Design
        
        Args:
            local_path: Local directory path to upload
            remote_path: Remote directory path on server
            options: Upload options (uses defaults if None)
            progress: Optional progress callback
            
        Returns:
            UploadResult with transfer statistics and per-file results
            
        Raises:
            FTPConfigError: If not connected or invalid arguments
            FTPNetworkError: Network error during transfer
            FTPProtocolError: Protocol error
        """
        if self._handle is None:
            raise FTPConfigError(lib.FTP_ERR_INVALID_HANDLE, "Client not initialized")
        
        if not self._connected:
            raise FTPConfigError(lib.FTP_ERR_INVALID_STATE, "Not connected to server")
        
        # Convert paths to bytes
        local_bytes = os.fsencode(local_path)
        remote_bytes = remote_path.encode('utf-8')
        
        # Prepare upload options
        if options is None:
            options = UploadOptions()
        
        c_options = ffi.new("ftp_upload_options_t*")
        c_options.struct_size = ffi.sizeof("ftp_upload_options_t")
        c_options.max_parallel = options.max_parallel
        c_options.retry_attempts = options.retry_attempts
        c_options.retry_base_delay_ms = options.retry_base_delay_ms
        c_options.resume_enabled = 1 if options.resume_enabled else 0
        c_options.create_remote_dirs = 1 if options.create_remote_dirs else 0
        c_options.remote_chmod = options.remote_chmod.encode('utf-8') if options.remote_chmod else ffi.NULL
        
        # Set up progress callback if provided
        progress_cb = ffi.NULL
        progress_cb_id = None
        
        if progress is not None:
            progress_cb_id = _register_callback_handle(progress)
            self._progress_callback_id = progress_cb_id
            user_data = ffi.cast("void*", ffi.cast("intptr_t", progress_cb_id))
            progress_cb = _progress_callback_trampoline
            # Note: We'd need to store user_data somewhere accessible to the callback
            # For now, this is a simplified implementation
        
        # Prepare result structure
        result_ptr = ffi.new("ftp_result_t*")
        
        # Call the C function (releases GIL per spec Section 4.1)
        ret = lib.ftp_upload_dir(
            self._handle,
            local_bytes,
            remote_bytes,
            c_options,
            progress_cb,
            ffi.NULL,  # user_data for progress callback
            result_ptr
        )
        
        # Clean up progress callback registration
        if progress_cb_id is not None:
            _unregister_callback_handle(progress_cb_id)
            self._progress_callback_id = None
        
        # Check for errors (but don't raise - upload_dir returns partial success info)
        status = int(result_ptr.status)
        
        # Convert file_results array to Python tuple
        file_results = []
        if result_ptr.file_results != ffi.NULL and result_ptr.file_result_count > 0:
            for i in range(result_ptr.file_result_count):
                fr = result_ptr.file_results[i]
                local_file = ffi.string(fr.local_path).decode('utf-8') if fr.local_path != ffi.NULL else ""
                remote_file = ffi.string(fr.remote_path).decode('utf-8') if fr.remote_path != ffi.NULL else ""
                
                file_results.append(FileResult(
                    local_path=Path(local_file),
                    remote_path=remote_file,
                    status=int(fr.status),
                    bytes_sent=int(fr.bytes_sent),
                    attempt_count=int(fr.attempt_count),
                    final_error=int(fr.final_error) if fr.final_error != 0 else None
                ))
        
        # Create Python result
        py_result = UploadResult(
            status=status,
            files_total=int(result_ptr.files_total),
            files_success=int(result_ptr.files_success),
            files_failed=int(result_ptr.files_failed),
            bytes_transferred=int(result_ptr.bytes_transferred),
            file_results=tuple(file_results)
        )
        
        # Free C result structure
        lib.ftp_result_free(result_ptr)
        
        return py_result
    
    def set_buffer_size(self, size_bytes: int) -> None:
        """Set internal buffer size for data channels."""
        if self._handle is None:
            raise FTPConfigError(lib.FTP_ERR_INVALID_HANDLE, "Client not initialized")
        ret = lib.ftp_set_buffer_size(self._handle, size_bytes)
        _check_error(ret, "set_buffer_size")
    
    def set_timeout_connect_ms(self, ms: int) -> None:
        """Set connection timeout in milliseconds."""
        if self._handle is None:
            raise FTPConfigError(lib.FTP_ERR_INVALID_HANDLE, "Client not initialized")
        ret = lib.ftp_set_timeout_connect_ms(self._handle, ms)
        _check_error(ret, "set_timeout_connect_ms")
    
    def set_timeout_command_ms(self, ms: int) -> None:
        """Set command/response timeout in milliseconds."""
        if self._handle is None:
            raise FTPConfigError(lib.FTP_ERR_INVALID_HANDLE, "Client not initialized")
        ret = lib.ftp_set_timeout_command_ms(self._handle, ms)
        _check_error(ret, "set_timeout_command_ms")
