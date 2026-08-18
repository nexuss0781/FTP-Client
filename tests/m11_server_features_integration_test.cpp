#include "ftpclient.h"

#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

class FeatureServer {
public:
    FeatureServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(listen_fd_ >= 0);
        int reuse = 1;
        assert(::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        assert(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        assert(::listen(listen_fd_, 1) == 0);
        socklen_t length = sizeof(address);
        assert(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
        port_ = ntohs(address.sin_port);
        worker_ = std::thread(&FeatureServer::run, this);
    }

    ~FeatureServer() {
        if (worker_.joinable()) worker_.join();
        if (listen_fd_ >= 0) ::close(listen_fd_);
    }

    uint16_t port() const { return port_; }
    int feat_count() const { return feat_count_; }

private:
    static void send_all(int fd, const std::string& value) {
        size_t offset = 0;
        while (offset < value.size()) {
            const ssize_t written = ::send(fd, value.data() + offset,
                                           value.size() - offset, MSG_NOSIGNAL);
            assert(written > 0);
            offset += static_cast<size_t>(written);
        }
    }

    static std::string read_line(int fd) {
        std::string line;
        char ch = 0;
        while (true) {
            const ssize_t count = ::recv(fd, &ch, 1, 0);
            assert(count == 1);
            line.push_back(ch);
            if (line.size() >= 2 && line[line.size() - 2] == '\r' && ch == '\n') return line;
            assert(line.size() < 4096);
        }
    }

    void run() {
        sockaddr_in peer{};
        socklen_t length = sizeof(peer);
        const int fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &length);
        assert(fd >= 0);
        send_all(fd, "220 M11 feature server\r\n");
        while (true) {
            const std::string line = read_line(fd);
            if (line == "USER m11-user\r\n") {
                send_all(fd, "331 Password required\r\n");
            } else if (line == "PASS m11-password\r\n") {
                send_all(fd, "230 Login successful\r\n");
            } else if (line == "FEAT\r\n") {
                ++feat_count_;
                send_all(fd, "211-Features supported\r\n SIZE\r\n MDTM\r\n HASH SHA-256 SHA-512\r\n211 End\r\n");
            } else if (line == "MDTM /deploy/app.js\r\n") {
                send_all(fd, "213 20260818123456\r\n");
            } else if (line == "HASH /deploy/app.js\r\n") {
                send_all(fd, "213 SHA-256 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824\r\n");
            } else if (line == "QUIT\r\n") {
                send_all(fd, "221 Goodbye\r\n");
                break;
            } else {
                send_all(fd, "500 Unexpected command\r\n");
                break;
            }
        }
        ::close(fd);
    }

    int listen_fd_ = -1;
    uint16_t port_ = 0;
    int feat_count_ = 0;
    std::thread worker_;
};

} // namespace

int main() {
    FeatureServer server;
    ftp_client_t* client = nullptr;
    assert(ftp_client_create(&client) == FTP_OK);

    ftp_credentials_t credentials{};
    credentials.host = "127.0.0.1";
    credentials.port = server.port();
    credentials.username = "m11-user";
    credentials.password = "m11-password";
    credentials.use_tls = FTP_TLS_NONE;
    credentials.verify_cert = FTP_VERIFY_NONE;
    assert(ftp_connect(client, &credentials) == FTP_OK);

    ftp_server_capabilities_t capabilities{};
    capabilities.struct_size = sizeof(capabilities);
    assert(ftp_get_server_capabilities(client, &capabilities) == FTP_OK);
    assert(capabilities.feat_supported == 1);
    assert(capabilities.size_supported == 1);
    assert(capabilities.mdtm_supported == 1);
    assert(capabilities.hash_supported == 1);
    assert((capabilities.hash_algorithms & FTP_HASH_ALG_SHA256) != 0);
    assert((capabilities.hash_algorithms & FTP_HASH_ALG_SHA512) != 0);

    ftp_server_capabilities_t cached{};
    cached.struct_size = sizeof(cached);
    assert(ftp_get_server_capabilities(client, &cached) == FTP_OK);
    assert(server.feat_count() == 1);

    char modify[32] = {};
    assert(ftp_get_remote_file_mdtm(client, "/deploy/app.js", modify, sizeof(modify)) == FTP_OK);
    assert(std::string(modify) == "20260818123456");

    char hash[129] = {};
    assert(ftp_get_remote_file_hash(client, "/deploy/app.js", "SHA-256", hash, sizeof(hash)) == FTP_OK);
    assert(std::string(hash) == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");

    char too_small[4] = {};
    assert(ftp_get_remote_file_hash(client, "/deploy/app.js", "SHA-256", too_small, sizeof(too_small)) == FTP_ERR_INVALID_ARGUMENT);

    const std::string matching_local = "/tmp/m11-matching-app.js";
    const std::string mismatching_local = "/tmp/m11-mismatching-app.js";
    {
        std::ofstream output(matching_local, std::ios::binary);
        output << "hello";
    }
    {
        std::ofstream output(mismatching_local, std::ios::binary);
        output << "wrong";
    }
    assert(ftp_verify_local_file_with_remote_hash(
               client, matching_local.c_str(), "/deploy/app.js", "SHA-256") == FTP_OK);
    assert(ftp_verify_local_file_with_remote_hash(
               client, mismatching_local.c_str(), "/deploy/app.js", "SHA-256") == FTP_ERR_INTEGRITY);
    assert(ftp_verify_local_file_with_remote_hash(
               client, matching_local.c_str(), "/deploy/app.js", "MD5") == FTP_ERR_NOT_IMPLEMENTED);
    ::unlink(matching_local.c_str());
    ::unlink(mismatching_local.c_str());
    assert(ftp_disconnect(client) == FTP_OK);
    assert(ftp_client_destroy(client) == FTP_OK);
    std::cout << "M11 FEAT, MDTM, HASH, and verification integration passed" << std::endl;
    return 0;
}
