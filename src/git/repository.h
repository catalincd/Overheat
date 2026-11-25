#pragma once
#include "types.h"
#include <git2.h>
#include <string>
#include <vector>

namespace oh {

struct GitStatus {
    fs::path path;
    FileStatus status;
};

struct GitBlame {
    size_t line;
    std::string author;
    std::string date;
    std::string oid_short;
    std::string summary;
};

class GitRepository {
public:
    GitRepository();
    ~GitRepository();

    bool open(const fs::path& path);
    void close();
    bool is_open() const { return repo_ != nullptr; }
    // True if .gitignore rules (and info/exclude) exclude this path.
    bool is_ignored(const fs::path& path) const;

    std::string branch_name() const;
    std::vector<GitStatus> status() const;
    FileStatus file_status(const fs::path& path) const;
    std::vector<std::string> diff_lines(const fs::path& path) const;

    // Line-level diff info for gutter
    enum class LineDiff { None, Added, Modified, Deleted };
    std::vector<LineDiff> line_diffs(const fs::path& path) const;

    std::vector<GitBlame> blame(const fs::path& path) const;
    std::vector<std::string> branches() const;
    bool checkout(const std::string& branch);
    bool commit(const std::string& message);
    bool stage(const fs::path& path);
    bool unstage(const fs::path& path);

    static void global_init();
    static void global_shutdown();

private:
    git_repository* repo_ = nullptr;
};

} // namespace oh
