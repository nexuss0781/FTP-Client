/*
 * SecretProvider.cpp - Credential Provider Callback Integration
 * 
 * Per Phase 3 Spec Section 6.2
 */

#include "SecretProvider.hpp"
#include <cstring>

namespace ftpclient { namespace security {

SecretProvider::SecretProvider(ftp_credential_provider_cb_t provider, void* user_data)
    : provider_(provider)
    , user_data_(user_data)
    , is_set_(provider != nullptr)
{
}

SecretProvider::~SecretProvider() {
    clear();
}

int32_t SecretProvider::fetch_credentials(ftp_credentials_t* out_creds, int32_t attempt) {
    if (!is_set_ || !provider_) {
        return -202;  // FTP_ERR_INVALID_ARGUMENT
    }

    if (!out_creds) {
        return -202;  // FTP_ERR_INVALID_ARGUMENT
    }

    // Zero the output struct before calling provider
    std::memset(out_creds, 0, sizeof(ftp_credentials_t));

    // Invoke the callback
    return provider_(out_creds, user_data_, attempt);
}

bool SecretProvider::is_set() const {
    return is_set_ && provider_ != nullptr;
}

void SecretProvider::clear() {
    provider_ = nullptr;
    user_data_ = nullptr;
    is_set_ = false;
}

}} // namespace ftpclient::security
