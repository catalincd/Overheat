#pragma once
#include "types.h"
#include <string>
#include <vector>

namespace oh {

// The subset of OpenSSH client config Overheat needs to connect the way `ssh` would.
struct SshTarget {
    std::string alias;      // Host entry name, or what the user typed
    std::string hostname;
    std::string user;
    int port = 22;
    std::vector<std::string> identity_files;
    std::string known_hosts;  // UserKnownHostsFile
    bool identities_only = false;
    fs::path config_file;     // to pass `-F` to the system ssh for the terminal
    bool from_config = false; // alias matched a concrete Host entry
    std::string display() const { return user + "@" + hostname + (port != 22 ? ":" + std::to_string(port) : ""); }
};

class SshConfig {
public:
    // $OVERHEAT_SSH_CONFIG if set (tests), otherwise ~/.ssh/config.
    static fs::path default_path();
    void load(const fs::path& path);
    // Concrete Host aliases (no wildcards), in file order.
    std::vector<std::string> hosts() const;
    // Resolve an alias or a "user@host[:port]" string, applying matching blocks.
    SshTarget resolve(const std::string& target) const;

private:
    struct Block {
        std::vector<std::string> patterns;
        std::vector<std::pair<std::string, std::string>> options;
    };
    std::vector<Block> blocks_;
    fs::path path_;
};

} // namespace oh
