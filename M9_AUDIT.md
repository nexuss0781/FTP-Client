# M9 Audit and Next Milestone

## Audit scope

The audit covered the merged M9 `main` branch, including the MLSD parser, passive listing data path, recursive `ftp_download_dir()` orchestration, RETR SIZE/REST continuation, resume sidecars, C ABI layout and exports, Python synchronous/asynchronous wrappers, and existing Debug and sanitizer test coverage.

## Audit summary

M9 is functionally complete for its tested contract, but four hardening issues should be addressed before adding parallel directory transfers or server-side integrity negotiation. The most important issue is local path confinement: the MLSD parser accepts arbitrary non-empty names, and `ftp_download_dir()` joins them directly to the local root. A malicious or compromised FTP server could return `../` or an absolute path and cause writes outside the requested destination. The current tests cover spaces and ordinary names but do not cover hostile names.

The second issue is SIZE capability handling. When RETR resume is enabled, `get_remote_file_size()` sends SIZE. ControlThread treats only a 550 SIZE response as recoverable; common 500 or 502 “unsupported command” responses move the control state to ERROR. M9 therefore cannot safely fall back to a fresh download against every server that lacks SIZE. A capability-aware probe and recoverable 500/502 handling are required.

The third issue is resume-sidecar safety. Metadata-safe mode is opt-in, and ordinary resume can continue any partial file when remote SIZE is larger, without a cryptographic binding to the original remote object. This is documented and not an API lie, but production defaults should favor conservative metadata binding. Sidecar writes are also repeated during streaming and should be made crash-consistent through atomic replacement.

The fourth issue is directory-level integrity semantics. `ftp_download_dir()` forwards one optional expected SHA-256 value to every child file. That is only meaningful for a one-file operation and can cause every child to fail when the supplied digest is not identical to each file. M10 should replace this ambiguous directory option with a per-file manifest or explicitly reject a directory-wide single-file digest.

| Finding | Severity | Current state | Required action |
|---|---|---|---|
| MLSD names are not confined to the local root | High | Names are joined directly to `local_root` | Reject absolute, empty, `.`/`..`, separator-bearing, and platform-reserved traversal components; verify normalized path remains under root |
| SIZE 500/502 can wedge the control session | High | Only SIZE/550 is recoverable | Add FEAT/SIZE capability probing and classify unsupported SIZE replies as recoverable |
| Ordinary RETR resume is less conservative than metadata-safe mode | Medium | Safe mode is opt-in | Make safe metadata binding the default for new callers and preserve explicit legacy behavior only when requested |
| Resume sidecar updates are not atomic | Medium | Direct truncating writes during streaming | Write a temporary metadata file, flush/close, then atomically rename |
| One digest is forwarded to all directory children | Medium | Directory API accepts file-oriented `expected_sha256` | Add a per-file digest manifest or reject `expected_sha256` for directory downloads |
| Server-side HASH/FEAT/MDTM are not implemented | Planned scope | Correctly not advertised | Add capability negotiation only after the safety items above are covered |

## Quality evidence

The merged M9 branch passed 12/12 clean Debug CTest tests, 12/12 ASan/UBSan tests with leak detection, the Python ABI suite at 45/45 assertions, the Python exception suite at 4/4, the Python type suite at 5/5, Python bytecode compilation, and whitespace validation. The audit also confirmed that the Linux shared library exports the newly added download, cancellation, remote-filesystem, and directory-download symbols. The MSVC definition list was synchronized with the same public symbols.

## Next milestone: M10 — Capability-Aware Transfer Hardening

M10 should be a security and protocol-hardening milestone rather than another breadth milestone. Its goal is to make remote metadata and resume behavior safe under hostile names, unsupported optional commands, interrupted writes, and per-file integrity requirements.

M10 acceptance criteria are: every MLSD-derived local path is confined beneath the caller’s destination root; hostile names and traversal attempts are rejected with deterministic errors; SIZE 500, 502, and malformed replies are recoverable without killing an authenticated control session; FEAT capability probing is cached per session; resume metadata is atomically committed and bound to remote path, remote size, modification fact when available, expected digest, and confirmed byte count; directory downloads accept a per-file digest manifest or clearly reject ambiguous digest input; and all of these cases have plain FTP, explicit FTPS, ABI, Python, and sanitizer coverage.

Only after these criteria pass should M10 add optional server-side HASH/MDTM verification. Parallel directory scheduling remains a later milestone because it depends on a stable, safe per-file manifest and capability contract.
