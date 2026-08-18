/*
 * RemoteListing.hpp - M9 MLSD fact parser
 */
#ifndef FTPCLIENT_REMOTE_LISTING_HPP
#define FTPCLIENT_REMOTE_LISTING_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace ftpclient { namespace protocol {

struct RemoteListingEntry {
    std::string name;
    std::string type;
    uint64_t size_bytes = 0;
    bool has_size = false;
    std::string modify_fact;
};

inline bool is_safe_mlsd_name(const std::string& name) {
    if (name.empty() || name == "." || name == ".." || name.front() == '/' ||
        name.front() == '\\' || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos || name.find(':') != std::string::npos) {
        return false;
    }
    return true;
}

inline bool parse_mlsd_entry(const std::string& line, RemoteListingEntry& out) {
    out = RemoteListingEntry();
    const size_t separator = line.find(' ');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= line.size()) {
        return false;
    }

    const std::string facts = line.substr(0, separator);
    out.name = line.substr(separator + 1);
    if (out.name.empty()) {
        return false;
    }

    size_t begin = 0;
    while (begin <= facts.size()) {
        const size_t end = facts.find(';', begin);
        const std::string fact = facts.substr(begin,
            end == std::string::npos ? std::string::npos : end - begin);
        if (!fact.empty()) {
            const size_t equals = fact.find('=');
            if (equals == std::string::npos || equals == 0) {
                return false;
            }
            const std::string key = fact.substr(0, equals);
            const std::string value = fact.substr(equals + 1);
            if (key == "type") {
                out.type = value;
            } else if (key == "size") {
                if (value.empty()) return false;
                uint64_t parsed = 0;
                for (char ch : value) {
                    if (ch < '0' || ch > '9') return false;
                    const uint64_t digit = static_cast<uint64_t>(ch - '0');
                    if (parsed > (UINT64_MAX - digit) / 10) return false;
                    parsed = parsed * 10 + digit;
                }
                out.size_bytes = parsed;
                out.has_size = true;
            } else if (key == "modify") {
                out.modify_fact = value;
            }
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }

    if (out.type != "file" && out.type != "dir" &&
        out.type != "cdir" && out.type != "pdir") {
        return false;
    }
    if (out.type == "file" || out.type == "dir") {
        if (!is_safe_mlsd_name(out.name)) return false;
    } else if (out.name != "." && out.name != "..") {
        return false;
    }
    return true;
}

inline bool parse_mlsd_listing(const std::string& listing,
                               std::vector<RemoteListingEntry>& entries) {
    entries.clear();
    size_t begin = 0;
    while (begin < listing.size()) {
        const size_t end = listing.find('\n', begin);
        std::string line = listing.substr(begin,
            end == std::string::npos ? std::string::npos : end - begin);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            RemoteListingEntry entry;
            if (!parse_mlsd_entry(line, entry)) return false;
            if (entry.type != "cdir" && entry.type != "pdir") {
                entries.push_back(std::move(entry));
            }
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}

}} // namespace ftpclient::protocol

#endif /* FTPCLIENT_REMOTE_LISTING_HPP */
