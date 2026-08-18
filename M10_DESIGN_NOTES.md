# M10 Design Notes — Capability-Aware Transfer Hardening

## Path confinement

MLSD file and directory names are treated as untrusted server data. The parser accepts ordinary names containing spaces but rejects empty names, `.`, `..`, absolute names, separator-bearing names, and drive-like names. The C ABI directory walker additionally compares weakly canonical candidate paths against the weakly canonical destination root before creating or writing anything, protecting against existing symlink redirection.

## Optional SIZE behavior

SIZE is an optional resume aid, not a prerequisite for RETR. Replies 500, 502, 504, and 550 are recoverable on the authenticated control session. A failed SIZE probe leaves the remote size unknown and the download proceeds from zero unless a validated resume state is available. A successful 213 response remains subject to strict numeric parsing.

## Resume safety

Metadata-safe resume is the internal and Python default. The sidecar binds the partial file to a version, remote path, remote SIZE, expected digest, and confirmed byte count. A caller that intentionally needs the legacy unverified behavior must set the explicit `resume_allow_unverified` ABI field. Sidecar updates are written to `<sidecar>.tmp`, flushed and closed, and then replaced into place.

## Directory integrity

A directory operation must not interpret one file-oriented expected digest as the digest of every child. M10 adds `ftp_download_digest_t` entries keyed by exact remote path. When a manifest is supplied, every downloaded regular file must have a matching digest; a directory-wide `expected_sha256` without a manifest is rejected. Python exposes the same contract through `DownloadDigest` and `DownloadOptions.file_digests`.

## Scope boundary

M10 does not yet negotiate FEAT/HASH/MDTM capabilities, add parallel directory scheduling, or create durable upload manifests. Those features depend on this hardened path and metadata contract.
