#include "ftpclient.h"

#include <arpa/inet.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

class M12DownloadServer {
public:
    M12DownloadServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(listen_fd_ >= 0);
        int reuse = 1;
        assert(::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        assert(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        assert(::listen(listen_fd_, 16) == 0);
        socklen_t length = sizeof(address);
        assert(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        accept_thread_ = std::thread(&M12DownloadServer::accept_loop, this);
    }

    ~M12DownloadServer() {
        stopping_.store(true, std::memory_order_release);
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) accept_thread_.join();
        std::lock_guard<std::mutex> lock(session_mutex_);
        for (auto& session : sessions_) {
            if (session.joinable()) session.join();
        }
    }

    uint16_t port() const { return port_; }
    int max_active_retr() const { return max_active_retr_.load(std::memory_order_acquire); }
    int feat_count() const { return feat_count_.load(std::memory_order_acquire); }

private:
    static bool send_all(int fd, const std::string& value) {
        size_t offset = 0;
        while (offset < value.size()) {
            const ssize_t written = ::send(fd, value.data() + offset,
                                           value.size() - offset, MSG_NOSIGNAL);
            if (written <= 0) return false;
            offset += static_cast<size_t>(written);
        }
        return true;
    }

    static bool read_line(int fd, std::string& line) {
        line.clear();
        char ch = 0;
        while (true) {
            const ssize_t count = ::recv(fd, &ch, 1, 0);
            if (count != 1) return false;
            line.push_back(ch);
            if (line.size() >= 2 && line[line.size() - 2] == '\r' && ch == '\n') return true;
            if (line.size() >= 4096) return false;
        }
    }

    static int make_listener(uint16_t& port) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
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

    void accept_loop() {
        while (!stopping_.load(std::memory_order_acquire)) {
            sockaddr_in peer{};
            socklen_t length = sizeof(peer);
            const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &length);
            if (fd < 0) break;
            std::lock_guard<std::mutex> lock(session_mutex_);
            sessions_.emplace_back(&M12DownloadServer::handle_session, this, fd);
        }
    }

    void handle_session(int control_fd) {
        if (!send_all(control_fd, "220 M12 download server\r\n")) {
            ::close(control_fd);
            return;
        }
        std::string line;
        while (read_line(control_fd, line)) {
            if (line == "USER m12-user\r\n") {
                send_all(control_fd, "331 Password required\r\n");
            } else if (line == "PASS m12-password\r\n") {
                send_all(control_fd, "230 Login successful\r\n");
            } else if (line == "TYPE I\r\n") {
                send_all(control_fd, "200 Binary mode\r\n");
            } else if (line == "FEAT\r\n") {
                feat_count_.fetch_add(1, std::memory_order_relaxed);
                send_all(control_fd,
                         "211-Features supported\r\n SIZE\r\n MDTM\r\n HASH SHA-256\r\n211 End\r\n");
            } else if (line.rfind("EPSV\r\n", 0) == 0) {
                uint16_t data_port = 0;
                const int passive_fd = make_listener(data_port);
                if (passive_fd < 0) {
                    send_all(control_fd, "425 Cannot open passive connection\r\n");
                    continue;
                }
                send_all(control_fd, "229 Entering Extended Passive Mode (|||" +
                                      std::to_string(data_port) + "|)\r\n");
                if (!read_line(control_fd, line)) {
                    ::close(passive_fd);
                    break;
                }
                std::string payload;
                bool is_listing = false;
                if (line == "MLSD /deploy\r\n") {
                    payload = "type=file;size=5;modify=20260818120100; hello.txt\r\n"
                              "type=file;size=5;modify=20260818120200; world.txt\r\n";
                    is_listing = true;
                } else if (line == "RETR /deploy/hello.txt\r\n") {
                    payload = "hello";
                } else if (line == "RETR /deploy/world.txt\r\n") {
                    payload = "world";
                } else {
                    ::close(passive_fd);
                    send_all(control_fd, "550 Unexpected data command\r\n");
                    continue;
                }
                send_all(control_fd, is_listing ? "150 Opening MLSD\r\n" : "150 Opening RETR\r\n");
                sockaddr_in data_peer{};
                socklen_t data_length = sizeof(data_peer);
                const int data_fd = ::accept(passive_fd,
                                             reinterpret_cast<sockaddr*>(&data_peer),
                                             &data_length);
                ::close(passive_fd);
                if (data_fd >= 0) {
                    if (!is_listing) {
                        const int active = active_retr_.fetch_add(1, std::memory_order_acq_rel) + 1;
                        int observed = max_active_retr_.load(std::memory_order_acquire);
                        while (active > observed &&
                               !max_active_retr_.compare_exchange_weak(
                                   observed, active, std::memory_order_release,
                                   std::memory_order_relaxed)) {}
                        std::this_thread::sleep_for(std::chrono::milliseconds(150));
                    }
                    send_all(data_fd, payload);
                    ::shutdown(data_fd, SHUT_RDWR);
                    ::close(data_fd);
                    if (!is_listing) active_retr_.fetch_sub(1, std::memory_order_acq_rel);
                }
                send_all(control_fd, "226 Transfer complete\r\n");
            } else if (line == "HASH /deploy/hello.txt\r\n") {
                send_all(control_fd,
                         "213 SHA-256 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824\r\n");
            } else if (line == "HASH /deploy/world.txt\r\n") {
                send_all(control_fd,
                         "213 SHA-256 486ea46224d1bb4fb680f34f7c9ad96a8f24ec88be73ea8e5a6c65260e9cb8a7\r\n");
            } else if (line == "QUIT\r\n") {
                send_all(control_fd, "221 Goodbye\r\n");
                break;
            } else {
                send_all(control_fd, "502 Unsupported command\r\n");
            }
        }
        ::shutdown(control_fd, SHUT_RDWR);
        ::close(control_fd);
    }

    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<int> active_retr_{0};
    std::atomic<int> max_active_retr_{0};
    std::atomic<int> feat_count_{0};
    std::thread accept_thread_;
    std::mutex session_mutex_;
    std::vector<std::thread> sessions_;
};

} // namespace

int main() {
    M12DownloadServer server;
    ftp_client_t* client = nullptr;
    assert(ftp_client_create(&client) == FTP_OK);
    ftp_credentials_t credentials{};
    credentials.host = "127.0.0.1";
    credentials.port = server.port();
    credentials.username = "m12-user";
    credentials.password = "m12-password";
    credentials.use_tls = FTP_TLS_NONE;
    credentials.verify_cert = FTP_VERIFY_NONE;
    assert(ftp_connect(client, &credentials) == FTP_OK);

    const std::filesystem::path root = "/tmp/ftpclient_m12_download";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ftp_download_options_t options{};
    options.struct_size = sizeof(options);
    options.verify_remote_hash = 1;
    options.max_parallel = 2;
    ftp_result_t result{};
    const int32_t status = ftp_download_dir(
        client, root.c_str(), "/deploy", &options, nullptr, nullptr, &result);
    assert(status == FTP_OK);
    assert(result.status == FTP_OK);
    assert(result.files_total == 2);
    assert(result.files_success == 2);
    assert(result.files_failed == 0);
    assert(result.bytes_transferred == 10);
    assert(result.file_result_count == 2);
    assert(server.max_active_retr() >= 2);
    assert(server.feat_count() >= 2);

    std::ifstream hello(root / "hello.txt", std::ios::binary);
    std::ifstream world(root / "world.txt", std::ios::binary);
    std::string hello_content((std::istreambuf_iterator<char>(hello)), {});
    std::string world_content((std::istreambuf_iterator<char>(world)), {});
    assert(hello_content == "hello");
    assert(world_content == "world");
    for (uint64_t index = 0; index < result.file_result_count; ++index) {
        const auto& file = result.file_results[index];
        assert(file.verification_status == FTP_VERIFY_STATUS_PASSED);
        assert((file.verification_sources & FTP_VERIFY_SOURCE_LOCAL) != 0);
        assert((file.verification_sources & FTP_VERIFY_SOURCE_REMOTE) != 0);
        assert(std::string(file.verification_algorithm) == "SHA-256");
        assert(file.local_digest != nullptr);
        assert(file.remote_digest != nullptr);
        assert(std::string(file.local_digest) == std::string(file.remote_digest));
    }
    assert(ftp_result_free(&result) == FTP_OK);
    assert(ftp_disconnect(client) == FTP_OK);
    assert(ftp_client_destroy(client) == FTP_OK);
    std::filesystem::remove_all(root, error);
    std::cout << "M12 remote HASH verification and parallel downloads passed" << std::endl;
    return 0;
}
