"""
ftpclient.exceptions - Exception hierarchy for FTP client errors

Per Phase 6 Spec Section 5.3: Exception Hierarchy

C error codes map to typed Python exceptions:

FTPError (base)
├── FTPAuthError          # -3xx codes
├── FTPNetworkError       # -4xx codes (transient, retryable)
├── FTPProtocolError      # -5xx codes (server misbehavior)
├── FTPIOError            # -6xx codes (local or remote I/O)
├── FTPConfigError        # -2xx codes (invalid arguments)
└── FTPSystemError        # -1xx codes (OOM, system failure)
"""

from typing import Optional


class FTPError(Exception):
    """
    Base exception for all FTP client errors.
    
    Attributes:
        error_code: The raw C error code from the library
        message: Human-readable error message
    """
    
    def __init__(self, error_code: int, message: str = ""):
        self.error_code = error_code
        self.message = message
        super().__init__(message)
    
    def __str__(self) -> str:
        if self.message:
            return f"FTPError [{self.error_code}]: {self.message}"
        return f"FTPError [{self.error_code}]"
    
    def __repr__(self) -> str:
        return f"{self.__class__.__name__}(error_code={self.error_code}, message={self.message!r})"


class FTPAuthError(FTPError):
    """
    Authentication errors (-3xx codes).
    
    Raised when authentication fails, TLS is required but not provided,
    or certificate validation fails.
    """
    pass


class FTPNetworkError(FTPError):
    """
    Network/transport errors (-4xx codes).
    
    These are typically transient and may be retryable.
    Includes connection failures, timeouts, and network resets.
    """
    pass


class FTPProtocolError(FTPError):
    """
    Protocol/server errors (-5xx codes).
    
    Raised when the server violates the FTP protocol or denies an operation.
    """
    pass


class FTPIOError(FTPError):
    """
    I/O errors (-6xx codes).
    
    Includes local filesystem errors and remote transfer errors.
    """
    pass


class FTPConfigError(FTPError):
    """
    Configuration/argument errors (-2xx codes).
    
    Raised when invalid arguments are provided or the client is in an
    invalid state for the requested operation.
    """
    pass


class FTPSystemError(FTPError):
    """
    System/resource errors (-1xx codes).
    
    Raised for out-of-memory conditions and other system-level failures.
    """
    pass
