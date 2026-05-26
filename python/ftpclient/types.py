"""
ftpclient.types - Type-safe dataclasses for FTP client operations

Per Phase 6 Spec Section 5.2: Dataclasses (Type-Safe, Immutable)

This module defines:
- Credentials: Connection credentials with TLS options
- UploadOptions: Configuration for directory uploads
- UploadResult: Result of a directory upload operation
- FileResult: Per-file result details
"""

from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, Tuple


@dataclass(frozen=True)
class Credentials:
    """
    FTP connection credentials.
    
    Per Phase 6 Spec Section 5.2
    
    Attributes:
        host: Server hostname or IP address (UTF-8)
        port: Server port (default: 21)
        username: Username for authentication (None for anonymous)
        password: Password for authentication
        use_tls: TLS mode (0=plain, 1=explicit FTPS, 2=implicit FTPS)
        verify_cert: Certificate verification (0=none, 1=peer, 2=host+peer)
        ca_bundle_path: Path to CA bundle PEM file (optional)
    """
    host: str
    port: int = 21
    username: Optional[str] = None
    password: Optional[str] = None
    use_tls: int = 1  # Default to explicit FTPS
    verify_cert: int = 2  # Default to full verification
    ca_bundle_path: Optional[Path] = None
    
    def __post_init__(self) -> None:
        """Validate credential fields."""
        if not self.host:
            raise ValueError("host cannot be empty")
        if not (0 <= self.port <= 65535):
            raise ValueError(f"port must be 0-65535, got {self.port}")
        if not (0 <= self.use_tls <= 2):
            raise ValueError(f"use_tls must be 0, 1, or 2, got {self.use_tls}")
        if not (0 <= self.verify_cert <= 2):
            raise ValueError(f"verify_cert must be 0, 1, or 2, got {self.verify_cert}")
        if self.ca_bundle_path is not None and not isinstance(self.ca_bundle_path, (str, Path)):
            raise TypeError("ca_bundle_path must be a Path or string")


@dataclass(frozen=True)
class UploadOptions:
    """
    Options for directory upload operations.
    
    Per Phase 6 Spec Section 5.2
    
    Attributes:
        max_parallel: Max concurrent file uploads (0 = auto-detect based on hardware)
        retry_attempts: Per-file retry count (default: 3)
        retry_base_delay_ms: Base delay for exponential backoff in ms (default: 1000)
        resume_enabled: Whether to resume partial transfers (default: False)
        create_remote_dirs: Whether to auto-create remote directories (default: True)
        remote_chmod: Optional chmod mode string (e.g., "0644") applied after upload
    """
    max_parallel: int = 0  # 0 = library default (auto)
    retry_attempts: int = 3
    retry_base_delay_ms: int = 1000
    resume_enabled: bool = False
    create_remote_dirs: bool = True
    remote_chmod: Optional[str] = None
    
    def __post_init__(self) -> None:
        """Validate upload options."""
        if self.max_parallel < 0:
            raise ValueError(f"max_parallel must be >= 0, got {self.max_parallel}")
        if self.retry_attempts < 0:
            raise ValueError(f"retry_attempts must be >= 0, got {self.retry_attempts}")
        if self.retry_base_delay_ms < 0:
            raise ValueError(f"retry_base_delay_ms must be >= 0, got {self.retry_base_delay_ms}")


@dataclass(frozen=True)
class FileResult:
    """
    Result of a single file transfer.
    
    Per Phase 6 Spec Section 5.2 and Phase 5 Spec Section 8.2
    
    Attributes:
        local_path: Path to the local file
        remote_path: Remote path on the server
        status: Transfer status code (0 = success, negative = error)
        bytes_sent: Number of bytes successfully transferred
        attempt_count: Number of attempts (1 = first try succeeded, >1 = retried)
        final_error: Error code from last attempt (if failed)
    """
    local_path: Path
    remote_path: str
    status: int
    bytes_sent: int
    attempt_count: int = 1
    final_error: Optional[int] = None


@dataclass(frozen=True)
class UploadResult:
    """
    Result of a directory upload operation.
    
    Per Phase 6 Spec Section 5.2
    
    Attributes:
        status: Overall operation status code
        files_total: Total number of files attempted
        files_success: Number of files successfully uploaded
        files_failed: Number of files that failed
        bytes_transferred: Total bytes transferred
        file_results: Tuple of per-file results
    """
    status: int
    files_total: int
    files_success: int
    files_failed: int
    bytes_transferred: int
    file_results: Tuple[FileResult, ...] = field(default_factory=tuple)
    
    @property
    def success(self) -> bool:
        """Check if all files were uploaded successfully."""
        return self.status == 0 and self.files_failed == 0
    
    def __post_init__(self) -> None:
        """Validate result consistency."""
        if self.files_total != self.files_success + self.files_failed:
            # This could happen in edge cases, but let's warn
            pass  # Could add logging here


# Type alias for progress callback
# Per Phase 6 Spec Section 7.1
ProgressCallback = callable  # Callable[[Path, str, int, int, float], None]


# Type alias for credential provider callback
# Per Phase 6 Spec Section 6.1
CredentialProviderCallback = callable  # Callable[[int], Credentials]
