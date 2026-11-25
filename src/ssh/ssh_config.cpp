#include "ssh/ssh_config.h"
#include "platform/platform.h"
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace oh {

namespace {
std::string home() { return platform::home_dir().string(); }

// OpenSSH Host patterns: '*' and '?' wildcards.
bool glob_match(const char* p, const char* s) {
    if (*p == '\0') return *s == '\0';
    if (*p == '*') return glob_match(p + 1, s) || (*s && glob_match(p, s + 1));
    if (*s && (*p == '?' || *p == *s)) return glob_match(p + 1, s + 1);
    return false;
}
std::string expand_tilde(const std::string& p) {
    if (p.rfind("~/", 0) == 0) return home() + p.substr(1);
    return p;
}
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string local_user() { return platform::user_name(); }
} // namespace

fs::path SshConfig::default_path() {
    if (std::string p = platform::getenv_str("OVERHEAT_SSH_CONFIG"); !p.empty()) return p;
    return fs::path(home()) / ".ssh" / "config";
}

void SshConfig::load(const fs::path& path) {
    path_ = path;
    blocks_.clear();
    std::ifstream in(path);
    Block* cur = nullptr;
    for (std::string line; std::getline(in, line);) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream ss(line);
        std::string key;
        if (!(ss >> key)) continue;
        std::string rest;
        std::getline(ss, rest);
        auto eq = rest.find_first_not_of(" \t=");
        rest = eq == std::string::npos ? "" : rest.substr(eq);
        while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t' || rest.back() == '\r')) rest.pop_back();
        if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') rest = rest.substr(1, rest.size() - 2);
        key = lower(key);
        if (key == "host") {
            blocks_.push_back({});
            cur = &blocks_.back();
            std::istringstream ps(rest);
            for (std::string p; ps >> p;) cur->patterns.push_back(p);
        } else if (key == "match") {
            cur = nullptr;  // Match blocks are not supported; ignore their options
        } else if (cur) {
            cur->options.push_back({key, rest});
        } else if (blocks_.empty()) {
            blocks_.push_back({{"*"}, {}});
            cur = &blocks_.back();
            cur->options.push_back({key, rest});
        }
    }
}

std::vector<std::string> SshConfig::hosts() const {
    std::vector<std::string> out;
    for (auto& b : blocks_)
        for (auto& p : b.patterns)
            if (p.find_first_of("*?!") == std::string::npos) out.push_back(p);
    return out;
}

SshTarget SshConfig::resolve(const std::string& target) const {
    SshTarget t;
    t.config_file = path_;
    std::string host = target;
    std::string user;
    if (auto at = host.find('@'); at != std::string::npos) {
        user = host.substr(0, at);
        host = host.substr(at + 1);
    }
    int port = 0;
    if (auto colon = host.rfind(':'); colon != std::string::npos && host.find(']') == std::string::npos) {
        port = std::atoi(host.c_str() + colon + 1);
        host = host.substr(0, colon);
    }
    t.alias = host;
    // First value wins, as in OpenSSH.
    bool have_host = false, have_user = false, have_port = false, have_kh = false;
    for (auto& b : blocks_) {
        bool match = false;
        for (auto& p : b.patterns) {
            bool neg = !p.empty() && p[0] == '!';
            if (glob_match((neg ? p.substr(1) : p).c_str(), host.c_str())) {
                if (neg) { match = false; break; }
                match = true;
            }
        }
        if (!match) continue;
        for (auto& p : b.patterns)
            if (p == host) t.from_config = true;
        for (auto& [k, v] : b.options) {
            if (k == "hostname" && !have_host) { t.hostname = v; have_host = true; }
            else if (k == "user" && !have_user) { t.user = v; have_user = true; }
            else if (k == "port" && !have_port) { t.port = std::atoi(v.c_str()); have_port = true; }
            else if (k == "identityfile") t.identity_files.push_back(expand_tilde(v));
            else if (k == "userknownhostsfile" && !have_kh) { t.known_hosts = expand_tilde(v); have_kh = true; }
            else if (k == "identitiesonly") t.identities_only = lower(v) == "yes";
        }
    }
    if (t.hostname.empty()) t.hostname = host;
    if (!user.empty()) t.user = user;
    if (t.user.empty()) t.user = local_user();
    if (port) t.port = port;
    if (t.known_hosts.empty()) t.known_hosts = home() + "/.ssh/known_hosts";
    if (!t.identities_only || t.identity_files.empty())
        for (const char* k : {"id_ed25519", "id_ecdsa", "id_rsa"})
            t.identity_files.push_back(home() + "/.ssh/" + k);
    return t;
}

} // namespace oh
