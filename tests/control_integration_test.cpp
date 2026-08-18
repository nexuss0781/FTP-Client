#include "ftpclient.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef _WIN32
int main() {
    std::cout << "M1 control integration test skipped on Windows in this baseline" << std::endl;
    return 0;
}
#else
namespace {

class MockFtpServer {
public:
    explicit MockFtpServer(bool auth_failure = false, bool malformed_greeting = false)
        : auth_failure_(auth_failure), malformed_greeting_(malformed_greeting), listen_fd_(-1), port_(0) {}

    bool start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;

        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) return false;
        if (::listen(listen_fd_, 1) != 0) return false;

        socklen_t length = sizeof(address);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length) != 0) return false;
        port_ = ntohs(address.sin_port);
        worker_ = std::thread(&MockFtpServer::serve, this);
        return true;
    }

    ~MockFtpServer() {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
        }
        if (worker_.joinable()) worker_.join();
    }

    uint16_t port() const { return port_; }

private:
    static bool send_all(int fd, const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            ssize_t count = ::send(fd, data.data() + sent, data.size() - sent, 0);
            if (count <= 0) return false;
            sent += static_cast<size_t>(count);
        }
        return true;
    }

    static bool read_line(int fd, std::string& line) {
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

    void serve() {
        sockaddr_in peer{};
        socklen_t peer_length = sizeof(peer);
        int client = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (client < 0) return;

        timeval timeout{};
        timeout.tv_sec = 3;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        if (malformed_greeting_) {
            send_all(client, "this is not an ftp reply\r\n");
            ::shutdown(client, SHUT_RDWR);
            ::close(client);
            return;
        }

        send_all(client, "220 M1 loopback FTP ready\r\n");
        std::string line;
        while (read_line(client, line)) {
            if (line.rfind("USER ", 0) == 0) {
                send_all(client, "331 Password required\r\n");
            } else if (line.rfind("PASS ", 0) == 0) {
                if (auth_failure_) {
                    send_all(client, "530 Login incorrect\r\n");
                } else {
                    send_all(client, "230 Login successful\r\n");
                }
            } else if (line == "NOOP\r\n") {
                send_all(client, "200 NOOP ok\r\n");
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

    bool auth_failure_;
    bool malformed_greeting_;
    int listen_fd_;
    uint16_t port_;
    std::thread worker_;
};

bool check(bool condition, const char* label) {
    if (!condition) std::cerr << "[FAIL] " << label << std::endl;
    else std::cout << "[PASS] " << label << std::endl;
    return condition;
}

ftp_credentials_t credentials(uint16_t port) {
    ftp_credentials_t value{};
    value.host = "127.0.0.1";
    value.port = port;
    value.username = "m1-user";
    value.password = "m1-password";
    value.use_tls = FTP_TLS_NONE;
    value.verify_cert = FTP_VERIFY_NONE;
    return value;
}

bool test_successful_control_session() {
    MockFtpServer server;
    if (!check(server.start(), "loopback server starts")) return false;

    ftp_client_t* client = nullptr;
    bool ok = true;
    ok &= check(ftp_client_create(&client) == FTP_OK, "client creates for control session");
    auto creds = credentials(server.port());
    ok &= check(ftp_connect(client, &creds) == FTP_OK, "real USER/PASS control session connects");
    ok &= check(ftp_ping(client) == FTP_OK, "real NOOP control session succeeds");
    ok &= check(ftp_disconnect(client) == FTP_OK, "real QUIT control session disconnects");
    ok &= check(ftp_client_destroy(client) == FTP_OK, "control-session client destroys");
    return ok;
}

bool test_authentication_failure() {
    MockFtpServer server(true, false);
    if (!check(server.start(), "authentication-failure server starts")) return false;

    ftp_client_t* client = nullptr;
    bool ok = true;
    ok &= check(ftp_client_create(&client) == FTP_OK, "client creates for auth failure");
    auto creds = credentials(server.port());
    ok &= check(ftp_connect(client, &creds) == FTP_ERR_AUTH_FAILED, "530 maps to authentication failure");
    ok &= check(ftp_ping(client) == FTP_ERR_INVALID_STATE, "failed authentication does not expose a session");
    ok &= check(ftp_client_destroy(client) == FTP_OK, "failed-auth client destroys");
    return ok;
}

bool test_malformed_greeting() {
    MockFtpServer server(false, true);
    if (!check(server.start(), "malformed-greeting server starts")) return false;

    ftp_client_t* client = nullptr;
    bool ok = true;
    ok &= check(ftp_client_create(&client) == FTP_OK, "client creates for malformed greeting");
    auto creds = credentials(server.port());
    ok &= check(ftp_connect(client, &creds) == FTP_ERR_PROTOCOL, "malformed greeting maps to protocol error");
    ok &= check(ftp_client_destroy(client) == FTP_OK, "malformed-greeting client destroys");
    return ok;
}

}  // namespace

int main() {
    std::cout << "=== M1 Control Session Integration Tests ===" << std::endl;
    bool ok = true;
    ok &= test_successful_control_session();
    ok &= test_authentication_failure();
    ok &= test_malformed_greeting();
    return ok ? 0 : 1;
}
#endif
