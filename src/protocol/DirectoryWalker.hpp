/*
 * DirectoryWalker.hpp - Local Directory Traversal Engine
 * 
 * Recursively traverses local directory trees using std::filesystem.
 * Produces a structured manifest for upload operations.
 * Per Phase 2 spec Section 8.
 */

#ifndef FTPCLIENT_DIRECTORY_WALKER_HPP
#define FTPCLIENT_DIRECTORY_WALKER_HPP

#include <string>
#include <vector>
#include <cstdint>
#include <filesystem>
#include <algorithm>

namespace ftpclient { namespace protocol {

/**
 * Configuration for directory traversal
 * Per spec Section 8.1
 */
struct TraversalConfig {
    uint32_t max_depth;           // 0 = unlimited
    int32_t  symlink_policy;      // 0=ERROR, 1=FOLLOW, 2=SKIP
    uint64_t max_file_size;       // 0 = unlimited
    std::vector<std::string> prune_patterns;  // Patterns to skip (e.g., ".git")
    
    TraversalConfig()
        : max_depth(0)
        , symlink_policy(1)  // FOLLOW by default
        , max_file_size(0)
    {}
};

/**
 * Single entry in the file manifest
 * Per spec Section 8.2
 */
struct FileManifestEntry {
    std::string local_absolute_path;   // Native path, UTF-8
    std::string remote_relative_path;  // Forward-slash separated, relative to upload root
    uint64_t    size_bytes;
    bool        is_directory;          // true = remote mkdir needed, no data transfer
    uint64_t    mtime_sec;             // Unix epoch
    
    FileManifestEntry()
        : size_bytes(0)
        , is_directory(false)
        , mtime_sec(0)
    {}
};

/**
 * Complete file manifest for a directory tree
 * Per spec Section 8.2
 */
struct FileManifest {
    std::vector<FileManifestEntry> entries;
    uint64_t total_files;
    uint64_t total_dirs;
    uint64_t total_bytes;
    
    FileManifest()
        : total_files(0)
        , total_dirs(0)
        , total_bytes(0)
    {}
};

/**
 * Error codes for directory traversal
 */
enum class WalkError : int32_t {
    SUCCESS = 0,
    ERROR_PERMISSION_DENIED = -601,  // FTP_ERR_LOCAL_IO
    ERROR_NOT_DIRECTORY = -202,      // FTP_ERR_INVALID_ARGUMENT
    ERROR_FILESYSTEM_LOOP = -601,    // FTP_ERR_LOCAL_IO
    ERROR_INVALID_PATH = -202,       // FTP_ERR_INVALID_ARGUMENT
    ERROR_SYMLINK_FORBIDDEN = -502   // FTP_ERR_SERVER_DENIED
};

/**
 * Directory Walker
 * 
 * Recursively traverses local directories and produces upload manifests.
 * Implements depth-first pre-order traversal per spec Section 8.4.
 */
class DirectoryWalker {
public:
    /**
     * Traverse a local directory tree
     * 
     * @param local_root Root directory to traverse
     * @param remote_root Remote base path (relative paths will be appended)
     * @param config Traversal configuration
     * @param[out] manifest Output manifest
     * @return WalkError indicating success or failure
     */
    static WalkError traverse(
        const std::string& local_root,
        const std::string& remote_root,
        const TraversalConfig& config,
        FileManifest& manifest
    ) {
        namespace fs = std::filesystem;
        
        manifest = FileManifest();
        
        // Validate local root exists
        std::error_code ec;
        if (!fs::exists(local_root, ec) || ec) {
            return WalkError::ERROR_INVALID_PATH;
        }
        
        if (!fs::is_directory(local_root, ec) || ec) {
            return WalkError::ERROR_NOT_DIRECTORY;
        }
        
        // Get absolute path of local root
        fs::path local_root_abs = fs::absolute(local_root, ec);
        if (ec) {
            return WalkError::ERROR_INVALID_PATH;
        }
        
        // Use iterative stack-based traversal for depth-first pre-order
        struct StackEntry {
            fs::path local_path;
            std::string remote_path;
            uint32_t depth;
        };
        
        std::vector<StackEntry> stack;
        stack.push_back({local_root_abs, remote_root, 0});
        
        while (!stack.empty()) {
            StackEntry current = std::move(stack.back());
            stack.pop_back();
            
            // Check depth limit
            if (config.max_depth > 0 && current.depth > config.max_depth) {
                continue;
            }
            
            // Iterate directory contents
            std::error_code iter_ec;
            fs::directory_iterator dir_iter(current.local_path, iter_ec);
            if (iter_ec) {
                if (iter_ec == std::errc::permission_denied) {
                    // Skip inaccessible directories, continue with others
                    continue;
                }
                return WalkError::ERROR_PERMISSION_DENIED;
            }
            
            // Collect entries first for consistent ordering
            std::vector<fs::directory_entry> entries;
            for (const auto& entry : dir_iter) {
                entries.push_back(entry);
            }
            
            // Sort for deterministic order (directories first, then alphabetical)
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                bool a_is_dir = a.is_directory();
                bool b_is_dir = b.is_directory();
                if (a_is_dir != b_is_dir) {
                    return a_is_dir;  // Directories first
                }
                return a.path().filename() < b.path().filename();
            });
            
            // Process entries in reverse order (so they come out in correct order from stack)
            for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
                const auto& entry = *it;
                
                // Check prune patterns
                std::string filename = entry.path().filename().string();
                bool pruned = false;
                for (const auto& pattern : config.prune_patterns) {
                    if (filename == pattern || filename.find(pattern) != std::string::npos) {
                        pruned = true;
                        break;
                    }
                }
                if (pruned) {
                    continue;
                }
                
                // Build remote path
                std::string remote_path = current.remote_path + "/" + filename;
                
                // Handle symlinks
                std::error_code symlink_ec;
                bool is_symlink = entry.is_symlink(symlink_ec);
                
                if (is_symlink) {
                    if (config.symlink_policy == 2) {  // SKIP
                        continue;
                    } else if (config.symlink_policy == 0) {  // ERROR
                        return WalkError::ERROR_SYMLINK_FORBIDDEN;
                    }
                    // FOLLOW (policy == 1): resolve symlink and continue
                }
                
                // Check if directory
                bool is_dir = entry.is_directory(iter_ec);
                if (iter_ec) {
                    continue;  // Skip entries we can't stat
                }
                
                if (is_dir) {
                    // Add directory entry
                    FileManifestEntry dir_entry;
                    dir_entry.local_absolute_path = entry.path().string();
                    dir_entry.remote_relative_path = remote_path;
                    dir_entry.size_bytes = 0;
                    dir_entry.is_directory = true;
                    dir_entry.mtime_sec = 0;
                    
                    manifest.entries.push_back(std::move(dir_entry));
                    manifest.total_dirs++;
                    
                    // Push to stack for recursive traversal
                    stack.push_back({entry.path(), remote_path, current.depth + 1});
                } else {
                    // Regular file
                    uintmax_t file_size = entry.file_size(iter_ec);
                    if (iter_ec) {
                        continue;  // Skip files we can't stat
                    }
                    
                    // Check max file size
                    if (config.max_file_size > 0 && file_size > config.max_file_size) {
                        continue;
                    }
                    
                    // Get modification time
                    auto mtime = entry.last_write_time(iter_ec);
                    uint64_t mtime_sec = 0;
                    if (!iter_ec) {
                        auto epoch = mtime.time_since_epoch().count();
                        mtime_sec = static_cast<uint64_t>(epoch / 10000000ULL);  // Convert to seconds
                    }
                    
                    FileManifestEntry file_entry;
                    file_entry.local_absolute_path = entry.path().string();
                    file_entry.remote_relative_path = remote_path;
                    file_entry.size_bytes = static_cast<uint64_t>(file_size);
                    file_entry.is_directory = false;
                    file_entry.mtime_sec = mtime_sec;
                    
                    manifest.entries.push_back(std::move(file_entry));
                    manifest.total_files++;
                    manifest.total_bytes += file_size;
                }
            }
        }
        
        return WalkError::SUCCESS;
    }
};

}} // namespace ftpclient::protocol

#endif /* FTPCLIENT_DIRECTORY_WALKER_HPP */
