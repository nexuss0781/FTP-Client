#include "../include/ftpclient.h"

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef _WIN32
int main() {
    std::cout << "M4 orchestration integration test skipped on Windows in this baseline" << std::endl;
    return 0;
}
#else
namespace {

bool send_all(int fd, const std::string& value) {
    size_t sent = 0;
    while (sent < value.size()) {
        ssize_t count = ::send(fd, value.data() + sent, value.size() - sent, 0);
        if (count <= 0) return false;
        sent += static_cast<size_t>(count);
    }
    return true;
}

bool read_line(int fd, std::string& line) {
    line.clear();
    char ch = 0;
    while (line.size() < 4096) {
        ssize_t count = ::recv(fd, &ch, 1, 0);
        if (count <= 0) return false;
        line.push_back(ch);
        if (ch == '\n') return true;
    }
    return false;
}

int make_listener(uint16_t& port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(fd, 1) != 0) {
        ::close(fd);
        return -1;
    }
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::close(fd);
        return -1;
    }
    port = ntohs(address.sin_port);
    return fd;
}

class MockM4Server {
public:
    MockM4Server() : listen_fd_(-1), port_(0), worker_(), mutex_(), files_(), dirs_(),
                     rest_offset_(0), retry_failed_(false) {}

    bool start() {
        listen_fd_ = make_listener(port_);
        if (listen_fd_ < 0) return false;
        worker_ = std::thread(&MockM4Server::serve, this);
        return true;
    }

    ~MockM4Server() {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
        }
        if (worker_.joinable()) worker_.join();
    }

    uint16_t port() const { return port_; }

    std::string file(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = files_.find(path);
        return it == files_.end() ? std::string() : it->second;
    }

    bool has_dir(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dirs_.count(path) != 0;
    }

private:
    void serve_data(int passive_fd, const std::string& remote, int control_fd) {
        sockaddr_in peer{};
        socklen_t peer_length = sizeof(peer);
        int data_fd = ::accept(passive_fd, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        ::close(passive_fd);
        if (data_fd < 0) {
            send_all(control_fd, "426 Data accept failed\r\n");
            return;
        }

        std::string incoming;
        char buffer[8192];
        for (;;) {
            ssize_t count = ::recv(data_fd, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            incoming.append(buffer, static_cast<size_t>(count));
            if (remote == "deploy/retry.bin" && !retry_failed_) {
                break;
            }
        }
        ::shutdown(data_fd, SHUT_RDWR);
        ::close(data_fd);

        bool fail_once = remote == "deploy/retry.bin" && !retry_failed_;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (rest_offset_ == 0) {
                files_[remote].clear();
            }
            size_t start = static_cast<size_t>(std::min<uint64_t>(rest_offset_, files_[remote].size()));
            files_[remote].resize(start);
            if (fail_once) {
                if (incoming.size() > 3) incoming.resize(3);
                retry_failed_ = true;
            }
            files_[remote].append(incoming);
        }
        rest_offset_ = 0;
        send_all(control_fd, fail_once ? "426 Transfer aborted\r\n" : "226 Transfer complete\r\n");
    }

    void serve() {
        sockaddr_in peer{};
        socklen_t peer_length = sizeof(peer);
        int client = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (client < 0) return;
        send_all(client, "220 M4 orchestration server ready\r\n");

        std::string line;
        while (read_line(client, line)) {
            if (line == "USER m4-user\r\n") {
                send_all(client, "331 Password required\r\n");
            } else if (line == "PASS m4-password\r\n") {
                send_all(client, "230 Login successful\r\n");
            } else if (line.rfind("MKD ", 0) == 0) {
                std::string path = line.substr(4, line.size() - 6);
                std::lock_guard<std::mutex> lock(mutex_);
                if (!dirs_.insert(path).second) {
                    send_all(client, "550 Directory exists\r\n");
                } else {
                    send_all(client, "257 \"" + path + "\" created\r\n");
                }
            } else if (line.rfind("SIZE ", 0) == 0) {
                std::string path = line.substr(5, line.size() - 7);
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = files_.find(path);
                if (it == files_.end()) {
                    send_all(client, "550 File unavailable\r\n");
                } else {
                    send_all(client, "213 " + std::to_string(it->second.size()) + "\r\n");
                }
            } else if (line.rfind("REST ", 0) == 0) {
                rest_offset_ = std::strtoull(line.substr(5).c_str(), nullptr, 10);
                send_all(client, "350 Restart position accepted\r\n");
            } else if (line == "TYPE I\r\n") {
                send_all(client, "200 Binary mode\r\n");
            } else if (line == "EPSV\r\n") {
                uint16_t data_port = 0;
                int passive_fd = make_listener(data_port);
                if (passive_fd < 0) {
                    send_all(client, "425 Cannot open passive connection\r\n");
                    continue;
                }
                send_all(client, "229 Entering Extended Passive Mode (|||" +
                                   std::to_string(data_port) + "|)\r\n");
                if (!read_line(client, line)) {
                    ::close(passive_fd);
                    break;
                }
                if (line.rfind("STOR ", 0) != 0) {
                    ::close(passive_fd);
                    send_all(client, "550 Expected STOR\r\n");
                    continue;
                }
                std::string remote = line.substr(5, line.size() - 7);
                send_all(client, "150 Opening data connection\r\n");
                serve_data(passive_fd, remote, client);
            } else if (line == "QUIT\r\n") {
                send_all(client, "221 Goodbye\r\n");
                break;
            } else {
                send_all(client, "502 Command not implemented\r\n");
            }
        }
        ::shutdown(client, SHUT_RDWR);
        ::close(client);
    }

    int listen_fd_;
    uint16_t port_;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> files_;
    std::set<std::string> dirs_;
    uint64_t rest_offset_;
    bool retry_failed_;
};

struct ProgressState {
    uint64_t callbacks = 0;
    uint64_t last_bytes = 0;
};

void progress_callback(const char*, const char*, uint64_t current, uint64_t,
                       double, void* user_data) {
    auto* state = static_cast<ProgressState*>(user_data);
    if (state != nullptr) {
        ++state->callbacks;
        state->last_bytes = current;
    }
}

bool check(bool condition, const char* label) {
    if (!condition) std::cerr << "[FAIL] " << label << std::endl;
    else std::cout << "[PASS] " << label << std::endl;
    return condition;
}

ftp_credentials_t credentials(uint16_t port) {
    ftp_credentials_t creds{};
    creds.host = "localhost";
    creds.port = port;
    creds.username = "m4-user";
    creds.password = "m4-password";
    creds.use_tls = FTP_TLS_NONE;
    creds.verify_cert = FTP_VERIFY_NONE;
    return creds;
}

bool create_file(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << contents;
    return static_cast<bool>(file);
}

bool test_nested_multi_file_upload() {
    MockM4Server server;
    if (!check(server.start(), "M4 multi-file server starts")) return false;
    std::filesystem::path root = "/tmp/ftpclient_m4_tree";
    std::filesystem::remove_all(root);
    bool ok = create_file(root / "a.txt", "alpha") &&
              create_file(root / "nested" / "b.txt", "bravo-data");
    ok &= check(ok, "M4 test files created");

    ftp_client_t* client = nullptr;
    ok &= check(ftp_client_create(&client) == FTP_OK, "M4 multi-file client creates");
    auto creds = credentials(server.port());
    ok &= check(ftp_connect(client, &creds) == FTP_OK, "M4 multi-file control connects");

    ftp_upload_options_t options{};
    options.struct_size = sizeof(options);
    options.retry_attempts = 0;
    options.max_parallel = 1;
    options.create_remote_dirs = 1;
    ftp_result_t result{};
    int32_t ret = ftp_upload_dir(client, root.c_str(), "deploy", &options,
                                 progress_callback, nullptr, &result);
    ok &= check(ret == FTP_OK, "M4 nested multi-file upload succeeds");
    ok &= check(result.files_total == 2 && result.files_success == 2 &&
                result.files_failed == 0 && result.bytes_transferred == 15,
                "M4 aggregate counts and bytes are accurate");
    ok &= check(server.has_dir("deploy/nested"), "M4 nested remote directory created");
    ok &= check(server.file("deploy/a.txt") == "alpha" &&
                server.file("deploy/nested/b.txt") == "bravo-data",
                "M4 server received every file exactly");
    ok &= check(ftp_result_free(&result) == FTP_OK, "M4 multi-file result frees");
    ok &= check(ftp_disconnect(client) == FTP_OK, "M4 multi-file disconnects");
    ok &= check(ftp_client_destroy(client) == FTP_OK, "M4 multi-file client destroys");
    std::filesystem::remove_all(root);
    return ok;
}

bool test_retry_resume_and_progress() {
    MockM4Server server;
    if (!check(server.start(), "M4 retry server starts")) return false;
    std::filesystem::path root = "/tmp/ftpclient_m4_retry";
    std::filesystem::remove_all(root);
    const std::string payload(300000, 'R');
    bool ok = create_file(root / "retry.bin", payload);
    ok &= check(ok, "M4 retry file created");

    ftp_client_t* client = nullptr;
    ok &= check(ftp_client_create(&client) == FTP_OK, "M4 retry client creates");
    auto creds = credentials(server.port());
    ok &= check(ftp_connect(client, &creds) == FTP_OK, "M4 retry control connects");

    ProgressState progress;
    ftp_upload_options_t options{};
    options.struct_size = sizeof(options);
    options.retry_attempts = 1;
    options.retry_base_delay_ms = 0;
    options.resume_enabled = 1;
    options.create_remote_dirs = 1;
    ftp_result_t result{};
    int32_t ret = ftp_upload_dir(client, root.c_str(), "deploy", &options,
                                 progress_callback, &progress, &result);
    ok &= check(ret == FTP_OK, "M4 retry/resume upload succeeds");
    ok &= check(result.files_total == 1 && result.files_success == 1 &&
                result.file_results != nullptr &&
                result.file_results[0].attempt_count == 2 &&
                result.file_results[0].bytes_sent == payload.size(),
                "M4 retry attempt and byte accounting are accurate");
    ok &= check(server.file("deploy/retry.bin") == payload,
                "M4 REST resume reconstructs the complete file");
    ok &= check(progress.callbacks > 0 && progress.last_bytes == payload.size(),
                "M4 progress callback reports final bytes");
    ok &= check(ftp_result_free(&result) == FTP_OK, "M4 retry result frees");
    ok &= check(ftp_disconnect(client) == FTP_OK, "M4 retry disconnects");
    ok &= check(ftp_client_destroy(client) == FTP_OK, "M4 retry client destroys");
    std::filesystem::remove_all(root);
    return ok;
}

} // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    bool ok = true;
    ok &= test_nested_multi_file_upload();
    ok &= test_retry_resume_and_progress();
    return ok ? 0 : 1;
}
#endif
