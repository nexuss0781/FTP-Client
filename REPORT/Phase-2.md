# Phase 2 Specification: Protocol Engine Core

**Document Version:** 2.0-DRAFT  
**Depends On:** Phase 1 (Frozen ABI Contract)  
**Scope:** RFC 959 / RFC 3659 command pipeline, control/data channel mechanics, directory traversal, transport abstraction. No TLS (Phase 3), no concurrent transfer scheduling (Phase 4), no retry logic (Phase 5).

---

## 1. Purpose & Scope

Phase 2 implements the **FTP protocol semantics** behind the Phase 1 opaque handle. It transforms the ABI shell into a functional FTP client capable of:

- Establishing and maintaining a standards-compliant control channel
- Authenticating via USER/PASS (and future AUTH TLS hooks)
- Negotiating passive data channels (PASV/EPSV) with NAT traversal intelligence
- Executing directory listings, uploads, and downloads **serially** (concurrency deferred to Phase 4)
- Recursively traversing local directory trees into a structured manifest
- Translating FTP server reply codes into the Phase 1 error taxonomy

**Explicitly Out of Scope:**
- TLS/SSL wrapping of sockets (Phase 3)
- Concurrent file transfer scheduling/thread pool (Phase 4)
- Resume, retry, and circuit breaker logic (Phase 5)
- Progress callback invocation (Phase 4)
- Python binding ergonomics (Phase 6)
- Platform zero-copy optimizations (`sendfile`, io_uring) (Phase 7)

---

## 2. Architectural Decisions & Trade-offs

| Decision | Selected Approach | Rejected Alternative | Rationale |
|----------|------------------|---------------------|-----------|
| **Control I/O Model** | Blocking `recv()` on dedicated per-handle thread | Async io_uring / epoll | FTP control channel is low-frequency, strict request-response text protocol. Blocking I/O with a thread is simpler, debuggable, and latency-bound by RTT not syscall overhead. io_uring adds complexity with no proportional throughput gain for ~10 commands/sec. |
| **Protocol Pipelining** | **None.** Strict lock-step command/response | SMTP-style pipelining | FTP (RFC 959) is inherently stateful and lock-step. Sending `PASV` before `USER` completes is protocol violation. "Pipelining" in our context means **architectural decoupling** via an internal command queue, not wire-level pipelining. |
| **Data Channel Model** | One blocking data socket per transfer, managed by the control thread's delegate | Multiplexed data over control channel | FTP separates control and data. We maintain this separation with a clean socket pair per transfer. Phase 4 will parallelize multiple data sockets. |
| **Transport Layer** | Abstract `Transport` interface with `PlainTransport` implementation | Raw BSD socket calls everywhere | Mandatory prerequisite for Phase 3 TLS injection. All socket I/O goes through the Transport vtable. |
| **Directory Traversal** | Eager full-tree walk with `std::filesystem` | Lazy streaming walk | Eager traversal allows Phase 4 scheduler to optimize upload order (largest-first, directory creation batching) and provide accurate total progress. Latency to first byte is acceptable for batch tools. |
| **Response Parsing** | Hand-written state machine scanner | Regex (`std::regex`) | Regex compilation and execution is 10-100× slower for simple fixed-pattern parsing. FTP responses are rigidly formatted; manual scanning with `std::string_view` is optimal. |
| **String Encoding** | UTF-8 on wire; transparent pass-through | Server-side charset negotiation | Modern FTP servers speak UTF-8 by default (RFC 2640). We assume UTF-8. Phase 7 may add `OPTS UTF8` negotiation if needed. |

---

## 3. Transport Abstraction Layer (TAL)

This interface is the **critical seam** between Phase 2 (protocol) and Phase 3 (security/TLS). It must be finalized in Phase 2 and treated as immutable by Phase 3.

### 3.1 Internal C++ Interface (Header-Only or Virtual)

```cpp
// src/protocol/Transport.hpp
class Transport {
public:
    virtual ~Transport() = default;

    /* Returns 0 on success, negative errno-style on failure */
    virtual int32_t connect(const char* host, uint16_t port) = 0;

    /* Blocking read. Returns bytes read, 0 on orderly close, negative on error. */
    virtual int32_t read(void* buffer, uint32_t length) = 0;

    /* Blocking write. Returns bytes written, negative on error. */
    virtual int32_t write(const void* buffer, uint32_t length) = 0;

    /* Graceful shutdown + close. Returns 0 on success. */
    virtual int32_t shutdown() = 0;

    virtual bool is_connected() const = 0;
};
```

### 3.2 `PlainTransport` Implementation
- Uses standard blocking TCP sockets (`socket()`, `connect()`, `send()`, `recv()`).
- Platform abstraction: `AF_INET` / `AF_INET6` dual-stack. Windows `Winsock2` vs POSIX `sys/socket.h` isolated here.
- **No other component in the codebase may include `<sys/socket.h>` or `<winsock2.h>` directly.** This rule is enforced by code review and CI linting.

### 3.3 Phase 3 Extension Point
`TlsTransport` (Phase 3) will implement the same interface, wrapping OpenSSL or mbedTLS. The Protocol Engine holds a `std::unique_ptr<<Transport>` and is agnostic to encryption.

---

## 4. Control Channel Subsystem

### 4.1 Thread Model
Each `ftp_client_t` handle owns **exactly one control thread** (`ControlChannelThread`).

**Responsibilities of Control Thread:**
- Maintain TCP connection to server port 21 (or custom).
- Execute the state machine (Section 5).
- Read and parse server replies.
- Manage the command dispatch queue.
- Send periodic NOOP keep-alives during idle.

**Responsibilities of API Thread (caller):**
- Enqueue commands via the C ABI functions.
- Block on completion futures/semaphores where required.

**Synchronization Primitive:**
- Command Queue: `std::deque<<Command>` protected by `std::mutex` + `std::condition_variable`.
- Rationale: Lock-free queues are unnecessary. FTP command rate is <100/sec. Mutex contention is negligible and code clarity is paramount.

### 4.2 Command Structure
```cpp
struct Command {
    std::string verb;           /* e.g., "USER", "PASV", "STOR" */
    std::string arg;            /* Payload, may be empty */
    std::promise<int32_t> result; /* Fulfilled by control thread with error code */
};
```

**Exception Safety:** The control thread catches all exceptions at the top loop, maps to `FTP_ERR_PROTOCOL` or `FTP_ERR_NETWORK_RESET`, and transitions to `ERROR` state.

### 4.3 Read Loop Algorithm
```
1. Block on command queue (CV wait with 30s timeout).
2. If timeout expires and state is AUTHENTICATED: enqueue internal NOOP.
3. Pop command.
4. Format: "VERB ARG\r\n" (or "VERB\r\n" if arg empty).
5. write() to Transport.
6. Enter reply reading loop:
   a. Read lines until final line detected (code + space).
   b. Parse reply code (3 digits).
   c. Store reply text.
7. Update state machine based on command + reply.
8. Set promise value (success or mapped error).
9. Loop.
```

**Buffer Management:**
- Use a single `std::string` buffer per control thread, `reserve(4096)`.
- Read via `transport->read()` into a stack `char[4096]` append buffer.
- Scan for `\r\n` using `memchr` or manual loop (faster than `std::find` on raw bytes).

---

## 5. Protocol State Machine (Formal Definition)

### 5.1 States
| State | Description |
|-------|-------------|
| `DISCONNECTED` | Initial. No socket. |
| `TCP_CONNECTED` | TCP 3-way complete. Awaiting greeting. |
| `GREETING_WAIT` | Reading 220 banner. |
| `AUTH_IN_PROGRESS` | USER sent, awaiting 331/230. |
| `AUTHENTICATED` | Logged in. Ready for file system commands. |
| `CWD_PENDING` | CWD sent, awaiting 250. |
| `PASV_PENDING` | PASV/EPSV sent, awaiting 227/229. |
| `DATA_CONNECTING` | Parsing PASV, opening data socket. |
| `DATA_READY` | Data channel established. Ready for STOR/RETR/LIST. |
| `DATA_TRANSFERRING` | STOR/RETR/LIST in progress. Data socket active. |
| `DATA_FINALIZING` | Data socket closed. Awaiting 226/250 on control. |
| `DISCONNECTING` | QUIT sent or closing. |
| `ERROR` | Unrecoverable. All future commands fail fast. |

### 5.2 Valid Transitions
```
DISCONNECTED --ftp_connect()--> TCP_CONNECTED --220 received--> GREETING_WAIT
GREETING_WAIT --ftp_connect() sends USER--> AUTH_IN_PROGRESS
AUTH_IN_PROGRESS --331--> AUTH_IN_PROGRESS (send PASS)
AUTH_IN_PROGRESS --230--> AUTHENTICATED
AUTHENTICATED --ftp_disconnect()--> DISCONNECTING
AUTHENTICATED --CWD--> CWD_PENDING --250--> AUTHENTICATED
AUTHENTICATED --PASV/EPSV--> PASV_PENDING --227/229--> DATA_CONNECTING
DATA_CONNECTING --connect success--> DATA_READY
DATA_READY --STOR/RETR/LIST--> DATA_TRANSFERRING
DATA_TRANSFERRING --data EOF + 226--> DATA_FINALIZING --ack--> AUTHENTICATED
[Any] --4xx/5xx (fatal)--> ERROR
[Any] --TCP RST/timeout--> ERROR
```

### 5.3 Invalid Transition Handling
Any command issued in an incompatible state returns `FTP_ERR_INVALID_STATE` immediately at the API layer (before enqueueing) or via the promise if a race occurs. The state machine is the single source of truth.

---

## 6. Response Parser Specification

### 6.1 RFC 959 Reply Format
```
reply-line = code SP [message] CRLF   ; final line
           | code "-" [message] CRLF   ; continuation
code       = 3DIGIT
```

### 6.2 Parsing Algorithm (Pseudocode)
```
function parse_response(buffer):
    while true:
        find CRLF in buffer
        if not found: return NEED_MORE_DATA
        
        line = buffer[0..crlf_pos]
        buffer = buffer[crlf_pos+2..end]
        
        if length(line) < 3: return MALFORMED
        
        code = atoi(line[0..2])
        if length(line) >= 4 and line[3] == ' ':
            // Final line
            return COMPLETE(code, accumulated_lines)
        else if length(line) >= 4 and line[3] == '-':
            // Continuation
            accumulated_lines.push_back(line)
            continue
        else:
            return MALFORMED
```

### 6.3 Performance Rules
- **No dynamic allocation per line.** Use `std::string_view` referencing the receive buffer.
- **No `std::stoi` or `atoi`.** Use manual digit parsing: `(line[0]-'0')*100 + (line[1]-'0')*10 + (line[2]-'0')`. This is branch-predictable and allocation-free.
- **No regex.** A `std::regex` object compiling `"^\\d{3}[ -]"` has been measured at >50× slower than 5 lines of C string scanning.

---

## 7. Data Channel Subsystem

### 7.1 Passive Mode Negotiation Algorithm
```
1. If server advertised IPv6 capability (FEAT response contains "EPSV"):
       send "EPSV"
   else:
       send "PASV"
2. If EPSV fails (500/502/522):
       fallback to "PASV"
3. If PASV fails:
       return FTP_ERR_PASSIVE_FAILED
```

### 7.2 PASV Parser (IPv4)
**Input:** `227 Entering Passive Mode (192,168,1,1,200,1)`  
**Extraction:**
- Find `(` and `)`.
- Split by comma into 6 integers: h1,h2,h3,h4,p1,p2.
- IP = `h1.h2.h3.h4`
- Port = `p1*256 + p2`

**NAT Traversal Intelligence:**
```
if extracted_ip is RFC1918 (private) AND control_host is public:
    use control_host_ip for data connection
    log/trace: "PASV returned private IP, using public control host IP"
```
This is **mandatory** for modern cloud/NAT deployments. FileZilla implements this; many naive clients fail.

### 7.3 EPSV Parser (IPv4/IPv6)
**Input:** `229 Entering Extended Passive Mode (|||12345|)`  
**Extraction:**
- Find `(` and `)`.
- Extract port number between delimiters (typically `|`).
- IP is implicitly the same as control connection host (EPSV does not return IP).

### 7.4 Data Socket Lifecycle
1. Parse port (and IP for PASV).
2. Create new `Transport` instance (`PlainTransport` in Phase 2).
3. `transport->connect(parsed_ip, parsed_port)`.
4. Issue STOR/RETR/LIST on control channel.
5. Server connects (passive mode implies server listens; client connects to server's passive port).
6. Stream data on data `Transport`.
7. `shutdown()` data transport.
8. Read 226/250 on control channel.

---

## 8. Directory Traversal Engine

### 8.1 Configuration (from `ftp_upload_options_t` + Phase 2 extensions)
```cpp
struct TraversalConfig {
    uint32_t max_depth = 0;          /* 0 = unlimited */
    int32_t  symlink_policy = 1;     /* 0=ERROR, 1=FOLLOW, 2=SKIP */
    uint64_t max_file_size = 0;      /* 0 = unlimited. Skip files larger than this. */
    const char* const* prune_patterns; /* NULL-terminated array of skip patterns (e.g., ".git") */
};
```

### 8.2 Output Manifest Schema
```cpp
struct FileManifestEntry {
    std::string local_absolute_path;   /* Native path, UTF-8 */
    std::string remote_relative_path;  /* Forward-slash separated, relative to upload root */
    uint64_t    size_bytes;
    bool        is_directory;          /* true = remote mkdir needed, no data transfer */
    uint64_t    mtime_sec;             /* Unix epoch. For future resume/sync. */
};

struct FileManifest {
    std::vector<FileManifestEntry> entries;
    uint64_t total_files = 0;
    uint64_t total_dirs = 0;
    uint64_t total_bytes = 0;
};
```

### 8.3 Traversal Algorithm
```
function traverse(local_root, remote_root, config):
    manifest = new FileManifest
    stack = [(local_root, remote_root, depth=0)]
    
    while stack not empty:
        (local_dir, remote_dir, depth) = stack.pop()
        
        if config.max_depth > 0 and depth > config.max_depth:
            continue
            
        for entry in std::filesystem::directory_iterator(local_dir):
            if entry.name() matches prune_patterns: continue
            
            rel = remote_dir + "/" + entry.name()
            
            if is_symlink(entry):
                if config.symlink_policy == SKIP: continue
                if config.symlink_policy == ERROR: throw/return error
                // FOLLOW: resolve and continue
            
            if is_directory(entry):
                manifest.entries.push({path, rel, 0, true, 0})
                stack.push((entry.path(), rel, depth+1))
            else:
                size = file_size(entry)
                if config.max_file_size > 0 and size > config.max_file_size:
                    continue
                mtime = last_write_time(entry) converted to unix_sec
                manifest.entries.push({path, rel, size, false, mtime})
                manifest.total_files++
                manifest.total_bytes += size
                
    return manifest
```

**Error Handling:**
- `std::filesystem` exceptions caught and mapped:
  - `permission_denied` → `FTP_ERR_LOCAL_IO`
  - `not_directory` → `FTP_ERR_INVALID_ARGUMENT`
  - `filesystem_loop` (symlink cycle) → `FTP_ERR_LOCAL_IO`
- All `std::filesystem` calls use `std::error_code` overloads where available to avoid exceptions on non-fatal errors (e.g., permission denied on one file should skip it, not abort entire traversal).

### 8.4 Ordering Guarantee
Entries are produced in **depth-first pre-order** with directories preceding their children. This ensures that when Phase 4 creates remote directories, parent directories are created before children.

---

## 9. Connection Lifecycle & Keep-alive

### 9.1 TCP Tuning (applied by `PlainTransport::connect`)
| Socket Option | Value | Purpose |
|---------------|-------|---------|
| `TCP_NODELAY` | 1 | Disable Nagle for control channel responsiveness |
| `SO_KEEPALIVE` | 1 | Enable OS-level keepalive probes |
| `SO_RCVBUF` | 256KB | Large receive buffer for data channel |
| `SO_SNDBUF` | 256KB | Large send buffer for data channel |

### 9.2 Idle Detection & NOOP
- If control thread blocks on command queue for > 30 seconds (configurable via `ftp_set_timeout_command_ms`), send `NOOP\r\n`.
- If `NOOP` fails (no 200 response), transition to `ERROR` and trigger reconnect logic (Phase 5).

### 9.3 Graceful Disconnect Sequence
```
1. If state >= AUTHENTICATED: send "QUIT\r\n", await 221.
2. transport->shutdown().
3. Transition to DISCONNECTED.
```
If server does not respond to QUIT within 5s, force `shutdown()` and transition anyway.

---

## 10. Error Translation Matrix

FTP server reply codes are mapped to Phase 1 error taxonomy:

| FTP Code | Meaning | Phase 1 Mapping |
|----------|---------|-----------------|
| `421` | Service not available, closing | `FTP_ERR_NETWORK_RESET` |
| `425` | Can't open data connection | `FTP_ERR_PASSIVE_FAILED` |
| `426` | Connection closed; transfer aborted | `FTP_ERR_NETWORK_RESET` |
| `430` | Invalid username or password | `FTP_ERR_AUTH_FAILED` |
| `431` | Need some unavailable resource | `FTP_ERR_SYSTEM` |
| `450` | File unavailable (busy) | `FTP_ERR_REMOTE_IO` |
| `451` | Local error in processing | `FTP_ERR_REMOTE_IO` |
| `452` | Insufficient storage | `FTP_ERR_REMOTE_IO` |
| `500` | Syntax error / command unrecognized | `FTP_ERR_PROTOCOL` |
| `501` | Syntax error in parameters | `FTP_ERR_PROTOCOL` |
| `502` | Command not implemented | `FTP_ERR_PROTOCOL` |
| `530` | Not logged in | `FTP_ERR_AUTH_FAILED` |
| `550` | File unavailable / not found / no access | `FTP_ERR_SERVER_DENIED` |
| `552` | Exceeded storage allocation | `FTP_ERR_REMOTE_IO` |

**Implementation Rule:** A centralized `map_ftp_code_to_error(uint16_t code)` function must implement this table. No ad-hoc `if` statements scattered through the codebase.

---

## 11. Internal Interface Contract (for Phase 3/4/5)

Phase 2 exposes the following internal C++ API to subsequent phases:

### 11.1 `ProtocolEngine` Class (Pseudocode)
```cpp
class ProtocolEngine {
public:
    // Phase 2
    int32_t connect(const ftp_credentials_t& creds);
    int32_t disconnect();
    int32_t ping(); // NOOP wrapper
    FileManifest traverse_local(const char* local_path, const TraversalConfig& cfg);
    
    // Phase 4 will call these:
    int32_t upload_file(const FileManifestEntry& entry, Transport* data_transport);
    int32_t download_file(const FileManifestEntry& entry, Transport* data_transport);
    int32_t create_remote_dir(const char* remote_path);
    
    // Phase 3 will inject:
    void set_control_transport_factory(std::unique_ptr<<Transport> (*factory)());
    
private:
    std::unique_ptr<<Transport> control_transport_;
    std::unique_ptr<<ControlThread> control_thread_;
    StateMachine state_;
};
```

### 11.2 Phase 3 Seam: Transport Factory
Phase 3 will not modify `ProtocolEngine`. Instead, it registers a factory function that produces `TlsTransport` instances. `ProtocolEngine` calls this factory instead of `new PlainTransport()` when `use_tls != 0`.

---

## 12. Security Precursors (Phase 3 Preparation)

Although TLS is Phase 3, Phase 2 must observe these security rules:

1. **Command Sanitization:** Diagnostic logs must never emit the argument of `PASS`. Log format: `> PASS ***`.
2. **Path Sanitization:** Local paths are validated to prevent directory traversal attacks (e.g., rejecting paths containing `..` that escape the upload root).
3. **Buffer Limits:** No unbounded reads. Control channel read buffer capped at 64KB (RFC 959 replies never exceed this). Overflow triggers `FTP_ERR_PROTOCOL`.
4. **No Credential Persistence:** Phase 2 receives credentials during `connect()`, passes them to the control thread for the `USER`/`PASS` sequence, and immediately zeroes the temporary buffer. No long-term credential storage in ProtocolEngine (Phase 3 vault handles that).

---

## 13. Quality Gates & Acceptance Criteria

### 13.1 Protocol Compliance Suite
**Setup:** Docker containers running vsftpd, ProFTPD, Pure-FTPd, and Windows Server IIS FTP (via VM or cloud instance).  
**Tests:**
- Connect, authenticate (plain), disconnect.
- Execute: PWD, CWD, TYPE I, PASV, EPSV, LIST, STOR (small file), RETR, DELE, MKD, RMD.
- Verify correct state transitions via internal instrumentation hooks.

**Pass Criteria:** 100% success against all 4 server types.

### 13.2 PASV Parser Torture Test
**Input:** 100 syntactically malformed PASV/EPSV responses including:
- Missing parentheses
- Extra commas
- Non-digit characters in port fields
- Private IP ranges when connected to public hosts (NAT test)
- IPv6 addresses in PASV (invalid but must not crash)

**Pass Criteria:** Zero crashes. All invalid inputs return `FTP_ERR_PASSIVE_FAILED`. NAT case correctly substitutes public IP.

### 13.3 State Machine Exhaustion
**Method:** Fuzz state transitions. From every state, inject every possible command.  
**Pass Criteria:** Invalid transitions return `FTP_ERR_INVALID_STATE` without socket side effects. No undefined behavior under UBSan/ASan.

### 13.4 Directory Traversal Scale Test
**Setup:** Generate tree with 100,000 files across 1,000 directories, including 500 symlinks (cycles and dead links).  
**Pass Criteria:** Manifest produced in < 5 seconds on SSD. Symlink cycles detected and handled per policy. Memory usage < 100MB.

### 13.5 Memory Safety
**Tool:** Valgrind (`memcheck`) + AddressSanitizer.  
**Procedure:** 1,000 iterations of: connect → STOR 1MB file → RETR → disconnect.  
**Pass Criteria:** Zero leaks. Zero use-after-free. Zero buffer overflow.

### 13.6 Performance Baseline
**Metric:** Single-threaded upload of 1GB file to localhost vsftpd.  
**Target:** Achieve ≥ 85% of raw `nc` netcat throughput on loopback.  
**Rationale:** Protocol overhead (control channel chatter) should not materially reduce throughput. If we hit 85%, Phase 4's parallelism will saturate any real network link.

---

## 14. Deliverables

| File | Purpose |
|------|---------|
| `src/protocol/Transport.hpp` | Transport abstraction interface |
| `src/protocol/PlainTransport.hpp/cpp` | TCP socket implementation |
| `src/protocol/ControlThread.hpp/cpp` | Command queue + read loop |
| `src/protocol/StateMachine.hpp/cpp` | Formal state definitions & transitions |
| `src/protocol/ReplyParser.hpp/cpp` | Response parsing (allocation-free) |
| `src/protocol/DataChannel.hpp/cpp` | PASV/EPSV negotiation & socket creation |
| `src/protocol/DirectoryWalker.hpp/cpp` | `std::filesystem` traversal + manifest |
| `src/protocol/ErrorMap.hpp/cpp` | FTP code → Phase 1 error code table |
| `src/protocol/ProtocolEngine.hpp` | Internal facade exposed to Phase 3/4 |
| `tests/protocol_compliance_test.cpp` | Integration tests against Dockerized servers |
| `tests/pasv_parser_torture.cpp` | Unit tests for PASV/EPSV parsing |
| `tests/state_machine_fuzz.cpp` | Invalid transition injection |
| `tests/traversal_scale_test.cpp` | 100k file tree benchmark |
| `tests/throughput_baseline.cpp` | 1GB loopback throughput measurement |

---

## 15. Transition Criteria to Phase 3

Phase 2 is **ratified** when:

1. All 6 Quality Gates (Section 13) pass on CI for Linux x86_64 and Windows x64.
2. `ProtocolEngine` successfully completes a full upload/download cycle against at least **3 distinct server implementations** (vsftpd, ProFTPD, IIS).
3. The `Transport` interface is reviewed and signed off by the Phase 3 TLS architect as sufficient for OpenSSL/mbedTLS wrapping.
4. ASan/Valgrind reports are clean with zero suppressions.
5. The PASV NAT detection logic is verified against a mocked server returning `192.168.x.x` while client connects to public IP.
6. **No raw socket syscalls exist outside `PlainTransport.cpp`.** Verified by `grep` audit in CI.

**Upon ratification:** The internal `ProtocolEngine` API and `Transport` interface enter soft-lockdown. Phase 3 may extend but not break these contracts.

---

**End of Phase 2 Specification**
