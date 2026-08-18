# Milestone 1 Status

**Branch:** `feature/ftps-m1-control-session`  
**Status:** Complete for the scoped control-session milestone

## Implemented

The public C ABI now owns a real `ProtocolEngine` and delegates plain control lifecycle operations through it. M1 performs TCP connection, server greeting parsing, `USER`/`PASS` authentication, authenticated `NOOP`, and graceful `QUIT` shutdown. Credentials are stored only after the live session is authenticated and are purged during disconnect.

The control thread now reads the greeting before accepting queued commands, recognizes intermediate authentication replies, maps server failures through the published error taxonomy, applies plain-socket I/O deadlines, and joins worker threads after fatal protocol errors. The plain transport source is now part of the library target. The public capability mask exposes `FTP_CAP_CONTROL_FTP` and leaves TLS/data-transfer flags clear.

A loopback mock-server integration test exercises successful login, NOOP, QUIT, authentication failure, malformed greeting handling, and failed-session cleanup without using an external account or network service.

## Validation

A clean non-sanitized M1 build passed all five CTest targets: `abi_test`, `control_integration_test`, `header_purity_test`, `phase2_test`, and `phase5_test`. The Python ABI test passed 33 checks, the Python exception test passed 4/4, and the opt-in ASan/UBSan build passed all five CTest targets.

## Explicit boundary

M1 does not yet implement TLS negotiation, certificate verification through the public control factory, passive data channels, upload/download, directory transfer, retries, or transfer concurrency. Those remain the next FTPS milestones.
