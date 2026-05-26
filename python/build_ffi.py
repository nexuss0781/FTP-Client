#!/usr/bin/env python3
"""
build_ffi.py - cffi out-of-line build script for ftpclient

This script reads include/ftpclient.h and generates the _ftpclient C extension module.
The generated module is then vendored into the wheel alongside libftpclient.so.

Per Phase 6 Spec Section 3.1: Build-time vs Run-time
"""

import os
import sys
from pathlib import Path
from cffi import FFI


def get_header_content():
    """Read the C header file for cffi parsing."""
    header_path = Path(__file__).parent.parent / "include" / "ftpclient.h"
    if not header_path.exists():
        raise FileNotFoundError(f"Header not found: {header_path}")
    
    with open(header_path, 'r', encoding='utf-8') as f:
        return f.read()


def get_source_files():
    """Return list of C++ source files to compile into the extension."""
    src_dir = Path(__file__).parent.parent / "src"
    sources = []
    
    # Main implementation
    sources.append(str(src_dir / "ftpclient.cpp"))
    
    # Security module
    security_dir = src_dir / "security"
    sources.append(str(security_dir / "CredentialVault.cpp"))
    sources.append(str(security_dir / "TlsTransport.cpp"))
    sources.append(str(security_dir / "OpenSSLInit.cpp"))
    sources.append(str(security_dir / "SecretProvider.cpp"))
    
    # Transfer module
    transfer_dir = src_dir / "transfer"
    sources.append(str(transfer_dir / "BufferPool.cpp"))
    sources.append(str(transfer_dir / "ThreadPool.cpp"))
    sources.append(str(transfer_dir / "ResultAggregator.cpp"))
    sources.append(str(transfer_dir / "TransferEngine.cpp"))
    
    # Resilience module
    resilience_dir = src_dir / "resilience"
    sources.append(str(resilience_dir / "RetryPolicy.cpp"))
    sources.append(str(resilience_dir / "CircuitBreaker.cpp"))
    sources.append(str(resilience_dir / "StallDetector.cpp"))
    sources.append(str(resilience_dir / "IdempotencyClassifier.cpp"))
    sources.append(str(resilience_dir / "ResilienceController.cpp"))
    
    return sources


def build_ffi():
    """Build the cffi extension module."""
    ffi = FFI()
    
    # Parse the C header
    header_content = get_header_content()
    ffi.cdef(header_content)
    
    # Set up the source for the extension module
    # This creates a thin wrapper that links against libftpclient.so
    source_files = get_source_files()
    
    # Determine platform-specific settings
    if sys.platform == 'win32':
        libraries = ['ws2_32', 'libssl', 'libcrypto']
        extra_compile_args = ['/W4', '/permissive-', '-D_CRT_SECURE_NO_WARNINGS']
        library_dirs = []
    elif sys.platform == 'darwin':
        libraries = ['ssl', 'crypto']
        extra_compile_args = ['-Wall', '-Wextra', '-fvisibility=hidden']
        library_dirs = []
    else:  # Linux
        libraries = ['pthread', 'ssl', 'crypto']
        extra_compile_args = ['-Wall', '-Wextra', '-fvisibility=hidden', '-fstack-protector-strong']
        library_dirs = []
    
    ffi.set_source(
        "_ftpclient",
        r"""
            // Include the header for the CFFI wrapper
            #include "ftpclient.h"
        """,
        sources=source_files,
        include_dirs=[str(Path(__file__).parent.parent / "include")],
        libraries=libraries,
        library_dirs=library_dirs,
        extra_compile_args=extra_compile_args,
        define_macros=[('FTP_BUILDING_DLL', None)],
    )
    
    # Compile the extension
    ffi.compile(verbose=True)


if __name__ == "__main__":
    build_ffi()
