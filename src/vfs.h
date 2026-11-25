#pragma once
#include "types.h"
#include <string>
#include <vector>

namespace oh {

// File access used by the UI, so the explorer and editors work the same on the
// local disk and on a remote host (SFTP).
class Vfs {
public:
    struct DirEntry {
        std::string name;
        bool is_dir = false;
    };
    virtual ~Vfs() = default;
    virtual bool list(const fs::path& dir, std::vector<DirEntry>& out) = 0;
    virtual bool read(const fs::path& path, std::string& out) = 0;
    virtual bool write(const fs::path& path, const std::string& data) = 0;
    virtual bool is_remote() const { return false; }
};

class LocalVfs : public Vfs {
public:
    bool list(const fs::path& dir, std::vector<DirEntry>& out) override;
    bool read(const fs::path& path, std::string& out) override;
    bool write(const fs::path& path, const std::string& data) override;
};

} // namespace oh
