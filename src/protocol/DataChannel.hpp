/*
 * DataChannel.hpp - FTP Data Channel Management
 * 
 * Handles PASV/EPSV negotiation, NAT traversal intelligence, and data socket lifecycle.
 * Per Phase 2 spec Section 7.
 */

#ifndef FTPCLIENT_DATA_CHANNEL_HPP
#define FTPCLIENT_DATA_CHANNEL_HPP

#include "Transport.hpp"
#include <memory>
#include <string>
#include <cstdint>
#include <cstdio>

namespace ftpclient { namespace protocol {

/**
 * Parsed passive mode result
 */
struct PassiveModeResult {
    std::string ip;         // IP address to connect to
    uint16_t port;          // Port number
    bool is_ipv6;           // true if EPSV (IPv6 capable)
    bool nat_detected;      // true if private IP detected and substituted
    
    PassiveModeResult() : port(0), is_ipv6(false), nat_detected(false) {}
};

/**
 * Data Channel Manager
 * 
 * Manages passive mode negotiation and data socket creation.
 * Implements NAT traversal intelligence per spec Section 7.2.
 */
class DataChannel {
public:
    /**
     * Parse PASV response (IPv4)
     * 
     * Input format: "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)"
     * 
     * @param response Full server response string
     * @param control_host_ip IP of control connection (for NAT detection)
     * @return PassiveModeResult with parsed IP/port, or error state
     */
    static PassiveModeResult parse_pasv(const std::string& response, const std::string& control_host_ip);
    
    /**
     * Parse EPSV response (IPv6/IPv4)
     * 
     * Input format: "229 Entering Extended Passive Mode (|||port|)"
     * 
     * @param response Full server response string
     * @return PassiveModeResult with parsed port (IP = control host)
     */
    static PassiveModeResult parse_epsv(const std::string& response);
    
    /**
     * Create data channel transport
     * 
     * @param result Passive mode result from parse_pasv/parse_epsv
     * @return New Transport instance for data connection, or nullptr on failure
     */
    static std::unique_ptr<Transport> create_data_transport(const PassiveModeResult& result);
    
    /**
     * Check if an IP address is in a private range (RFC 1918)
     * 
     * @param ip IPv4 address string
     * @return true if private, false if public
     */
    static bool is_private_ip(const std::string& ip);

private:
    /**
     * Helper to check if character is a digit
     */
    static inline bool is_digit(char c) {
        return c >= '0' && c <= '9';
    }
    
    /**
     * Helper to parse integer from string
     */
    static int32_t parse_int(const char*& ptr);
};

inline PassiveModeResult DataChannel::parse_pasv(const std::string& response, const std::string& control_host_ip) {
    PassiveModeResult result;
    result.is_ipv6 = false;
    result.nat_detected = false;
    
    // Find opening parenthesis
    size_t start = response.find('(');
    size_t end = response.find(')');
    
    if (start == std::string::npos || end == std::string::npos || end <= start) {
        return result;  // Invalid format
    }
    
    // Extract content between parentheses
    std::string content = response.substr(start + 1, end - start - 1);
    
    // Parse h1,h2,h3,h4,p1,p2
    const char* ptr = content.c_str();
    
    int32_t h1 = parse_int(ptr);
    if (*ptr != ',') return result;
    ++ptr;
    
    int32_t h2 = parse_int(ptr);
    if (*ptr != ',') return result;
    ++ptr;
    
    int32_t h3 = parse_int(ptr);
    if (*ptr != ',') return result;
    ++ptr;
    
    int32_t h4 = parse_int(ptr);
    if (*ptr != ',') return result;
    ++ptr;
    
    int32_t p1 = parse_int(ptr);
    if (*ptr != ',') return result;
    ++ptr;
    
    int32_t p2 = parse_int(ptr);
    
    // Validate ranges
    if (h1 < 0 || h1 > 255 || h2 < 0 || h2 > 255 ||
        h3 < 0 || h3 > 255 || h4 < 0 || h4 > 255 ||
        p1 < 0 || p1 > 255 || p2 < 0 || p2 > 255) {
        return result;  // Invalid values
    }
    
    // Construct IP and port
    char ip_str[16];
    std::snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", h1, h2, h3, h4);
    result.ip = ip_str;
    result.port = static_cast<uint16_t>(p1 * 256 + p2);
    
    // NAT Traversal Intelligence (spec Section 7.2)
    // Only apply NAT detection if control_host_ip is a valid IPv4 address
    // If server returned private IP but we're connected to public host, use control host IP
    if (!control_host_ip.empty() && is_private_ip(result.ip) && !is_private_ip(control_host_ip)) {
        result.ip = control_host_ip;
        result.nat_detected = true;
    }
    
    return result;
}

inline PassiveModeResult DataChannel::parse_epsv(const std::string& response) {
    PassiveModeResult result;
    result.is_ipv6 = true;
    result.nat_detected = false;
    
    // Find opening and closing delimiters
    // Format: (|||port|) or similar
    size_t start = response.find('(');
    size_t end = response.find(')');
    
    if (start == std::string::npos || end == std::string::npos || end <= start) {
        return result;  // Invalid format
    }
    
    std::string content = response.substr(start + 1, end - start - 1);
    
    // Find the port number between delimiters (typically |)
    // Look for pattern like "|||12345|"
    size_t last_delim = content.rfind('|');
    if (last_delim == std::string::npos || last_delim == 0) {
        return result;
    }
    
    // Find second-to-last delimiter
    size_t prev_delim = content.rfind('|', last_delim - 1);
    if (prev_delim == std::string::npos) {
        return result;
    }
    
    // Extract port string
    std::string port_str = content.substr(prev_delim + 1, last_delim - prev_delim - 1);
    
    // Parse port
    const char* ptr = port_str.c_str();
    int32_t port = parse_int(ptr);
    
    if (port < 0 || port > 65535) {
        return result;  // Invalid port
    }
    
    result.port = static_cast<uint16_t>(port);
    // IP will be same as control connection (EPSV doesn't return IP)
    result.ip.clear();
    
    return result;
}

inline std::unique_ptr<Transport> DataChannel::create_data_transport(const PassiveModeResult& result) {
    if (result.port == 0) {
        return nullptr;
    }
    
    // For EPSV, IP is empty - use control host IP (caller must set it)
    if (result.ip.empty()) {
        return nullptr;  // Caller needs to provide control host IP
    }
    
    // Note: PlainTransport is defined in PlainTransport.hpp which is not included here
    // to avoid circular dependencies. This function is intended to be used by code
    // that includes both DataChannel.hpp and PlainTransport.hpp.
    // For now, we return nullptr and let the caller create the transport.
    // Phase 4 will implement this properly with factory injection.
    return nullptr;
}

inline bool DataChannel::is_private_ip(const std::string& ip) {
    // Parse IPv4 address
    const char* ptr = ip.c_str();
    int32_t octets[4] = {0, 0, 0, 0};
    
    for (int i = 0; i < 4; ++i) {
        // Check if we have a digit
        if (!is_digit(*ptr)) {
            return false;  // Invalid format - treat as NOT private (public or hostname)
        }
        octets[i] = parse_int(ptr);
        if (i < 3 && *ptr != '.') {
            return false;  // Invalid format
        }
        if (i < 3) ++ptr;
    }
    
    // RFC 1918 private ranges:
    // 10.0.0.0/8
    if (octets[0] == 10) return true;
    
    // 172.16.0.0/12
    if (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) return true;
    
    // 192.168.0.0/16
    if (octets[0] == 192 && octets[1] == 168) return true;
    
    // 127.0.0.0/8 (loopback - treat as private)
    if (octets[0] == 127) return true;
    
    return false;
}

inline int32_t DataChannel::parse_int(const char*& ptr) {
    int32_t result = 0;
    while (*ptr >= '0' && *ptr <= '9') {
        result = result * 10 + (*ptr - '0');
        ++ptr;
    }
    return result;
}

}} // namespace ftpclient::protocol

#endif /* FTPCLIENT_DATA_CHANNEL_HPP */
