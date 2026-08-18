# M11 Design Notes: Capability Negotiation and Server Integrity

**Project:** FTP-Client  
**Milestone:** M11  
**Branch:** `feature/ftps-m11-capability-integrity`  
**Author:** Manus AI  
**Status:** Implementation and validation complete; merge authorization pending

## Purpose and scope

M11 adds a truthful, session-scoped capability contract for servers that advertise `FEAT`, together with accessors for `MDTM` and `HASH`. The implementation is deliberately conservative: a capability is marked supported only when the server advertises it, unsupported feature responses remain recoverable, and all new public APIs preserve the existing C ABI error model.

The milestone also adds an explicit local-versus-remote verification operation. It computes a local SHA-256 digest using the existing OpenSSL EVP helper and compares it with a validated server-side `HASH` response. The operation name identifies both provenance sources, and a mismatch returns `FTP_ERR_INTEGRITY` rather than being represented as an indistinguishable generic transfer failure.

## Design overview

| Layer | M11 responsibility | Compatibility boundary |
| --- | --- | --- |
| Reply parsing | Preserve the complete multiline FEAT reply in an owned `full_message` string. | Existing single-line `message` behavior remains unchanged. |
| Command state | Add authenticated-state `HASH` command support and map the `HASH` verb. | Existing command sequencing and state transitions are preserved. |
| Control thread | Propagate complete replies and classify FEAT, MDTM, and HASH 4xx responses as recoverable. | Unsupported server features do not poison the control session. |
| Protocol engine | Parse FEAT tokens, cache capabilities per session, query MDTM/HASH, and compare local SHA-256 with remote HASH. | Cache is reset on disconnect and aborted control sessions. |
| C ABI | Expose a size-gated capability struct, MDTM/HASH output functions, and explicit verification. | Null handles, invalid states, short buffers, and invalid arguments return established error codes. |
| Python bindings | Provide low-level cffi wrappers, high-level `FTPClient` methods, async facade methods, dataclass properties, and stubs. | Python exceptions continue to derive from the existing error-band mapping. |

## FEAT parsing and capability cache

The control reply parser retains the final status line in `message` and concatenates every complete line into `full_message`. The protocol engine splits `full_message` on line boundaries, trims whitespace, converts feature tokens to uppercase, and recognizes `SIZE`, `MDTM`, and `HASH`. HASH algorithm lists accept both whitespace-separated and semicolon-separated forms, including common `SHA1`/`SHA-1` and `SHA256`/`SHA-256` spellings.

The first capability-dependent operation sends one `FEAT` probe. The resulting `ServerCapabilities` value is retained in the protocol session and reused by subsequent operations. A disconnect or control-session abort clears both the value and its loaded flag, preventing stale capabilities from crossing sessions.

## Public C ABI contract

The capability structure is append-friendly and guarded by its caller-supplied `struct_size` field. The implementation clears only the caller-provided region, writes only fields whose offsets fit within that region, and never writes beyond the supplied size.

| Field | Type | Meaning |
| --- | --- | --- |
| `struct_size` | `uint32_t` | Size of the caller-visible structure region. |
| `feat_supported` | `int32_t` | Whether FEAT completed successfully and supplied a usable reply. |
| `size_supported` | `int32_t` | Whether the server advertised SIZE. |
| `mdtm_supported` | `int32_t` | Whether the server advertised MDTM. |
| `hash_supported` | `int32_t` | Whether the server advertised HASH. |
| `hash_algorithms` | `uint32_t` | Bitmask of `FTP_HASH_ALG_*` algorithm flags. |

The M11 operation set is summarized below.

| Function | Success result | Important failure results |
| --- | --- | --- |
| `ftp_get_server_capabilities` | Populates the capability structure. | `FTP_ERR_INVALID_HANDLE`, `FTP_ERR_INVALID_ARGUMENT`, `FTP_ERR_INVALID_STATE`, or server/protocol status. |
| `ftp_get_remote_file_mdtm` | Copies the validated MDTM text into a bounded output buffer. | `FTP_ERR_NOT_IMPLEMENTED` when unsupported, `FTP_ERR_INVALID_ARGUMENT` for insufficient output space. |
| `ftp_get_remote_file_hash` | Copies a validated hexadecimal digest into a bounded output buffer. | `FTP_ERR_NOT_IMPLEMENTED` for an unadvertised algorithm, `FTP_ERR_PROTOCOL` for malformed digest replies. |
| `ftp_verify_local_file_with_remote_hash` | Returns `FTP_OK` when local and remote SHA-256 values match. | `FTP_ERR_LOCAL_IO`, `FTP_ERR_INTEGRITY`, or the underlying capability/HASH error. |

## Integrity verification boundary

The explicit verification API currently supports SHA-256 because the existing local integrity subsystem provides a production-tested streaming SHA-256 implementation. Requests for MD5, SHA-1, or SHA-512 return `FTP_ERR_NOT_IMPLEMENTED` rather than silently applying the wrong local algorithm. This is an intentional truthfulness boundary; future algorithm support can be added by extending the local helper and the verification contract together.

The server digest parser accepts a response containing a digest token alongside algorithm or path text, validates that the token is hexadecimal and of an accepted digest length, and normalizes it to lowercase. Local SHA-256 output is already lowercase. A mismatch is non-retryable and maps to `FTP_ERR_INTEGRITY`, consistent with the resilience policy’s treatment of integrity failures.

## Python surface

The cffi layer derives declarations from the public C header, constructs the capability structure, and exposes the four M11 operations. `ServerCapabilities` is an immutable dataclass with `supports_sha256` and `supports_sha512` convenience properties. `FTPClient` exposes `server_capabilities()`, `remote_file_mdtm()`, `remote_file_hash()`, and `verify_local_file_with_remote_hash()`. `AsyncFTPClient` delegates the same methods through its existing thread-pool facade, and the package stub declares all methods.

## Validation strategy

M11 uses a loopback control server that emits a multiline FEAT reply, advertises SIZE/MDTM/HASH with SHA-256 and SHA-512, returns deterministic MDTM and HASH responses, and counts FEAT requests. The test verifies one-probe caching, output-buffer rejection, successful local verification, mismatch reporting, and unsupported local algorithm behavior. The complete result is recorded in the milestone status document.
