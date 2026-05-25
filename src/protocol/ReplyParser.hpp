/*
 * ReplyParser.hpp - FTP Response Parser
 * 
 * Parses FTP server replies according to RFC 959 format.
 * Uses allocation-free parsing with std::string_view for performance.
 * No regex, no dynamic allocation per line.
 */

#ifndef FTPCLIENT_REPLY_PARSER_HPP
#define FTPCLIENT_REPLY_PARSER_HPP

#include <cstdint>
#include <cstring>
#include <string_view>

namespace ftpclient { namespace protocol {

/**
 * Parsed FTP reply structure
 */
struct FtpReply {
    uint16_t code;                      // 3-digit reply code
    bool is_multiline;                  // true if reply spans multiple lines
    std::string_view message;           // Message text (borrowed from buffer)
    
    FtpReply() : code(0), is_multiline(false) {}
};

/**
 * Parse result codes
 */
enum class ParseResult : int32_t {
    COMPLETE = 0,       // Full reply parsed successfully
    NEED_MORE_DATA = 1, // Need more data to complete parse
    MALFORMED = -1,     // Invalid reply format
    BUFFER_OVERFLOW = -2 // Buffer too small
};

/**
 * FTP Reply Parser
 * 
 * Stateless parser that operates on a receive buffer.
 * Maintains no internal state between calls.
 */
class ReplyParser {
public:
    /**
     * Maximum allowed reply size (64KB per spec Section 12 Rule 3)
     */
    static constexpr size_t MAX_REPLY_SIZE = 64 * 1024;
    
    /**
     * Parse FTP reply from buffer
     * 
     * @param buffer Input buffer containing reply data
     * @param length Length of data in buffer
     * @param[out] reply Output structure to receive parsed reply
     * @param[out] consumed Number of bytes consumed from buffer (including CRLF)
     * @return ParseResult indicating success or failure
     */
    static ParseResult parse(const char* buffer, size_t length, FtpReply& reply, size_t& consumed) {
        consumed = 0;
        
        if (buffer == nullptr || length == 0) {
            return ParseResult::MALFORMED;
        }
        
        // Check for buffer overflow
        if (length > MAX_REPLY_SIZE) {
            return ParseResult::BUFFER_OVERFLOW;
        }
        
        // Find first CRLF
        const char* crlf = find_crlf(buffer, length);
        if (crlf == nullptr) {
            return ParseResult::NEED_MORE_DATA;
        }
        
        size_t line_len = static_cast<size_t>(crlf - buffer);
        
        // Minimum valid line: "NNN" (3 digits)
        if (line_len < 3) {
            return ParseResult::MALFORMED;
        }
        
        // Parse reply code using manual digit parsing (faster than atoi)
        if (!is_digit(buffer[0]) || !is_digit(buffer[1]) || !is_digit(buffer[2])) {
            return ParseResult::MALFORMED;
        }
        
        uint16_t code = static_cast<uint16_t>(
            (buffer[0] - '0') * 100 +
            (buffer[1] - '0') * 10 +
            (buffer[2] - '0')
        );
        
        reply.code = code;
        reply.is_multiline = false;
        
        // Check for continuation marker
        if (line_len >= 4) {
            if (buffer[3] == '-') {
                // Multiline reply start
                reply.is_multiline = true;
                reply.message = std::string_view(buffer + 4, line_len - 4);
            } else if (buffer[3] == ' ') {
                // Single line reply or final line of multiline
                reply.message = std::string_view(buffer + 4, line_len - 4);
            } else {
                return ParseResult::MALFORMED;
            }
        } else {
            // Just code, no message
            reply.message = std::string_view();
        }
        
        consumed = line_len + 2; // Include CRLF
        
        // If this is a multiline reply, we need to find the final line
        if (reply.is_multiline) {
            return parse_multiline(buffer, length, reply, consumed);
        }
        
        return ParseResult::COMPLETE;
    }

private:
    /**
     * Check if character is a digit
     */
    static inline bool is_digit(char c) {
        return c >= '0' && c <= '9';
    }
    
    /**
     * Find CRLF in buffer
     */
    static const char* find_crlf(const char* buffer, size_t length) {
        if (length < 2) return nullptr;
        
        for (size_t i = 0; i < length - 1; ++i) {
            if (buffer[i] == '\r' && buffer[i + 1] == '\n') {
                return buffer + i;
            }
        }
        return nullptr;
    }
    
    /**
     * Parse multiline reply - find the final line
     */
    static ParseResult parse_multiline(const char* buffer, size_t length, FtpReply& reply, size_t& consumed) {
        // We need to find the final line which has format: NNN space ...
        // All intermediate lines have format: NNN dash ...
        
        size_t pos = 0;
        uint16_t expected_code = reply.code;
        
        while (pos < length) {
            const char* crlf = find_crlf(buffer + pos, length - pos);
            if (crlf == nullptr) {
                return ParseResult::NEED_MORE_DATA;
            }
            
            size_t line_len = static_cast<size_t>(crlf - (buffer + pos));
            
            // Must have at least 3 digits + separator
            if (line_len < 4) {
                return ParseResult::MALFORMED;
            }
            
            // Check if this is the final line
            if (is_digit(buffer[pos]) && is_digit(buffer[pos + 1]) && is_digit(buffer[pos + 2])) {
                uint16_t line_code = static_cast<uint16_t>(
                    (buffer[pos] - '0') * 100 +
                    (buffer[pos + 1] - '0') * 10 +
                    (buffer[pos + 2] - '0')
                );
                
                if (line_code == expected_code && buffer[pos + 3] == ' ') {
                    // This is the final line
                    reply.message = std::string_view(buffer + pos + 4, line_len - 4);
                    reply.is_multiline = false;  // Mark as complete
                    consumed = static_cast<size_t>(crlf - buffer) + 2;
                    return ParseResult::COMPLETE;
                }
            }
            
            // Move to next line
            pos = static_cast<size_t>(crlf - buffer) + 2;
        }
        
        return ParseResult::NEED_MORE_DATA;
    }
};

}} // namespace ftpclient::protocol

#endif /* FTPCLIENT_REPLY_PARSER_HPP */
