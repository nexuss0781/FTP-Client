/*
 * Verification.hpp - M12 per-file verification provenance
 */
#ifndef FTPCLIENT_TRANSFER_VERIFICATION_HPP
#define FTPCLIENT_TRANSFER_VERIFICATION_HPP

#include <cstdint>
#include <string>

namespace ftpclient {

constexpr uint32_t kVerificationStatusNone = 0;
constexpr uint32_t kVerificationStatusPassed = 1;
constexpr uint32_t kVerificationStatusFailed = 2;
constexpr uint32_t kVerificationStatusUnavailable = 3;
constexpr uint32_t kVerificationSourceLocal = 0x0001;
constexpr uint32_t kVerificationSourceRemote = 0x0002;
constexpr uint32_t kVerificationPolicyNone = 0;
constexpr uint32_t kVerificationPolicyLocalExpected = 1;
constexpr uint32_t kVerificationPolicyRemoteOptional = 2;
constexpr uint32_t kVerificationPolicyRemoteRequired = 3;
constexpr uint32_t kVerificationPolicyLocalAndRemote = 4;

struct VerificationMetadata {
    uint32_t status = 0;
    uint32_t sources = 0;
    std::string algorithm;
    std::string local_digest;
    std::string remote_digest;
};

} // namespace ftpclient

#endif /* FTPCLIENT_TRANSFER_VERIFICATION_HPP */
