# M3 Error Cycles

## Control-thread detailed replies

The original control worker exposed only an integer status, which was insufficient to parse 227/229 passive replies or distinguish STOR’s preliminary 150 from its final 226. M3 added a detailed command result carrying the parsed reply code and message, plus a receive-only final-transfer command that preserves control-channel serialization.

## FTP preliminary reply classification

The existing `is_ftp_intermediate()` helper covered only 3xx replies. STOR’s valid 125/150 preliminary replies were therefore rejected as protocol errors before any data bytes were sent. M3 added explicit 1xx preliminary classification and accepts it during command execution.

## Passive fallback state

A server rejecting EPSV with 502 must not poison an authenticated control session. Passive-command errors now reset to AUTHENTICATED, allowing PASV fallback. The integration fixture verifies this path by rejecting EPSV and accepting PASV.

## Python ABI layout

The C ABI result structure had gained `file_result_count` and `file_results`, but the Python ctypes regression definition still modeled only the original counters. The Python test now declares `ftp_file_result_t` and the extended result pointer, and uses ctypes-correct null-pointer checks.

## Sanitizer runtime timing

The complete ASan/UBSan CTest suite takes materially longer than the normal suite because leak detection and instrumented loopback tests add startup and teardown cost. A bounded verbose run completed all seven tests successfully in 32.32 seconds.
