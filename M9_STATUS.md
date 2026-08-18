# M9 Status — Remote Listing, Directory Download, and Durable RETR Resume

## Delivered

M9 adds a strict machine-readable MLSD parser and a `ProtocolEngine::list_directory()` operation. The implementation supports EPSV with PASV fallback, explicit-FTPS protected data channels, cancellation and stall handling inherited from M8, validated file/directory facts, and names containing spaces.

The public C ABI now exposes `ftp_download_dir()`. It recursively enumerates remote directories through MLSD, creates the corresponding local directories, downloads regular files through the existing protected RETR path, sorts entries for deterministic result ordering, and returns owned per-file results through the existing `ftp_result_t` contract. The Python synchronous and asynchronous facades expose the new download options and directory operation.

M9 extends RETR with optional REST continuation. A valid `<destination>.ftpclient.part` can be continued after a SIZE probe. Metadata-safe mode additionally requires a matching sidecar containing the remote path, remote size, expected digest, and confirmed byte count. Successful publication removes both temporary artifacts; interruption preserves them for a future explicit resume.

The MSVC export-definition list now includes all M7-M9 public symbols. Local SHA-256 verification remains authoritative. Server-side HASH/FEAT negotiation is explicitly deferred because it is not yet implemented and must not be implied by the public capability surface.

## Validation

The clean Debug build passes all twelve CTest tests. New coverage includes the MLSD parser/state machine, recursive directory download, deterministic result ordering, SIZE/REST RETR continuation, and temporary sidecar cleanup. The Python ABI suite passes 45 assertions, the exception suite passes 4/4, the type suite passes 5/5, and Python bytecode compilation succeeds.

The clean ASan/UBSan run with leak detection passes all twelve CTest tests in 66.01 seconds. No sanitizer diagnostics were reported.

## Scope boundary

M9 does not add server-side HASH or FEAT negotiation, MDTM enrichment, parallel directory downloads, durable upload manifests, or upload-side cross-process resume. Those remain candidates for a later milestone after a portable remote capability contract is added.
