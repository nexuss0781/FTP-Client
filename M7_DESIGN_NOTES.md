# M7 Design Notes — Download and Remote Filesystem Operations

## Download contract

The current public ABI exposes upload-directory results but no download entry point. M7 will add an ABI-compatible `ftp_download_file()` operation with caller-owned `ftp_result_t` output, using the existing result and cleanup structures. The operation will create parent directories locally, write to a temporary local filename where practical, validate the final 226/250 reply, then rename the temporary file into place only after a successful transfer.

`ProtocolEngine::download_file()` will prefer EPSV and fall back to PASV using the existing passive parser. It will open the local output before issuing RETR, establish a TLS data transport for explicit FTPS, require a 125/150 preliminary reply, read until orderly data EOF, close the data socket, and require a positive final control reply. A missing or negative final reply will not be reported as success.

## Remote filesystem wrappers

M7 will add typed ProtocolEngine wrappers for CWD, CDUP, DELE, RMD, RNFR/RNTO, FEAT, and MDTM where the existing state machine supports the command. RNFR/RNTO will remain one serialized pair on the same control worker. Remote-operation wrappers will preserve server reply mapping rather than collapsing all errors into protocol failure.

## Directory download

The first directory-download path will use the existing local traversal and result aggregation patterns, but requires a remote listing parser. M7 will keep the parser scope controlled: MLSD is preferred, LIST is a fallback only when a fixture supplies a deterministic compatible format. If a server cannot provide a supported listing, the operation returns an explicit unsupported/protocol result instead of guessing paths.

## Test boundary

The integration fixture will cover plain and explicit-FTPS RETR, zero-byte downloads, binary payload preservation, negative final replies, local parent creation, and remote command sequencing. Upload, pooling, ABI, and sanitizer regressions remain mandatory.
