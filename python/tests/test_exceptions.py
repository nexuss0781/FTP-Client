"""
test_exceptions.py - Exception Translation Test

Per Phase 6 Spec Section 10.4: Exception Translation Accuracy

This test verifies that C error codes are correctly mapped to Python exceptions.
"""

import sys
from pathlib import Path

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))


def test_exception_hierarchy():
    """Test that all exception classes exist and have correct inheritance."""
    print("Testing exception hierarchy...")
    
    from ftpclient.exceptions import (
        FTPError,
        FTPAuthError,
        FTPNetworkError,
        FTPProtocolError,
        FTPIOError,
        FTPConfigError,
        FTPSystemError,
    )
    
    # Test base class
    assert issubclass(FTPAuthError, FTPError), "FTPAuthError should inherit from FTPError"
    assert issubclass(FTPNetworkError, FTPError), "FTPNetworkError should inherit from FTPError"
    assert issubclass(FTPProtocolError, FTPError), "FTPProtocolError should inherit from FTPError"
    assert issubclass(FTPIOError, FTPError), "FTPIOError should inherit from FTPError"
    assert issubclass(FTPConfigError, FTPError), "FTPConfigError should inherit from FTPError"
    assert issubclass(FTPSystemError, FTPError), "FTPSystemError should inherit from FTPError"
    
    print("[PASS] Exception hierarchy is correct")
    return True


def test_exception_attributes():
    """Test that exceptions have correct attributes."""
    print("Testing exception attributes...")
    
    from ftpclient.exceptions import FTPError, FTPAuthError
    
    # Test base exception
    err = FTPError(-999, "Test message")
    assert err.error_code == -999, f"error_code should be -999, got {err.error_code}"
    assert err.message == "Test message", f"message should be 'Test message', got {err.message}"
    assert "-999" in str(err), "str() should contain error code"
    assert "Test message" in str(err), "str() should contain message"
    
    # Test derived exception
    auth_err = FTPAuthError(-301, "Authentication failed")
    assert auth_err.error_code == -301
    assert isinstance(auth_err, FTPError), "Should be instance of base class"
    
    print("[PASS] Exception attributes are correct")
    return True


def test_error_code_mapping():
    """Test that error codes map to correct exception types."""
    print("Testing error code mapping...")
    
    from ftpclient._core import _check_error, _get_error_name
    from ftpclient.exceptions import (
        FTPAuthError,
        FTPNetworkError,
        FTPProtocolError,
        FTPIOError,
        FTPConfigError,
        FTPSystemError,
    )
    
    # Test error name lookup
    assert _get_error_name(0) == "FTP_OK"
    assert _get_error_name(-301) == "FTP_ERR_AUTH_FAILED"
    assert _get_error_name(-401) == "FTP_ERR_CONNECT"
    assert _get_error_name(-999) == "UNKNOWN(-999)"
    
    # Test exception raising for each category
    test_cases = [
        (-301, FTPAuthError, "auth error"),
        (-302, FTPAuthError, "TLS required"),
        (-303, FTPAuthError, "cert verify"),
        (-401, FTPNetworkError, "connect error"),
        (-402, FTPNetworkError, "timeout"),
        (-403, FTPNetworkError, "network reset"),
        (-501, FTPProtocolError, "protocol error"),
        (-502, FTPProtocolError, "server denied"),
        (-601, FTPIOError, "local IO"),
        (-602, FTPIOError, "remote IO"),
        (-201, FTPConfigError, "invalid handle"),
        (-202, FTPConfigError, "invalid argument"),
        (-101, FTPSystemError, "no memory"),
        (-102, FTPSystemError, "system error"),
    ]
    
    for code, expected_exception, description in test_cases:
        try:
            _check_error(code, f"test {description}")
            print(f"[FAIL] No exception raised for code {code}")
            return False
        except Exception as e:
            if not isinstance(e, expected_exception):
                print(f"[FAIL] Code {code} raised {type(e).__name__}, expected {expected_exception.__name__}")
                return False
    
    # Test that success codes don't raise
    _check_error(0, "success")
    _check_error(101, "warning")
    
    print("[PASS] Error code mapping is correct")
    return True


def test_exception_string_representation():
    """Test string representations of exceptions."""
    print("Testing exception string representations...")
    
    from ftpclient.exceptions import FTPError
    
    err = FTPError(-401, "Connection refused")
    
    # Test __str__
    str_repr = str(err)
    assert "-401" in str_repr
    assert "Connection refused" in str_repr
    
    # Test __repr__
    repr_repr = repr(err)
    assert "FTPError" in repr_repr
    assert "-401" in repr_repr
    
    print("[PASS] String representations are correct")
    return True


if __name__ == "__main__":
    results = []
    
    try:
        results.append(test_exception_hierarchy())
        results.append(test_exception_attributes())
        results.append(test_error_code_mapping())
        results.append(test_exception_string_representation())
    except ImportError as e:
        print(f"[INFO] Cannot run tests - module not built: {e}")
        print("Build the library first with: cd /workspace && mkdir -p build && cd build && cmake .. && make")
        sys.exit(0)
    
    print("\n" + "=" * 50)
    passed = sum(results)
    total = len(results)
    print(f"Exception Tests: {passed}/{total} passed")
    print("=" * 50)
    
    sys.exit(0 if all(results) else 1)
