#include "ftpclient.h"

#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <openssl/err.h>
#include <openssl/ssl.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifndef FTPS_TEST_CERT_PATH
#error "FTPS_TEST_CERT_PATH must point to the test certificate"
#endif
#ifndef FTPS_TEST_KEY_PATH
#error "FTPS_TEST_KEY_PATH must point to the test private key"
#endif

#ifdef _WIN32
int main() {
    std::cout << "M2 FTPS integration test skipped on Windows in this baseline" << std::endl;
    return 0;
}
#else
namespace {

class MockFtpsServer {
public:
    explicit MockFtpsServer(bool reject_auth_tls = false)
        : reject_auth_tls_(reject_auth_tls), listen_fd_(-1), port_(0), worker_() {}

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
        worker_ = std::thread(&MockFtpsServer::serve, this);
        return true;
    }

    ~MockFtpsServer() {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
        }
        if (worker_.joinable()) worker_.join();
    }

    uint16_t port() const { return port_; }

private:
    static bool send_plain(int fd, const std::string& value) {
        size_t sent = 0;
        while (sent < value.size()) {
            ssize_t count = ::send(fd, value.data() + sent, value.size() - sent, 0);
            if (count <= 0) return false;
            sent += static_cast<size_t>(count);
        }
        return true;
    }

    static bool send_tls(SSL* ssl, const std::string& value) {
        size_t sent = 0;
        while (sent < value.size()) {
            int count = SSL_write(ssl, value.data() + sent, static_cast<int>(value.size() - sent));
            if (count <= 0) return false;
            sent += static_cast<size_t>(count);
        }
        return true;
    }

    static bool read_plain_line(int fd, std::string& line) {
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

    static bool read_tls_line(SSL* ssl, std::string& line) {
        line.clear();
        char ch = 0;
        while (line.size() < 4096) {
            int count = SSL_read(ssl, &ch, 1);
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
        ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        if (!send_plain(client, "220 M2 loopback FTPS ready\r\n")) {
            ::close(client);
            return;
        }

        std::string line;
        if (!read_plain_line(client, line)) {
            ::close(client);
            return;
        }
        if (line != "AUTH TLS\r\n") {
            send_plain(client, "530 TLS required\r\n");
            ::close(client);
            return;
        }
        if (reject_auth_tls_) {
            send_plain(client, "502 AUTH TLS not available\r\n");
            ::close(client);
            return;
        }
        if (!send_plain(client, "234 AUTH TLS OK\r\n")) {
            ::close(client);
            return;
        }

        SSL_CTX* context = SSL_CTX_new(TLS_server_method());
        if (!context || SSL_CTX_use_certificate_file(context, FTPS_TEST_CERT_PATH, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(context, FTPS_TEST_KEY_PATH, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(context) != 1) {
            if (context) SSL_CTX_free(context);
            ::close(client);
            return;
        }

        SSL* ssl = SSL_new(context);
        SSL_set_fd(ssl, client);
        if (SSL_accept(ssl) == 1) {
            while (read_tls_line(ssl, line)) {
                if (line.rfind("USER ", 0) == 0) {
                    send_tls(ssl, "331 Password required\r\n");
                } else if (line.rfind("PASS ", 0) == 0) {
                    send_tls(ssl, "230 Login successful\r\n");
                } else if (line == "PBSZ 0\r\n") {
                    send_tls(ssl, "200 PBSZ=0\r\n");
                } else if (line == "PROT P\r\n") {
                    send_tls(ssl, "200 PROT P accepted\r\n");
                } else if (line == "NOOP\r\n") {
                    send_tls(ssl, "200 NOOP ok\r\n");
                } else if (line == "QUIT\r\n") {
                    send_tls(ssl, "221 Goodbye\r\n");
                    break;
                } else {
                    send_tls(ssl, "502 Command not implemented\r\n");
                }
            }
        }
        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(context);
        ::shutdown(client, SHUT_RDWR);
        ::close(client);
    }

    bool reject_auth_tls_;
    int listen_fd_;
    uint16_t port_;
    std::thread worker_;
};

bool check(bool condition, const char* label) {
    if (!condition) std::cerr << "[FAIL] " << label << std::endl;
    else std::cout << "[PASS] " << label << std::endl;
    return condition;
}

ftp_credentials_t credentials(uint16_t port, int32_t verify_mode, const char* ca_bundle, const char* host = "localhost") {
    ftp_credentials_t value{};
    value.host = host;
    value.port = port;
    value.username = "m2-user";
    value.password = "m2-password";
    value.use_tls = FTP_TLS_EXPLICIT;
    value.verify_cert = verify_mode;
    value.ca_bundle_path = ca_bundle;
    return value;
}

bool test_trusted_hostname() {
    MockFtpsServer server;
    if (!check(server.start(), "trusted FTPS server starts")) return false;
    ftp_client_t* client = nullptr;
    bool ok = true;
    ok &= check(ftp_client_create(&client) == FTP_OK, "trusted client creates");
    auto creds = credentials(server.port(), FTP_VERIFY_HOST, FTPS_TEST_CERT_PATH);
    ok &= check(ftp_connect(client, &creds) == FTP_OK, "AUTH TLS and trusted hostname succeed");
    ok &= check(ftp_ping(client) == FTP_OK, "protected control NOOP succeeds");
    ok &= check(ftp_disconnect(client) == FTP_OK, "trusted FTPS disconnect succeeds");
    ok &= check(ftp_client_destroy(client) == FTP_OK, "trusted client destroys");
    return ok;
}

bool test_hostname_mismatch() {
    MockFtpsServer server;
    if (!check(server.start(), "hostname-mismatch server starts")) return false;
    ftp_client_t* client = nullptr;
    bool ok = true;
    ok &= check(ftp_client_create(&client) == FTP_OK, "hostname-mismatch client creates");
    auto creds = credentials(server.port(), FTP_VERIFY_HOST, FTPS_TEST_CERT_PATH, "127.0.0.1");
    ok &= check(ftp_connect(client, &creds) == FTP_ERR_CERT_VERIFY, "hostname mismatch is rejected");
    ok &= check(ftp_client_destroy(client) == FTP_OK, "hostname-mismatch client destroys");
    return ok;
}

bool test_untrusted_chain() {
    MockFtpsServer server;
    if (!check(server.start(), "untrusted-chain server starts")) return false;
    ftp_client_t* client = nullptr;
    bool ok = true;
    ok &= check(ftp_client_create(&client) == FTP_OK, "untrusted-chain client creates");
    auto creds = credentials(server.port(), FTP_VERIFY_PEER, nullptr);
    ok &= check(ftp_connect(client, &creds) == FTP_ERR_CERT_VERIFY, "untrusted certificate chain is rejected");
    ok &= check(ftp_client_destroy(client) == FTP_OK, "untrusted-chain client destroys");
    return ok;
}

bool test_auth_tls_rejection() {
    MockFtpsServer server(true);
    if (!check(server.start(), "AUTH TLS rejection server starts")) return false;
    ftp_client_t* client = nullptr;
    bool ok = true;
    ok &= check(ftp_client_create(&client) == FTP_OK, "AUTH TLS rejection client creates");
    auto creds = credentials(server.port(), FTP_VERIFY_NONE, nullptr);
    ok &= check(ftp_connect(client, &creds) == FTP_ERR_AUTH_TLS_REQUIRED, "AUTH TLS rejection is surfaced");
    ok &= check(ftp_client_destroy(client) == FTP_OK, "AUTH TLS rejection client destroys");
    return ok;
}

}  // namespace

int main() {
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
    std::cout << "=== M2 Explicit FTPS Integration Tests ===" << std::endl;
    bool ok = true;
    ok &= test_trusted_hostname();
    ok &= test_hostname_mismatch();
    ok &= test_untrusted_chain();
    ok &= test_auth_tls_rejection();
    return ok ? 0 : 1;
}
#endif
