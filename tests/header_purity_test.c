/*
 * header_purity_test.c - C99 Header Purity Test
 * 
 * This test verifies that ftpclient.h compiles as pure C99 with -pedantic -Werror.
 * As per spec Section 12.5: "Compile the public header with gcc -x c -std=c99 
 * -pedantic -Wall -Werror (forcing C mode, not C++)."
 */

#include "ftpclient.h"

/* Just including the header is the test - if it compiles, it passes */
/* We add some usage to ensure types are actually defined correctly */

int main(void) {
    /* Test type definitions compile correctly in C99 */
    ftp_client_t* handle = NULL;
    ftp_credentials_t creds;
    ftp_upload_options_t options;
    ftp_result_t result;
    
    /* Initialize structs to avoid unused warnings */
    (void)handle;
    
    creds.host = NULL;
    creds.port = 0;
    creds.username = NULL;
    creds.password = NULL;
    creds.use_tls = FTP_TLS_NONE;
    creds.verify_cert = FTP_VERIFY_NONE;
    creds.ca_bundle_path = NULL;
    (void)creds;
    
    options.struct_size = sizeof(ftp_upload_options_t);
    options.max_parallel = 0;
    options.retry_attempts = 0;
    options.retry_base_delay_ms = 0;
    options.resume_enabled = 0;
    options.create_remote_dirs = 0;
    options.remote_chmod = NULL;
    (void)options;
    
    result.status = 0;
    result.files_total = 0;
    result.files_success = 0;
    result.files_failed = 0;
    result.bytes_transferred = 0;
    (void)result;
    
    /* Test constants are available */
    (void)FTP_OK;
    (void)FTP_ERR_NOMEM;
    (void)FTP_ERR_INVALID_HANDLE;
    (void)FTP_CAP_TLS;
    (void)FTP_ABI_VERSION;
    
    /* Test function pointers can be declared */
    ftp_progress_cb_t cb = NULL;
    (void)cb;
    
    return 0;
}
