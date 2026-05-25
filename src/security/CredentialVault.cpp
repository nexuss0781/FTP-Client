/*
 * CredentialVault.cpp - Secure Credential Storage Implementation
 * 
 * Per Phase 3 Spec Section 3.3
 */

#include "CredentialVault.hpp"
#include "../include/ftpclient.h"
#include <cstdlib>
#include <cstring>

namespace ftpclient { namespace security {

// Static deleter implementation
void CredentialVault::secure_deleter(char* p) {
    if (p) {
        // Note: The memory was allocated with SecureAllocator which handles
        // secure zeroing on deallocate. We just need to ensure the pointer
        // is properly freed through the allocator.
        // For unique_ptr, we manually handle this since we can't pass allocator
        SecureMemoryOps::secure_zero(p, std::strlen(p));
        std::free(p);
    }
}

CredentialVault::CredentialVault()
    : host_(nullptr, &secure_deleter)
    , username_(nullptr, &secure_deleter)
    , password_(nullptr, &secure_deleter)
    , ca_bundle_path_(nullptr, &secure_deleter)
    , host_len_(0)
    , username_len_(0)
    , password_len_(0)
    , ca_bundle_path_len_(0)
    , port_(0)
    , use_tls_(0)
    , verify_cert_(1)  // Default: verify peer
{
}

CredentialVault::~CredentialVault() {
    purge();
}

CredentialVault::CredentialVault(CredentialVault&& other) noexcept
    : host_(std::move(other.host_))
    , username_(std::move(other.username_))
    , password_(std::move(other.password_))
    , ca_bundle_path_(std::move(other.ca_bundle_path_))
    , host_len_(other.host_len_)
    , username_len_(other.username_len_)
    , password_len_(other.password_len_)
    , ca_bundle_path_len_(other.ca_bundle_path_len_)
    , port_(other.port_)
    , use_tls_(other.use_tls_)
    , verify_cert_(other.verify_cert_)
{
    // Reset other
    other.host_len_ = 0;
    other.username_len_ = 0;
    other.password_len_ = 0;
    other.ca_bundle_path_len_ = 0;
    other.port_ = 0;
    other.use_tls_ = 0;
    other.verify_cert_ = 1;
}

CredentialVault& CredentialVault::operator=(CredentialVault&& other) noexcept {
    if (this != &other) {
        purge();
        
        host_ = std::move(other.host_);
        username_ = std::move(other.username_);
        password_ = std::move(other.password_);
        ca_bundle_path_ = std::move(other.ca_bundle_path_);
        
        host_len_ = other.host_len_;
        username_len_ = other.username_len_;
        password_len_ = other.password_len_;
        ca_bundle_path_len_ = other.ca_bundle_path_len_;
        port_ = other.port_;
        use_tls_ = other.use_tls_;
        verify_cert_ = other.verify_cert_;
        
        // Reset other
        other.host_len_ = 0;
        other.username_len_ = 0;
        other.password_len_ = 0;
        other.ca_bundle_path_len_ = 0;
        other.port_ = 0;
        other.use_tls_ = 0;
        other.verify_cert_ = 1;
    }
    return *this;
}

std::unique_ptr<char[], decltype(&CredentialVault::secure_deleter)> 
CredentialVault::copy_string(const char* src, std::size_t& out_len) {
    out_len = 0;
    
    if (!src || src[0] == '\0') {
        return std::unique_ptr<char[], decltype(&secure_deleter)>(nullptr, &secure_deleter);
    }
    
    std::size_t len = std::strlen(src);
    
    // Allocate with space for null terminator using standard malloc
    // (SecureAllocator would be ideal but unique_ptr with custom deleter works)
    char* buffer = static_cast<char*>(std::malloc(len + 1));
    if (!buffer) {
        return std::unique_ptr<char[], decltype(&secure_deleter)>(nullptr, &secure_deleter);
    }
    
    std::memcpy(buffer, src, len + 1);
    out_len = len;
    
    return std::unique_ptr<char[], decltype(&secure_deleter)>(buffer, &secure_deleter);
}

int32_t CredentialVault::store(const ftp_credentials_t* creds) {
    if (!creds) {
        return -202;  // FTP_ERR_INVALID_ARGUMENT
    }
    
    // Validate required fields
    if (!creds->host || creds->host[0] == '\0') {
        return -202;  // FTP_ERR_INVALID_ARGUMENT
    }
    
    // Purge existing credentials first
    purge();
    
    // Copy host (required)
    host_ = copy_string(creds->host, host_len_);
    if (!host_) {
        purge();
        return -101;  // FTP_ERR_NOMEM
    }
    
    // Copy username (optional - may be NULL for anonymous)
    if (creds->username) {
        username_ = copy_string(creds->username, username_len_);
        if (!username_) {
            purge();
            return -101;  // FTP_ERR_NOMEM
        }
    }
    
    // Copy password (optional)
    if (creds->password && creds->password[0] != '\0') {
        password_ = copy_string(creds->password, password_len_);
        if (!password_) {
            purge();
            return -101;  // FTP_ERR_NOMEM
        }
    }
    
    // Copy CA bundle path (optional)
    if (creds->ca_bundle_path) {
        ca_bundle_path_ = copy_string(creds->ca_bundle_path, ca_bundle_path_len_);
        if (!ca_bundle_path_) {
            purge();
            return -101;  // FTP_ERR_NOMEM
        }
    }
    
    // Store scalar values
    port_ = creds->port;
    use_tls_ = creds->use_tls;
    verify_cert_ = creds->verify_cert;
    
    return 0;  // FTP_OK
}

const char* CredentialVault::username() const {
    return username_.get();
}

const char* CredentialVault::password() const {
    return password_.get();
}

std::size_t CredentialVault::password_length() const {
    return password_len_;
}

const char* CredentialVault::host() const {
    return host_.get();
}

uint16_t CredentialVault::port() const {
    return port_;
}

int32_t CredentialVault::use_tls() const {
    return use_tls_;
}

int32_t CredentialVault::verify_cert() const {
    return verify_cert_;
}

const char* CredentialVault::ca_bundle_path() const {
    return ca_bundle_path_.get();
}

void CredentialVault::purge() {
    // Reset lengths first
    host_len_ = 0;
    username_len_ = 0;
    password_len_ = 0;
    ca_bundle_path_len_ = 0;
    
    port_ = 0;
    use_tls_ = 0;
    verify_cert_ = 1;
    
    // Reset unique_ptrs - this invokes secure_deleter which zeros and frees
    host_.reset(nullptr);
    username_.reset(nullptr);
    password_.reset(nullptr);
    ca_bundle_path_.reset(nullptr);
}

bool CredentialVault::empty() const {
    return !host_;
}

}} // namespace ftpclient::security
