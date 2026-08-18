#include "../include/ftpclient.h"

#include <csignal>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

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
    std::cout << "M3 data-transfer integration test skipped on Windows in this baseline" << std::endl;
    return 0;
}
#else
namespace {

bool send_plain(int fd, const std::string& value) {
    size_t sent = 0;
    while (sent < value.size()) {
        ssize_t count = ::send(fd, value.data() + sent, value.size() - sent, 0);
        if (count <= 0) return false;
        sent += static_cast<size_t>(count);
    }
    return true;
}

bool send_tls(SSL* ssl, const std::string& value) {
    size_t sent = 0;
    while (sent < value.size()) {
        int count = SSL_write(ssl, value.data() + sent, static_cast<int>(value.size() - sent));
        if (count <= 0) return false;
        sent += static_cast<size_t>(count);
    }
    return true;
}

bool read_plain_line(int fd, std::string& line) {
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

bool read_tls_line(SSL* ssl, std::string& line) {
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

void set_socket_timeout(int fd) {
    timeval timeout{};
    timeout.tv_sec = 5;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

class MockTransferServer {
public:
    explicit MockTransferServer(bool tls, bool reject_epsv = false)
        : tls_(tls), reject_epsv_(reject_epsv), listen_fd_(-1), port_(0), worker_(), received_() {}

    bool start() {
        listen_fd_ = make_listener(port_);
        if (listen_fd_ < 0) return false;
        worker_ = std::thread(&MockTransferServer::serve, this);
        return true;
    }

    ~MockTransferServer() {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
        }
        if (worker_.joinable()) worker_.join();
    }

    uint16_t port() const { return port_; }
    const std::string& received() const { return received_; }

private:
    SSL_CTX* make_server_context() {
        SSL_CTX* context = SSL_CTX_new(TLS_server_method());
        if (!context || SSL_CTX_use_certificate_file(context, FTPS_TEST_CERT_PATH, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(context, FTPS_TEST_KEY_PATH, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(context) != 1) {
            if (context) SSL_CTX_free(context);
            return nullptr;
        }
        return context;
    }

    bool receive_data(int passive_fd, SSL_CTX* context) {
        sockaddr_in peer{};
        socklen_t peer_length = sizeof(peer);
        int data_fd = ::accept(passive_fd, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        ::close(passive_fd);
        if (data_fd < 0) return false;
        set_socket_timeout(data_fd);

        bool ok = true;
        char buffer[8192];
        if (tls_) {
            SSL* data_ssl = SSL_new(context);
            SSL_set_fd(data_ssl, data_fd);
            ok = data_ssl != nullptr && SSL_accept(data_ssl) == 1;
            if (ok) {
                for (;;) {
                    int count = SSL_read(data_ssl, buffer, sizeof(buffer));
                    if (count <= 0) break;
                    received_.append(buffer, static_cast<size_t>(count));
                }
            }
            if (data_ssl) {
                SSL_shutdown(data_ssl);
                SSL_free(data_ssl);
            }
        } else {
            for (;;) {
                ssize_t count = ::recv(data_fd, buffer, sizeof(buffer), 0);
                if (count <= 0) break;
                received_.append(buffer, static_cast<size_t>(count));
            }
        }
        ::shutdown(data_fd, SHUT_RDWR);
        ::close(data_fd);
        return ok;
    }

    template <typename Reader, typename Writer>
    void command_loop(Reader read_line, Writer send_line, SSL_CTX* context) {
        std::string line;
        auto handle_passive = [&](int passive_fd, const std::string& passive_reply) {
            send_line(passive_reply);
            if (!read_line(line)) {
                ::close(passive_fd);
                return;
            }
            if (line == "STOR m3-remote.bin\r\n" || line == "STOR m3-empty.bin\r\n") {
                send_line("150 Opening protected data connection\r\n");
                bool data_ok = receive_data(passive_fd, context);
                send_line(data_ok ? "226 Transfer complete\r\n" : "426 Transfer aborted\r\n");
            } else {
                ::close(passive_fd);
                send_line("550 Unexpected transfer path\r\n");
            }
        };

        while (read_line(line)) {
            if (line == "USER m3-user\r\n") {
                send_line("331 Password required\r\n");
            } else if (line == "PASS m3-password\r\n") {
                send_line("230 Login successful\r\n");
            } else if (line == "PBSZ 0\r\n") {
                send_line("200 PBSZ=0\r\n");
            } else if (line == "PROT P\r\n") {
                send_line("200 PROT P accepted\r\n");
            } else if (line == "TYPE I\r\n") {
                send_line("200 Binary mode\r\n");
            } else if (line == "EPSV\r\n") {
                if (reject_epsv_) {
                    send_line("502 EPSV not implemented\r\n");
                    continue;
                }
                uint16_t data_port = 0;
                int passive_fd = make_listener(data_port);
                if (passive_fd < 0) {
                    send_line("425 Cannot open passive connection\r\n");
                    continue;
                }
                handle_passive(passive_fd,
                    "229 Entering Extended Passive Mode (|||" + std::to_string(data_port) + "|)\r\n");
            } else if (line == "PASV\r\n") {
                uint16_t data_port = 0;
                int passive_fd = make_listener(data_port);
                if (passive_fd < 0) {
                    send_line("425 Cannot open passive connection\r\n");
                    continue;
                }
                unsigned int p1 = data_port / 256;
                unsigned int p2 = data_port % 256;
                handle_passive(passive_fd,
                    "227 Entering Passive Mode (127,0,0,1," + std::to_string(p1) + "," +
                    std::to_string(p2) + ")\r\n");
            } else if (line == "QUIT\r\n") {
                send_line("221 Goodbye\r\n");
                break;
            } else {
                send_line("502 Command not implemented\r\n");
            }
        }
    }

    void serve() {
        sockaddr_in peer{};
        socklen_t peer_length = sizeof(peer);
        int client = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (client < 0) return;
        set_socket_timeout(client);

        SSL_CTX* context = nullptr;
        SSL* control_ssl = nullptr;
        if (!send_plain(client, "220 M3 transfer server ready\r\n")) {
            ::close(client);
            return;
        }

        if (tls_) {
            std::string line;
            if (!read_plain_line(client, line) || line != "AUTH TLS\r\n" ||
                !send_plain(client, "234 AUTH TLS OK\r\n")) {
                ::close(client);
                return;
            }
            context = make_server_context();
            if (!context) {
                ::close(client);
                return;
            }
            control_ssl = SSL_new(context);
            SSL_set_fd(control_ssl, client);
            if (SSL_accept(control_ssl) != 1) {
                SSL_free(control_ssl);
                SSL_CTX_free(context);
                ::close(client);
                return;
            }
            auto reader = [control_ssl](std::string& value) { return read_tls_line(control_ssl, value); };
            auto writer = [control_ssl](const std::string& value) { return send_tls(control_ssl, value); };
            command_loop(reader, writer, context);
            SSL_shutdown(control_ssl);
            SSL_free(control_ssl);
        } else {
            auto reader = [client](std::string& value) { return read_plain_line(client, value); };
            auto writer = [client](const std::string& value) { return send_plain(client, value); };
            command_loop(reader, writer, nullptr);
        }

        if (context) SSL_CTX_free(context);
        ::shutdown(client, SHUT_RDWR);
        ::close(client);
    }

    bool tls_;
    bool reject_epsv_;
    int listen_fd_;
    uint16_t port_;
    std::thread worker_;
    std::string received_;
};

bool check(bool condition, const char* label) {
    if (!condition) std::cerr << "[FAIL] " << label << std::endl;
    else std::cout << "[PASS] " << label << std::endl;
    return condition;
}

bool run_upload(bool tls, const char* local_path, const char* remote_path, const char* ca_path,
                const std::string& expected, bool reject_epsv = false) {
    MockTransferServer server(tls, reject_epsv);
    if (!check(server.start(), tls ? "FTPS data server starts" : "plain data server starts")) return false;

    ftp_client_t* client = nullptr;
    bool ok = true;
    ok &= check(ftp_client_create(&client) == FTP_OK, "M3 transfer client creates");
    ftp_credentials_t creds{};
    creds.host = "localhost";
    creds.port = server.port();
    creds.username = "m3-user";
    creds.password = "m3-password";
    creds.use_tls = tls ? FTP_TLS_EXPLICIT : FTP_TLS_NONE;
    creds.verify_cert = tls ? FTP_VERIFY_HOST : FTP_VERIFY_NONE;
    creds.ca_bundle_path = ca_path;
    int32_t connect_ret = ftp_connect(client, &creds);
    ok &= check(connect_ret == FTP_OK, "M3 transfer control connection succeeds");

    ftp_result_t result{};
    int32_t upload_ret = ftp_upload_dir(client, local_path, remote_path, nullptr, nullptr, nullptr, &result);
    ok &= check(upload_ret == FTP_OK, tls ? "protected STOR succeeds" : "plain STOR succeeds");
    ok &= check(result.status == FTP_OK && result.files_total == 1 &&
                result.files_success == 1 && result.files_failed == 0,
                "M3 aggregate result is accurate");
    ok &= check(result.file_result_count == 1 && result.file_results != nullptr &&
                result.file_results[0].status == FTP_OK &&
                result.file_results[0].attempt_count == 1 &&
                result.file_results[0].bytes_sent == expected.size(),
                "M3 per-file result is accurate");
    ok &= check(server.received() == expected, "server received exact upload bytes");
    ok &= check(ftp_result_free(&result) == FTP_OK, "M3 result resources free");
    ok &= check(ftp_disconnect(client) == FTP_OK, "M3 transfer disconnects");
    ok &= check(ftp_client_destroy(client) == FTP_OK, "M3 transfer client destroys");
    return ok;
}

} // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    bool ok = true;
    const std::string payload = "M3 protected payload\\nsecond line\\n";
    const std::string payload_path = "/tmp/ftpclient_m3_payload.bin";
    const std::string empty_path = "/tmp/ftpclient_m3_empty.bin";
    {
        std::ofstream file(payload_path, std::ios::binary | std::ios::trunc);
        file << payload;
    }
    {
        std::ofstream file(empty_path, std::ios::binary | std::ios::trunc);
    }

    ok &= run_upload(false, payload_path.c_str(), "m3-remote.bin", nullptr, payload, true);
    ok &= run_upload(true, empty_path.c_str(), "m3-empty.bin", FTPS_TEST_CERT_PATH, "");

    ::unlink(payload_path.c_str());
    ::unlink(empty_path.c_str());
    return ok ? 0 : 1;
}
#endif
