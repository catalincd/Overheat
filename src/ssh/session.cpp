#include "ssh/session.h"
#include <algorithm>
#include <cerrno>
#include <libssh2.h>
#include <libssh2_sftp.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define OH_CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#define OH_CLOSE_SOCKET ::close
#endif
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

namespace {

std::string base64(const unsigned char* data, size_t n) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16 |
                     (i + 1 < n ? static_cast<uint32_t>(data[i + 1]) << 8 : 0) |
                     (i + 2 < n ? data[i + 2] : 0);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        if (i + 1 < n) out += tbl[(v >> 6) & 63];
        if (i + 2 < n) out += tbl[v & 63];
    }
    return out;  // unpadded, like OpenSSH fingerprints
}

int knownhost_key_type(int hostkey_type) {
    switch (hostkey_type) {
        case LIBSSH2_HOSTKEY_TYPE_RSA: return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
        case LIBSSH2_HOSTKEY_TYPE_DSS: return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
        case LIBSSH2_HOSTKEY_TYPE_ED25519: return LIBSSH2_KNOWNHOST_KEY_ED25519;
        default: return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    }
}

} // namespace

void SSHSession::teardown() {
    if (session_) {
        libssh2_session_disconnect(session_, "bye");
        libssh2_session_free(session_);
        session_ = nullptr;
    }
    if (sock_ != LIBSSH2_INVALID_SOCKET) {
        OH_CLOSE_SOCKET(sock_);
        sock_ = LIBSSH2_INVALID_SOCKET;
    }
    connected_ = false;
}

SSHConnectStatus SSHSession::connect(const SSHConfig& config) {
    config_ = config;
    last_error_.clear();
    host_fingerprint_.clear();
    needs_passphrase_ = false;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string port_str = std::to_string(config_.port);
    if (int rc = getaddrinfo(config_.host.c_str(), port_str.c_str(), &hints, &res); rc != 0) {
        last_error_ = "Cannot resolve " + config_.host + ": " + gai_strerror(rc);
        return SSHConnectStatus::NetworkError;
    }
    int last_errno = 0;
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        sock_ = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock_ == LIBSSH2_INVALID_SOCKET) continue;
#ifdef _WIN32
        DWORD tv = 10000;  // bounded connect/handshake waits (ms)
        setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof tv);
#else
        timeval tv{10, 0};  // bounded connect/handshake waits
        setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
        if (::connect(sock_, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) break;
#ifdef _WIN32
        last_errno = WSAGetLastError();
#else
        last_errno = errno;
#endif
        OH_CLOSE_SOCKET(sock_);
        sock_ = LIBSSH2_INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (sock_ == LIBSSH2_INVALID_SOCKET) {
#ifdef _WIN32
        std::string reason = "error " + std::to_string(last_errno);
#else
        std::string reason = std::strerror(last_errno);
#endif
        last_error_ = "Cannot connect to " + config_.host + ":" + port_str + ": " + reason;
        return SSHConnectStatus::NetworkError;
    }

    session_ = libssh2_session_init();
    if (!session_) {
        teardown();
        last_error_ = "libssh2 session init failed";
        return SSHConnectStatus::NetworkError;
    }
    libssh2_session_set_blocking(session_, 1);
    libssh2_session_set_timeout(session_, config_.timeout_ms);
    if (libssh2_session_handshake(session_, sock_) != 0) {
        char* msg = nullptr;
        libssh2_session_last_error(session_, &msg, nullptr, 0);
        last_error_ = std::string("SSH handshake failed: ") + (msg ? msg : "");
        teardown();
        return SSHConnectStatus::NetworkError;
    }

    SSHConnectStatus host = verify_host_key();
    if (host != SSHConnectStatus::Ok) {
        teardown();
        return host;
    }

    if (!authenticate()) {
        last_error_ = "Authentication failed for " + config_.user + "@" + config_.host +
                      (needs_passphrase_ ? " (key needs a passphrase)" : "");
        teardown();
        return SSHConnectStatus::AuthFailed;
    }
    connected_ = true;
    return SSHConnectStatus::Ok;
}

SSHConnectStatus SSHSession::verify_host_key() {
    size_t key_len = 0;
    int key_type = 0;
    const char* key = libssh2_session_hostkey(session_, &key_len, &key_type);
    if (!key) {
        last_error_ = "Server sent no host key";
        return SSHConnectStatus::NetworkError;
    }
    if (const char* h = libssh2_hostkey_hash(session_, LIBSSH2_HOSTKEY_HASH_SHA256))
        host_fingerprint_ = "SHA256:" + base64(reinterpret_cast<const unsigned char*>(h), 32);
    if (!config_.pinned_fingerprint.empty()) {
        if (host_fingerprint_ == config_.pinned_fingerprint) return SSHConnectStatus::Ok;
        last_error_ = "HOST KEY CHANGED for " + config_.host + " (" + host_fingerprint_ + ")";
        return SSHConnectStatus::HostChanged;
    }

    LIBSSH2_KNOWNHOSTS* kh = libssh2_knownhost_init(session_);
    if (!kh) {
        last_error_ = "known_hosts init failed";
        return SSHConnectStatus::NetworkError;
    }
    if (!config_.known_hosts.empty())
        libssh2_knownhost_readfile(kh, config_.known_hosts.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    int mask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | knownhost_key_type(key_type);
    libssh2_knownhost* found = nullptr;
    int check = libssh2_knownhost_checkp(kh, config_.host.c_str(), config_.port, key, key_len, mask, &found);
    SSHConnectStatus result = SSHConnectStatus::Ok;
    if (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
        last_error_ = "HOST KEY CHANGED for " + config_.host + " (" + host_fingerprint_ +
                      "). Refusing to connect: this could be a man-in-the-middle attack.";
        result = SSHConnectStatus::HostChanged;
    } else if (check != LIBSSH2_KNOWNHOST_CHECK_MATCH) {
        if (!config_.trust_new_host) {
            last_error_ = "Unknown host " + config_.host;
            result = SSHConnectStatus::HostUnknown;
        } else if (!config_.known_hosts.empty()) {
            // Append one line rather than rewriting the file (libssh2 would drop entries it can't parse).
            std::string spec = config_.port == 22 ? config_.host
                                                  : "[" + config_.host + "]:" + std::to_string(config_.port);
            libssh2_knownhost* added = nullptr;
            if (libssh2_knownhost_addc(kh, spec.c_str(), nullptr, key, key_len, nullptr, 0, mask, &added) == 0) {
                char line[8192];
                size_t out_len = 0;
                if (libssh2_knownhost_writeline(kh, added, line, sizeof line, &out_len,
                                                LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0) {
                    std::error_code ec;
                    fs::create_directories(fs::path(config_.known_hosts).parent_path(), ec);
                    if (FILE* f = std::fopen(config_.known_hosts.c_str(), "a")) {
                        std::fwrite(line, 1, out_len, f);
                        std::fclose(f);
                    }
                }
            }
        }
    }
    libssh2_knownhost_free(kh);
    return result;
}

bool SSHSession::authenticate() {
    const char* user = config_.user.c_str();
    char* methods = libssh2_userauth_list(session_, user, static_cast<unsigned>(config_.user.size()));
    if (!methods) return libssh2_userauth_authenticated(session_) != 0;  // "none" auth accepted
    std::string allowed = methods;

    if (allowed.find("publickey") != std::string::npos) {
        // 1. ssh-agent
        if (LIBSSH2_AGENT* agent = libssh2_agent_init(session_)) {
            if (libssh2_agent_connect(agent) == 0 && libssh2_agent_list_identities(agent) == 0) {
                libssh2_agent_publickey* id = nullptr;
                libssh2_agent_publickey* prev = nullptr;
                while (libssh2_agent_get_identity(agent, &id, prev) == 0) {
                    if (libssh2_agent_userauth(agent, user, id) == 0) {
                        libssh2_agent_disconnect(agent);
                        libssh2_agent_free(agent);
                        return true;
                    }
                    prev = id;
                }
            }
            libssh2_agent_disconnect(agent);
            libssh2_agent_free(agent);
        }
        // 2. identity files
        std::vector<std::string> keys = config_.identity_files;
        if (!config_.key_path.empty()) keys.insert(keys.begin(), config_.key_path);
        for (auto& priv : keys) {
            std::error_code ec;
            if (!fs::exists(priv, ec)) continue;
            std::string pub = priv + ".pub";
            const char* pass = config_.key_passphrase.empty() ? nullptr : config_.key_passphrase.c_str();
            int rc = libssh2_userauth_publickey_fromfile(session_, user, fs::exists(pub, ec) ? pub.c_str() : nullptr,
                                                         priv.c_str(), pass);
            if (rc == 0) return true;
            if (rc == LIBSSH2_ERROR_FILE && !pass) needs_passphrase_ = true;
        }
    }
    // 3. password
    if (!config_.password.empty() && allowed.find("password") != std::string::npos)
        if (libssh2_userauth_password(session_, user, config_.password.c_str()) == 0) return true;
    return false;
}

LIBSSH2_SFTP* SSHSession::sftp() {
    if (!sftp_ && session_ && connected_) sftp_ = libssh2_sftp_init(session_);
    return sftp_;
}

std::string SSHSession::home_dir() {
    char buf[4096];
    LIBSSH2_SFTP* s = sftp();
    int n = s ? libssh2_sftp_realpath(s, ".", buf, sizeof buf - 1) : -1;
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : "/";
}

void SSHSession::disconnect() {
    close_shell();
    if (sftp_) {
        libssh2_sftp_shutdown(sftp_);
        sftp_ = nullptr;
    }
    teardown();
}

bool SSHSession::read_file(const std::string& remote_path, std::string& out, size_t max) {
    LIBSSH2_SFTP* s = sftp();
    if (!s) return false;
    LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open(s, remote_path.c_str(), LIBSSH2_FXF_READ, 0);
    if (!h) return false;
    out.clear();
    char buf[32768];
    ssize_t n = 0;
    while (out.size() < max &&
           (n = libssh2_sftp_read(h, buf, std::min(sizeof buf, max - out.size()))) > 0)
        out.append(buf, static_cast<size_t>(n));
    libssh2_sftp_close(h);
    return n >= 0;
}

bool SSHSession::file_size(const std::string& remote_path, uint64_t& out) {
    LIBSSH2_SFTP* s = sftp();
    if (!s) return false;
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    if (libssh2_sftp_stat(s, remote_path.c_str(), &attrs) != 0 || !(attrs.flags & LIBSSH2_SFTP_ATTR_SIZE))
        return false;
    out = attrs.filesize;
    return true;
}

bool SSHSession::write_file(const std::string& remote_path, const std::string& content) {
    LIBSSH2_SFTP* s = sftp();
    if (!s) return false;
    // Keep the original's permissions; default to 0644 for new files.
    long mode = LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH;
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    if (libssh2_sftp_stat(s, remote_path.c_str(), &attrs) == 0 && (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS))
        mode = static_cast<long>(attrs.permissions & 07777);

    // Write a sibling temp file, then rename over the target: a dropped connection
    // mid-save never leaves a truncated file behind.
    fs::path p(remote_path);
    std::string tmp = (p.parent_path() / ("." + p.filename().string() + ".overheat-tmp")).string();
    LIBSSH2_SFTP_HANDLE* h = libssh2_sftp_open(s, tmp.c_str(),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, mode);
    if (!h) return false;
    const char* ptr = content.data();
    size_t remaining = content.size();
    while (remaining > 0) {
        ssize_t n = libssh2_sftp_write(h, ptr, remaining);
        if (n < 0) {
            libssh2_sftp_close(h);
            libssh2_sftp_unlink(s, tmp.c_str());
            return false;
        }
        ptr += n;
        remaining -= static_cast<size_t>(n);
    }
    libssh2_sftp_close(h);
    const long flags = LIBSSH2_SFTP_RENAME_OVERWRITE | LIBSSH2_SFTP_RENAME_ATOMIC | LIBSSH2_SFTP_RENAME_NATIVE;
    if (libssh2_sftp_rename_ex(s, tmp.c_str(), static_cast<unsigned>(tmp.size()), remote_path.c_str(),
                               static_cast<unsigned>(remote_path.size()), flags) == 0)
        return true;
    // SFTP v3 servers (OpenSSH) refuse to rename onto an existing file.
    libssh2_sftp_unlink(s, remote_path.c_str());
    if (libssh2_sftp_rename(s, tmp.c_str(), remote_path.c_str()) == 0) return true;
    libssh2_sftp_unlink(s, tmp.c_str());
    return false;
}

std::vector<FileEntry> SSHSession::list_dir(const std::string& remote_path) {
    std::vector<FileEntry> entries;
    LIBSSH2_SFTP* s = sftp();
    if (!s) return entries;
    LIBSSH2_SFTP_HANDLE* dir = libssh2_sftp_opendir(s, remote_path.c_str());
    if (!dir) return entries;
    char name_buf[1024];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    while (libssh2_sftp_readdir(dir, name_buf, sizeof name_buf, &attrs) > 0) {
        std::string name(name_buf);
        if (name == "." || name == "..") continue;
        FileEntry entry;
        entry.name = name;
        entry.path = fs::path(remote_path) / name;
        entry.is_dir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
        if ((attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) && LIBSSH2_SFTP_S_ISLNK(attrs.permissions)) {
            LIBSSH2_SFTP_ATTRIBUTES target{};  // follow symlinks to decide dir vs file
            if (libssh2_sftp_stat(s, entry.path.generic_string().c_str(), &target) == 0)
                entry.is_dir = LIBSSH2_SFTP_S_ISDIR(target.permissions);
        }
        entries.push_back(std::move(entry));
    }
    libssh2_sftp_closedir(dir);
    return entries;
}

// ---------------------------------------------------------------------------
// SFTP: file_exists
// ---------------------------------------------------------------------------

bool SSHSession::file_exists(const std::string& remote_path) {
    if (!session_) return false;

    LIBSSH2_SFTP* sftp = this->sftp();
    if (!sftp) return false;

    LIBSSH2_SFTP_ATTRIBUTES attrs;
    return libssh2_sftp_stat(sftp, remote_path.c_str(), &attrs) == 0;
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
