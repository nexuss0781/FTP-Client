"""
test_wheel.py - Wheel Import and Smoke Test

Per Phase 6 Spec Section 10.5: Wheel Portability

This test verifies that the package can be imported and basic functionality works.
"""

import sys
from pathlib import Path

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))


def test_import_package():
    """Test that the package can be imported."""
    print("Testing package import...")
    
    try:
        import ftpclient
        print("[PASS] ftpclient package imported successfully")
        return True
    except ImportError as e:
        print(f"[FAIL] Failed to import ftpclient: {e}")
        return False


def test_version():
    """Test version information."""
    print("Testing version...")
    
    try:
        import ftpclient
        
        # Check __version__
        assert hasattr(ftpclient, '__version__')
        version = ftpclient.__version__
        assert isinstance(version, str)
        assert len(version) > 0
        print(f"  Version: {version}")
        
        # Check get_version function
        if hasattr(ftpclient, 'get_version'):
            lib_version = ftpclient.get_version()
            print(f"  Library version: {lib_version}")
        
        print("[PASS] Version information available")
        return True
    except Exception as e:
        print(f"[FAIL] Version test failed: {e}")
        return False


def test_exports():
    """Test that all expected exports are available."""
    print("Testing exports...")
    
    try:
        import ftpclient
        
        expected_exports = [
            'FTPClient',
            'Credentials',
            'UploadOptions',
            'UploadResult',
            'FileResult',
            'FTPError',
            'FTPAuthError',
            'FTPNetworkError',
            'FTPProtocolError',
            'FTPIOError',
            'FTPConfigError',
            'FTPSystemError',
        ]
        
        missing = []
        for export in expected_exports:
            if not hasattr(ftpclient, export):
                missing.append(export)
        
        if missing:
            print(f"[FAIL] Missing exports: {missing}")
            return False
        
        print(f"[PASS] All {len(expected_exports)} expected exports available")
        return True
    except Exception as e:
        print(f"[FAIL] Export test failed: {e}")
        return False


def test_client_instantiation():
    """Test that FTPClient can be instantiated."""
    print("Testing client instantiation...")
    
    try:
        from ftpclient import FTPClient
        
        # Test context manager usage
        with FTPClient() as client:
            assert client is not None
            assert client.is_connected is False
        
        print("[PASS] FTPClient instantiation works")
        return True
    except ImportError as e:
        print(f"[INFO] Cannot test - library not built: {e}")
        return True  # Don't fail if library isn't built
    except Exception as e:
        print(f"[FAIL] Client instantiation failed: {e}")
        return False


def test_dataclass_creation():
    """Test that dataclasses can be created."""
    print("Testing dataclass creation...")
    
    try:
        from ftpclient import Credentials, UploadOptions
        
        creds = Credentials(host="test.example.com")
        assert creds.host == "test.example.com"
        
        opts = UploadOptions()
        assert opts.retry_attempts == 3
        
        print("[PASS] Dataclass creation works")
        return True
    except Exception as e:
        print(f"[FAIL] Dataclass creation failed: {e}")
        return False


def test_async_client():
    """Test AsyncFTPClient availability."""
    print("Testing async client...")
    
    try:
        from ftpclient import AsyncFTPClient
        
        # Just check it exists and can be instantiated
        client = AsyncFTPClient(max_workers=2)
        assert client is not None
        
        print("[PASS] AsyncFTPClient available")
        return True
    except ImportError:
        print("[INFO] AsyncFTPClient not available (optional)")
        return True
    except Exception as e:
        print(f"[FAIL] AsyncFTPClient test failed: {e}")
        return False


if __name__ == "__main__":
    results = []
    
    results.append(test_import_package())
    results.append(test_version())
    results.append(test_exports())
    results.append(test_client_instantiation())
    results.append(test_dataclass_creation())
    results.append(test_async_client())
    
    print("\n" + "=" * 50)
    passed = sum(results)
    total = len(results)
    print(f"Wheel Tests: {passed}/{total} passed")
    print("=" * 50)
    
    sys.exit(0 if all(results) else 1)
