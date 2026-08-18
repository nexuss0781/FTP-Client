# M10 Status — Capability-Aware Transfer Hardening

## Delivered

M10 hardens the M9 transfer surface against hostile remote metadata and optional-command variability. MLSD file and directory names now reject empty, dot, absolute, separator-bearing, and drive-like path components. The recursive directory walker additionally checks weakly canonical candidate paths against the canonical local root, including existing symlink redirection, before creating or publishing files.

SIZE is now treated as an optional resume aid. Unsupported 500, 502, 504, and 550 replies remain recoverable on the authenticated control session, allowing RETR to fall back to a fresh transfer. The integration fixture returns 502 for the first SIZE probe and verifies that the following transfer succeeds.

Resume metadata is now written through a temporary sidecar and replaced only after flush and close. Metadata-safe resume is the internal and Python default. The C ABI provides an explicit tail-only `resume_allow_unverified` field for callers that knowingly require legacy unverified continuation.

Directory downloads now support exact remote-path keyed SHA-256 manifests through `ftp_download_digest_t`. A directory-wide file digest without a manifest is rejected, preventing one digest from being incorrectly applied to every child. Python exposes the same contract through `DownloadDigest` and `DownloadOptions.file_digests`.

## Focused coverage

M10 tests cover traversal-shaped MLSD names, absolute and separator-bearing names, drive-like names, existing symlink escape attempts, per-file digest verification, ambiguous digest rejection, unsupported SIZE recovery, metadata-bound REST continuation, atomic sidecar cleanup, and the M9 regression suite.

## Validation

The final clean Debug build passed all twelve CTest tests. The Python ABI suite passed 45/45 assertions, the exception suite passed 4/4, the expanded type suite passed 6/6, Python bytecode compilation passed, and `git diff --check` passed.

The final clean ASan/UBSan run with leak detection passed all twelve CTest tests in 36.70 seconds. The captured CTest output contained no AddressSanitizer or UndefinedBehaviorSanitizer diagnostics. The log used color escape sequences, so final verification stripped ANSI color codes before checking the pass summary.

## Scope boundary

M10 does not yet implement FEAT/HASH/MDTM capability negotiation, parallel directory scheduling, or durable upload manifests. Those remain future milestones and are not advertised by the current capability surface.
