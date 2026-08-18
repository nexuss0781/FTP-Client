/*
 * Verification.hpp - M12 per-file verification provenance
 */
#ifndef FTPCLIENT_TRANSFER_VERIFICATION_HPP
#define FTPCLIENT_TRANSFER_VERIFICATION_HPP

#include <cstdint>
#include <string>

namespace ftpclient {

struct VerificationMetadata {
    uint32_t status = 0;
    uint32_t sources = 0;
    std::string algorithm;
    std::string local_digest;
    std::string remote_digest;
};

} // namespace ftpclient

#endif /* FTPCLIENT_TRANSFER_VERIFICATION_HPP */
