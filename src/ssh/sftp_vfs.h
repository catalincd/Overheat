#pragma once
#include "vfs.h"
#include "ssh/session.h"

namespace oh {

class SftpVfs : public Vfs {
public:
    explicit SftpVfs(SSHSession& session) : s_(session) {}
    bool list(const fs::path& dir, std::vector<DirEntry>& out) override {
        if (!s_.is_connected()) return false;
        for (auto& e : s_.list_dir(dir.string())) out.push_back({e.name, e.is_dir});
        return true;
    }
    bool read(const fs::path& path, std::string& out) override { return s_.read_file(path.string(), out); }
    bool write(const fs::path& path, const std::string& data) override { return s_.write_file(path.string(), data); }
    bool is_remote() const override { return true; }

private:
    SSHSession& s_;
};

} // namespace oh
