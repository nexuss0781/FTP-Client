/*
 * phase2_test.cpp - Phase 2 Protocol Engine End-to-End Test
 * 
 * Tests all Phase 2 components:
 * - Transport layer (PlainTransport)
 * - Reply Parser
 * - State Machine
 * - Data Channel (PASV/EPSV parsing)
 * - Directory Walker
 */

#include <iostream>
#include <string>
#include <vector>
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>

// Include Phase 2 headers
#include "protocol/Transport.hpp"
#include "protocol/PlainTransport.hpp"
#include "protocol/ReplyParser.hpp"
#include "protocol/StateMachine.hpp"
#include "protocol/ErrorMap.hpp"
#include "protocol/DataChannel.hpp"
#include "protocol/DirectoryWalker.hpp"

using namespace ftpclient::protocol;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(name, condition) do { \
    if (condition) { \
        std::cout << "[PASS] " << name << std::endl; \
        tests_passed++; \
    } else { \
        std::cout << "[FAIL] " << name << std::endl; \
        tests_failed++; \
    } \
} while(0)

/* ============================================================================
 * Reply Parser Tests
 * ============================================================================
 */

void test_reply_parser_single_line() {
    std::cout << "\n=== Testing Reply Parser: Single Line ===" << std::endl;
    
    const char* response = "220 Welcome to FTP server\r\n";
    FtpReply reply;
    size_t consumed = 0;
    
    auto result = ReplyParser::parse(response, strlen(response), reply, consumed);
    
    TEST_ASSERT("Single line parse returns COMPLETE", result == ParseResult::COMPLETE);
    TEST_ASSERT("Reply code is 220", reply.code == 220);
    TEST_ASSERT("Not multiline", !reply.is_multiline);
    TEST_ASSERT("Message extracted", reply.message.find("Welcome") != std::string::npos);
    TEST_ASSERT("Correct bytes consumed", consumed == strlen(response));
}

void test_reply_parser_multiline() {
    std::cout << "\n=== Testing Reply Parser: Multiline ===" << std::endl;
    
    const char* response = 
        "214-Features:\r\n"
        "214-SITE\r\n"
        "214 SYST\r\n";
    
    FtpReply reply;
    size_t consumed = 0;
    
    auto result = ReplyParser::parse(response, strlen(response), reply, consumed);
    
    TEST_ASSERT("Multiline parse returns COMPLETE", result == ParseResult::COMPLETE);
    TEST_ASSERT("Reply code is 214", reply.code == 214);
    TEST_ASSERT("Final line detected (not multiline)", !reply.is_multiline);
}

void test_reply_parser_need_more_data() {
    std::cout << "\n=== Testing Reply Parser: Need More Data ===" << std::endl;
    
    const char* response = "220 Welcome";  // No CRLF
    
    FtpReply reply;
    size_t consumed = 0;
    
    auto result = ReplyParser::parse(response, strlen(response), reply, consumed);
    
    TEST_ASSERT("Incomplete parse returns NEED_MORE_DATA", result == ParseResult::NEED_MORE_DATA);
}

void test_reply_parser_malformed() {
    std::cout << "\n=== Testing Reply Parser: Malformed ===" << std::endl;
    
    const char* response = "ABC Invalid\r\n";
    
    FtpReply reply;
    size_t consumed = 0;
    
    auto result = ReplyParser::parse(response, strlen(response), reply, consumed);
    
    TEST_ASSERT("Malformed parse returns MALFORMED", result == ParseResult::MALFORMED);
}

/* ============================================================================
 * Error Map Tests
 * ============================================================================
 */

void test_error_map() {
    std::cout << "\n=== Testing Error Map ===" << std::endl;
    
    TEST_ASSERT("421 maps to NETWORK_RESET", map_ftp_code_to_error(421) == -403);
    TEST_ASSERT("425 maps to PASSIVE_FAILED", map_ftp_code_to_error(425) == -503);
    TEST_ASSERT("430 maps to AUTH_FAILED", map_ftp_code_to_error(430) == -301);
    TEST_ASSERT("500 maps to PROTOCOL", map_ftp_code_to_error(500) == -501);
    TEST_ASSERT("530 maps to AUTH_FAILED", map_ftp_code_to_error(530) == -301);
    TEST_ASSERT("550 maps to SERVER_DENIED", map_ftp_code_to_error(550) == -502);
    
    TEST_ASSERT("220 is success", is_ftp_success(220));
    TEST_ASSERT("230 is success", is_ftp_success(230));
    TEST_ASSERT("226 is success", is_ftp_success(226));
    TEST_ASSERT("331 is intermediate", is_ftp_intermediate(331));
    TEST_ASSERT("220 is not intermediate", !is_ftp_intermediate(220));
}

/* ============================================================================
 * State Machine Tests
 * ============================================================================
 */

void test_state_machine_initial() {
    std::cout << "\n=== Testing State Machine: Initial State ===" << std::endl;
    
    StateMachine sm;
    
    TEST_ASSERT("Initial state is DISCONNECTED", sm.get_state() == ProtocolState::DISCONNECTED);
    TEST_ASSERT("Not connected initially", !sm.is_connected());
    TEST_ASSERT("Not authenticated initially", !sm.is_authenticated());
}

void test_state_machine_connect_transition() {
    std::cout << "\n=== Testing State Machine: Connect Transition ===" << std::endl;
    
    StateMachine sm;
    
    auto result = sm.transition(FtpCommand::CONNECT);
    TEST_ASSERT("CONNECT from DISCONNECTED succeeds", result == TransitionResult::SUCCESS);
    TEST_ASSERT("State is TCP_CONNECTED", sm.get_state() == ProtocolState::TCP_CONNECTED);
}

void test_state_machine_auth_transition() {
    std::cout << "\n=== Testing State Machine: Auth Transition ===" << std::endl;
    
    StateMachine sm;
    sm.set_state(ProtocolState::TCP_CONNECTED);
    
    // Simulate greeting received, USER sent
    auto result = sm.transition(FtpCommand::USER, 331);
    TEST_ASSERT("USER with 331 transitions to AUTH_IN_PROGRESS", 
                result == TransitionResult::SUCCESS);
    TEST_ASSERT("State is AUTH_IN_PROGRESS", sm.get_state() == ProtocolState::AUTH_IN_PROGRESS);
    
    // Send PASS
    result = sm.transition(FtpCommand::PASS, 230);
    TEST_ASSERT("PASS with 230 transitions to AUTHENTICATED", 
                result == TransitionResult::SUCCESS);
    TEST_ASSERT("State is AUTHENTICATED", sm.get_state() == ProtocolState::AUTHENTICATED);
    TEST_ASSERT("Is authenticated", sm.is_authenticated());
}

void test_state_machine_invalid_transition() {
    std::cout << "\n=== Testing State Machine: Invalid Transition ===" << std::endl;
    
    StateMachine sm;
    
    // Try to send LIST without being authenticated
    auto result = sm.transition(FtpCommand::LIST);
    TEST_ASSERT("LIST from DISCONNECTED fails", result == TransitionResult::INVALID_STATE);
}

/* ============================================================================
 * Data Channel / PASV Parser Tests
 * ============================================================================
 */

void test_pasv_parser_standard() {
    std::cout << "\n=== Testing PASV Parser: Standard ===" << std::endl;
    
    std::string response = "227 Entering Passive Mode (192,168,1,1,200,1)";
    std::string control_host = "";  // Empty control host - no NAT detection
    
    auto result = DataChannel::parse_pasv(response, control_host);
    
    TEST_ASSERT("PASV IP parsed correctly", result.ip == "192.168.1.1");
    TEST_ASSERT("PASV port parsed correctly", result.port == 51201);  // 200*256 + 1
    TEST_ASSERT("Not IPv6", !result.is_ipv6);
    TEST_ASSERT("NAT not triggered with empty control host", !result.nat_detected);
}

void test_pasv_parser_nat_detection() {
    std::cout << "\n=== Testing PASV Parser: NAT Detection ===" << std::endl;
    
    // Server returns private IP but we're connected to public host
    std::string response = "227 Entering Passive Mode (192,168,1,100,100,100)";
    std::string control_host = "203.0.113.1";  // Public IP
    
    auto result = DataChannel::parse_pasv(response, control_host);
    
    TEST_ASSERT("NAT detected and IP substituted", result.nat_detected);
    TEST_ASSERT("IP replaced with control host", result.ip == "203.0.113.1");
    TEST_ASSERT("Port still correct", result.port == 25700);
}

void test_epsv_parser() {
    std::cout << "\n=== Testing EPSV Parser ===" << std::endl;
    
    std::string response = "229 Entering Extended Passive Mode (|||12345|)";
    
    auto result = DataChannel::parse_epsv(response);
    
    TEST_ASSERT("EPSV port parsed correctly", result.port == 12345);
    TEST_ASSERT("Is IPv6 capable", result.is_ipv6);
}

void test_private_ip_detection() {
    std::cout << "\n=== Testing Private IP Detection ===" << std::endl;
    
    TEST_ASSERT("10.x.x.x is private", DataChannel::is_private_ip("10.0.0.1"));
    TEST_ASSERT("172.16.x.x is private", DataChannel::is_private_ip("172.16.0.1"));
    TEST_ASSERT("172.31.x.x is private", DataChannel::is_private_ip("172.31.255.255"));
    TEST_ASSERT("192.168.x.x is private", DataChannel::is_private_ip("192.168.1.1"));
    TEST_ASSERT("127.x.x.x is private", DataChannel::is_private_ip("127.0.0.1"));
    TEST_ASSERT("8.8.8.8 is NOT private", !DataChannel::is_private_ip("8.8.8.8"));
    TEST_ASSERT("203.0.113.1 is NOT private", !DataChannel::is_private_ip("203.0.113.1"));
}

/* ============================================================================
 * Directory Walker Tests
 * ============================================================================
 */

void test_directory_walker_basic() {
    std::cout << "\n=== Testing Directory Walker: Basic ===" << std::endl;
    
    namespace fs = std::filesystem;
    
    // Create test directory structure
    std::string test_root_str = (fs::temp_directory_path() / "ftp_test_" / std::to_string(getpid())).string();
    fs::path test_root = test_root_str;
    fs::create_directories(test_root / "subdir1");
    fs::create_directories(test_root / "subdir2");
    
    // Create some files
    {
        std::ofstream f1(test_root / "file1.txt");
        f1 << "content1";
    }
    {
        std::ofstream f2(test_root / "subdir1" / "file2.txt");
        f2 << "content2";
    }
    
    TraversalConfig config;
    FileManifest manifest;
    
    auto result = DirectoryWalker::traverse(
        test_root.string(),
        "/remote",
        config,
        manifest
    );
    
    TEST_ASSERT("Traversal succeeds", result == WalkError::SUCCESS);
    TEST_ASSERT("Has entries", manifest.entries.size() > 0);
    TEST_ASSERT("Total files >= 2", manifest.total_files >= 2);
    TEST_ASSERT("Total dirs >= 2", manifest.total_dirs >= 2);
    TEST_ASSERT("Total bytes > 0", manifest.total_bytes > 0);
    
    // Cleanup
    fs::remove_all(test_root);
}

void test_directory_walker_prune() {
    std::cout << "\n=== Testing Directory Walker: Prune Patterns ===" << std::endl;
    
    namespace fs = std::filesystem;
    
    std::string test_root_str = (fs::temp_directory_path() / "ftp_test_prune_" / std::to_string(getpid())).string();
    fs::path test_root = test_root_str;
    fs::create_directories(test_root / ".git");
    fs::create_directories(test_root / "src");
    
    {
        std::ofstream f1(test_root / ".git" / "config");
        f1 << "git config";
    }
    {
        std::ofstream f2(test_root / "src" / "main.cpp");
        f2 << "int main() {}";
    }
    
    TraversalConfig config;
    config.prune_patterns.push_back(".git");
    
    FileManifest manifest;
    auto result = DirectoryWalker::traverse(
        test_root.string(),
        "/remote",
        config,
        manifest
    );
    
    TEST_ASSERT("Traversal with prune succeeds", result == WalkError::SUCCESS);
    
    // Check .git was pruned
    bool found_git = false;
    for (const auto& entry : manifest.entries) {
        if (entry.remote_relative_path.find(".git") != std::string::npos) {
            found_git = true;
            break;
        }
    }
    TEST_ASSERT(".git directory was pruned", !found_git);
    
    // Cleanup
    fs::remove_all(test_root);
}

void test_directory_walker_invalid_path() {
    std::cout << "\n=== Testing Directory Walker: Invalid Path ===" << std::endl;
    
    TraversalConfig config;
    FileManifest manifest;
    
    auto result = DirectoryWalker::traverse(
        "/nonexistent/path/xyz123",
        "/remote",
        config,
        manifest
    );
    
    TEST_ASSERT("Invalid path returns ERROR_INVALID_PATH", 
                result == WalkError::ERROR_INVALID_PATH);
}

/* ============================================================================
 * Main Test Runner
 * ============================================================================
 */

int main() {
    std::cout << "==============================================" << std::endl;
    std::cout << "FTP Client Library - Phase 2 Component Tests" << std::endl;
    std::cout << "==============================================" << std::endl;
    
    // Reply Parser Tests
    test_reply_parser_single_line();
    test_reply_parser_multiline();
    test_reply_parser_need_more_data();
    test_reply_parser_malformed();
    
    // Error Map Tests
    test_error_map();
    
    // State Machine Tests
    test_state_machine_initial();
    test_state_machine_connect_transition();
    test_state_machine_auth_transition();
    test_state_machine_invalid_transition();
    
    // Data Channel Tests
    test_pasv_parser_standard();
    test_pasv_parser_nat_detection();
    test_epsv_parser();
    test_private_ip_detection();
    
    // Directory Walker Tests
    test_directory_walker_basic();
    test_directory_walker_prune();
    test_directory_walker_invalid_path();
    
    std::cout << "\n==============================================" << std::endl;
    std::cout << "Test Summary: " << tests_passed << " passed, " 
              << tests_failed << " failed" << std::endl;
    std::cout << "==============================================" << std::endl;
    
    return tests_failed > 0 ? 1 : 0;
}
