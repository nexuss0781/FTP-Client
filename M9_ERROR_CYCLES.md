# M9 Error Cycles

## cdir and pdir MLSD facts

The first parser test rejected valid RFC 3659 `type=cdir` and `type=pdir` entries because the name guard rejected `.` and `..` before examining the type fact. The parser now classifies the fact first and filters cdir/pdir entries at listing level, while still rejecting dot-named regular files and directories.

## Directory fixture command sequencing

The first recursive directory fixture appeared to hang after the second listing because it enabled RETR resume against a server that intentionally implemented only MLSD and RETR. M9 correctly probed SIZE, but the fixture’s unsupported 502 reply terminated the control session before RETR. The fixture was corrected to disable resume for the directory orchestration case; the dedicated resume case adds SIZE and REST support and passes end to end.

## Resume-sidecar serialization

The first generated sidecar stream used literal backslash-n characters because the source-generation escape level was wrong. Inspection with line-preserving output found the issue, and the writer now emits actual newline separators. The resume integration verifies that a two-byte `.part` is continued through REST and that both `.part` and `.meta` are removed after publication.

## Mock-server string operation

The resume fixture initially attempted `.substr()` directly on a string literal. The compile error was corrected by constructing a `std::string` before applying the restart offset.

## Cross-platform export drift

The manual MSVC module-definition file still listed only the original ABI functions. M9 synchronizes it with M7-M9 download, cancellation, remote-filesystem, and directory-download symbols so Windows exports do not silently diverge from Linux symbol discovery.

## Truthful remote integrity scope

M9 does not implement server-side HASH or FEAT negotiation. The design record was corrected to state that local SHA-256 is authoritative and that remote digest verification remains a future capability rather than an implied guarantee.
