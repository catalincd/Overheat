#include "vfs.h"
#include <fstream>
#include <iterator>

namespace oh {

bool LocalVfs::list(const fs::path& dir, std::vector<DirEntry>& out) {
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) return false;
    for (auto& de : it) out.push_back({de.path().filename().string(), de.is_directory(ec)});
    return true;
}

bool LocalVfs::read(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

bool LocalVfs::read_head(const fs::path& path, size_t max, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.resize(max);
    in.read(out.data(), static_cast<std::streamsize>(max));
    out.resize(static_cast<size_t>(in.gcount()));
    return !in.bad();
}

bool LocalVfs::file_size(const fs::path& path, uint64_t& out) {
    std::error_code ec;
    auto n = fs::file_size(path, ec);
    if (ec) return false;
    out = n;
    return true;
}

bool LocalVfs::write(const fs::path& path, const std::string& data) {
    // Write to a temp file and rename, so a failed save never truncates the original.
    fs::path tmp = path;
    tmp += ".overheat-tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
    }
    std::error_code ec;
    std::error_code perm_ec;
    auto perms = fs::status(path, perm_ec).permissions();
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    if (!perm_ec) fs::permissions(path, perms, perm_ec);
    return true;
}

} // namespace oh
