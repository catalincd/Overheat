#pragma once
#include "types.h"
#include <git2.h>
#include <string>
#include <optional>
#include <vector>

namespace oh {

struct GitStatus {
    fs::path path;          // relative to the work tree
    FileStatus status;      // combined (index wins), for the file tree
    FileStatus index_status = FileStatus::Unmodified;  // staged change
    FileStatus wt_status = FileStatus::Unmodified;     // unstaged change
};

// Gutter change marker for lines of the current text versus HEAD.
struct GutterMark {
    enum Kind : uint8_t { Added, Modified, Deleted } kind;
    size_t line;   // 0-based first line (for Deleted: the line below the removed text)
    size_t count;  // lines covered (0 for Deleted)
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
    fs::path workdir() const;
    const std::string& last_error() const { return last_error_; }
    // File content at HEAD, or nullopt if untracked/new.
    std::optional<std::string> head_content(const fs::path& path) const;
    // Commits ahead/behind the upstream of the current branch. False if no upstream.
    bool ahead_behind(int& ahead, int& behind) const;
    static std::vector<GutterMark> diff_marks(const std::string& base, const std::string& current);
    // Status computed with a private repository handle, safe on a worker thread.
    static std::vector<GitStatus> status_in(const fs::path& workdir);
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
    mutable std::string last_error_;
    void set_error(const char* what) const;
};

} // namespace oh
