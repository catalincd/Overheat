#pragma once
#include "types.h"
#include "editor/buffer.h"
#include <tree_sitter/api.h>
#include <string>
#include <vector>
#include <future>

namespace oh {

class TextBuffer;

class SyntaxHighlighter {
public:
    SyntaxHighlighter();
    ~SyntaxHighlighter();

    void set_language(const std::string& lang_id);
    void update(const TextBuffer& buffer);
    // Full parse on a worker thread with its own parser. Until poll() adopts the
    // result, the buffer keeps logging edits; the caller replays them afterwards.
    void parse_async(std::string text);
    bool parsing() const { return pending_.valid(); }
    bool poll();
    // Replays buffer edits into the tree, then reparses reusing unchanged subtrees.
    void apply_changes(const TextBuffer& buffer, const std::vector<TextBuffer::Change>& changes);

    // Tokens of one line, in document order and non-overlapping. `buffer` is the text
    // the tree was parsed from (some languages look at identifier spelling).
    std::vector<Token> tokens_for_line(const TextBuffer& buffer, size_t line_start_byte,
                                       size_t line_length) const;
    std::string language_id() const { return lang_id_; }

    // Enclosing named scopes at a byte offset, outermost first (for breadcrumbs).
    struct Scope {
        enum Kind { Namespace, Class, Function } kind;
        std::string name;
    };
    std::vector<Scope> scope_at(const TextBuffer& buffer, size_t offset) const;

    static std::string detect_language(const fs::path& path);
    // True when a tree-sitter grammar is built in for this language id.
    static bool has_grammar(const std::string& lang_id);
    // Current tree (null without a grammar or before the first parse lands).
    const TSTree* tree() const { return tree_; }

    struct Grammar;  // one entry of the language table in syntax.cpp

private:
    TSParser* parser_ = nullptr;
    TSTree* tree_ = nullptr;
    std::future<TSTree*> pending_;
    void discard_pending();
    std::string lang_id_;

    const Grammar* grammar_ = nullptr;
};

} // namespace oh
