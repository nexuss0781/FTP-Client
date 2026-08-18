# M10 Error Cycles

## Untrusted MLSD names

The M9 audit found that parsed names were joined directly to the local destination. M10 added strict name validation for separator-bearing, absolute, dot, and drive-like names, then added a canonical root-confinement check in the recursive C ABI walker. Focused tests now reject traversal-shaped and absolute listing entries.

## SIZE 500/502 recovery

M9 only treated SIZE/550 as recoverable. A server that returned 502 for unsupported SIZE could wedge the control state before RETR. M10 classifies 500, 502, 504, and 550 SIZE replies as recoverable; RETR then falls back to a fresh transfer. The integration fixture now returns 502 on the first SIZE request and verifies the subsequent transfer succeeds.

## Atomic sidecar replacement

M9 wrote the resume sidecar directly in place. M10 writes `<sidecar>.tmp`, flushes and closes it, and replaces the published sidecar. The integration test verifies that the temporary sidecar is absent after successful publication.

## Ambiguous directory digest

M9 forwarded one expected SHA-256 to every directory child. M10 rejects a directory-wide digest without a per-file manifest and adds exact remote-path keyed `ftp_download_digest_t` entries. The fixture verifies two different child digests and rejects the ambiguous form.

## Conservative resume default

M10 changed the internal and Python default to metadata-safe resume. The C ABI preserves an explicit `resume_allow_unverified` tail field for callers that knowingly require legacy behavior. The resume fixture now creates a matching sidecar before REST continuation.

## Python cffi lifetime and import repair

Adding the digest manifest briefly affected the multiline client import and initially applied a download-only field to the upload marshaling block. The import was repaired, the upload assignment removed, and digest byte strings are retained in a local list for the duration of the cffi call.
