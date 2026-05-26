/*
 * ftpclient.h - High-Performance FTP Client Library C ABI
 * 
 * This header defines the immutable binary contract between the C++17 implementation
 * and all foreign-language consumers (primarily Python via cffi/ctypes).
 * 
 * ABI Version: 1.0
 * 
 * COMPILATION GUARANTEE: This header compiles as pure C99 with -pedantic -Werror
 */

#ifndef FTPCLIENT_H
#define FTPCLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 * SECTION 2: ABI STABILITY CHARTER
 * ============================================================================
 * 
 * Calling Convention:
 *   - Linux/macOS: System V AMD64 ABI (cdecl)
 *   - Windows x64: Microsoft x64 calling convention
 *   - ARM64: AAPCS64 standard
 * 
 * Symbol Visibility:
 *   - All exported functions marked with FTP_API macro
 *   - Default visibility is hidden; only FTP_API symbols are exported
 */

#if defined(_WIN32) || defined(__CYGWIN__)
    #ifdef FTP_BUILDING_DLL
        #define FTP_API __declspec(dllexport)
    #else
        #define FTP_API __declspec(dllimport)
    #endif
    #define FTP_CALL
#else
    #define FTP_API __attribute__((visibility("default")))
    #define FTP_CALL
#endif

/* ============================================================================
 * SECTION 3: OPAQUE HANDLE ARCHITECTURE
 * ============================================================================
 * 
 * ftp_client_t is an opaque forward declaration.
 * Implementation is a C++ object behind void*.
 * Python/cffi sees this as an opaque pointer type.
 */

typedef struct ftp_client_internal ftp_client_t;

/* ============================================================================
 * SECTION 4: TYPE SYSTEM & DATA STRUCTURES
 * ============================================================================
 */

/*
 * Credential Structure for ftp_connect()
 * 
 * Ownership Semantics:
 *   - All const char* fields are borrowed references during the call
 *   - Implementation deep-copies into secure internal storage before returning
 *   - Caller may free/alter credential strings immediately after ftp_connect() returns
 */
typedef struct {
    const char* host;           /* UTF-8, null-terminated. IP or hostname. */
    uint16_t    port;           /* Host byte order (not network byte order) */
    const char* username;       /* UTF-8, null-terminated. Nullable (NULL = anonymous) */
    const char* password;       /* UTF-8, null-terminated. Nullable */
    int32_t     use_tls;        /* 0 = Plain FTP, 1 = FTPS Explicit, 2 = FTPS Implicit */
    int32_t     verify_cert;    /* 0 = No verify, 1 = Verify peer (default), 2 = Verify + hostname match */
    const char* ca_bundle_path; /* UTF-8, null-terminated. Nullable. Path to PEM bundle. */
} ftp_credentials_t;

/* TLS mode constants */
#define FTP_TLS_NONE      0
#define FTP_TLS_EXPLICIT  1
#define FTP_TLS_IMPLICIT  2

/* Certificate verification constants */
#define FTP_VERIFY_NONE    0
#define FTP_VERIFY_PEER    1
#define FTP_VERIFY_HOST    2

/*
 * Certificate Pinning Structure (Optional Extension - Phase 3 Spec Section 5.4)
 * 
 * For advanced certificate pinning scenarios.
 * If pins are provided, library validates that at least one pin matches
 * the leaf certificate's SubjectPublicKeyInfo.
 */
typedef struct {
    const char* sha256_pin;      /* Base64-encoded SHA-256 SPKI pin */
    int32_t     pin_count;       /* Number of pins in array */
} ftp_cert_pins_t;

/*
 * Certificate Validation Callback Type (Phase 3 Spec Section 5.4)
 * 
 * For advanced Python use cases (e.g., interactive "trust this host?" prompts).
 * 
 * @param subject      Certificate subject (UTF-8, borrowed, valid only during call)
 * @param issuer       Certificate issuer (UTF-8, borrowed, valid only during call)
 * @param fingerprint  SHA-256 hex fingerprint (borrowed, valid only during call)
 * @param error_code   OpenSSL X509_V_ERR_* code
 * @param user_data    Opaque pointer from registration
 * @return 1 = Accept certificate, 0 = Reject
 */
typedef int32_t (*ftp_cert_verify_cb_t)(
    const char*   subject,
    const char*   issuer,
    const char*   fingerprint,
    int32_t       error_code,
    void*         user_data
);

/*
 * Credential Provider Callback Type (Phase 3 Spec Section 6.2)
 * 
 * Callback invoked at connection time to fetch credentials dynamically.
 * Allows integration with Python keyring modules or HSMs without the
 * C++ library ever persisting secrets.
 * 
 * @param out_creds  Pre-allocated struct to fill (caller allocates)
 * @param user_data  Opaque pointer from registration
 * @param attempt    0 = first attempt, 1+ = retry after failure
 * @return FTP_OK on success, negative error code on failure
 */
typedef int32_t (*ftp_credential_provider_cb_t)(
    ftp_credentials_t* out_creds,
    void*              user_data,
    int32_t            attempt
);

/*
 * Upload Options Structure
 * 
 * Extensibility: First member is struct_size for version detection
 */
typedef struct {
    uint32_t    struct_size;        /* sizeof(this struct) at compile time */
    int32_t     max_parallel;       /* Max concurrent file uploads. 0 = library default (4) */
    int32_t     retry_attempts;     /* Per-file retry count. 0 = library default (3) */
    uint64_t    retry_base_delay_ms;/* Exponential backoff base. 0 = default (1000ms) */
    int32_t     resume_enabled;     /* 0 = overwrite, 1 = resume partial */
    int32_t     create_remote_dirs; /* 0 = fail if missing, 1 = auto-create */
    const char* remote_chmod;       /* Nullable. e.g., "0644". Applied after upload */
} ftp_upload_options_t;

/*
 * Result Structure for directory upload completion
 * 
 * Ownership: Caller allocates stack/heap struct; implementation fills fields
 * 
 * Phase 4 Extension: Added file_results array for per-file details.
 * See ftp_result_free() for cleanup requirements.
 */
typedef struct {
    int32_t     status;             /* Overall operation status code */
    uint64_t    files_total;
    uint64_t    files_success;
    uint64_t    files_failed;
    uint64_t    bytes_transferred;
    
    /* Phase 4 extension fields - only valid if struct_size >= sizeof(extended struct) */
    uint64_t            file_result_count;
    struct ftp_file_result* file_results; /* Array allocated by C++, freed by ftp_result_free() */
} ftp_result_t;

/*
 * Per-file result structure (Phase 4 extension)
 * Per spec Section 9.1
 * 
 * Phase 5 Extension: Added attempt_count and final_error fields per Phase 5 Spec Section 8.2
 */
typedef struct ftp_file_result {
    const char* local_path;       /* Owned by library, freed by ftp_result_free() */
    const char* remote_path;      /* Owned by library, freed by ftp_result_free() */
    int32_t     status;           /* FTP_OK or error code */
    uint64_t    bytes_sent;       /* Bytes successfully transferred */
    /* Phase 5 extensions - attempt tracking per Phase 5 Spec Section 8.2 */
    uint32_t    attempt_count;    /* 1 = first try succeeded, >1 = retried */
    int32_t     final_error;      /* Error code from last attempt (if failed) */
} ftp_file_result_t;

/*
 * Progress Callback Type
 * 
 * Thread Safety: Invoked from internal C++ worker threads.
 * Python binding must acquire GIL before calling Python callables.
 * Callback duration should be minimal (under 1ms) to avoid stalling transfer pipeline.
 */
typedef void (*ftp_progress_cb_t)(
    const char* local_path,     /* UTF-8, borrowed string valid only during callback */
    const char* remote_path,    /* UTF-8, borrowed */
    uint64_t    bytes_current,
    uint64_t    bytes_total,
    double      bytes_per_second,
    void*       user_data       /* Opaque pointer passed through from caller */
);

/* ============================================================================
 * SECTION 5: ERROR CODE TAXONOMY
 * ============================================================================
 * 
 * All functions return int32_t status codes.
 * Zero is success. Negative codes are errors. Positive codes are warnings.
 */

#define FTP_OK                          0

/* System / Resource Errors (-1xx) */
#define FTP_ERR_NOMEM                   -101
#define FTP_ERR_SYSTEM                  -102

/* Argument / Precondition Errors (-2xx) */
#define FTP_ERR_INVALID_HANDLE          -201
#define FTP_ERR_INVALID_ARGUMENT        -202
#define FTP_ERR_INVALID_STATE           -203

/* Authentication Errors (-3xx) */
#define FTP_ERR_AUTH_FAILED             -301
#define FTP_ERR_AUTH_TLS_REQUIRED       -302
#define FTP_ERR_CERT_VERIFY             -303

/* Network / Transport Errors (-4xx) */
#define FTP_ERR_CONNECT                 -401
#define FTP_ERR_TIMEOUT                 -402
#define FTP_ERR_NETWORK_RESET           -403
#define FTP_ERR_DNS                     -404

/* Protocol / Server Errors (-5xx) */
#define FTP_ERR_PROTOCOL                -501
#define FTP_ERR_SERVER_DENIED           -502
#define FTP_ERR_PASSIVE_FAILED          -503

/* Transfer / I/O Errors (-6xx) */
#define FTP_ERR_LOCAL_IO                -601
#define FTP_ERR_REMOTE_IO               -602
#define FTP_ERR_PARTIAL                 -603

/* Warnings (+1xx) */
#define FTP_WARN_PARTIAL_RETRY          101
#define FTP_WARN_SKIPPED                102

/* ============================================================================
 * SECTION 6: EXPORTED FUNCTION SURFACE API
 * ============================================================================
 */

/* ----------------------------------------------------------------------------
 * 6.1 Lifecycle Functions
 * ----------------------------------------------------------------------------
 */

/**
 * Allocates internal state. Does NOT connect.
 * 
 * @param out_handle  Output pointer to receive the handle. Must be NULL on input.
 * @return FTP_OK on success, FTP_ERR_NOMEM on allocation failure
 */
FTP_API int32_t FTP_CALL ftp_client_create(ftp_client_t** out_handle);

/**
 * Releases all resources. Idempotent only if called once.
 * 
 * @param handle  The client handle to destroy
 * @return FTP_OK on success, FTP_ERR_INVALID_HANDLE if handle is NULL or already destroyed
 */
FTP_API int32_t FTP_CALL ftp_client_destroy(ftp_client_t* handle);

/* ----------------------------------------------------------------------------
 * 6.2 Configuration Functions (Pre-Connect)
 * ----------------------------------------------------------------------------
 */

/**
 * Set internal buffer size for data channels.
 * 
 * @param handle      The client handle
 * @param size_bytes  Buffer size in bytes. 0 = default (256KB)
 * @return FTP_OK on success, FTP_ERR_INVALID_HANDLE if handle is invalid
 */
FTP_API int32_t FTP_CALL ftp_set_buffer_size(ftp_client_t* handle, uint64_t size_bytes);

/**
 * Set connect timeout in milliseconds.
 * 
 * @param handle  The client handle
 * @param ms      Timeout in milliseconds. 0 = default (5000ms)
 * @return FTP_OK on success, FTP_ERR_INVALID_HANDLE if handle is invalid
 */
FTP_API int32_t FTP_CALL ftp_set_timeout_connect_ms(ftp_client_t* handle, uint32_t ms);

/**
 * Set command/response timeout in milliseconds.
 * 
 * @param handle  The client handle
 * @param ms      Timeout in milliseconds. 0 = default (30000ms)
 * @return FTP_OK on success, FTP_ERR_INVALID_HANDLE if handle is invalid
 */
FTP_API int32_t FTP_CALL ftp_set_timeout_command_ms(ftp_client_t* handle, uint32_t ms);

/* ----------------------------------------------------------------------------
 * 6.3 Connection Management
 * ----------------------------------------------------------------------------
 */

/**
 * Establish control connection and authenticate.
 * 
 * @param handle  The client handle
 * @param creds   Pointer to credentials structure (borrowed, deep-copied internally)
 * @return FTP_OK on success, or error code from Section 5
 */
FTP_API int32_t FTP_CALL ftp_connect(ftp_client_t* handle, const ftp_credentials_t* creds);

/**
 * Disconnect from the server.
 * 
 * @param handle  The client handle
 * @return FTP_OK on success, FTP_ERR_INVALID_HANDLE if handle is invalid
 */
FTP_API int32_t FTP_CALL ftp_disconnect(ftp_client_t* handle);

/**
 * Connection health check.
 * 
 * @param handle  The client handle
 * @return FTP_OK if idle and responsive, error code otherwise
 */
FTP_API int32_t FTP_CALL ftp_ping(ftp_client_t* handle);

/* ----------------------------------------------------------------------------
 * 6.4 Transfer Operations (Stubs for Phase 1 — signatures only)
 * ----------------------------------------------------------------------------
 */

/**
 * Upload directory tree. Actual implementation in Phase 4.
 * 
 * @param handle        The client handle
 * @param local_path    UTF-8, null-terminated local directory path
 * @param remote_path   UTF-8, null-terminated remote directory path
 * @param options       Nullable upload options (uses defaults if NULL)
 * @param progress_cb   Nullable progress callback
 * @param user_data     Opaque pointer passed to progress_cb
 * @param out_result    Nullable result structure (caller-allocated, must call ftp_result_free())
 * @return FTP_OK on success, or error code from Section 5
 */
FTP_API int32_t FTP_CALL ftp_upload_dir(
    ftp_client_t*           handle,
    const char*             local_path,
    const char*             remote_path,
    const ftp_upload_options_t* options,
    ftp_progress_cb_t       progress_cb,
    void*                   user_data,
    ftp_result_t*           out_result
);

/**
 * Free result resources allocated by ftp_upload_dir().
 * 
 * Per Phase 4 Spec Section 9.2 - Memory Ownership Amendment
 * 
 * Caller MUST call this function on any ftp_result_t passed to ftp_upload_dir().
 * This frees the file_results array and zeroes all strings before deallocation.
 * Safe to call with file_results == NULL (no-op for that field).
 * 
 * @param result  Result structure to free (may be NULL, then no-op)
 * @return FTP_OK on success, FTP_ERR_INVALID_ARGUMENT if result is NULL
 */
FTP_API int32_t FTP_CALL ftp_result_free(ftp_result_t* result);

/* ----------------------------------------------------------------------------
 * 6.5 Version / Capability Introspection
 * ----------------------------------------------------------------------------
 */

/**
 * Returns library version as packed integer.
 * 
 * Format: 0xMMmmpp00 (Major, Minor, Patch)
 * Example: Version 1.2.3 returns 0x01020300
 * 
 * @return Packed version integer
 */
FTP_API uint32_t FTP_CALL ftp_get_version(void);

/**
 * Fills capability flags. Allows caller to detect optional features compiled in.
 * 
 * @param out_caps  Pointer to uint64_t to receive capability bitmask
 * @return FTP_OK on success, FTP_ERR_INVALID_ARGUMENT if out_caps is NULL
 */
FTP_API int32_t FTP_CALL ftp_get_capabilities(uint64_t* out_caps);

/* Capability Flags (bitmask) */
#define FTP_CAP_TLS             0x0001  /* TLS/FTPS support compiled in */
#define FTP_CAP_SENDFILE        0x0002  /* sendfile() zero-copy available (Linux) */
#define FTP_CAP_COMPRESSION     0x0004  /* Compression (MODE Z) support */
#define FTP_CAP_IPV6            0x0008  /* IPv6 support */
#define FTP_CAP_RESUME          0x0010  /* Resume/REST support */

/* ----------------------------------------------------------------------------
 * 6.6 Security Functions (Phase 3 - Credential Provider & Certificate Validation)
 * ----------------------------------------------------------------------------
 */

/**
 * Set a credential provider callback. Overrides static credentials.
 * 
 * Per Phase 3 Spec Section 6.2
 * 
 * @param handle       The client handle
 * @param provider     Callback function to fetch credentials dynamically
 * @param user_data    Opaque pointer passed to provider callback
 * @return FTP_OK on success, FTP_ERR_INVALID_HANDLE if handle is invalid
 */
FTP_API int32_t FTP_CALL ftp_set_credential_provider(
    ftp_client_t*               handle,
    ftp_credential_provider_cb_t provider,
    void*                       user_data
);

/**
 * Clear any set provider, reverting to static credentials.
 * 
 * Per Phase 3 Spec Section 6.2
 * 
 * @param handle  The client handle
 * @return FTP_OK on success, FTP_ERR_INVALID_HANDLE if handle is invalid
 */
FTP_API int32_t FTP_CALL ftp_clear_credential_provider(ftp_client_t* handle);

/**
 * Set certificate validation callback for advanced verification scenarios.
 * 
 * Per Phase 3 Spec Section 5.4
 * 
 * @param handle    The client handle
 * @param callback  Callback function for certificate validation override
 * @param user_data Opaque pointer passed to callback
 * @return FTP_OK on success, FTP_ERR_INVALID_HANDLE if handle is invalid
 */
FTP_API int32_t FTP_CALL ftp_set_cert_verify_callback(
    ftp_client_t*         handle,
    ftp_cert_verify_cb_t  callback,
    void*                 user_data
);

/**
 * Set certificate pins for pin-based validation.
 * 
 * Per Phase 3 Spec Section 5.4
 * 
 * @param handle  The client handle
 * @param pins    Array of certificate pins (sha256_pin array)
 * @param count   Number of pins in array
 * @return FTP_OK on success, FTP_ERR_INVALID_HANDLE if handle is invalid
 */
FTP_API int32_t FTP_CALL ftp_set_cert_pins(
    ftp_client_t*      handle,
    const char* const* pins,
    int32_t            count
);

/* ============================================================================
 * SECTION 10: EXTENSIBILITY MECHANISM (Future-Proofing)
 * ============================================================================
 */

/**
 * Reserved for future options without breaking ABI.
 * 
 * All option structs must include struct_size as first member.
 * This allows the C++ implementation to detect struct version from caller
 * and handle older/newer callers gracefully.
 */
typedef struct {
    uint32_t    struct_size;    /* sizeof(this struct) at compile time */
    uint32_t    flags;
    void*       reserved1;
    void*       reserved2;
} ftp_context_ext_t;

/* ABI Version constant for runtime detection */
#define FTP_ABI_VERSION 1

/* ============================================================================
 * SECTION 11: PHASE 5 RESILIENCE FUNCTIONS
 * ============================================================================
 * 
 * Retry policy configuration for fault recovery per Phase 5 Spec Section 4.
 */

/**
 * Set retry policy parameters for a client handle.
 * 
 * Per Phase 5 Spec Section 4.1 - ABI Amendment
 * 
 * @param handle          The client handle
 * @param max_attempts    Maximum retry attempts (default: 3)
 * @param base_delay_ms   Base delay for exponential backoff in ms (default: 1000)
 * @param max_delay_ms    Maximum delay cap in ms (default: 30000)
 * @return FTP_OK on success, FTP_ERR_INVALID_HANDLE if handle is invalid
 */
FTP_API int32_t FTP_CALL ftp_set_retry_policy(
    ftp_client_t* handle,
    uint32_t      max_attempts,
    uint64_t      base_delay_ms,
    uint64_t      max_delay_ms
);

#ifdef __cplusplus
}
#endif

#endif /* FTPCLIENT_H */
