"""
ftpclient.utils - Utility functions and helpers

Per Phase 6 Spec Section 7.3: Thread Safety - TqdmProgressAdapter

This module provides convenience utilities like progress bar adapters.
"""

import logging
from pathlib import Path
from typing import Optional

try:
    from tqdm import tqdm
    _HAS_TQDM = True
except ImportError:
    _HAS_TQDM = False


logger = logging.getLogger(__name__)


class TqdmProgressAdapter:
    """
    Progress callback adapter using tqdm for terminal progress bars.
    
    Per Phase 6 Spec Section 7.3: Thread Safety
    
    This adapter creates a tqdm progress bar and updates it during
    file transfers. Note that tqdm is not inherently thread-safe,
    so this adapter uses a lock when updating from multiple threads.
    
    Usage:
        from ftpclient.utils import TqdmProgressAdapter
        
        with FTPClient() as client:
            client.connect(creds)
            result = client.upload_directory(
                "/local/path",
                "/remote/path",
                progress=TqdmProgressAdapter(desc="Uploading")
            )
    """
    
    def __init__(self, desc: str = "Transferring", unit: str = "B", unit_scale: bool = True):
        """
        Initialize the progress adapter.
        
        Args:
            desc: Description for the progress bar
            unit: Unit for the progress bar
            unit_scale: Whether to scale units automatically
        """
        if not _HAS_TQDM:
            raise ImportError("tqdm is required for TqdmProgressAdapter. Install with: pip install tqdm")
        
        self.desc = desc
        self.unit = unit
        self.unit_scale = unit_scale
        self._pbar: Optional[tqdm] = None
        self._current_file: Optional[str] = None
        self._lock = None  # Will be threading.Lock when needed
    
    def _ensure_lock(self):
        """Lazy-initialize the lock."""
        if self._lock is None:
            import threading
            self._lock = threading.Lock()
    
    def __call__(
        self,
        local_path: Path,
        remote_path: str,
        bytes_current: int,
        bytes_total: int,
        bytes_per_second: float
    ) -> None:
        """
        Progress callback invoked by the FTP client.
        
        This method is called from C++ worker threads, so it must be thread-safe.
        """
        self._ensure_lock()
        
        with self._lock:
            try:
                # Check if we're starting a new file
                file_key = str(local_path)
                if file_key != self._current_file:
                    # Close previous progress bar
                    if self._pbar is not None:
                        self._pbar.close()
                    
                    self._current_file = file_key
                    
                    # Create new progress bar for this file
                    self._pbar = tqdm(
                        total=bytes_total,
                        desc=f"{self.desc}: {local_path.name}",
                        unit=self.unit,
                        unit_scale=self.unit_scale,
                        leave=False
                    )
                
                # Update progress
                if self._pbar is not None:
                    self._pbar.n = bytes_current
                    self._pbar.refresh()
                    
                    # Add speed info
                    speed_mbps = (bytes_per_second / (1024 * 1024))
                    self._pbar.set_postfix_str(f"{speed_mbps:.2f} MB/s")
                    
            except Exception as e:
                # Log but don't propagate exceptions from callbacks
                logger.warning(f"Progress adapter error: {e}")
    
    def close(self) -> None:
        """Close any open progress bars."""
        self._ensure_lock()
        
        with self._lock:
            if self._pbar is not None:
                self._pbar.close()
                self._pbar = None
    
    def __enter__(self):
        return self
    
    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()


def format_bytes(bytes_value: int) -> str:
    """
    Format bytes as human-readable string.
    
    Args:
        bytes_value: Number of bytes
        
    Returns:
        Formatted string (e.g., "1.5 MB")
    """
    for unit in ['B', 'KB', 'MB', 'GB', 'TB']:
        if abs(bytes_value) < 1024.0:
            return f"{bytes_value:.1f} {unit}"
        bytes_value /= 1024.0
    return f"{bytes_value:.1f} PB"


def format_speed(bytes_per_second: float) -> str:
    """
    Format transfer speed as human-readable string.
    
    Args:
        bytes_per_second: Transfer speed in bytes per second
        
    Returns:
        Formatted string (e.g., "10.5 MB/s")
    """
    return f"{format_bytes(int(bytes_per_second))}/s"
