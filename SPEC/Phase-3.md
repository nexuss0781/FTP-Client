# Phase 3 Specification: Security & Credential Vault

**Document Version:** 3.0-DRAFT  
**Depends On:** Phase 1 (Frozen ABI Contract), Phase 2 (Protocol Engine Core)  
**Scope:** TLS/FTPS encryption, secure memory management, credential lifecycle, certificate validation, and authentication negotiation. No concurrent transfers (Phase 4), no retry logic (Phase 5).

---

## 1. Purpose & Scope

Phase 3 secures the wire protocol and hardens the runtime against credential exposure. It transforms the Phase 2 plain-text engine into a **security-first** client capable of enterprise deployment. This phase is not merely "adding OpenSSL" — it is a systematic defense-in-depth specification covering memory, identity, trust, and transport encryption.

**In Scope:**
- FTPS (Explicit TLS via `AUTH TLS`) and FTPS Implicit (port 990)
- TLS 1.2 minimum, TLS 1.3 preferred with cipher suite control
- Certificate validation: system CA, custom CA bundle, pinning, callback override
- Secure memory allocator: locked pages, core-dump exclusion, explicit zeroing
- Credential vault: deep-copy on connect, zero-on-disconnect, no standard-container persistence
- Secret injection interface: callback-based credential providers for HSM/keyring integration
- Auth method negotiation: plain fallback policies, TLS enforcement modes

**Explicitly Out of Scope:**
- Concurrent file transfer scheduling (Phase 4)
- Resume, retry, and circuit breaker logic (Phase 5)
- SFTP/SSH (different protocol stack; future major version only)
- FIPS 140-2 compliance (requires validated module; document as future gap)
- Kerberos/GSSAPI authentication (`AUTH GSSAPI`) — deferred to Phase 7

---

## 2. Architectural Decisions & Trade-offs

| Decision | Selected Approach | Rejected Alternative | Rationale |
|----------|------------------|---------------------|-----------|
| **TLS Backend** | OpenSSL 3.x (statically linked) | mbedTLS, GnuTLS, Schannel | OpenSSL 3.x is the de facto standard, supports TLS 1.3, provider architecture, and has broad platform support. Static linking prevents "DLL hell" on Python deployment. |
| **TLS Wrapper Pattern** | `TlsTransport` implements Phase 2 `Transport` interface | Raw OpenSSL calls inside ProtocolEngine | Preserves Phase 2's clean abstraction. Phase 3 injects `TlsTransport` without modifying ProtocolEngine or ControlThread. |
| **Certificate Store** | Hybrid: system default + optional custom CA + pinning override | Exclusively system store | Enterprise users need custom CA bundles. Python `certifi` module integration is a common requirement. |
| **Memory Locking** | `mlock()` / `VirtualLock` + `madvise(MADV_DONTDUMP)` | No locking (rely on OS swap encryption) | Defense in depth. Even if swap is encrypted, locked pages prevent paging to disk during hibernation or crash dumps. |
| **Credential String Storage** | Custom allocator with `explicit_bzero` | `std::string` in C++ classes | `std::string` may reallocate and leave copies in freed memory. Custom allocator ensures single contiguous locked region. |
| **Secret Provider Model** | C ABI callback (`ftp_credential_provider_cb_t`) | Environment variables / files | Environment variables leak to `/proc/*/environ`. Files require read permissions. A callback allows Python to fetch from keyring/HSM without C++ knowing the source. |
| **Implicit vs Explicit FTPS** | Explicit FTPS (port 21 + `AUTH TLS`) as default; Implicit (990) as opt-in | Implicit as default | Explicit FTPS is firewall-friendly and RFC-compliant (RFC 4217). Implicit is legacy and often blocked by corporate firewalls. |

---

## 3. Secure Memory Subsystem

### 3.1 Design Principles
1. **Minimize Attack Surface:** Credential memory exists only when necessary.
2. **No Residuals:** Freed credential memory is overwritten before deallocation.
3. **No Swap:** Credential pages are pinned in RAM.
4. **No Core Dumps:** Credential pages are excluded from crash dumps.

### 3.2 `SecureAllocator` Specification
A C++ allocator template used **exclusively** for credential and key material containers.

```cpp
template <typename T>
class SecureAllocator {
public:
    using value_type = T;
    
    T* allocate(std::size_t n);
    void deallocate(T* p, std::size_t n);
    
private:
    void lock_page(void* addr, std::size_t len);
    void unlock_page(void* addr, std::size_t len);
    void exclude_from_dump(void* addr, std::size_t len);
};
```

**Platform Mapping:**

| Feature | Linux | Windows | macOS |
|---------|-------|---------|-------|
| Page Lock | `mlock()` | `VirtualLock()` | `mlock()` |
| Dump Exclude | `madvise(MADV_DONTDUMP)` | `SetProcessValidCallTargets` / `MEMORY_INFO` | `madvise(MADV_NOCORE)` |
| Secure Zero | `explicit_bzero()` (glibc 2.25+) or `memset_s()` | `SecureZeroMemory()` | `memset_s()` |
| Fallback Zero | `volatile` pointer deref loop | `RtlSecureZeroMemory()` | `volatile` loop |

**Rules:**
- Allocation granularity is page-aligned (typically 4KB).
- Deallocation always calls secure-zero before `munlock`/`VirtualUnlock`.
- If `mlock()` fails (e.g., RLIMIT_MEMLOCK too low), allocation **still succeeds** but logs a warning. The library degrades gracefully rather than failing to connect.
- Containers using `SecureAllocator` must not be copied into standard containers. Move semantics only.

### 3.3 Credential Vault (`CredentialVault`)
Internal C++ class instantiated per `ftp_client_t` handle.

**Lifecycle:**
1. `ftp_connect()` receives `ftp_credentials_t` (borrowed pointers).
2. Vault deep-copies all strings into `SecureAllocator`-backed buffers.
3. Original stack/heap copies in caller space are caller's responsibility.
4. During `USER`/`PASS` transmission, vault provides `const char*` views to ProtocolEngine.
5. On `ftp_disconnect()` or `ftp_client_destroy()`, vault zeroes and frees all buffers.

**Interface (Internal C++):**
```cpp
class CredentialVault {
public:
    int32_t store(const ftp_credentials_t* creds);  // Deep copy into secure memory
    const char* username() const;                   // Borrowed view
    const char* password() const;                   // Borrowed view
    const char* host() const;                       // Borrowed view
    void purge();                                   // Zero and free all
    
    bool empty() const;
};
```

**Audit Rule:** A debug/ASan build mode shall instrument `CredentialVault` to verify that no `std::string` or `malloc`-allocated copy of credentials exists elsewhere in the process address space. This is verified by Phase 3's "Memory Forensic" quality gate.

---

## 4. TLS Transport Implementation

### 4.1 `TlsTransport` Class
Implements the Phase 2 `Transport` interface (Section 3 of Phase 2 spec). This is the **only** Phase 3 component that ProtocolEngine interacts with directly.

```cpp
class TlsTransport : public Transport {
public:
    TlsTransport(SSL_CTX* shared_ctx, const TlsConfig& config);
    
    int32_t connect(const char* host, uint16_t port) override;
    int32_t read(void* buffer, uint32_t length) override;
    int32_t write(const void* buffer, uint32_t length) override;
    int32_t shutdown() override;
    bool is_connected() const override;
    
    // TLS-specific
    int32_t handshake();          // Perform TLS handshake after TCP connect
    int32_t shutdown_tls();       // Send close_notify before TCP close
    
private:
    SSL* ssl_;
    BIO* bio_;                    // OpenSSL BIO wrapping socket
    // ... platform socket fd stored for plain transport fallback
};
```

### 4.2 Connection Flow (Explicit FTPS)
```
1. PlainTransport::connect(host, 21)   // TCP established
2. Read 220 banner (plain text)
3. Send "AUTH TLS\r\n"
4. Expect 234 (server accepts TLS)
5. TlsTransport::handshake() over existing socket
6. All subsequent control traffic encrypted
7. Data channel: after PASV/EPSV, TlsTransport::connect() to data port
8. Data channel TLS handshake (if server supports PROT P)
```

### 4.3 Connection Flow (Implicit FTPS)
```
1. TlsTransport::connect(host, 990)  // TCP + immediate TLS handshake
2. Read 220 banner (already encrypted)
3. No AUTH TLS command sent
```

### 4.4 Data Channel Protection Levels (RFC 4217)
| Level | Command | Meaning | Phase 3 Support |
|-------|---------|---------|-----------------|
| `C` | `PROT C` | Clear (no TLS on data) | Yes (default if server rejects P) |
| `P` | `PROT P` | Private (TLS on data) | **Yes (default, mandatory)** |

**Policy:** If server supports `AUTH TLS`, the library **must** attempt `PBSZ 0` followed by `PROT P`. If server rejects `PROT P` (e.g., 534), behavior is configurable:
- **Strict mode:** Return `FTP_ERR_AUTH_TLS_REQUIRED` and disconnect.
- **Permissive mode:** Log warning, continue with `PROT C` (control encrypted, data clear). Default: **Strict**.

---

## 5. Certificate Validation & Trust

### 5.1 Validation Modes (from `ftp_credentials_t.verify_cert`)
| `verify_cert` Value | Behavior |
|---------------------|----------|
| `0` | **INSECURE.** No certificate verification. Accept any certificate. Log CRITICAL warning. |
| `1` | Verify peer certificate chain against trusted CAs. Do **not** verify hostname match. |
| `2` | (Default) Verify peer certificate + verify hostname/SAN match against `host` field. |

### 5.2 Trust Anchor Sources (Priority Order)
1. **Custom CA Bundle:** If `ca_bundle_path` is non-NULL, load exclusively from this PEM file.
2. **System Store:** Use OpenSSL default paths (`SSL_CTX_set_default_verify_paths`).
3. **Python `certifi`:** If called from Python binding and no custom bundle provided, the Python layer may pass `certifi.where()` as `ca_bundle_path`.

### 5.3 Certificate Pinning (Optional Extension)
A future-proofing mechanism exposed through the C ABI:

```c
typedef struct {
    const char* sha256_pin;      /* Base64-encoded SHA-256 SPKI pin */
    int32_t     pin_count;
} ftp_cert_pins_t;
```

If pins are provided, the library validates that at least one pin matches the leaf certificate's SubjectPublicKeyInfo. Failure returns `FTP_ERR_CERT_VERIFY`.

### 5.4 Custom Validation Callback
For advanced Python use cases (e.g., interactive "trust this host?" prompts):

```c
typedef int32_t (*ftp_cert_verify_cb_t)(
    const char*   subject,       /* UTF-8, borrowed */
    const char*   issuer,        /* UTF-8, borrowed */
    const char*   fingerprint,   /* SHA-1 or SHA-256 hex, borrowed */
    int32_t       error_code,    /* OpenSSL X509_V_ERR_* code */
    void*         user_data
);
```

**Return Value:**
- `1` = Override, accept certificate.
- `0` = Reject, return `FTP_ERR_CERT_VERIFY`.
- If callback is NULL, library uses default validation logic.

**Security Warning:** The callback receives borrowed strings valid only during the call. The C++ implementation must not store these pointers.

---

## 6. Secret Injection Interface

### 6.1 Motivation
Hard-coding passwords in Python scripts is a common anti-pattern. The library must support runtime credential resolution without the C++ core ever persisting secrets on disk or in standard memory.

### 6.2 C ABI Extension
Two new functions added to the Phase 1 ABI (preserving backward compatibility):

```c
/* Set a credential provider callback. Overrides static credentials. */
int32_t ftp_set_credential_provider(
    ftp_client_t*               handle,
    ftp_credential_provider_cb_t provider,
    void*                       user_data
);

/* Clear any set provider, reverting to static credentials. */
int32_t ftp_clear_credential_provider(ftp_client_t* handle);
```

### 6.3 Callback Signature
```c
typedef int32_t (*ftp_credential_provider_cb_t)(
    ftp_credentials_t* out_creds,   /* Caller-allocated struct to fill */
    void*              user_data,    /* Opaque pointer from registration */
    int32_t            attempt       /* 0=first attempt, 1+=retry after failure */
);
```

**Contract:**
- `out_creds` is pre-allocated by C++ with all fields zeroed.
- The callback fills `host`, `port`, `username`, `password`, etc.
- The callback allocates strings using standard allocator (Python `ctypes`/`cffi` handles this).
- C++ immediately deep-copies into `CredentialVault` and returns.
- The callback is invoked **inside** `ftp_connect()`, not at registration time.

**Python Use Case:**
```python
def my_provider(out_creds, user_data, attempt):
    out_creds.username = b"user"
    out_creds.password = keyring.get_password("ftp", "user").encode()
    return 0  # FTP_OK

client = ftp_client_create()
ftp_set_credential_provider(client, my_provider, None)
ftp_connect(client, None)  # Creds come from provider, not argument
```

---

## 7. TLS Configuration & Hardening

### 7.1 Minimum TLS Version
- **TLS 1.2** is the absolute minimum. Handshake attempts with TLS 1.0/1.1 are rejected with `FTP_ERR_CERT_VERIFY` (mapped from `SSL_R_VERSION_TOO_LOW`).
- **TLS 1.3** is negotiated if both sides support it.

### 7.2 Cipher Suite Policy
Default OpenSSL 3.x cipher list is acceptable, but with these exclusions:
- No NULL encryption (`eNULL`)
- No anonymous DH (`aNULL`)
- No export ciphers (`EXPORT`)
- No RC4
- No MD5 signature algorithms

**Configuration Override:** Future Phase 7 may expose `ftp_set_cipher_list(handle, "HIGH:!aNULL:!MD5")`.

### 7.3 SNI (Server Name Indication)
SNI is **mandatory** for Explicit FTPS. The `host` field from credentials is sent as the TLS SNI extension. If `host` is an IP address, SNI is omitted (some servers reject IP-based SNI).

### 7.4 Session Resumption
OpenSSL session tickets are enabled by default for control channel. Data channel connections attempt to resume using the control channel's session ID/ticket. This reduces TLS handshake overhead for concurrent transfers in Phase 4.

---

## 8. Integration with Phase 2 (Seam Preservation)

### 8.1 Transport Factory Injection
Phase 2's `ProtocolEngine` holds a `std::unique_ptr<<Transport>`. Phase 3 does **not** modify `ProtocolEngine`. Instead, the factory function is selected based on `use_tls`:

```cpp
// Inside ftp_connect() implementation:
std::unique_ptr<<Transport> transport;
if (creds->use_tls == 1) {          // Explicit FTPS
    transport = std::make_unique<<TlsTransport>(ssl_ctx_, TlsConfig{...});
} else if (creds->use_tls == 2) {   // Implicit FTPS
    transport = std::make_unique<<TlsTransport>(ssl_ctx_, TlsConfig{...});
    transport->set_implicit_mode(true);
} else {
    transport = std::make_unique<<PlainTransport>();
}

protocol_engine_->set_transport(std::move(transport));
```

### 8.2 State Machine Extension
Phase 2's state machine requires two new states for Explicit FTPS:

| State | Description | Transition |
|-------|-------------|------------|
| `TLS_HANDSHAKE` | `AUTH TLS` accepted (234), performing TLS handshake | `TCP_CONNECTED` → `TLS_HANDSHAKE` → `GREETING_WAIT` |
| `TLS_DATA_NEGOTIATING` | `PBSZ`/`PROT` negotiation in progress | `AUTHENTICATED` → `TLS_DATA_NEGOTIATING` → `AUTHENTICATED` |

These are additive; Phase 2 states remain unchanged.

### 8.3 Error Code Mapping (OpenSSL → Phase 1)
| OpenSSL Error | Phase 1 Code | Context |
|---------------|------------|---------|
| `SSL_ERROR_SYSCALL` + `ECONNRESET` | `FTP_ERR_NETWORK_RESET` | TCP dropped during handshake |
| `SSL_ERROR_SSL` (version mismatch) | `FTP_ERR_CERT_VERIFY` | Server too old |
| `X509_V_ERR_SELF_SIGNED_CERT` | `FTP_ERR_CERT_VERIFY` | Untrusted CA |
| `X509_V_ERR_HOSTNAME_MISMATCH` | `FTP_ERR_CERT_VERIFY` | SNI/hostname mismatch |
| `SSL_ERROR_ZERO_RETURN` | `FTP_ERR_NETWORK_RESET` | Close notify received |

---

## 9. Security Audit & Hardening Checklist

### 9.1 Static Analysis Gates
- **No `strcpy` / `sprintf`:** All string operations use bounded copies.
- **No `system()` / `popen()`:** No shell execution.
- **No `tmpnam` / `mktemp`:** No temporary file creation in Phase 3.
- **No `rand()`:** Use OpenSSL `RAND_bytes` if randomness needed.

### 9.2 Dynamic Analysis Gates
- **Valgrind `memcheck`:** Verify no credential strings in uninitialized/freed memory.
- **AddressSanitizer:** Detect heap-buffer-overflow in TLS buffer handling.
- **MemorySanitizer:** Detect use of uninitialized TLS session data.

### 9.3 Penetration Test Scenarios
1. **Rogue CA:** Server presents cert signed by untrusted CA. Library must reject (`verify_cert >= 1`).
2. **Self-Signed:** Server presents self-signed cert. Reject unless pin matches or callback overrides.
3. **Hostname Mismatch:** Cert valid for `ftp.example.com`, client connected to `203.0.113.1`. Reject (`verify_cert == 2`).
4. **Downgrade Attack:** Server supports TLS 1.0 only. Reject with version error.
5. **Strip Attack:** Server rejects `AUTH TLS` (500). In strict mode, disconnect. In permissive mode, log and continue plain (with warning).
6. **Memory Dump:** Attach debugger, dump core during active connection. Search for password string. Must not appear in plaintext.

---

## 10. Quality Gates & Acceptance Criteria

### 10.1 TLS Handshake Compliance
**Setup:** Docker containers with:
- vsftpd + OpenSSL (TLS 1.3)
- ProFTPD + OpenSSL (TLS 1.2 only)
- Pure-FTPd with self-signed cert
- A server rejecting `AUTH TLS` (500)

**Tests:**
- Connect with `use_tls=1` (Explicit) to all 4. Expect success on first 3, failure on 4th (strict mode).
- Connect with `use_tls=2` (Implicit) to port 990.
- Connect with `verify_cert=0` to self-signed server. Expect success with CRITICAL log.
- Connect with `verify_cert=2` to IP-address server with valid cert. Expect failure (hostname mismatch).

**Pass Criteria:** All handshake outcomes match expected behavior. Zero memory leaks under Valgrind.

### 10.2 Secure Memory Forensic Audit
**Tool:** Custom `grep_core_dump` test harness.
**Procedure:**
1. Launch library, connect to server, `ftp_connect()` with password `"SECRETPHASE3TEST"`.
2. Trigger `SIGABRT` (simulated crash).
3. Read `/proc/<pid>/mem` or core file.
4. Search for ASCII and UTF-8 encoding of `"SECRETPHASE3TEST"`.

**Pass Criteria:** Zero occurrences in process memory outside the locked `CredentialVault` region. If found in OpenSSL internal buffers (session ticket, etc.), this is documented as accepted risk with mitigation (short session lifetime).

### 10.3 Credential Provider Integration
**Test:** Python `cffi` script using `ftp_set_credential_provider`.
**Procedure:**
1. Register provider that fetches password from environment variable.
2. Call `ftp_connect()` with NULL credentials struct.
3. Verify connection succeeds.
4. Verify provider called exactly once (attempt=0).
5. Disconnect. Verify provider not called again.

**Pass Criteria:** Provider pattern works end-to-end. No credential strings persist in C++ after disconnect.

### 10.4 Certificate Pinning Accuracy
**Test:** Connect to server with known SPKI SHA-256 pin.
**Procedure:**
1. Extract correct pin from server cert offline.
2. Connect with correct pin: expect success.
3. Connect with incorrect pin (single bit flipped): expect `FTP_ERR_CERT_VERIFY`.
4. Connect with no pin but valid system CA: expect success (pin is optional).

**Pass Criteria:** Pin match is exact. No false positives/negatives.

### 10.5 Performance Baseline
**Metric:** TLS handshake latency + single-file upload throughput vs Phase 2 plain FTP.
**Target:** Handshake overhead ≤ 150ms on loopback. Throughput ≥ 80% of Phase 2 plain FTP (AES-GCM overhead is minimal on modern CPUs).

---

## 11. Deliverables

| File | Purpose |
|------|---------|
| `src/security/SecureAllocator.hpp/cpp` | Locked-page allocator with secure zeroing |
| `src/security/CredentialVault.hpp/cpp` | Per-handle secure credential storage |
| `src/security/TlsTransport.hpp/cpp` | OpenSSL-backed Transport implementation |
| `src/security/TlsConfig.hpp` | TLS parameters, cipher policies, version enforcement |
| `src/security/CertValidator.hpp/cpp` | X.509 chain validation, hostname check, pinning |
| `src/security/SecretProvider.hpp/cpp` | Credential provider callback integration |
| `src/security/OpenSSLInit.hpp/cpp` | Global OpenSSL initialization/finalization (thread-safe) |
| `include/ftpclient.h` *(amendment)* | Add `ftp_set_credential_provider`, `ftp_clear_credential_provider`, `ftp_cert_pins_t` |
| `tests/tls_handshake_compliance.cpp` | Multi-server TLS negotiation tests |
| `tests/secure_memory_forensic.cpp` | Core dump credential leak detection |
| `tests/cert_pinning_test.cpp` | SPKI pin match/mismatch validation |
| `tests/secret_provider_test.py` | Python `cffi` provider callback integration |
| `tests/tls_throughput_baseline.cpp` | Performance comparison vs Phase 2 |

---

## 12. Transition Criteria to Phase 4

Phase 3 is **ratified** when:

1. All 5 Quality Gates (Section 10) pass on CI for Linux x86_64 and Windows x64.
2. The secure memory forensic audit finds **zero** credential strings in standard heap or core dumps.
3. TLS handshake succeeds against:
   - vsftpd (TLS 1.3)
   - ProFTPD (TLS 1.2)
   - Windows Server IIS FTP with FTPS
   - A public test server (e.g., `test.rebex.net`) for real-world validation.
4. The Python `cffi` provider callback test passes, proving the C ABI extension is sound.
5. **No OpenSSL symbols leak across the ABI boundary.** Verified by `nm -D`: only `ftp_*` symbols exported; `SSL_*`, `X509_*`, etc., must be internal or statically resolved.
6. Valgrind/ASan reports clean with zero suppressions related to TLS or credential handling.

**Upon ratification:** The `Transport` interface remains frozen. `TlsTransport` becomes the reference implementation for all future transport security extensions. The `CredentialVault` pattern is adopted as mandatory for any future auth methods (e.g., OAuth tokens in Phase 7).

---

**End of Phase 3 Specification**
