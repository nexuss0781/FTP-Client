/*
 * SecretProvider.hpp - Credential Provider Callback Integration
 * 
 * Implements the secret injection interface per Phase 3 Spec Section 6.
 * Allows runtime credential resolution without the C++ core ever persisting secrets.
 */

#ifndef FTPCLIENT_SECRET_PROVIDER_HPP
#define FTPCLIENT_SECRET_PROVIDER_HPP

#include <cstdint>
#include "../include/ftpclient.h"

namespace ftpclient { namespace security {

/**
 * SecretProvider - Dynamic credential resolution
 * 
 * This class wraps the C ABI callback and manages credential lifecycle.
 * The callback is invoked inside ftp_connect(), not at registration time.
 * 
 * Per Phase 3 Spec Section 6.2
 */
class SecretProvider {
public:
    /**
     * Constructor
     * 
     * @param provider Callback function to fetch credentials dynamically
     * @param user_data Opaque pointer passed to provider callback
     */
    explicit SecretProvider(ftp_credential_provider_cb_t provider, void* user_data);

    /**
     * Destructor
     */
    ~SecretProvider();

    // Prevent copying - callbacks must not be duplicated
    SecretProvider(const SecretProvider&) = delete;
    SecretProvider& operator=(const SecretProvider&) = delete;

    /**
     * Fetch credentials from provider
     * 
     * Invokes the registered callback to obtain credentials.
     * Credentials are returned in a caller-allocated struct.
     * 
     * @param attempt 0 = first attempt, 1+ = retry after failure
     * @return FTP_OK on success, negative error code on failure
     */
    int32_t fetch_credentials(ftp_credentials_t* out_creds, int32_t attempt);

    /**
     * Check if provider is set
     */
    bool is_set() const;

    /**
     * Clear the provider
     */
    void clear();

private:
    ftp_credential_provider_cb_t provider_;
    void* user_data_;
    bool is_set_;
};

}} // namespace ftpclient::security

#endif /* FTPCLIENT_SECRET_PROVIDER_HPP */
