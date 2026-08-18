# M2 Status — Explicit FTPS Control Session

## Scope

M2 implements production-grade explicit FTPS control-channel establishment under [RFC 4217](https://www.rfc-editor.org/rfc/rfc4217), including `AUTH TLS`, TLS handshake, `PBSZ 0`, and `PROT P`. It also adds certificate-chain and hostname verification suitable for an application that supplies a CA bundle and expected hostname.

## Delivered

The public C ABI now owns a real explicit-FTPS control session through the protocol engine and reports `FTP_CAP_TLS` only for the tested explicit mode. TLS uses an owned OpenSSL client context, optional per-connection CA-bundle loading, SNI, and OpenSSL-native hostname verification via `SSL_set1_host()`. PlainTransport transfers the connected control socket safely into TlsTransport, and the TLS transport applies connection and I/O timeouts and closes its socket during shutdown.

The explicit negotiation sequence is validated as greeting, `AUTH TLS`, 234 reply, TLS handshake, `PBSZ 0`, `PROT P`, and post-upgrade control operation. Implicit FTPS remains explicitly unsupported and returns `FTP_ERR_NOT_IMPLEMENTED`.

## Security tests

The loopback FTPS integration suite covers a trusted hostname, hostname mismatch, untrusted certificate chain, and AUTH TLS rejection. The fixture certificate contains the `localhost` DNS SAN only, so connecting by `127.0.0.1` does not accidentally bypass hostname verification.

## Validation

The clean Debug build passes all six registered CTest tests. The Python ABI suite passes 33 assertions, the Python exception suite passes 4/4 checks, and Python bytecode compilation succeeds. The clean ASan/UBSan build also passes all six registered CTest tests with leak detection enabled.

## Deliberate boundary for M3

Protected data transfer is not advertised yet. M3 will implement passive-channel negotiation, TLS data-socket protection, `STOR`, final-reply validation, zero-byte files, and accurate per-file result aggregation.
