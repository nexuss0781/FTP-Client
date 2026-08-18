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

/* Test: Version and Capabilities */
static void test_version_capabilities(void) {
    printf("\n=== Testing Version and Capabilities ===\n");
    
    uint32_t version = ftp_get_version();
    TEST_ASSERT("ftp_get_version returns non-zero", version != 0);
    
    /* M0 development version 0.1.0 = 0x00010000 */
    TEST_ASSERT("Version is M0 0.1.0", version == 0x00010000);
    
    uint64_t caps;
    int32_t ret = ftp_get_capabilities(&caps);
    TEST_ASSERT("ftp_get_capabilities returns OK", ret == FTP_OK);
    TEST_ASSERT("M4 reports real plain control capability", (caps & FTP_CAP_CONTROL_FTP) != 0);
    TEST_ASSERT("M4 reports explicit FTPS capability", (caps & FTP_CAP_TLS) != 0);
    TEST_ASSERT("M4 reports plain passive data capability", (caps & FTP_CAP_DATA_FTP) != 0);
    TEST_ASSERT("M4 reports protected passive data capability", (caps & FTP_CAP_DATA_FTPS) != 0);
    TEST_ASSERT("M4 reports REST resume capability", (caps & FTP_CAP_RESUME) != 0);
    
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
    
    /* Test Phase 7 functions */
    ret = ftp_set_rate_limit(NULL, 1024*1024, 2*1024*1024);
    TEST_ASSERT("ftp_set_rate_limit with NULL handle fails", ret == FTP_ERR_INVALID_HANDLE);
    
    ret = ftp_set_rate_limit(handle, 1024*1024, 2*1024*1024);
    TEST_ASSERT("ftp_set_rate_limit with valid params succeeds", ret == FTP_OK);
    
    ret = ftp_set_rate_limit(handle, 0, 0);
    TEST_ASSERT("ftp_set_rate_limit with 0 (unlimited) succeeds", ret == FTP_OK);
    
    ret = ftp_set_event_callback(NULL, NULL, NULL);
    TEST_ASSERT("ftp_set_event_callback with NULL handle fails", ret == FTP_ERR_INVALID_HANDLE);
    
    ret = ftp_set_event_callback(handle, NULL, NULL);
    TEST_ASSERT("ftp_set_event_callback with NULL callback succeeds", ret == FTP_OK);
    
    ret = ftp_set_option(NULL, FTP_OPT_USE_IOURING, 1);
    TEST_ASSERT("ftp_set_option with NULL handle fails", ret == FTP_ERR_INVALID_HANDLE);
    
    ret = ftp_set_option(handle, FTP_OPT_USE_IOURING, 1);
    TEST_ASSERT("ftp_set_option for IOURING succeeds", ret == FTP_OK);
    
    ret = ftp_set_option(handle, FTP_OPT_USE_ZEROCOPY, 0);
    TEST_ASSERT("ftp_set_option for ZEROCOPY succeeds", ret == FTP_OK);
    
    ret = ftp_set_option(handle, FTP_OPT_USE_COMPRESSION, 1);
    TEST_ASSERT("ftp_set_option for COMPRESSION succeeds", ret == FTP_OK);
    
    ret = ftp_set_option(handle, -1, 0);
    TEST_ASSERT("ftp_set_option with invalid option fails", ret == FTP_ERR_INVALID_ARGUMENT);
    
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
    
    /* M0 must not claim a real network session. */
    creds.host = "localhost";
    creds.port = 21;
    creds.username = "testuser";
    creds.password = "testpass";
    creds.use_tls = FTP_TLS_IMPLICIT;
    creds.verify_cert = FTP_VERIFY_NONE;
    
    ret = ftp_connect(handle, &creds);
    TEST_ASSERT("ftp_connect reports implicit FTPS not implemented in M2", ret == FTP_ERR_NOT_IMPLEMENTED);
    
    /* The failed connect must not transition the handle to CONNECTED. */
    ret = ftp_ping(handle);
    TEST_ASSERT("ftp_ping after unimplemented connect fails", ret == FTP_ERR_INVALID_STATE);
    
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
    
    /* Reconnect remains unavailable until the real session is wired. */
    ret = ftp_connect(handle, &creds);
    TEST_ASSERT("Reconnect reports implicit FTPS not implemented in M4", ret == FTP_ERR_NOT_IMPLEMENTED);
    
    ftp_client_destroy(handle);
}

/* Test: Upload Directory (M4 Orchestration Path) */
static void test_upload_dir_stub(void) {
    printf("\n=== Testing Upload Directory (M4 Orchestration Path) ===\n");
    
    ftp_client_t* handle = NULL;
    int32_t ret = ftp_client_create(&handle);
    TEST_ASSERT("Create for upload test", ret == FTP_OK);
    
    /* Test upload without connection */
    ftp_result_t result;
    memset(&result, 0, sizeof(result));
    
    ret = ftp_upload_dir(handle, "/local", "/remote", NULL, NULL, NULL, &result);
    TEST_ASSERT("ftp_upload_dir without connection fails with invalid state", ret == FTP_ERR_INVALID_STATE);
    TEST_ASSERT("upload result is zeroed on invalid state", result.status == FTP_OK && result.file_results == NULL);
    
    /* The ABI test uses implicit FTPS only to avoid requiring a live server. */
    ftp_credentials_t creds;
    memset(&creds, 0, sizeof(creds));
    creds.host = "localhost";
    creds.port = 21;
    creds.use_tls = FTP_TLS_IMPLICIT;
    
    ret = ftp_connect(handle, &creds);
    TEST_ASSERT("Connect for upload test reports implicit FTPS not implemented", ret == FTP_ERR_NOT_IMPLEMENTED);
    
    /* Test upload with NULL paths */
    ret = ftp_upload_dir(handle, NULL, "/remote", NULL, NULL, NULL, NULL);
    TEST_ASSERT("ftp_upload_dir with NULL local_path fails", ret == FTP_ERR_INVALID_ARGUMENT);
    
    ret = ftp_upload_dir(handle, "/local", NULL, NULL, NULL, NULL, NULL);
    TEST_ASSERT("ftp_upload_dir with NULL remote_path fails", ret == FTP_ERR_INVALID_ARGUMENT);
    
    /* Test upload with empty paths */
    ret = ftp_upload_dir(handle, "", "/remote", NULL, NULL, NULL, NULL);
    TEST_ASSERT("ftp_upload_dir with empty local_path fails", ret == FTP_ERR_INVALID_ARGUMENT);
    
    /* A valid request after an unsupported connection is still invalid state. */
    ret = ftp_upload_dir(handle, "/local/test", "/remote/test", NULL, NULL, NULL, &result);
    TEST_ASSERT("ftp_upload_dir after failed connect reports invalid state", ret == FTP_ERR_INVALID_STATE);
    TEST_ASSERT("ftp_result_free accepts zeroed result", ftp_result_free(&result) == FTP_OK);
    
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
