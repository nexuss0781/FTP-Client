"""
ftpclient - High-Performance FTP Client Library for Python

A C++17 native FTP client library with C ABI, exposed to Python via cffi.
Provides high-throughput directory uploads with fault resilience and credential security.
"""

from ftpclient._core import (
    # Core functions
    lib,
    ffi,
    _check_error,
    _get_error_name,
)
from ftpclient.types import (
    Credentials,
    UploadOptions,
    DownloadOptions,
    UploadResult,
    DownloadResult,
    FileResult,
)
from ftpclient.exceptions import (
    FTPError,
    FTPAuthError,
    FTPNetworkError,
    FTPProtocolError,
    FTPIOError,
    FTPConfigError,
    FTPNotImplementedError,
    FTPSystemError,
)
from ftpclient.client import FTPClient

# Optional async support
try:
    from ftpclient._async import AsyncFTPClient
    __all__ = [
        'FTPClient',
        'AsyncFTPClient',
        'Credentials',
        'UploadOptions',
        'DownloadOptions',
        'UploadResult',
        'DownloadResult',
        'FileResult',
        'FTPError',
        'FTPAuthError',
        'FTPNetworkError',
        'FTPProtocolError',
        'FTPIOError',
        'FTPConfigError',
        'FTPNotImplementedError',
        'FTPSystemError',
    ]
except ImportError:
    __all__ = [
        'FTPClient',
        'Credentials',
        'UploadOptions',
        'DownloadOptions',
        'UploadResult',
        'DownloadResult',
        'FileResult',
        'FTPError',
        'FTPAuthError',
        'FTPNetworkError',
        'FTPProtocolError',
        'FTPIOError',
        'FTPConfigError',
        'FTPNotImplementedError',
        'FTPSystemError',
    ]

__version__ = "0.1.0"
__author__ = "FTP Client Library Team"


def get_version() -> str:
    """Return the library version as a string."""
    version_packed = lib.ftp_get_version()
    major = (version_packed >> 24) & 0xFF
    minor = (version_packed >> 16) & 0xFF
    patch = (version_packed >> 8) & 0xFF
    return f"{major}.{minor}.{patch}"


__all__ += ['get_version']
