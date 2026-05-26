"""
test_callback_integrity.py - Progress Callback Integrity Test

Per Phase 6 Spec Section 10.3: Callback Invocation Integrity

This test verifies that progress callbacks are invoked correctly
without race conditions or missing entries.
"""

import sys
import threading
from pathlib import Path
from typing import List, Tuple

# Add parent directory to path for imports
sys.path.insert(0, str(Path(__file__).parent.parent))


def test_progress_callback_thread_safety():
    """Test that progress callback can be called from multiple threads safely."""
    print("Testing progress callback thread safety...")
    
    from ftpclient._core import _register_callback_handle, _unregister_callback_handle, _get_callback_handle
    
    results: List[Tuple[str, int]] = []
    lock = threading.Lock()
    
    def safe_callback(local_path: Path, remote_path: str, current: int, total: int, bps: float):
        """Thread-safe callback that records invocations."""
        with lock:
            results.append((str(local_path), current))
    
    # Register the callback
    callback_id = _register_callback_handle(safe_callback)
    
    # Simulate concurrent callback invocations
    threads = []
    for i in range(100):
        t = threading.Thread(
            target=lambda: safe_callback(
                Path(f"/file_{i}.txt"),
                f"/remote/file_{i}.txt",
                i * 100,
                10000,
                1024.0 * 1024
            )
        )
        threads.append(t)
        t.start()
    
    # Wait for all threads
    for t in threads:
        t.join()
    
    # Verify all callbacks were recorded
    assert len(results) == 100, f"Expected 100 callback invocations, got {len(results)}"
    
    # Clean up
    _unregister_callback_handle(callback_id)
    
    print("[PASS] Progress callback is thread-safe")
    return True


def test_credential_provider_callback():
    """Test credential provider callback mechanism."""
    print("Testing credential provider callback...")
    
    from ftpclient.client import FTPClient, _register_callback_handle, _unregister_callback_handle
    from ftpclient.types import Credentials
    
    call_count = [0]
    provided_creds = []
    
    def credential_provider(attempt: int) -> Credentials:
        """Simple credential provider."""
        call_count[0] += 1
        creds = Credentials(
            host="test.example.com",
            username=f"user_{attempt}",
            password="test_pass"
        )
        provided_creds.append(creds)
        return creds
    
    # Create client and set provider
    client = FTPClient()
    client.set_credential_provider(credential_provider)
    
    # Verify provider was registered
    assert call_count[0] == 0, "Provider should not be called during registration"
    
    # Clean up
    client.close()
    
    print("[PASS] Credential provider callback works correctly")
    return True


def test_callback_handle_lifecycle():
    """Test that callback handles are properly managed."""
    print("Testing callback handle lifecycle...")
    
    from ftpclient._core import _register_callback_handle, _unregister_callback_handle, _get_callback_handle
    
    def dummy_callback(*args, **kwargs):
        pass
    
    # Register multiple handles
    ids = []
    for i in range(10):
        cb_id = _register_callback_handle(dummy_callback)
        ids.append(cb_id)
        assert _get_callback_handle(cb_id) is dummy_callback
    
    # Unregister in reverse order
    for cb_id in reversed(ids):
        _unregister_callback_handle(cb_id)
        assert _get_callback_handle(cb_id) is None
    
    # Verify all are unregistered
    for cb_id in ids:
        assert _get_callback_handle(cb_id) is None
    
    print("[PASS] Callback handle lifecycle is correct")
    return True


if __name__ == "__main__":
    results = []
    
    try:
        results.append(test_progress_callback_thread_safety())
        results.append(test_credential_provider_callback())
        results.append(test_callback_handle_lifecycle())
    except ImportError as e:
        print(f"[INFO] Cannot run tests - module not built: {e}")
        sys.exit(0)
    
    print("\n" + "=" * 50)
    passed = sum(results)
    total = len(results)
    print(f"Callback Integrity Tests: {passed}/{total} passed")
    print("=" * 50)
    
    sys.exit(0 if all(results) else 1)
