#include "git/repository.h"
#include <git2.h>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace oh {

// ---------------------------------------------------------------------------
// Static init / shutdown
// ---------------------------------------------------------------------------

void GitRepository::global_init() {
    git_libgit2_init();
}

void GitRepository::global_shutdown() {
    git_libgit2_shutdown();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

GitRepository::GitRepository() = default;

GitRepository::~GitRepository() {
    close();
}

bool GitRepository::open(const fs::path& path) {
    close();
    int err = git_repository_open_ext(&repo_, path.c_str(),
                                      GIT_REPOSITORY_OPEN_FROM_ENV, nullptr);
    if (err != 0) {
        repo_ = nullptr;
        return false;
    }
    return true;
}

void GitRepository::close() {
    if (repo_) {
        git_repository_free(repo_);
        repo_ = nullptr;
    }
}

bool GitRepository::is_ignored(const fs::path& path) const {
    if (!repo_) return false;
    const char* workdir = git_repository_workdir(repo_);
    if (!workdir) return false;
    std::string rel = path.lexically_relative(workdir).generic_string();
    if (rel.empty() || rel.rfind("..", 0) == 0) return false;
    int ignored = 0;
    if (git_ignore_path_is_ignored(&ignored, repo_, rel.c_str()) != 0) return false;
    return ignored != 0;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static FileStatus map_status_flags(unsigned int flags) {
    // Index (staged) flags take priority, then workdir flags.
    if (flags & GIT_STATUS_INDEX_NEW)        return FileStatus::Added;
    if (flags & GIT_STATUS_INDEX_MODIFIED)    return FileStatus::Modified;
    if (flags & GIT_STATUS_INDEX_DELETED)     return FileStatus::Deleted;
    if (flags & GIT_STATUS_INDEX_RENAMED)     return FileStatus::Renamed;

    if (flags & GIT_STATUS_WT_NEW)            return FileStatus::Untracked;
    if (flags & GIT_STATUS_WT_MODIFIED)       return FileStatus::Modified;
    if (flags & GIT_STATUS_WT_DELETED)        return FileStatus::Deleted;
    if (flags & GIT_STATUS_WT_RENAMED)        return FileStatus::Renamed;

    if (flags & GIT_STATUS_IGNORED)           return FileStatus::Ignored;

    return FileStatus::Unmodified;
}

// ---------------------------------------------------------------------------
// branch_name
// ---------------------------------------------------------------------------

std::string GitRepository::branch_name() const {
    if (!repo_) return {};

    git_reference* head = nullptr;
    if (git_repository_head(&head, repo_) != 0)
        return {};

    const char* name = git_reference_shorthand(head);
    std::string result = name ? name : "";
    git_reference_free(head);
    return result;
}

// ---------------------------------------------------------------------------
// status (all files)
// ---------------------------------------------------------------------------

std::vector<GitStatus> GitRepository::status() const {
    std::vector<GitStatus> result;
    if (!repo_) return result;

    git_status_options opts = GIT_STATUS_OPTIONS_INIT;
    opts.show  = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                 GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
                 GIT_STATUS_OPT_SORT_CASE_SENSITIVELY;

    git_status_list* list = nullptr;
    if (git_status_list_new(&list, repo_, &opts) != 0)
        return result;

    size_t count = git_status_list_entrycount(list);
    for (size_t i = 0; i < count; ++i) {
        const git_status_entry* entry = git_status_byindex(list, i);
        if (!entry) continue;

        const char* path = nullptr;
        if (entry->head_to_index && entry->head_to_index->new_file.path)
            path = entry->head_to_index->new_file.path;
        else if (entry->index_to_workdir && entry->index_to_workdir->new_file.path)
            path = entry->index_to_workdir->new_file.path;
        if (!path) continue;

        GitStatus gs;
        gs.path   = path;
        gs.status = map_status_flags(entry->status);
        result.push_back(std::move(gs));
    }

    git_status_list_free(list);
    return result;
}

// ---------------------------------------------------------------------------
// file_status (single path)
// ---------------------------------------------------------------------------

FileStatus GitRepository::file_status(const fs::path& path) const {
    if (!repo_) return FileStatus::Unmodified;

    // path must be relative to the workdir
    std::string rel = path.lexically_relative(
        git_repository_workdir(repo_)).string();

    unsigned int flags = 0;
    if (git_status_file(&flags, repo_, rel.c_str()) != 0)
        return FileStatus::Unmodified;

    return map_status_flags(flags);
}

// ---------------------------------------------------------------------------
// diff_lines  (patch text for a single file)
// ---------------------------------------------------------------------------

static int diff_print_cb(const git_diff_delta* /*delta*/,
                          const git_diff_hunk* /*hunk*/,
                          const git_diff_line* line,
                          void* payload) {
    auto* lines = static_cast<std::vector<std::string>*>(payload);
    if (line->content && line->content_len > 0)
        lines->emplace_back(line->content, line->content_len);
    return 0;
}

std::vector<std::string> GitRepository::diff_lines(const fs::path& path) const {
    std::vector<std::string> result;
    if (!repo_) return result;

    std::string rel = path.lexically_relative(
        git_repository_workdir(repo_)).string();

    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    char* pathspec = const_cast<char*>(rel.c_str());
    opts.pathspec.strings = &pathspec;
    opts.pathspec.count   = 1;

    git_diff* diff = nullptr;
    if (git_diff_index_to_workdir(&diff, repo_, nullptr, &opts) != 0)
        return result;

    git_diff_print(diff, GIT_DIFF_FORMAT_PATCH, diff_print_cb, &result);
    git_diff_free(diff);
    return result;
}

// ---------------------------------------------------------------------------
// line_diffs  (per-line diff markers for the gutter)
// ---------------------------------------------------------------------------

struct LineDiffPayload {
    std::vector<GitRepository::LineDiff>* diffs;
    std::string target_path;
    bool in_target_file = false;
    // Track old-side line to detect deletes mapped into new-side position
    int hunk_new_start = 0;
};

static void ensure_size(std::vector<GitRepository::LineDiff>& v, size_t idx) {
    if (idx >= v.size())
        v.resize(idx + 1, GitRepository::LineDiff::None);
}

static int line_diff_file_cb(const git_diff_delta* delta, float /*progress*/,
                              void* payload) {
    auto* p = static_cast<LineDiffPayload*>(payload);
    p->in_target_file = false;
    if (delta && delta->new_file.path) {
        if (p->target_path == delta->new_file.path)
            p->in_target_file = true;
    }
    return 0;
}

static int line_diff_hunk_cb(const git_diff_delta* /*delta*/,
                              const git_diff_hunk* hunk,
                              void* payload) {
    auto* p = static_cast<LineDiffPayload*>(payload);
    if (p->in_target_file)
        p->hunk_new_start = hunk->new_start;
    return 0;
}

static int line_diff_line_cb(const git_diff_delta* /*delta*/,
                              const git_diff_hunk* /*hunk*/,
                              const git_diff_line* line,
                              void* payload) {
    auto* p = static_cast<LineDiffPayload*>(payload);
    if (!p->in_target_file) return 0;

    if (line->origin == GIT_DIFF_LINE_ADDITION) {
        int idx = line->new_lineno - 1;  // 0-based
        if (idx >= 0) {
            ensure_size(*p->diffs, static_cast<size_t>(idx));
            (*p->diffs)[static_cast<size_t>(idx)] = GitRepository::LineDiff::Added;
        }
    } else if (line->origin == GIT_DIFF_LINE_DELETION) {
        // Mark the corresponding position in the new file.
        // Use the hunk's new_start as the anchor for the delete marker.
        int idx = p->hunk_new_start - 1; // 0-based
        if (idx >= 0) {
            ensure_size(*p->diffs, static_cast<size_t>(idx));
            if ((*p->diffs)[static_cast<size_t>(idx)] == GitRepository::LineDiff::None)
                (*p->diffs)[static_cast<size_t>(idx)] = GitRepository::LineDiff::Deleted;
        }
    } else if (line->origin == GIT_DIFF_LINE_CONTEXT) {
        // No action for context lines
    }
    return 0;
}

std::vector<GitRepository::LineDiff> GitRepository::line_diffs(const fs::path& path) const {
    std::vector<LineDiff> result;
    if (!repo_) return result;

    std::string rel = path.lexically_relative(
        git_repository_workdir(repo_)).string();

    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    char* pathspec = const_cast<char*>(rel.c_str());
    opts.pathspec.strings = &pathspec;
    opts.pathspec.count   = 1;

    git_diff* diff = nullptr;
    if (git_diff_index_to_workdir(&diff, repo_, nullptr, &opts) != 0)
        return result;

    LineDiffPayload payload;
    payload.diffs       = &result;
    payload.target_path = rel;

    git_diff_foreach(diff, line_diff_file_cb, nullptr, line_diff_hunk_cb,
                     line_diff_line_cb, &payload);

    git_diff_free(diff);
    return result;
}

// ---------------------------------------------------------------------------
// blame
// ---------------------------------------------------------------------------

std::vector<GitBlame> GitRepository::blame(const fs::path& path) const {
    std::vector<GitBlame> result;
    if (!repo_) return result;

    std::string rel = path.lexically_relative(
        git_repository_workdir(repo_)).string();

    git_blame_options opts = GIT_BLAME_OPTIONS_INIT;
    git_blame* blame_obj = nullptr;
    if (git_blame_file(&blame_obj, repo_, rel.c_str(), &opts) != 0)
        return result;

    uint32_t hunk_count = git_blame_get_hunk_count(blame_obj);
    for (uint32_t i = 0; i < hunk_count; ++i) {
        const git_blame_hunk* hunk = git_blame_get_hunk_byindex(blame_obj, i);
        if (!hunk) continue;

        // Format the OID
        char oid_buf[GIT_OID_SHA1_HEXSIZE + 1];
        git_oid_tostr(oid_buf, 8, &hunk->final_commit_id); // short hash

        // Format the date
        std::string date_str;
        if (hunk->final_signature) {
            time_t time = static_cast<time_t>(hunk->final_signature->when.time);
            struct tm tm_buf;
            gmtime_r(&time, &tm_buf);
            char date_buf[64];
            strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm_buf);
            date_str = date_buf;
        }

        // Get commit summary
        std::string summary;
        git_commit* commit = nullptr;
        if (git_commit_lookup(&commit, repo_, &hunk->final_commit_id) == 0) {
            const char* s = git_commit_summary(commit);
            if (s) summary = s;
            git_commit_free(commit);
        }

        // One entry per line in the hunk
        for (size_t line = hunk->final_start_line_number;
             line < hunk->final_start_line_number + hunk->lines_in_hunk;
             ++line) {
            GitBlame gb;
            gb.line      = line;
            gb.author    = (hunk->final_signature && hunk->final_signature->name)
                               ? hunk->final_signature->name : "";
            gb.date      = date_str;
            gb.oid_short = oid_buf;
            gb.summary   = summary;
            result.push_back(std::move(gb));
        }
    }

    git_blame_free(blame_obj);
    return result;
}

// ---------------------------------------------------------------------------
// branches
// ---------------------------------------------------------------------------

std::vector<std::string> GitRepository::branches() const {
    std::vector<std::string> result;
    if (!repo_) return result;

    git_branch_iterator* iter = nullptr;
    if (git_branch_iterator_new(&iter, repo_, GIT_BRANCH_LOCAL) != 0)
        return result;

    git_reference* ref = nullptr;
    git_branch_t type;
    while (git_branch_next(&ref, &type, iter) == 0) {
        const char* name = nullptr;
        if (git_branch_name(&name, ref) == 0 && name)
            result.emplace_back(name);
        git_reference_free(ref);
    }

    git_branch_iterator_free(iter);
    return result;
}

// ---------------------------------------------------------------------------
// checkout
// ---------------------------------------------------------------------------

bool GitRepository::checkout(const std::string& branch) {
    if (!repo_) return false;

    // Resolve the branch to a commit
    std::string ref_name = "refs/heads/" + branch;

    git_object* target = nullptr;
    if (git_revparse_single(&target, repo_, ref_name.c_str()) != 0)
        return false;

    git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
    opts.checkout_strategy = GIT_CHECKOUT_SAFE;

    if (git_checkout_tree(repo_, target, &opts) != 0) {
        git_object_free(target);
        return false;
    }

    if (git_repository_set_head(repo_, ref_name.c_str()) != 0) {
        git_object_free(target);
        return false;
    }

    git_object_free(target);
    return true;
}

// ---------------------------------------------------------------------------
// commit
// ---------------------------------------------------------------------------

bool GitRepository::commit(const std::string& message) {
    if (!repo_) return false;

    git_index* index = nullptr;
    if (git_repository_index(&index, repo_) != 0)
        return false;

    git_oid tree_oid;
    if (git_index_write_tree(&tree_oid, index) != 0) {
        git_index_free(index);
        return false;
    }

    git_tree* tree = nullptr;
    if (git_tree_lookup(&tree, repo_, &tree_oid) != 0) {
        git_index_free(index);
        return false;
    }

    git_signature* sig = nullptr;
    if (git_signature_default(&sig, repo_) != 0) {
        // Fallback: try a generic signature
        if (git_signature_now(&sig, "User", "user@example.com") != 0) {
            git_tree_free(tree);
            git_index_free(index);
            return false;
        }
    }

    // Get HEAD as parent (may not exist for initial commit)
    git_commit* parent = nullptr;
    git_reference* head_ref = nullptr;
    int parent_count = 0;
    if (git_repository_head(&head_ref, repo_) == 0) {
        const git_oid* head_oid = git_reference_target(head_ref);
        if (head_oid && git_commit_lookup(&parent, repo_, head_oid) == 0)
            parent_count = 1;
        git_reference_free(head_ref);
    }

    git_oid commit_oid;
    int err;
    if (parent_count > 0) {
        err = git_commit_create_v(&commit_oid, repo_, "HEAD", sig, sig,
                                  nullptr, message.c_str(), tree,
                                  1, parent);
    } else {
        err = git_commit_create_v(&commit_oid, repo_, "HEAD", sig, sig,
                                  nullptr, message.c_str(), tree,
                                  0);
    }

    if (parent) git_commit_free(parent);
    git_signature_free(sig);
    git_tree_free(tree);
    git_index_free(index);

    return err == 0;
}

// ---------------------------------------------------------------------------
// stage
// ---------------------------------------------------------------------------

bool GitRepository::stage(const fs::path& path) {
    if (!repo_) return false;

    std::string rel = path.lexically_relative(
        git_repository_workdir(repo_)).string();

    git_index* index = nullptr;
    if (git_repository_index(&index, repo_) != 0)
        return false;

    if (git_index_add_bypath(index, rel.c_str()) != 0) {
        git_index_free(index);
        return false;
    }

    if (git_index_write(index) != 0) {
        git_index_free(index);
        return false;
    }

    git_index_free(index);
    return true;
}

// ---------------------------------------------------------------------------
// unstage
// ---------------------------------------------------------------------------

bool GitRepository::unstage(const fs::path& path) {
    if (!repo_) return false;

    std::string rel = path.lexically_relative(
        git_repository_workdir(repo_)).string();

    // Get HEAD commit
    git_reference* head_ref = nullptr;
    git_object* head_obj = nullptr;

    int has_head = (git_repository_head(&head_ref, repo_) == 0);
    if (has_head) {
        git_reference_peel(&head_obj, head_ref, GIT_OBJECT_COMMIT);
        git_reference_free(head_ref);
    }

    // Use git_reset_default to unstage the path
    char* pathspec = const_cast<char*>(rel.c_str());
    git_strarray paths;
    paths.strings = &pathspec;
    paths.count   = 1;

    int err = git_reset_default(repo_, head_obj, &paths);

    if (head_obj) git_object_free(head_obj);

    return err == 0;
}

} // namespace oh
