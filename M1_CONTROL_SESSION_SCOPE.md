# Milestone 1: Real Control Session

## Scope

M1 wires the public C ABI to a real serialized control session over the existing `PlainTransport` seam. It establishes the protocol lifecycle required before FTPS TLS and data-channel work: TCP connect, server greeting, `USER`/`PASS` authentication, `NOOP` health check, graceful `QUIT`, deterministic state transitions, and transport error mapping.

M1 does not implement file upload/download, passive data channels, directory traversal over the network, retries, transfer concurrency, or TLS negotiation. The transport factory remains the seam for the next FTPS milestone. A plain control capability is advertised only after loopback integration tests prove it through the exported API.

## Deliverables

The public facade will own one `ProtocolEngine`, delegate connection lifecycle calls to it, preserve credentials only for the live session, and purge them on disconnect. The client will return server-derived error codes rather than claiming success for simulated behavior. `ftp_get_capabilities()` will expose the new control-session capability and will continue to omit TLS/data-transfer capabilities until those paths are real.

The protocol layer will map socket precondition failures to the published taxonomy, use configured command timeouts, and ensure failed greeting/authentication attempts leave the engine disconnected. The test suite will include a loopback mock FTP server that exercises greeting, login, NOOP, QUIT, authentication failure, malformed greeting, and disconnect cleanup through the C ABI.

## Acceptance gates

M1 is complete only when a clean non-sanitized build, the existing CTest suite, the loopback control integration tests, the Python ABI test, and the opt-in ASan/UBSan suite pass. No M1 test may require an external FTP account or internet access.
