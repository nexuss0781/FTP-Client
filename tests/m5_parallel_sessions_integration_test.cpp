#include "../include/ftpclient.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

namespace {

bool send_all(int fd, const std::string& text) {
    size_t offset = 0;
    while (offset < text.size()) {
        ssize_t written = ::send(fd, text.data() + offset, text.size() - offset, 0);
        if (written <= 0) return false;
        offset += static_cast<size_t>(written);
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
        ::listen(fd, 8) != 0) {
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

class ParallelServer {
public:
    explicit ParallelServer(int expected_connections, std::string rejected_path = {})
        : expected_connections_(expected_connections), listen_fd_(-1), port_(0),
          accept_thread_(), client_threads_(), mutex_(), files_(),
          rejected_path_(std::move(rejected_path)), connections_(0),
          active_transfers_(0), max_active_transfers_(0) {}

    bool start() {
        listen_fd_ = make_listener(port_);
        if (listen_fd_ < 0) return false;
        accept_thread_ = std::thread(&ParallelServer::accept_loop, this);
        return true;
    }

    ~ParallelServer() {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        for (auto& thread : client_threads_) {
            if (thread.joinable()) thread.join();
        }
    }

    uint16_t port() const { return port_; }
    int connections() const { return connections_.load(); }
    int max_active_transfers() const { return max_active_transfers_.load(); }

    void set_rejected_path(std::string path) {
        std::lock_guard<std::mutex> lock(mutex_);
        rejected_path_ = std::move(path);
    }

    std::string file(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = files_.find(path);
        return it == files_.end() ? std::string() : it->second;
    }

private:
    void accept_loop() {
        while (connections_.load() < expected_connections_) {
            sockaddr_in peer{};
            socklen_t length = sizeof(peer);
            int client = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &length);
            if (client < 0) break;
            connections_.fetch_add(1);
            client_threads_.emplace_back(&ParallelServer::serve_client, this, client);
        }
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
    }

    void serve_data(int passive_fd, const std::string& remote, int control_fd) {
        sockaddr_in peer{};
        socklen_t length = sizeof(peer);
        int data_fd = ::accept(passive_fd, reinterpret_cast<sockaddr*>(&peer), &length);
        ::close(passive_fd);
        if (data_fd < 0) {
            send_all(control_fd, "426 Data accept failed\r\n");
            return;
        }

        int active = active_transfers_.fetch_add(1) + 1;
        int observed = max_active_transfers_.load();
        while (active > observed &&
               !max_active_transfers_.compare_exchange_weak(observed, active)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        std::string content;
        char buffer[8192];
        for (;;) {
            ssize_t count = ::recv(data_fd, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            content.append(buffer, static_cast<size_t>(count));
        }
        ::shutdown(data_fd, SHUT_RDWR);
        ::close(data_fd);
        active_transfers_.fetch_sub(1);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            files_[remote] = std::move(content);
        }
        send_all(control_fd, "226 Transfer complete\r\n");
    }

    void serve_client(int client) {
        send_all(client, "220 M5 parallel server ready\r\n");
        std::string line;
        while (read_line(client, line)) {
            if (line == "USER m5-user\r\n") {
                send_all(client, "331 Password required\r\n");
            } else if (line == "PASS m5-password\r\n") {
                send_all(client, "230 Login successful\r\n");
            } else if (line == "TYPE I\r\n") {
                send_all(client, "200 Binary mode\r\n");
            } else if (line.rfind("MKD ", 0) == 0) {
                send_all(client, "257 Directory created\r\n");
            } else if (line == "EPSV\r\n") {
                uint16_t data_port = 0;
                int passive_fd = make_listener(data_port);
                if (passive_fd < 0) {
                    send_all(client, "425 Passive failed\r\n");
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
                bool reject = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    reject = !rejected_path_.empty() && remote == rejected_path_;
                }
                if (reject) {
                    sockaddr_in data_peer{};
                    socklen_t data_length = sizeof(data_peer);
                    int data_fd = ::accept(passive_fd,
                                           reinterpret_cast<sockaddr*>(&data_peer),
                                           &data_length);
                    if (data_fd >= 0) {
                        ::shutdown(data_fd, SHUT_RDWR);
                        ::close(data_fd);
                    }
                    ::close(passive_fd);
                    send_all(client, "550 Deliberate worker failure\r\n");
                    continue;
                }
                send_all(client, "150 Opening data connection\r\n");
                serve_data(passive_fd, remote, client);
            } else if (line == "QUIT\r\n") {
                send_all(client, "221 Goodbye\r\n");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                break;
            } else {
                send_all(client, "502 Command not implemented\r\n");
            }
        }
        ::shutdown(client, SHUT_RDWR);
        ::close(client);
    }

    int expected_connections_;
    int listen_fd_;
    uint16_t port_;
    std::thread accept_thread_;
    std::vector<std::thread> client_threads_;
    mutable std::mutex mutex_;
    std::map<std::string, std::string> files_;
    std::string rejected_path_;
    std::atomic<int> connections_;
    std::atomic<int> active_transfers_;
    std::atomic<int> max_active_transfers_;
};

bool check(bool condition, const char* label) {
    if (!condition) std::cerr << "[FAIL] " << label << std::endl;
    else std::cout << "[PASS] " << label << std::endl;
    return condition;
}

bool write_file(const std::filesystem::path& path, const std::string& data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << data;
    return static_cast<bool>(file);
}

} // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    ParallelServer server(4); // one base session plus one new session per file worker task
    bool ok = check(server.start(), "M5 parallel server starts");
    if (!ok) return 1;

    std::filesystem::path root = "/tmp/ftpclient_m5_parallel";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const std::string alpha(120000, 'A');
    const std::string bravo(80000, 'B');
    const std::string charlie(40000, 'C');
    ok &= check(write_file(root / "a.bin", alpha) &&
                write_file(root / "b.bin", bravo) &&
                write_file(root / "c.bin", charlie),
                "M5 parallel files created");

    ftp_client_t* client = nullptr;
    ok &= check(ftp_client_create(&client) == FTP_OK, "M5 client creates");
    ftp_credentials_t creds{};
    creds.host = "localhost";
    creds.port = server.port();
    creds.username = "m5-user";
    creds.password = "m5-password";
    creds.use_tls = FTP_TLS_NONE;
    creds.verify_cert = FTP_VERIFY_NONE;
    ok &= check(ftp_connect(client, &creds) == FTP_OK, "M5 base control connects");

    ftp_upload_options_t options{};
    options.struct_size = sizeof(options);
    options.max_parallel = 2;
    options.retry_attempts = 0;
    options.resume_enabled = 0;
    options.create_remote_dirs = 0;
    ftp_result_t result{};
    int32_t ret = ftp_upload_dir(client, root.c_str(), "deploy", &options,
                                 nullptr, nullptr, &result);
    ok &= check(ret == FTP_OK, "M5 parallel upload succeeds");
    ok &= check(result.files_total == 3 && result.files_success == 3 &&
                result.files_failed == 0 && result.bytes_transferred == 240000,
                "M5 aggregate result is accurate");
    ok &= check(result.file_results != nullptr && result.file_result_count == 3 &&
                std::string(result.file_results[0].remote_path) == "deploy/a.bin" &&
                std::string(result.file_results[1].remote_path) == "deploy/b.bin" &&
                std::string(result.file_results[2].remote_path) == "deploy/c.bin",
                "M5 results are deterministic by remote path");
    ok &= check(server.max_active_transfers() >= 2,
                "M5 worker sessions overlap independently");
    ok &= check(server.file("deploy/a.bin") == alpha &&
                server.file("deploy/b.bin") == bravo &&
                server.file("deploy/c.bin") == charlie,
                "M5 independent sessions preserve file isolation");
    ok &= check(server.connections() == 3,
                "M6 reuses one independent session per worker");
    ok &= check(ftp_result_free(&result) == FTP_OK, "M5 result frees");
    ok &= check(ftp_disconnect(client) == FTP_OK, "M5 base session disconnects");
    ok &= check(ftp_client_destroy(client) == FTP_OK, "M5 client destroys");
    std::filesystem::remove_all(root);

    ParallelServer failure_server(4, "deploy/fail.bin");
    ok &= check(failure_server.start(), "M5 failure-isolation server starts");
    std::filesystem::path failure_root = "/tmp/ftpclient_m5_failure";
    std::filesystem::remove_all(failure_root);
    std::filesystem::create_directories(failure_root);
    const std::string good_payload(60000, 'G');
    const std::string fail_payload(50000, 'F');
    const std::string after_payload(40000, 'A');
    ok &= check(write_file(failure_root / "good.bin", good_payload) &&
                write_file(failure_root / "fail.bin", fail_payload) &&
                write_file(failure_root / "after.bin", after_payload),
                "M5 failure-isolation files created");

    ftp_client_t* failure_client = nullptr;
    ok &= check(ftp_client_create(&failure_client) == FTP_OK,
                "M5 failure-isolation client creates");
    creds.port = failure_server.port();
    ok &= check(ftp_connect(failure_client, &creds) == FTP_OK,
                "M5 failure-isolation base connects");
    ok &= check(ftp_set_retry_policy(failure_client, 0, 0, 0) == FTP_OK,
                "M5 failure-isolation retries disabled");
    ftp_upload_options_t failure_options{};
    failure_options.struct_size = sizeof(failure_options);
    failure_options.max_parallel = 2;
    failure_options.retry_attempts = 0;
    failure_options.create_remote_dirs = 0;
    ftp_result_t failure_result{};
    int32_t failure_ret = ftp_upload_dir(failure_client, failure_root.c_str(),
                                         "deploy", &failure_options,
                                         nullptr, nullptr, &failure_result);
    ok &= check(failure_ret != FTP_OK && failure_result.files_total == 3 &&
                failure_result.files_success == 2 && failure_result.files_failed == 1,
                "M6 one worker failure is isolated in aggregate results");
    uint64_t failed_files = 0;
    if (failure_result.file_results != nullptr) {
        for (uint64_t i = 0; i < failure_result.file_result_count; ++i) {
            if (failure_result.file_results[i].status != 0) ++failed_files;
        }
    }
    ok &= check(failure_server.file("deploy/good.bin") == good_payload &&
                failure_server.file("deploy/after.bin") == after_payload &&
                failed_files == 1,
                "M6 failed worker reconnects and completes its next file");
    ok &= check(ftp_result_free(&failure_result) == FTP_OK,
                "M6 failure result frees");
    ok &= check(ftp_disconnect(failure_client) == FTP_OK,
                "M6 failure-isolation disconnects");
    ok &= check(ftp_client_destroy(failure_client) == FTP_OK,
                "M6 failure-isolation client destroys");
    std::filesystem::remove_all(failure_root);
    return ok ? 0 : 1;
}
