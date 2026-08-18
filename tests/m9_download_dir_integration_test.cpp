#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "ftpclient.h"

namespace fs = std::filesystem;

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

class M9DirectoryServer {
public:
    bool start() {
        listen_fd_ = make_listener(port_);
        if (listen_fd_ < 0) return false;
        worker_ = std::thread(&M9DirectoryServer::serve, this);
        return true;
    }

    ~M9DirectoryServer() {
        if (control_fd_ >= 0) {
            ::shutdown(control_fd_, SHUT_RDWR);
            ::close(control_fd_);
            control_fd_ = -1;
        }
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (worker_.joinable()) worker_.join();
    }

    uint16_t port() const { return port_; }

private:
    void serve() {
        sockaddr_in peer{};
        socklen_t length = sizeof(peer);
        control_fd_ = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &length);
        if (control_fd_ < 0) return;
        if (!send_all(control_fd_, "220 M9 directory server\r\n")) return;

        std::string line;
        while (read_line(control_fd_, line)) {
            if (line == "USER m9-user\r\n") {
                send_all(control_fd_, "331 Password required\r\n");
            } else if (line == "PASS m9-password\r\n") {
                send_all(control_fd_, "230 Login successful\r\n");
            } else if (line == "TYPE I\r\n") {
                send_all(control_fd_, "200 Binary mode\r\n");
            } else if (line == "SIZE /deploy/hello.txt\r\n") {
                send_all(control_fd_, "213 5\r\n");
            } else if (line.rfind("REST ", 0) == 0) {
                restart_offset_ = static_cast<size_t>(std::stoul(
                    line.substr(5, line.size() - 7)));
                send_all(control_fd_, "350 Restart position accepted\r\n");
            } else if (line.rfind("EPSV\r\n", 0) == 0) {
                uint16_t data_port = 0;
                int passive_fd = make_listener(data_port);
                if (passive_fd < 0) {
                    send_all(control_fd_, "425 Cannot open passive connection\r\n");
                    continue;
                }
                send_all(control_fd_, "229 Entering Extended Passive Mode (|||" +
                                      std::to_string(data_port) + "|)\r\n");
                if (!read_line(control_fd_, line)) {
                    ::close(passive_fd);
                    break;
                }
                std::string payload;
                bool is_listing = false;
                if (line == "MLSD /deploy\r\n") {
                    payload = "type=dir;modify=20260818120000; child\r\n"
                              "type=file;size=5;modify=20260818120100; hello.txt\r\n";
                    is_listing = true;
                } else if (line == "MLSD /deploy/child\r\n") {
                    payload = "type=file;size=6;modify=20260818120200; nested.bin\r\n";
                    is_listing = true;
                } else if (line == "RETR /deploy/hello.txt\r\n") {
                    payload = std::string("hello").substr(restart_offset_);
                    restart_offset_ = 0;
                } else if (line == "RETR /deploy/child/nested.bin\r\n") {
                    payload = "nested";
                } else {
                    ::close(passive_fd);
                    send_all(control_fd_, "550 Unexpected M9 command\r\n");
                    continue;
                }
                send_all(control_fd_, is_listing ? "150 Opening MLSD\r\n"
                                                  : "150 Opening RETR\r\n");
                sockaddr_in data_peer{};
                socklen_t data_length = sizeof(data_peer);
                int data_fd = ::accept(passive_fd,
                                       reinterpret_cast<sockaddr*>(&data_peer),
                                       &data_length);
                ::close(passive_fd);
                if (data_fd >= 0) {
                    send_all(data_fd, payload);
                    ::shutdown(data_fd, SHUT_RDWR);
                    ::close(data_fd);
                }
                send_all(control_fd_, "226 Transfer complete\r\n");
            } else if (line == "QUIT\r\n") {
                send_all(control_fd_, "221 Goodbye\r\n");
                break;
            } else {
                send_all(control_fd_, "502 Unsupported command\r\n");
            }
        }
    }

    int listen_fd_ = -1;
    int control_fd_ = -1;
    size_t restart_offset_ = 0;
    uint16_t port_ = 0;
    std::thread worker_;
};

int main() {
    M9DirectoryServer server;
    assert(server.start());

    ftp_client_t* client = nullptr;
    assert(ftp_client_create(&client) == FTP_OK);
    ftp_credentials_t credentials{};
    credentials.host = "127.0.0.1";
    credentials.port = server.port();
    credentials.username = "m9-user";
    credentials.password = "m9-password";
    credentials.use_tls = FTP_TLS_NONE;
    credentials.verify_cert = FTP_VERIFY_NONE;
    assert(ftp_connect(client, &credentials) == FTP_OK);

    const fs::path root = "/tmp/ftpclient_m9_directory";
    std::error_code error;
    fs::remove_all(root, error);
    ftp_download_digest_t digests[] = {
        {"/deploy/hello.txt", "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"},
        {"/deploy/child/nested.bin", "233562de1a0288b139c4fa40b7d189f806e906eeb048517aeb67f34ac0e2faf1"},
    };
    ftp_download_options_t options{};
    options.struct_size = sizeof(options);
    options.resume_enabled = 0;
    options.resume_metadata_enabled = 0;
    options.file_digests = digests;
    options.file_digest_count = 2;
    ftp_result_t result{};
    const int32_t status = ftp_download_dir(client, root.c_str(), "/deploy",
                                            &options, nullptr, nullptr, &result);
    assert(status == FTP_OK);
    assert(result.files_total == 2);
    assert(result.files_success == 2);
    assert(result.files_failed == 0);
    assert(result.bytes_transferred == 11);
    assert(fs::is_directory(root / "child"));

    std::ifstream first(root / "hello.txt", std::ios::binary);
    std::ifstream second(root / "child" / "nested.bin", std::ios::binary);
    std::string first_content((std::istreambuf_iterator<char>(first)), {});
    std::string second_content((std::istreambuf_iterator<char>(second)), {});
    assert(first_content == "hello");
    assert(second_content == "nested");
    assert(result.file_result_count == 2);
    assert(std::string(result.file_results[0].remote_path) == "/deploy/child/nested.bin");
    assert(std::string(result.file_results[1].remote_path) == "/deploy/hello.txt");
    assert(ftp_result_free(&result) == FTP_OK);

    ftp_download_options_t ambiguous_options{};
    ambiguous_options.struct_size = sizeof(ambiguous_options);
    ambiguous_options.expected_sha256 = digests[0].sha256;
    ftp_result_t ambiguous_result{};
    assert(ftp_download_dir(client, root.c_str(), "/deploy", &ambiguous_options,
                            nullptr, nullptr, &ambiguous_result) == FTP_ERR_INVALID_ARGUMENT);

    const fs::path resume_root = "/tmp/ftpclient_m9_resume";
    fs::remove_all(resume_root, error);
    fs::create_directories(resume_root, error);
    const fs::path resume_final = resume_root / "hello.txt";
    const fs::path resume_part = resume_final.string() + ".ftpclient.part";
    const fs::path resume_meta = resume_part.string() + ".meta";
    {
        std::ofstream partial(resume_part, std::ios::binary);
        partial << "he";
    }
    ftp_download_options_t resume_options{};
    resume_options.struct_size = sizeof(resume_options);
    resume_options.resume_enabled = 1;
    ftp_result_t resume_result{};
    assert(ftp_download_file_ex(client, resume_final.c_str(), "/deploy/hello.txt",
                                &resume_options, nullptr, nullptr, &resume_result) == FTP_OK);
    std::ifstream resumed(resume_final, std::ios::binary);
    std::string resumed_content((std::istreambuf_iterator<char>(resumed)), {});
    assert(resumed_content == "hello");
    assert(!fs::exists(resume_part));
    assert(!fs::exists(resume_meta));
    assert(ftp_result_free(&resume_result) == FTP_OK);
    assert(ftp_disconnect(client) == FTP_OK);
    assert(ftp_client_destroy(client) == FTP_OK);
    fs::remove_all(root, error);
    fs::remove_all(resume_root, error);
    std::cout << "M9 directory download and RETR resume integration passed" << std::endl;
    return 0;
}
