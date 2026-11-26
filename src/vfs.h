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
    // The first `max` bytes (fewer for a shorter file), e.g. to sniff binaries.
    virtual bool read_head(const fs::path& path, size_t max, std::string& out) = 0;
    // Size in bytes without reading the file; false if it cannot be stat'ed.
    virtual bool file_size(const fs::path& path, uint64_t& out) = 0;
    virtual bool write(const fs::path& path, const std::string& data) = 0;
    virtual bool is_remote() const { return false; }
};

class LocalVfs : public Vfs {
public:
    bool list(const fs::path& dir, std::vector<DirEntry>& out) override;
    bool read(const fs::path& path, std::string& out) override;
    bool read_head(const fs::path& path, size_t max, std::string& out) override;
    bool file_size(const fs::path& path, uint64_t& out) override;
    bool write(const fs::path& path, const std::string& data) override;
};

} // namespace oh
