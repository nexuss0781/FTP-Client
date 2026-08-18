# M8 Error Cycles

## SHA-256 fixture correction

The first expected digest used a 31-byte shell payload, while the C++ fixture actually contained 30 bytes: one NUL between the two text segments and `0xff` as the final byte. The temporary artifact was inspected byte-for-byte, the correct digest was computed, and all diagnostics were removed from production code.

## Timeout classification

Plain TCP and TLS data transports previously collapsed receive/send deadline expirations into `FTP_ERR_NETWORK_RESET`. M8 maps EAGAIN/EWOULDBLOCK/ETIMEDOUT and OpenSSL WANT_READ/WANT_WRITE cases to `FTP_ERR_TIMEOUT`, allowing the transfer loop to distinguish a bounded stall from a peer reset.

## Cancellation session contamination

Closing a data socket during RETR or STOR can leave a final transfer reply pending on the control channel. M8 cancellation and stall exits shut down and replace the control worker session instead of leaving that reply in the queue. The caller must reconnect before another transfer on the reset session.

## Retry-policy cancellation loop

The existing retry classifier treated unknown negative codes as transient. Without an explicit classification, `FTP_ERR_CANCELLED` could be retried after the user had requested cancellation. M8 classifies cancellation and integrity mismatch as terminal local outcomes.

## Truthful download options

A first draft exposed download resume fields before RETR REST was wired through the implementation. Those fields were removed from `ftp_download_options_t`; the extended download API currently exposes only the implemented SHA-256 and stall controls. Metadata-safe resume is implemented on the active upload retry path, where REST already existed and is integration-tested.

## Python ABI library-path mismatch

The Python ABI test selected the repository’s `build/` library while changes were initially compiled in `build-m8/`. The stale directory was removed and reconfigured from source, after which the Python ABI suite passed 43 assertions against the correct M8 shared library.

## Sanitizer CTest wrapper behavior

The ASan/UBSan log reported all ten tests passed in 36.29 seconds, but the shell wrapper did not emit its final line and remained unresponsive after the child process had exited. Process inspection confirmed no CTest or test child remained; the captured log is authoritative, and the stale wrapper was terminated without altering the test result.
