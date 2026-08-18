/*
 * Integrity.hpp - M8 file integrity and resume fingerprint helpers
 */
#ifndef FTPCLIENT_INTEGRITY_HPP
#define FTPCLIENT_INTEGRITY_HPP

#include <openssl/evp.h>
#include <cstdint>
#include <fstream>
#include <string>

namespace ftpclient { namespace protocol { namespace integrity {

inline std::string lowercase_hex(const unsigned char* bytes, unsigned int length) {
    static const char* digits = "0123456789abcdef";
    std::string result;
    result.reserve(static_cast<size_t>(length) * 2);
    for (unsigned int i = 0; i < length; ++i) {
        result.push_back(digits[(bytes[i] >> 4) & 0x0f]);
        result.push_back(digits[bytes[i] & 0x0f]);
    }
    return result;
}

inline std::string normalize_sha256(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (char ch : value) {
        if (ch != ':' && ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            if (ch >= 'A' && ch <= 'F') {
                normalized.push_back(static_cast<char>(ch - 'A' + 'a'));
            } else {
                normalized.push_back(ch);
            }
        }
    }
    return normalized;
}

inline bool is_sha256_hex(const std::string& value) {
    if (value.size() != 64) {
        return false;
    }
    for (char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F'))) {
            return false;
        }
    }
    return true;
}

inline bool sha256_file(const std::string& path, std::string& out_hex) {
    out_hex.clear();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        return false;
    }
    bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
    char buffer[256 * 1024];
    while (ok && input.read(buffer, sizeof(buffer))) {
        ok = EVP_DigestUpdate(context, buffer,
                              static_cast<size_t>(input.gcount())) == 1;
    }
    if (ok && input.gcount() > 0) {
        ok = EVP_DigestUpdate(context, buffer,
                              static_cast<size_t>(input.gcount())) == 1;
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    if (ok) {
        ok = EVP_DigestFinal_ex(context, digest, &digest_length) == 1;
    }
    EVP_MD_CTX_free(context);
    if (!ok) {
        return false;
    }
    out_hex = lowercase_hex(digest, digest_length);
    return true;
}

inline bool sha256_bytes(const std::string& bytes, std::string& out_hex) {
    out_hex.clear();
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        if (context != nullptr) EVP_MD_CTX_free(context);
        return false;
    }
    bool ok = EVP_DigestUpdate(context, bytes.data(), bytes.size()) == 1;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    if (ok) ok = EVP_DigestFinal_ex(context, digest, &digest_length) == 1;
    EVP_MD_CTX_free(context);
    if (!ok) return false;
    out_hex = lowercase_hex(digest, digest_length);
    return true;
}

}}} // namespace ftpclient::protocol::integrity

#endif /* FTPCLIENT_INTEGRITY_HPP */
