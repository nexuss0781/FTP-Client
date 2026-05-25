/*
 * CredentialVault.hpp - Secure Credential Storage
 * 
 * Internal C++ class for per-handle secure credential storage.
 * Credentials are deep-copied into SecureAllocator-backed buffers
 * on connect and zeroed on disconnect/destroy.
 * 
 * Per Phase 3 Spec Section 3.3
 */

#ifndef FTPCLIENT_CREDENTIAL_VAULT_HPP
#define FTPCLIENT_CREDENTIAL_VAULT_HPP

#include <cstdint>
#include <cstring>
#include <memory>
#include "SecureAllocator.hpp"

// Include the C ABI credentials struct definition
#include "../../include/ftpclient.h"

namespace ftpclient { namespace security {

/**
 * CredentialVault - Secure credential storage
 * 
 * Lifecycle:
 * 1. ftp_connect() receives ftp_credentials_t (borrowed pointers)
 * 2. Vault deep-copies all strings into SecureAllocator-backed buffers
 * 3. During USER/PASS transmission, vault provides const char* views
 * 4. On ftp_disconnect() or destroy, vault zeroes and frees all buffers
 * 
 * Audit Rule: No std::string or malloc-allocated copy of credentials
 * exists elsewhere in the process address space.
 */
class CredentialVault {
public:
    /**
     * Default constructor - empty vault
     */
    CredentialVault();

    /**
     * Destructor - securely purges all credentials
     */
    ~CredentialVault();

    // Prevent copying - credentials must not be duplicated
    CredentialVault(const CredentialVault&) = delete;
    CredentialVault& operator=(const CredentialVault&) = delete;

    // Allow moving
    CredentialVault(CredentialVault&& other) noexcept;
    CredentialVault& operator=(CredentialVault&& other) noexcept;

    /**
     * Store credentials - deep copy into secure memory
     * 
     * @param creds Pointer to credentials struct (borrowed)
     * @return 0 on success, negative error code on failure
     */
    int32_t store(const ftp_credentials_t* creds);

    /**
     * Get username - borrowed view, valid until purge()
     */
    const char* username() const;

    /**
     * Get password - borrowed view, valid until purge()
     */
    const char* password() const;

    /**
     * Get password length
     */
    std::size_t password_length() const;

    /**
     * Get host - borrowed view, valid until purge()
     */
    const char* host() const;

    /**
     * Get port
     */
    uint16_t port() const;

    /**
     * Get TLS mode
     */
    int32_t use_tls() const;

    /**
     * Get verify certificate mode
     */
    int32_t verify_cert() const;

    /**
     * Get CA bundle path - borrowed view, valid until purge()
     */
    const char* ca_bundle_path() const;

    /**
     * Zero and free all credentials
     */
    void purge();

    /**
     * Check if vault is empty (no credentials stored)
     */
    bool empty() const;

private:
    // Custom deleter that securely zeros memory before freeing
    static void secure_deleter(char* p);

    // Credential storage - using unique_ptr with custom deleter for secure zeroing
    std::unique_ptr<char[], decltype(&secure_deleter)> host_;
    std::unique_ptr<char[], decltype(&secure_deleter)> username_;
    std::unique_ptr<char[], decltype(&secure_deleter)> password_;
    std::unique_ptr<char[], decltype(&secure_deleter)> ca_bundle_path_;

    std::size_t host_len_;
    std::size_t username_len_;
    std::size_t password_len_;
    std::size_t ca_bundle_path_len_;

    uint16_t port_;
    int32_t use_tls_;
    int32_t verify_cert_;

    /**
     * Helper to securely copy a string into allocated buffer
     */
    static std::unique_ptr<char[], decltype(&secure_deleter)> copy_string(
        const char* src, std::size_t& out_len);
};

}} // namespace ftpclient::security

#endif /* FTPCLIENT_CREDENTIAL_VAULT_HPP */
