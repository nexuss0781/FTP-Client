"""
ftpclient - High-Performance FTP Client Library for Python

Type stubs for IDE completion and static analysis.
Per Phase 6 Spec Section 10.7: Type Stub Completeness
"""

from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Tuple, Callable, Any, AsyncContextManager, ContextManager
import asyncio


# ============================================================================
# Data Types
# ============================================================================

@dataclass(frozen=True)
class ServerCapabilities:
    feat_supported: bool
    size_supported: bool
    mdtm_supported: bool
    hash_supported: bool
    hash_algorithms: int
    @property
    def supports_sha256(self) -> bool: ...
    @property
    def supports_sha512(self) -> bool: ...


@dataclass(frozen=True)
class Credentials:
    host: str
    port: int
    username: Optional[str]
    password: Optional[str]
    use_tls: int
    verify_cert: int
    ca_bundle_path: Optional[Path]


@dataclass(frozen=True)
class UploadOptions:
    max_parallel: int
    retry_attempts: int
    retry_base_delay_ms: int
    resume_enabled: bool
    create_remote_dirs: bool
    remote_chmod: Optional[str]
    expected_sha256: Optional[str]
    stall_timeout_ms: int
    resume_metadata_enabled: bool


@dataclass(frozen=True)
class DownloadDigest:
    remote_path: str
    sha256: str


@dataclass(frozen=True)
class DownloadOptions:
    expected_sha256: Optional[str]
    stall_timeout_ms: int
    resume_enabled: bool
    resume_metadata_enabled: bool
    file_digests: Tuple[DownloadDigest, ...]
    resume_allow_unverified: bool


@dataclass(frozen=True)
class FileResult:
    local_path: Path
    remote_path: str
    status: int
    bytes_sent: int
    attempt_count: int
    final_error: Optional[int]


@dataclass(frozen=True)
class DownloadResult:
    status: int
    files_total: int
    files_success: int
    files_failed: int
    bytes_transferred: int
    file_results: Tuple[FileResult, ...]

    @property
    def success(self) -> bool: ...


@dataclass(frozen=True)
class UploadResult:
    status: int
    files_total: int
    files_success: int
    files_failed: int
    bytes_transferred: int
    file_results: Tuple[FileResult, ...]
    
    @property
    def success(self) -> bool: ...


# ============================================================================
# Exceptions
# ============================================================================

class FTPError(Exception):
    error_code: int
    message: str
    
    def __init__(self, error_code: int, message: str = ...) -> None: ...


class FTPAuthError(FTPError): ...
class FTPNetworkError(FTPError): ...
class FTPProtocolError(FTPError): ...
class FTPIOError(FTPError): ...
class FTPConfigError(FTPError): ...
class FTPSystemError(FTPError): ...


# ============================================================================
# Callback Types
# ============================================================================

ProgressCallback = Callable[[Path, str, int, int, float], None]
CredentialProviderCallback = Callable[[int], Credentials]


# ============================================================================
# Main Client Classes
# ============================================================================

class FTPClient:
    """High-level FTP client with context manager support."""
    
    handle: Optional[Any]
    is_connected: bool
    
    def __init__(self) -> None: ...
    def __enter__(self) -> 'FTPClient': ...
    def __exit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> None: ...
    def close(self) -> None: ...
    
    def connect(self, credentials: Credentials) -> None: ...
    def disconnect(self) -> None: ...
    def ping(self) -> bool: ...
    
    def upload_directory(
        self,
        local_path: str,
        remote_path: str,
        options: Optional[UploadOptions] = ...,
        progress: Optional[ProgressCallback] = ...
    ) -> UploadResult: ...

    def download_file(
        self,
        local_path: str,
        remote_path: str,
        options: Optional[DownloadOptions] = ...,
        progress: Optional[ProgressCallback] = ...
    ) -> DownloadResult: ...

    def cancel(self) -> None: ...
    def clear_cancel(self) -> None: ...
    
    def set_credential_provider(self, provider: CredentialProviderCallback) -> None: ...
    def server_capabilities(self) -> ServerCapabilities: ...
    def remote_file_mdtm(self, remote_path: str) -> str: ...
    def remote_file_hash(self, remote_path: str, algorithm: str = ...) -> str: ...
    def set_buffer_size(self, size_bytes: int) -> None: ...
    def set_timeout_connect_ms(self, ms: int) -> None: ...
    def set_timeout_command_ms(self, ms: int) -> None: ...


class AsyncFTPClient:
    """Async-compatible FTP client using thread pool executor."""
    
    is_connected: bool
    
    def __init__(self, max_workers: int = ...) -> None: ...
    async def __aenter__(self) -> 'AsyncFTPClient': ...
    async def __aexit__(self, exc_type: Any, exc_val: Any, exc_tb: Any) -> None: ...
    async def close(self) -> None: ...
    
    async def connect(self, credentials: Credentials) -> None: ...
    async def disconnect(self) -> None: ...
    async def ping(self) -> bool: ...
    
    async def upload_directory(
        self,
        local_path: str,
        remote_path: str,
        options: Optional[UploadOptions] = ...,
        progress: Optional[ProgressCallback] = ...
    ) -> UploadResult: ...

    async def download_file(
        self,
        local_path: str,
        remote_path: str,
        options: Optional[DownloadOptions] = ...,
        progress: Optional[ProgressCallback] = ...
    ) -> DownloadResult: ...

    async def download_directory(
        self,
        local_path: str,
        remote_path: str,
        options: Optional[DownloadOptions] = ...,
        progress: Optional[ProgressCallback] = ...
    ) -> DownloadResult: ...

    async def cancel(self) -> None: ...
    async def clear_cancel(self) -> None: ...
    
    async def server_capabilities(self) -> ServerCapabilities: ...
    async def remote_file_mdtm(self, remote_path: str) -> str: ...
    async def remote_file_hash(self, remote_path: str, algorithm: str = ...) -> str: ...
    async def set_buffer_size(self, size_bytes: int) -> None: ...
    async def set_timeout_connect_ms(self, ms: int) -> None: ...
    async def set_timeout_command_ms(self, ms: int) -> None: ...


# ============================================================================
# Utility Functions
# ============================================================================

def get_version() -> str: ...


# ============================================================================
# Module Exports
# ============================================================================

__version__: str
__author__: str
__all__: Tuple[str, ...]
