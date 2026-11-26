#pragma once
#include "types.h"
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

namespace oh {

struct SSHConfig {
    std::string host;
    int port = 22;
    std::string user;
    std::string password;
    std::string key_path;                  // legacy single key; prefer identity_files
    std::vector<std::string> identity_files;
    std::string key_passphrase;
    std::string known_hosts;               // OpenSSH known_hosts file to verify against
    bool trust_new_host = false;           // add an unknown host key (after user consent)
    // If set, the host key's "SHA256:..." fingerprint must equal this and known_hosts
    // is not consulted (a second connection to a host the user already verified).
    std::string pinned_fingerprint;
    int timeout_ms = 20000;                // libssh2 blocking-call timeout
};

enum class SSHConnectStatus { Ok, NetworkError, HostUnknown, HostChanged, AuthFailed };

class SSHSession {
public:
    SSHSession();
    ~SSHSession();

    // Blocking; run off the UI thread. On HostUnknown, host_fingerprint() holds the
    // SHA256 fingerprint to show the user before retrying with trust_new_host.
    SSHConnectStatus connect(const SSHConfig& config);
    const std::string& last_error() const { return last_error_; }
    const std::string& host_fingerprint() const { return host_fingerprint_; }
    bool needs_passphrase() const { return needs_passphrase_; }
    // Remote absolute path of the login directory (SFTP realpath of ".").
    std::string home_dir();
    void disconnect();
    bool is_connected() const { return connected_; }

    // Remote file operations
    bool read_file(const std::string& remote_path, std::string& out);
    bool write_file(const std::string& remote_path, const std::string& content);
    std::vector<FileEntry> list_dir(const std::string& remote_path);
    bool file_exists(const std::string& remote_path);

    // Remote command execution
    struct ExecResult {
        std::string stdout_data;
        std::string stderr_data;
        int exit_code;
    };
    ExecResult exec(const std::string& command);

    // Port forwarding for LSP
    bool forward_port(int local_port, const std::string& remote_host, int remote_port);

    // Interactive shell channel
    using OutputCallback = std::function<void(const char*, size_t)>;
    bool open_shell(OutputCallback on_output);
    void write_shell(const std::string& data);
    void close_shell();

    const SSHConfig& config() const { return config_; }

    static void global_init();
    static void global_shutdown();

private:
    LIBSSH2_SESSION* session_ = nullptr;
    LIBSSH2_SFTP* sftp_ = nullptr;  // one SFTP channel for the connection
    LIBSSH2_SFTP* sftp();
    LIBSSH2_CHANNEL* shell_channel_ = nullptr;
    libssh2_socket_t sock_ = LIBSSH2_INVALID_SOCKET;
    bool connected_ = false;
    SSHConfig config_;

    std::thread shell_reader_;
    std::atomic<bool> shell_alive_{false};
    OutputCallback shell_callback_;

    bool authenticate();
    SSHConnectStatus verify_host_key();
    void teardown();
    std::string last_error_;
    std::string host_fingerprint_;
    bool needs_passphrase_ = false;
};

} // namespace oh
