#include "../include/ftpclient.h"

#include <csignal>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include <openssl/ssl.h>

#ifndef FTPS_TEST_CERT_PATH
#error "FTPS_TEST_CERT_PATH must point to the test certificate"
#endif
#ifndef FTPS_TEST_KEY_PATH
#error "FTPS_TEST_KEY_PATH must point to the test private key"
#endif

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef _WIN32
int main() {
    std::cout << "M7 download integration test skipped on Windows in this baseline" << std::endl;
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
        int count = SSL_write(ssl, value.data() + sent,
                              static_cast<int>(value.size() - sent));
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

class MockDownloadServer {
public:
    MockDownloadServer(bool tls, bool reject_epsv, bool negative_final,
                       std::string payload)
        : tls_(tls), reject_epsv_(reject_epsv), negative_final_(negative_final),
          payload_(std::move(payload)), listen_fd_(-1), port_(0), worker_() {}

    bool start() {
        listen_fd_ = make_listener(port_);
        if (listen_fd_ < 0) return false;
        worker_ = std::thread(&MockDownloadServer::serve, this);
        return true;
    }

    ~MockDownloadServer() {
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (worker_.joinable()) worker_.join();
    }

    uint16_t port() const { return port_; }

private:
    SSL_CTX* make_server_context() {
        SSL_CTX* context = SSL_CTX_new(TLS_server_method());
        if (!context || SSL_CTX_use_certificate_file(context, FTPS_TEST_CERT_PATH,
                                                     SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(context, FTPS_TEST_KEY_PATH,
                                        SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(context) != 1) {
            if (context) SSL_CTX_free(context);
            return nullptr;
        }
        return context;
    }

    bool send_data(int passive_fd, SSL_CTX* context) {
        sockaddr_in peer{};
        socklen_t peer_length = sizeof(peer);
        int data_fd = ::accept(passive_fd, reinterpret_cast<sockaddr*>(&peer),
                               &peer_length);
        ::close(passive_fd);
        if (data_fd < 0) return false;

        bool ok = true;
        if (tls_) {
            SSL* data_ssl = SSL_new(context);
            if (data_ssl == nullptr) {
                ::close(data_fd);
                return false;
            }
            SSL_set_fd(data_ssl, data_fd);
            ok = SSL_accept(data_ssl) == 1 && send_tls(data_ssl, payload_);
            SSL_shutdown(data_ssl);
            SSL_free(data_ssl);
        } else {
            ok = send_plain(data_fd, payload_);
        }
        ::shutdown(data_fd, SHUT_RDWR);
        ::close(data_fd);
        return ok;
    }

    template <typename Reader, typename Writer>
    void command_loop(Reader read_line, Writer send_line, SSL_CTX* context) {
        std::string line;
        while (read_line(line)) {
            if (line == "USER m7-user\r\n") {
                send_line("331 Password required\r\n");
            } else if (line == "PASS m7-password\r\n") {
                send_line("230 Login successful\r\n");
            } else if (line == "PBSZ 0\r\n") {
                send_line("200 PBSZ=0\r\n");
            } else if (line == "PROT P\r\n") {
                send_line("200 PROT P accepted\r\n");
            } else if (line == "TYPE I\r\n") {
                send_line("200 Binary mode\r\n");
            } else if (line == "CWD m7-dir\r\n") {
                send_line("250 Directory changed\r\n");
            } else if (line == "CDUP\r\n") {
                send_line("200 Parent directory\r\n");
            } else if (line == "DELE m7-delete.bin\r\n") {
                send_line("250 File deleted\r\n");
            } else if (line == "RMD m7-dir\r\n") {
                send_line("250 Directory removed\r\n");
            } else if (line == "RNFR m7-old.bin\r\n") {
                send_line("350 Ready for destination\r\n");
            } else if (line == "RNTO m7-new.bin\r\n") {
                send_line("250 Rename complete\r\n");
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
                send_line("229 Entering Extended Passive Mode (|||" +
                          std::to_string(data_port) + "|)\r\n");
                if (!read_line(line)) {
                    ::close(passive_fd);
                    break;
                }
                if (line != "RETR m7-remote.bin\r\n" &&
                    line != "RETR m7-empty.bin\r\n" &&
                    line != "RETR m7-negative.bin\r\n") {
                    ::close(passive_fd);
                    send_line("550 Unexpected retrieval path\r\n");
                    continue;
                }
                send_line("150 Opening data connection\r\n");
                bool data_ok = send_data(passive_fd, context);
                send_line(negative_final_ ? "550 Retrieval failed\r\n" :
                          (data_ok ? "226 Transfer complete\r\n" :
                                     "426 Transfer aborted\r\n"));
            } else if (line == "PASV\r\n") {
                uint16_t data_port = 0;
                int passive_fd = make_listener(data_port);
                if (passive_fd < 0) {
                    send_line("425 Cannot open passive connection\r\n");
                    continue;
                }
                unsigned int p1 = data_port / 256;
                unsigned int p2 = data_port % 256;
                send_line("227 Entering Passive Mode (127,0,0,1," +
                          std::to_string(p1) + "," + std::to_string(p2) + ")\r\n");
                if (!read_line(line)) {
                    ::close(passive_fd);
                    break;
                }
                send_line("150 Opening data connection\r\n");
                bool data_ok = send_data(passive_fd, context);
                send_line(negative_final_ ? "550 Retrieval failed\r\n" :
                          (data_ok ? "226 Transfer complete\r\n" :
                                     "426 Transfer aborted\r\n"));
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
        int client = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer),
                              &peer_length);
        if (client < 0) return;

        SSL_CTX* context = nullptr;
        SSL* control_ssl = nullptr;
        if (!send_plain(client, "220 M7 download server ready\r\n")) {
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
            auto reader = [control_ssl](std::string& value) {
                return read_tls_line(control_ssl, value);
            };
            auto writer = [control_ssl](const std::string& value) {
                return send_tls(control_ssl, value);
            };
            command_loop(reader, writer, context);
            SSL_shutdown(control_ssl);
            SSL_free(control_ssl);
        } else {
            auto reader = [client](std::string& value) {
                return read_plain_line(client, value);
            };
            auto writer = [client](const std::string& value) {
                return send_plain(client, value);
            };
            command_loop(reader, writer, nullptr);
        }

        if (context) SSL_CTX_free(context);
        ::shutdown(client, SHUT_RDWR);
        ::close(client);
    }

    bool tls_;
    bool reject_epsv_;
    bool negative_final_;
    std::string payload_;
    int listen_fd_;
    uint16_t port_;
    std::thread worker_;
};

bool check(bool condition, const char* label) {
    if (!condition) std::cerr << "[FAIL] " << label << std::endl;
    else std::cout << "[PASS] " << label << std::endl;
    return condition;
}

bool run_download(bool tls, bool reject_epsv, bool negative_final,
                  const std::string& payload, const char* remote_path,
                  const char* local_path) {
    MockDownloadServer server(tls, reject_epsv, negative_final, payload);
    bool ok = check(server.start(), tls ? "M7 FTPS download server starts"
                                         : "M7 plain download server starts");
    if (!ok) return false;

    ::unlink(local_path);
    std::string parent(local_path);
    size_t slash = parent.find_last_of('/');
    if (slash != std::string::npos) {
        std::string part = parent.substr(0, slash) + ".ftpclient.part";
        ::unlink(part.c_str());
    }

    ftp_client_t* client = nullptr;
    ok &= check(ftp_client_create(&client) == FTP_OK, "M7 download client creates");
    ftp_credentials_t creds{};
    creds.host = "localhost";
    creds.port = server.port();
    creds.username = "m7-user";
    creds.password = "m7-password";
    creds.use_tls = tls ? FTP_TLS_EXPLICIT : FTP_TLS_NONE;
    creds.verify_cert = tls ? FTP_VERIFY_HOST : FTP_VERIFY_NONE;
    creds.ca_bundle_path = tls ? FTPS_TEST_CERT_PATH : nullptr;
    ok &= check(ftp_connect(client, &creds) == FTP_OK,
                "M7 download control connection succeeds");

    ftp_result_t result{};
    int32_t ret = ftp_download_file(client, local_path, remote_path,
                                    nullptr, nullptr, &result);
    if (negative_final) {
        ok &= check(ret != FTP_OK, "M7 negative final reply fails download");
        ok &= check(result.files_failed == 1 && result.files_success == 0,
                    "M7 negative result is accurate");
        ok &= check(!std::ifstream(local_path).good(),
                    "M7 negative transfer does not publish local file");
    } else {
        ok &= check(ret == FTP_OK, tls ? "M7 protected RETR succeeds"
                                        : "M7 plain RETR succeeds");
        std::ifstream input(local_path, std::ios::binary);
        std::string actual((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
        ok &= check(actual == payload, "M7 downloaded bytes are exact");
        ok &= check(result.status == FTP_OK && result.files_total == 1 &&
                    result.files_success == 1 && result.files_failed == 0 &&
                    result.bytes_transferred == payload.size(),
                    "M7 download result is accurate");
    }
    ok &= check(ftp_result_free(&result) == FTP_OK, "M7 download result frees");
    ok &= check(ftp_disconnect(client) == FTP_OK, "M7 download disconnects");
    ok &= check(ftp_client_destroy(client) == FTP_OK, "M7 download client destroys");
    ::unlink(local_path);
    return ok;
}

} // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    bool ok = true;
    const std::string payload = std::string("M7 binary", 9) + char(0) +
                                 std::string("payload\nsecond line", 19) + char(0xff);
    ok &= run_download(false, false, false, payload, "m7-remote.bin",
                       "/tmp/ftpclient_m7/plain/nested.bin");
    ok &= run_download(true, true, false, "", "m7-empty.bin",
                       "/tmp/ftpclient_m7/ftps/empty.bin");
    ok &= run_download(false, false, true, "failed-payload", "m7-negative.bin",
                       "/tmp/ftpclient_m7/negative.bin");
    return ok ? 0 : 1;
}
#endif
