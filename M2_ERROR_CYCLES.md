# M2 Error Cycles

## Explicit FTPS sequencing

The first AUTH TLS implementation sent `AUTH TLS` before consuming the server greeting. The loopback server correctly waited for the greeting, so the client received the 220 banner while expecting 234 and returned a connection error. M2 was corrected to parse the initial greeting, then send AUTH TLS, complete the TLS handshake, and continue with PBSZ 0 and PROT P.

## OpenSSL context ownership

OpenSSL 3 in the build environment did not expose `SSL_CTX_dup`. The custom-CA implementation was changed to create and initialize an owned per-connection client context instead of mutating the process-wide shared context.

## Localhost address family

The initial local FTPS test selected an IPv6 localhost result while the mock server listened on IPv4. PlainTransport now prefers an IPv4 result when both families are available and retains IPv6 as fallback.

## Test contract updates

The M1 ABI tests still expected TLS to be unavailable after explicit FTPS became real. They were updated to assert the M2 FTPS capability and use implicit FTPS as the deterministic not-implemented path.
