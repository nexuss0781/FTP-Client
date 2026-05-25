/*
 * ErrorMap.hpp - FTP Reply Code to Phase 1 Error Code Mapping
 * 
 * Centralized mapping of FTP server reply codes (RFC 959) to Phase 1 error taxonomy.
 * No ad-hoc if statements scattered through the codebase.
 */

#ifndef FTPCLIENT_ERROR_MAP_HPP
#define FTPCLIENT_ERROR_MAP_HPP

#include <cstdint>

namespace ftpclient { namespace protocol {

/**
 * Map FTP server reply code to Phase 1 error code
 * 
 * @param ftp_code 3-digit FTP reply code
 * @return Corresponding Phase 1 error code
 */
inline int32_t map_ftp_code_to_error(uint16_t ftp_code) {
    switch (ftp_code) {
        // 4xx - Transient Negative Completion
        case 421: return -403;  // FTP_ERR_NETWORK_RESET - Service not available, closing
        case 425: return -503;  // FTP_ERR_PASSIVE_FAILED - Can't open data connection
        case 426: return -403;  // FTP_ERR_NETWORK_RESET - Connection closed; transfer aborted
        case 430: return -301;  // FTP_ERR_AUTH_FAILED - Invalid username or password
        case 431: return -102;  // FTP_ERR_SYSTEM - Need some unavailable resource
        case 450: return -602;  // FTP_ERR_REMOTE_IO - File unavailable (busy)
        case 451: return -602;  // FTP_ERR_REMOTE_IO - Local error in processing
        case 452: return -602;  // FTP_ERR_REMOTE_IO - Insufficient storage
        
        // 5xx - Permanent Negative Completion
        case 500: return -501;  // FTP_ERR_PROTOCOL - Syntax error / command unrecognized
        case 501: return -501;  // FTP_ERR_PROTOCOL - Syntax error in parameters
        case 502: return -501;  // FTP_ERR_PROTOCOL - Command not implemented
        case 530: return -301;  // FTP_ERR_AUTH_FAILED - Not logged in
        case 550: return -502;  // FTP_ERR_SERVER_DENIED - File unavailable / not found / no access
        case 552: return -602;  // FTP_ERR_REMOTE_IO - Exceeded storage allocation
        
        default:
            // Unknown codes - treat as protocol error
            return -501;  // FTP_ERR_PROTOCOL
    }
}

/**
 * Check if FTP reply code indicates success
 * 
 * @param ftp_code 3-digit FTP reply code
 * @return true if success (2xx), false otherwise
 */
inline bool is_ftp_success(uint16_t ftp_code) {
    return ftp_code >= 200 && ftp_code < 300;
}

/**
 * Check if FTP reply code indicates intermediate state (need more commands)
 * 
 * @param ftp_code 3-digit FTP reply code
 * @return true if intermediate (3xx), false otherwise
 */
inline bool is_ftp_intermediate(uint16_t ftp_code) {
    return ftp_code >= 300 && ftp_code < 400;
}

}} // namespace ftpclient::protocol

#endif /* FTPCLIENT_ERROR_MAP_HPP */
