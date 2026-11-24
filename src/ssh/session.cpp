#include "ssh/session.h"
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <thread>

namespace oh {

// ---------------------------------------------------------------------------
// Static global init / shutdown
// ---------------------------------------------------------------------------

void SSHSession::global_init() {
    libssh2_init(0);
}

void SSHSession::global_shutdown() {
    libssh2_exit();
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

SSHSession::SSHSession() = default;

SSHSession::~SSHSession() {
    disconnect();
}

// ---------------------------------------------------------------------------
// connect / disconnect
// ---------------------------------------------------------------------------

bool SSHSession::connect(const SSHConfig& config) {
    config_ = config;

    // Resolve host
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(config_.port);

    struct addrinfo* res = nullptr;
    if (getaddrinfo(config_.host.c_str(), port_str.c_str(), &hints, &res) != 0) {
        return false;
    }

    // Create socket and connect
    sock_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock_ < 0) {
        freeaddrinfo(res);
        return false;
    }

    if (::connect(sock_, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        ::close(sock_);
        sock_ = -1;
        return false;
    }
    freeaddrinfo(res);

    // Init libssh2 session
    session_ = libssh2_session_init();
    if (!session_) {
        ::close(sock_);
        sock_ = -1;
        return false;
    }

    // Blocking mode for setup
    libssh2_session_set_blocking(session_, 1);

    if (libssh2_session_handshake(session_, sock_) != 0) {
        libssh2_session_free(session_);
        session_ = nullptr;
        ::close(sock_);
        sock_ = -1;
        return false;
    }

    if (!authenticate()) {
        libssh2_session_disconnect(session_, "auth failed");
        libssh2_session_free(session_);
        session_ = nullptr;
        ::close(sock_);
        sock_ = -1;
        return false;
    }

    connected_ = true;
    return true;
}

bool SSHSession::authenticate() {
    // Determine private key path
    std::string priv_key = config_.key_path;
    if (priv_key.empty()) {
        const char* home = getenv("HOME");
        if (home) {
            priv_key = std::string(home) + "/.ssh/id_rsa";
        }
    }

    // Try public-key authentication first
    if (!priv_key.empty()) {
        std::string pub_key = priv_key + ".pub";
        const char* passphrase = config_.key_passphrase.empty()
                                     ? nullptr
                                     : config_.key_passphrase.c_str();
        int rc = libssh2_userauth_publickey_fromfile(
            session_, config_.user.c_str(),
            pub_key.c_str(), priv_key.c_str(), passphrase);
        if (rc == 0) {
            return true;
        }
    }

    // Fall back to password authentication
    if (!config_.password.empty()) {
        int rc = libssh2_userauth_password(
            session_, config_.user.c_str(), config_.password.c_str());
        if (rc == 0) {
            return true;
        }
    }

    return false;
}

void SSHSession::disconnect() {
    close_shell();

    if (session_) {
        libssh2_session_disconnect(session_, "bye");
        libssh2_session_free(session_);
        session_ = nullptr;
    }

    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }

    connected_ = false;
}

// ---------------------------------------------------------------------------
// SFTP: read_file
// ---------------------------------------------------------------------------

std::string SSHSession::read_file(const std::string& remote_path) {
    if (!session_) return {};

    LIBSSH2_SFTP* sftp = libssh2_sftp_init(session_);
    if (!sftp) return {};

    LIBSSH2_SFTP_HANDLE* handle = libssh2_sftp_open(
        sftp, remote_path.c_str(), LIBSSH2_FXF_READ, LIBSSH2_SFTP_S_IRUSR);
    if (!handle) {
        libssh2_sftp_shutdown(sftp);
        return {};
    }

    std::string result;
    char buf[8192];
    for (;;) {
        ssize_t n = libssh2_sftp_read(handle, buf, sizeof(buf));
        if (n > 0) {
            result.append(buf, static_cast<size_t>(n));
        } else {
            break;
        }
    }

    libssh2_sftp_close(handle);
    libssh2_sftp_shutdown(sftp);
    return result;
}

// ---------------------------------------------------------------------------
// SFTP: write_file
// ---------------------------------------------------------------------------

bool SSHSession::write_file(const std::string& remote_path,
                            const std::string& content) {
    if (!session_) return false;

    LIBSSH2_SFTP* sftp = libssh2_sftp_init(session_);
    if (!sftp) return false;

    LIBSSH2_SFTP_HANDLE* handle = libssh2_sftp_open(
        sftp, remote_path.c_str(),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
        LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (!handle) {
        libssh2_sftp_shutdown(sftp);
        return false;
    }

    const char* ptr = content.data();
    size_t remaining = content.size();
    while (remaining > 0) {
        ssize_t n = libssh2_sftp_write(handle, ptr, remaining);
        if (n < 0) {
            libssh2_sftp_close(handle);
            libssh2_sftp_shutdown(sftp);
            return false;
        }
        ptr += n;
        remaining -= static_cast<size_t>(n);
    }

    libssh2_sftp_close(handle);
    libssh2_sftp_shutdown(sftp);
    return true;
}

// ---------------------------------------------------------------------------
// SFTP: list_dir
// ---------------------------------------------------------------------------

std::vector<FileEntry> SSHSession::list_dir(const std::string& remote_path) {
    std::vector<FileEntry> entries;
    if (!session_) return entries;

    LIBSSH2_SFTP* sftp = libssh2_sftp_init(session_);
    if (!sftp) return entries;

    LIBSSH2_SFTP_HANDLE* dir = libssh2_sftp_opendir(sftp, remote_path.c_str());
    if (!dir) {
        libssh2_sftp_shutdown(sftp);
        return entries;
    }

    char name_buf[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    while (libssh2_sftp_readdir(dir, name_buf, sizeof(name_buf), &attrs) > 0) {
        std::string name(name_buf);
        if (name == "." || name == "..") continue;

        FileEntry entry;
        entry.name = name;
        entry.path = fs::path(remote_path) / name;
        entry.is_dir = false;
        if (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
            entry.is_dir = S_ISDIR(attrs.permissions);
        }
        entry.depth = 0;
        entry.git_status = FileStatus::Unmodified;
        entries.push_back(std::move(entry));
    }

    libssh2_sftp_closedir(dir);
    libssh2_sftp_shutdown(sftp);
    return entries;
}

// ---------------------------------------------------------------------------
// SFTP: file_exists
// ---------------------------------------------------------------------------

bool SSHSession::file_exists(const std::string& remote_path) {
    if (!session_) return false;

    LIBSSH2_SFTP* sftp = libssh2_sftp_init(session_);
    if (!sftp) return false;

    LIBSSH2_SFTP_ATTRIBUTES attrs;
    int rc = libssh2_sftp_stat(sftp, remote_path.c_str(), &attrs);
    libssh2_sftp_shutdown(sftp);
    return rc == 0;
}

// ---------------------------------------------------------------------------
// exec
// ---------------------------------------------------------------------------

SSHSession::ExecResult SSHSession::exec(const std::string& command) {
    ExecResult result{{}, {}, -1};
    if (!session_) return result;

    LIBSSH2_CHANNEL* channel = libssh2_channel_open_session(session_);
    if (!channel) return result;

    if (libssh2_channel_exec(channel, command.c_str()) != 0) {
        libssh2_channel_free(channel);
        return result;
    }

    // Read stdout
    char buf[8192];
    for (;;) {
        ssize_t n = libssh2_channel_read(channel, buf, sizeof(buf));
        if (n > 0) {
            result.stdout_data.append(buf, static_cast<size_t>(n));
        } else {
            break;
        }
    }

    // Read stderr
    for (;;) {
        ssize_t n = libssh2_channel_read_stderr(channel, buf, sizeof(buf));
        if (n > 0) {
            result.stderr_data.append(buf, static_cast<size_t>(n));
        } else {
            break;
        }
    }

    libssh2_channel_close(channel);
    libssh2_channel_wait_closed(channel);
    result.exit_code = libssh2_channel_get_exit_status(channel);
    libssh2_channel_free(channel);
    return result;
}

// ---------------------------------------------------------------------------
// forward_port (MVP: open direct-tcpip channel, verify connectivity)
// ---------------------------------------------------------------------------

bool SSHSession::forward_port(int /*local_port*/,
                              const std::string& remote_host,
                              int remote_port) {
    if (!session_) return false;

    LIBSSH2_CHANNEL* channel = libssh2_channel_direct_tcpip(
        session_, remote_host.c_str(), remote_port);
    if (!channel) return false;

    // MVP: we verified the tunnel can be opened. A full implementation would
    // bind a local socket and relay traffic through this channel.
    libssh2_channel_close(channel);
    libssh2_channel_free(channel);
    return true;
}

// ---------------------------------------------------------------------------
// Interactive shell
// ---------------------------------------------------------------------------

bool SSHSession::open_shell(OutputCallback on_output) {
    if (!session_ || shell_channel_) return false;

    LIBSSH2_CHANNEL* channel = libssh2_channel_open_session(session_);
    if (!channel) return false;

    if (libssh2_channel_request_pty(channel, "xterm") != 0) {
        libssh2_channel_free(channel);
        return false;
    }

    if (libssh2_channel_shell(channel) != 0) {
        libssh2_channel_free(channel);
        return false;
    }

    shell_channel_ = channel;
    shell_callback_ = std::move(on_output);
    shell_alive_ = true;

    // Switch to non-blocking for the reader thread
    libssh2_session_set_blocking(session_, 0);

    shell_reader_ = std::thread([this]() {
        char buf[4096];
        while (shell_alive_) {
            ssize_t n = libssh2_channel_read(shell_channel_, buf, sizeof(buf));
            if (n > 0) {
                if (shell_callback_) {
                    shell_callback_(buf, static_cast<size_t>(n));
                }
            } else if (n == LIBSSH2_ERROR_EAGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else {
                // Channel closed or error
                break;
            }
        }
    });

    return true;
}

void SSHSession::write_shell(const std::string& data) {
    if (!shell_channel_) return;

    const char* ptr = data.data();
    size_t remaining = data.size();
    while (remaining > 0) {
        ssize_t n = libssh2_channel_write(shell_channel_, ptr, remaining);
        if (n > 0) {
            ptr += n;
            remaining -= static_cast<size_t>(n);
        } else if (n == LIBSSH2_ERROR_EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } else {
            break; // error
        }
    }
}

void SSHSession::close_shell() {
    shell_alive_ = false;

    if (shell_reader_.joinable()) {
        shell_reader_.join();
    }

    if (session_) {
        libssh2_session_set_blocking(session_, 1);
    }

    if (shell_channel_) {
        libssh2_channel_close(shell_channel_);
        libssh2_channel_free(shell_channel_);
        shell_channel_ = nullptr;
    }

    shell_callback_ = nullptr;
}

} // namespace oh
