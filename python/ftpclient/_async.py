"""
ftpclient._async - Async wrapper for FTPClient using ThreadPoolExecutor

Per Phase 6 Spec Section 8: Async Wrapper (Optional Facade)

This module provides asyncio-compatible wrappers around the synchronous
FTPClient. This is NOT true async I/O - it uses a thread pool to run
blocking operations without blocking the event loop.
"""

import asyncio
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Optional, Callable, Any

from ftpclient.client import FTPClient
from ftpclient.types import Credentials, UploadOptions, UploadResult


class AsyncFTPClient:
    """
    Async-compatible FTP client using thread pool executor.
    
    Per Phase 6 Spec Section 8.1: asyncio Integration
    
    This is a facade over the synchronous FTPClient. Operations run in
    a thread pool to avoid blocking the asyncio event loop.
    
    Example usage:
        async with AsyncFTPClient() as client:
            await client.connect(Credentials(host="ftp.example.com"))
            result = await client.upload_directory("/local", "/remote")
    """
    
    def __init__(self, max_workers: int = 4):
        """
        Initialize async FTP client.
        
        Args:
            max_workers: Maximum number of worker threads for the executor
        """
        self._sync_client: Optional[FTPClient] = None
        self._executor = ThreadPoolExecutor(max_workers=max_workers)
        self._loop: Optional[asyncio.AbstractEventLoop] = None
    
    async def __aenter__(self) -> 'AsyncFTPClient':
        """Async context manager entry."""
        self._loop = asyncio.get_event_loop()
        self._sync_client = FTPClient()
        return self
    
    async def __aexit__(self, exc_type, exc_val, exc_tb) -> None:
        """Async context manager exit."""
        await self.close()
        if self._executor:
            self._executor.shutdown(wait=True)
    
    @property
    def is_connected(self) -> bool:
        """Check if the underlying client is connected."""
        if self._sync_client is None:
            return False
        return self._sync_client.is_connected
    
    def _run_in_executor(self, func: Callable, *args) -> asyncio.Future:
        """Run a synchronous function in the thread pool executor."""
        if self._loop is None:
            self._loop = asyncio.get_event_loop()
        return self._loop.run_in_executor(self._executor, func, *args)
    
    async def connect(self, credentials: Credentials) -> None:
        """
        Establish connection to the FTP server (async).
        
        Args:
            credentials: Connection credentials
        """
        if self._sync_client is None:
            self._sync_client = FTPClient()
        
        await self._run_in_executor(self._sync_client.connect, credentials)
    
    async def disconnect(self) -> None:
        """Disconnect from the FTP server (async)."""
        if self._sync_client is not None:
            await self._run_in_executor(self._sync_client.disconnect)
    
    async def close(self) -> None:
        """Close the client and free all resources (async)."""
        if self._sync_client is not None:
            await self._run_in_executor(self._sync_client.close)
            self._sync_client = None
    
    async def ping(self) -> bool:
        """
        Check connection health (async).
        
        Returns:
            True if connected and responsive
        """
        if self._sync_client is None:
            return False
        return await self._run_in_executor(self._sync_client.ping)
    
    async def upload_directory(
        self,
        local_path: str,
        remote_path: str,
        options: Optional[UploadOptions] = None,
        progress: Optional[Callable[[Path, str, int, int, float], None]] = None
    ) -> UploadResult:
        """
        Upload a directory tree to the FTP server (async).
        
        Args:
            local_path: Local directory path
            remote_path: Remote directory path
            options: Upload options
            progress: Progress callback (called from worker thread)
            
        Returns:
            UploadResult with transfer statistics
        """
        if self._sync_client is None:
            raise RuntimeError("Client not initialized. Call connect() first.")
        
        # Note: Progress callbacks are invoked from C++ worker threads,
        # not the main event loop thread. Users should ensure their
        # callbacks are thread-safe.
        return await self._run_in_executor(
            self._sync_client.upload_directory,
            local_path,
            remote_path,
            options,
            progress
        )
    
    async def set_buffer_size(self, size_bytes: int) -> None:
        """Set internal buffer size (async)."""
        if self._sync_client is None:
            raise RuntimeError("Client not initialized")
        await self._run_in_executor(self._sync_client.set_buffer_size, size_bytes)
    
    async def set_timeout_connect_ms(self, ms: int) -> None:
        """Set connection timeout (async)."""
        if self._sync_client is None:
            raise RuntimeError("Client not initialized")
        await self._run_in_executor(self._sync_client.set_timeout_connect_ms, ms)
    
    async def set_timeout_command_ms(self, ms: int) -> None:
        """Set command timeout (async)."""
        if self._sync_client is None:
            raise RuntimeError("Client not initialized")
        await self._run_in_executor(self._sync_client.set_timeout_command_ms, ms)
