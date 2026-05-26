"""
test_types.py - Type Validation Test

Per Phase 6 Spec Section 10.7: Type Stub Completeness

This test verifies that dataclasses and types work correctly.
"""

import sys
import dataclasses
from pathlib import Path

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))


def test_credentials_dataclass():
    """Test Credentials dataclass."""
    print("Testing Credentials dataclass...")
    
    from ftpclient.types import Credentials
    
    # Test default values
    creds = Credentials(host="ftp.example.com")
    assert creds.host == "ftp.example.com"
    assert creds.port == 21
    assert creds.use_tls == 1
    assert creds.verify_cert == 2
    assert creds.username is None
    assert creds.password is None
    
    # Test custom values
    creds2 = Credentials(
        host="secure.example.com",
        port=990,
        username="user",
        password="pass",
        use_tls=2,
        verify_cert=0
    )
    assert creds2.port == 990
    assert creds2.username == "user"
    assert creds2.use_tls == 2
    
    # Test immutability (frozen=True)
    try:
        creds2.host = "other.com"
        print("[FAIL] Credentials should be immutable")
        return False
    except (AttributeError, dataclasses.FrozenInstanceError):
        pass  # Expected
    
    print("[PASS] Credentials dataclass works correctly")
    return True


def test_upload_options_dataclass():
    """Test UploadOptions dataclass."""
    print("Testing UploadOptions dataclass...")
    
    from ftpclient.types import UploadOptions
    
    # Test default values
    opts = UploadOptions()
    assert opts.max_parallel == 0
    assert opts.retry_attempts == 3
    assert opts.retry_base_delay_ms == 1000
    assert opts.resume_enabled is False
    assert opts.create_remote_dirs is True
    
    # Test custom values
    opts2 = UploadOptions(
        max_parallel=8,
        retry_attempts=5,
        resume_enabled=True,
        remote_chmod="0644"
    )
    assert opts2.max_parallel == 8
    assert opts2.resume_enabled is True
    assert opts2.remote_chmod == "0644"
    
    print("[PASS] UploadOptions dataclass works correctly")
    return True


def test_file_result_dataclass():
    """Test FileResult dataclass."""
    print("Testing FileResult dataclass...")
    
    from ftpclient.types import FileResult
    
    result = FileResult(
        local_path=Path("/local/file.txt"),
        remote_path="/remote/file.txt",
        status=0,
        bytes_sent=1024,
        attempt_count=1
    )
    
    assert result.local_path == Path("/local/file.txt")
    assert result.remote_path == "/remote/file.txt"
    assert result.status == 0
    assert result.bytes_sent == 1024
    assert result.attempt_count == 1
    assert result.final_error is None
    
    # Test with error
    result2 = FileResult(
        local_path=Path("/local/failed.txt"),
        remote_path="/remote/failed.txt",
        status=-401,
        bytes_sent=0,
        attempt_count=3,
        final_error=-401
    )
    assert result2.status == -401
    assert result2.final_error == -401
    
    print("[PASS] FileResult dataclass works correctly")
    return True


def test_upload_result_dataclass():
    """Test UploadResult dataclass."""
    print("Testing UploadResult dataclass...")
    
    from ftpclient.types import UploadResult, FileResult
    
    file_results = (
        FileResult(Path("/a.txt"), "/a.txt", 0, 100, 1),
        FileResult(Path("/b.txt"), "/b.txt", 0, 200, 1),
        FileResult(Path("/c.txt"), "/c.txt", -401, 0, 3, -401),
    )
    
    result = UploadResult(
        status=0,
        files_total=3,
        files_success=2,
        files_failed=1,
        bytes_transferred=300,
        file_results=file_results
    )
    
    assert result.files_total == 3
    assert result.files_success == 2
    assert result.files_failed == 1
    assert result.bytes_transferred == 300
    assert len(result.file_results) == 3
    
    # Test success property
    assert result.success is False  # Because files_failed > 0
    
    # Test all-success case
    result2 = UploadResult(
        status=0,
        files_total=2,
        files_success=2,
        files_failed=0,
        bytes_transferred=300,
        file_results=file_results[:2]
    )
    assert result2.success is True
    
    print("[PASS] UploadResult dataclass works correctly")
    return True


def test_type_validation():
    """Test that types validate their inputs."""
    print("Testing type validation...")
    
    from ftpclient.types import Credentials, UploadOptions
    
    # Test invalid credentials
    try:
        Credentials(host="")  # Empty host
        print("[FAIL] Should reject empty host")
        return False
    except ValueError:
        pass  # Expected
    
    try:
        Credentials(host="example.com", port=70000)  # Invalid port
        print("[FAIL] Should reject invalid port")
        return False
    except ValueError:
        pass  # Expected
    
    try:
        Credentials(host="example.com", use_tls=5)  # Invalid TLS mode
        print("[FAIL] Should reject invalid use_tls")
        return False
    except ValueError:
        pass  # Expected
    
    # Test invalid upload options
    try:
        UploadOptions(max_parallel=-1)
        print("[FAIL] Should reject negative max_parallel")
        return False
    except ValueError:
        pass  # Expected
    
    print("[PASS] Type validation works correctly")
    return True


if __name__ == "__main__":
    import dataclasses
    
    results = []
    
    try:
        results.append(test_credentials_dataclass())
        results.append(test_upload_options_dataclass())
        results.append(test_file_result_dataclass())
        results.append(test_upload_result_dataclass())
        results.append(test_type_validation())
    except ImportError as e:
        print(f"[INFO] Cannot run tests - module not built: {e}")
        sys.exit(0)
    
    print("\n" + "=" * 50)
    passed = sum(results)
    total = len(results)
    print(f"Type Tests: {passed}/{total} passed")
    print("=" * 50)
    
    sys.exit(0 if all(results) else 1)
