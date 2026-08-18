# M9 Design Notes

## Remote metadata negotiation

M9 will add a typed MLSD listing path using RFC 3659-style facts. The parser will accept `type=file`, `type=dir`, `size`, and `modify` facts, preserve names containing spaces, reject malformed entries, and ignore unsupported facts. SIZE remains the portable file-length probe. MDTM will be optional metadata enrichment; a server that rejects MDTM must not make a valid listing unusable.

Server-side HASH negotiation is intentionally deferred. M9 does not claim remote hash verification because the current implementation has no portable FEAT/HASH capability probe. Local SHA-256 verification remains authoritative, and the public result never claims a remote digest was checked.

## Durable RETR resume

An interrupted download keeps `<destination>.ftpclient.part` and a sidecar `<destination>.ftpclient.part.meta`. The sidecar records a version, remote path, remote SIZE, expected digest, and confirmed local byte count. Resume is allowed only when the sidecar matches the current request and remote SIZE; metadata-safe mode additionally requires an expected digest. Without a valid sidecar, the client starts from zero and truncates the temporary file.

REST is sent before the passive negotiation command that precedes RETR. The received byte count begins at the accepted restart offset. A successful final reply plus local digest verification publishes the file and removes both temporary artifacts. Cancellation, stall, local I/O, and transport failures preserve the sidecar and partial file for a future explicit resume; negative final replies remove them because the server rejected the operation.

## Directory downloads

`ftp_download_dir()` will recursively enumerate remote directories through MLSD, create local directories, and download regular files through the same protected RETR path. The initial M9 implementation is serialized per client handle to preserve control-channel ordering and deterministic results. Each file contributes an owned per-file result with a remote path, local path, status, and received bytes.

## ABI compatibility

Existing `ftp_download_file()` remains unchanged. `ftp_download_file_ex()` gains only tail fields in its struct-size-governed options object. `ftp_download_dir()` uses the same options and result structures. New capability bits are advertised only after the corresponding integration fixtures pass.
