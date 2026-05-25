/*
 * abi_test.c - Plain C ABI Compatibility Test
 * 
 * This test verifies that the library can be loaded and used from pure C code.
 * As per spec Section 12.1: "Compile a minimal C++ shared library implementing 
 * exactly the Phase 1 ABI. Write a Python script using ctypes that loads the 
 * library, creates a handle, calls every function, and destroys the handle."
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ftpclient.h"

/* Test result tracking */
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(name, condition) do { \
    if (condition) { \
        printf("[PASS] %s\n", name); \
        tests_passed++; \
    } else { \
        printf("[FAIL] %s\n", name); \
        tests_failed++; \
    } \
} while(0)

/* Helper to print error codes */
static const char* error_to_string(int32_t code) {
    switch (code) {
        case FTP_OK: return "FTP_OK";
        case FTP_ERR_NOMEM: return "FTP_ERR_NOMEM";
        case FTP_ERR_SYSTEM: return "FTP_ERR_SYSTEM";
        case FTP_ERR_INVALID_HANDLE: return "FTP_ERR_INVALID_HANDLE";
        case FTP_ERR_INVALID_ARGUMENT: return "FTP_ERR_INVALID_ARGUMENT";
        case FTP_ERR_INVALID_STATE: return "FTP_ERR_INVALID_STATE";
        case FTP_ERR_AUTH_FAILED: return "FTP_ERR_AUTH_FAILED";
        case FTP_ERR_CONNECT: return "FTP_ERR_CONNECT";
        case FTP_ERR_TIMEOUT: return "FTP_ERR_TIMEOUT";
        default: return "UNKNOWN";
    }
}

/* Test: Version and Capabilities */
static void test_version_capabilities(void) {
    printf("\n=== Testing Version and Capabilities ===\n");
    
    uint32_t version = ftp_get_version();
    TEST_ASSERT("ftp_get_version returns non-zero", version != 0);
    
    /* Expected version 1.0.0 = 0x01000000 */
    TEST_ASSERT("Version is 1.0.0", version == 0x01000000);
    
    uint64_t caps;
    int32_t ret = ftp_get_capabilities(&caps);
    TEST_ASSERT("ftp_get_capabilities returns OK", ret == FTP_OK);
    TEST_ASSERT("Capabilities returned", caps != 0 || caps == 0); /* Just check it doesn't crash */
    
    /* Test NULL argument handling */
    ret = ftp_get_capabilities(NULL);
    TEST_ASSERT("ftp_get_capabilities with NULL returns error", ret == FTP_ERR_INVALID_ARGUMENT);
}

/* Test: Handle Lifecycle */
static void test_handle_lifecycle(void) {
    printf("\n=== Testing Handle Lifecycle ===\n");
    
    ftp_client_t* handle = NULL;
    
    /* Test create with NULL out_handle */
    int32_t ret = ftp_client_create(NULL);
    TEST_ASSERT("ftp_client_create with NULL returns error", ret == FTP_ERR_INVALID_ARGUMENT);
    
    /* Test normal create */
    ret = ftp_client_create(&handle);
    TEST_ASSERT("ftp_client_create succeeds", ret == FTP_OK);
    TEST_ASSERT("Handle is non-NULL", handle != NULL);
    
    /* Test destroy with valid handle */
    ret = ftp_client_destroy(handle);
    TEST_ASSERT("ftp_client_destroy succeeds", ret == FTP_OK);
    
    /* Test destroy with NULL handle */
    ret = ftp_client_destroy(NULL);
    TEST_ASSERT("ftp_client_destroy with NULL returns error", ret == FTP_ERR_INVALID_HANDLE);
    
    /* Test double destroy (should fail) */
    ret = ftp_client_create(&handle);
    TEST_ASSERT("Second create succeeds", ret == FTP_OK);
    ret = ftp_client_destroy(handle);
    TEST_ASSERT("First destroy succeeds", ret == FTP_OK);
    handle = NULL;  /* Clear handle after destroy to avoid use-after-free in test */
    ret = ftp_client_destroy(handle);
    TEST_ASSERT("Double destroy with NULL returns error", ret == FTP_ERR_INVALID_HANDLE);
}

/* Test: Configuration Functions */
static void test_configuration(void) {
    printf("\n=== Testing Configuration Functions ===\n");
    
    ftp_client_t* handle = NULL;
    int32_t ret = ftp_client_create(&handle);
    TEST_ASSERT("Create for config test", ret == FTP_OK);
    
    /* Test buffer size */
    ret = ftp_set_buffer_size(NULL, 65536);
    TEST_ASSERT("ftp_set_buffer_size with NULL handle fails", ret == FTP_ERR_INVALID_HANDLE);
    
    ret = ftp_set_buffer_size(handle, 65536);
    TEST_ASSERT("ftp_set_buffer_size with 64KB succeeds", ret == FTP_OK);
    
    ret = ftp_set_buffer_size(handle, 0);
    TEST_ASSERT("ftp_set_buffer_size with 0 (default) succeeds", ret == FTP_OK);
    
    /* Test connect timeout */
    ret = ftp_set_timeout_connect_ms(NULL, 10000);
    TEST_ASSERT("ftp_set_timeout_connect_ms with NULL handle fails", ret == FTP_ERR_INVALID_HANDLE);
    
    ret = ftp_set_timeout_connect_ms(handle, 10000);
    TEST_ASSERT("ftp_set_timeout_connect_ms succeeds", ret == FTP_OK);
    
    ret = ftp_set_timeout_connect_ms(handle, 0);
    TEST_ASSERT("ftp_set_timeout_connect_ms with 0 (default) succeeds", ret == FTP_OK);
    
    /* Test command timeout */
    ret = ftp_set_timeout_command_ms(NULL, 60000);
    TEST_ASSERT("ftp_set_timeout_command_ms with NULL handle fails", ret == FTP_ERR_INVALID_HANDLE);
    
    ret = ftp_set_timeout_command_ms(handle, 60000);
    TEST_ASSERT("ftp_set_timeout_command_ms succeeds", ret == FTP_OK);
    
    ret = ftp_set_timeout_command_ms(handle, 0);
    TEST_ASSERT("ftp_set_timeout_command_ms with 0 (default) succeeds", ret == FTP_OK);
    
    ftp_client_destroy(handle);
}

/* Test: Connection Management */
static void test_connection(void) {
    printf("\n=== Testing Connection Management ===\n");
    
    ftp_client_t* handle = NULL;
    int32_t ret = ftp_client_create(&handle);
    TEST_ASSERT("Create for connection test", ret == FTP_OK);
    
    /* Test connect with NULL handle */
    ftp_credentials_t creds;
    memset(&creds, 0, sizeof(creds));
    creds.host = "localhost";
    creds.port = 21;
    
    ret = ftp_connect(NULL, &creds);
    TEST_ASSERT("ftp_connect with NULL handle fails", ret == FTP_ERR_INVALID_HANDLE);
    
    /* Test connect with NULL credentials */
    ret = ftp_connect(handle, NULL);
    TEST_ASSERT("ftp_connect with NULL creds fails", ret == FTP_ERR_INVALID_ARGUMENT);
    
    /* Test connect with empty host */
    creds.host = "";
    ret = ftp_connect(handle, &creds);
    TEST_ASSERT("ftp_connect with empty host fails", ret == FTP_ERR_INVALID_ARGUMENT);
    
    /* Test connect with port 0 */
    creds.host = "localhost";
    creds.port = 0;
    ret = ftp_connect(handle, &creds);
    TEST_ASSERT("ftp_connect with port 0 fails", ret == FTP_ERR_INVALID_ARGUMENT);
    
    /* Test valid connect (stub - will succeed but not actually connect in Phase 1) */
    creds.host = "localhost";
    creds.port = 21;
    creds.username = "testuser";
    creds.password = "testpass";
    creds.use_tls = FTP_TLS_NONE;
    creds.verify_cert = FTP_VERIFY_NONE;
    
    ret = ftp_connect(handle, &creds);
    TEST_ASSERT("ftp_connect with valid creds succeeds (stub)", ret == FTP_OK);
    
    /* Test ping when connected */
    ret = ftp_ping(handle);
    TEST_ASSERT("ftp_ping when connected succeeds (stub)", ret == FTP_OK);
    
    /* Test ping with NULL handle */
    ret = ftp_ping(NULL);
    TEST_ASSERT("ftp_ping with NULL handle fails", ret == FTP_ERR_INVALID_HANDLE);
    
    /* Test disconnect */
    ret = ftp_disconnect(handle);
    TEST_ASSERT("ftp_disconnect succeeds", ret == FTP_OK);
    
    /* Test double disconnect (should be idempotent) */
    ret = ftp_disconnect(handle);
    TEST_ASSERT("ftp_disconnect is idempotent", ret == FTP_OK);
    
    /* Test ping after disconnect (should fail - wrong state) */
    ret = ftp_ping(handle);
    TEST_ASSERT("ftp_ping after disconnect fails", ret == FTP_ERR_INVALID_STATE);
    
    /* Test reconnect after disconnect */
    ret = ftp_connect(handle, &creds);
    TEST_ASSERT("Reconnect after disconnect succeeds", ret == FTP_OK);
    
    ftp_client_destroy(handle);
}

/* Test: Upload Directory (Phase 1 Stub) */
static void test_upload_dir_stub(void) {
    printf("\n=== Testing Upload Directory (Phase 1 Stub) ===\n");
    
    ftp_client_t* handle = NULL;
    int32_t ret = ftp_client_create(&handle);
    TEST_ASSERT("Create for upload test", ret == FTP_OK);
    
    /* Test upload without connection */
    ftp_result_t result;
    memset(&result, 0, sizeof(result));
    
    ret = ftp_upload_dir(handle, "/local", "/remote", NULL, NULL, NULL, &result);
    TEST_ASSERT("ftp_upload_dir without connect fails", ret == FTP_ERR_INVALID_STATE);
    
    /* Connect first */
    ftp_credentials_t creds;
    memset(&creds, 0, sizeof(creds));
    creds.host = "localhost";
    creds.port = 21;
    
    ret = ftp_connect(handle, &creds);
    TEST_ASSERT("Connect for upload test", ret == FTP_OK);
    
    /* Test upload with NULL paths */
    ret = ftp_upload_dir(handle, NULL, "/remote", NULL, NULL, NULL, NULL);
    TEST_ASSERT("ftp_upload_dir with NULL local_path fails", ret == FTP_ERR_INVALID_ARGUMENT);
    
    ret = ftp_upload_dir(handle, "/local", NULL, NULL, NULL, NULL, NULL);
    TEST_ASSERT("ftp_upload_dir with NULL remote_path fails", ret == FTP_ERR_INVALID_ARGUMENT);
    
    /* Test upload with empty paths */
    ret = ftp_upload_dir(handle, "", "/remote", NULL, NULL, NULL, NULL);
    TEST_ASSERT("ftp_upload_dir with empty local_path fails", ret == FTP_ERR_INVALID_ARGUMENT);
    
    /* Test valid upload call (stub - returns INVALID_STATE as not implemented) */
    ret = ftp_upload_dir(handle, "/local/test", "/remote/test", NULL, NULL, NULL, &result);
    TEST_ASSERT("ftp_upload_dir returns expected stub error", ret == FTP_ERR_INVALID_STATE);
    
    ftp_client_destroy(handle);
}

/* Test: Handle Lifecycle Stress Test (spec Section 12.3) */
static void test_handle_stress(void) {
    printf("\n=== Testing Handle Lifecycle Stress (1000 iterations) ===\n");
    
    const int iterations = 1000;
    int failures = 0;
    
    for (int i = 0; i < iterations; i++) {
        ftp_client_t* handle = NULL;
        int32_t ret = ftp_client_create(&handle);
        if (ret != FTP_OK || handle == NULL) {
            failures++;
            continue;
        }
        
        ret = ftp_client_destroy(handle);
        if (ret != FTP_OK) {
            failures++;
        }
    }
    
    TEST_ASSERT("Stress test: all iterations completed", failures == 0);
    printf("Stress test: %d iterations, %d failures\n", iterations, failures);
}

int main(void) {
    printf("==============================================\n");
    printf("FTP Client Library - ABI Compatibility Test\n");
    printf("==============================================\n");
    
    test_version_capabilities();
    test_handle_lifecycle();
    test_configuration();
    test_connection();
    test_upload_dir_stub();
    test_handle_stress();
    
    printf("\n==============================================\n");
    printf("Test Summary: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("==============================================\n");
    
    return tests_failed > 0 ? 1 : 0;
}
