/*
 * TlsConfig.hpp - TLS Configuration Parameters
 * 
 * Defines TLS configuration options including:
 * - Minimum TLS version enforcement
 * - Cipher suite policies
 * - Certificate validation modes
 * - SNI configuration
 * 
 * Per Phase 3 Spec Section 7
 */

#ifndef FTPCLIENT_TLS_CONFIG_HPP
#define FTPCLIENT_TLS_CONFIG_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace ftpclient { namespace security {

/**
 * TLS Version Enumeration
 * 
 * Maps to OpenSSL TLS method versions
 */
enum class TlsVersion : int32_t {
    TLS_1_0 = 0x0301,  // Not recommended, will be rejected by default
    TLS_1_1 = 0x0302,  // Not recommended, will be rejected by default
    TLS_1_2 = 0x0303,  // Minimum allowed version
    TLS_1_3 = 0x0304   // Preferred version
};

/**
 * Certificate Verification Mode
 * 
 * Matches the ftp_credentials_t verify_cert field
 */
enum class CertVerifyMode : int32_t {
    NONE = 0,     // No verification (INSECURE)
    PEER = 1,     // Verify peer certificate chain
    HOST = 2      // Verify peer + hostname match (default)
};

/**
 * FTPS Mode
 * 
 * Explicit: Start plain, upgrade with AUTH TLS (port 21)
 * Implicit: Start TLS immediately (port 990)
 */
enum class FtpsMode : int32_t {
    NONE = 0,      // Plain FTP
    EXPLICIT = 1,  // FTPS Explicit (AUTH TLS)
    IMPLICIT = 2   // FTPS Implicit
};

/**
 * TLS Configuration Structure
 * 
 * Passed to TlsTransport constructor
 */
struct TlsConfig {
    /**
     * Minimum TLS version to accept
     * Default: TLS_1_2 (per spec Section 7.1)
     */
    TlsVersion min_version = TlsVersion::TLS_1_2;

    /**
     * Maximum TLS version (0 = no limit)
     * Default: 0 (negotiate highest available)
     */
    TlsVersion max_version = static_cast<TlsVersion>(0);

    /**
     * Certificate verification mode
     * Default: HOST (verify peer + hostname)
     */
    CertVerifyMode verify_mode = CertVerifyMode::HOST;

    /**
     * Path to custom CA bundle (PEM file)
     * If empty, use system default CA store
     */
    std::string ca_bundle_path;

    /**
     * Server hostname for SNI and hostname verification
     * May be empty if connecting by IP
     */
    std::string server_name;

    /**
     * Certificate pinning - SHA-256 SPKI pins (base64 encoded)
     * Empty vector = no pinning
     */
    std::vector<std::string> cert_pins;

    /**
     * Strict data protection mode
     * If true, fail if PROT P is rejected (default)
     * If false, fall back to PROT C with warning
     */
    bool strict_data_protection = true;

    /**
     * Cipher suite string (OpenSSL format)
     * Empty = use default secure cipher list
     * Example: "HIGH:!aNULL:!MD5:!RC4"
     */
    std::string cipher_list;

    /**
     * Enable session resumption
     * Default: true (per spec Section 7.4)
     */
    bool session_resumption = true;

    /**
     * Constructor with sensible defaults per Phase 3 spec
     */
    TlsConfig() = default;
};

}} // namespace ftpclient::security

#endif /* FTPCLIENT_TLS_CONFIG_HPP */
