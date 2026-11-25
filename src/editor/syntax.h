#pragma once
#include "types.h"
#include "editor/buffer.h"
#include <tree_sitter/api.h>
#include <string>
#include <vector>
#include <future>
#include <unordered_map>

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

    std::vector<Token> tokens_for_line(size_t line, size_t line_start_byte, size_t line_length) const;
    std::string language_id() const { return lang_id_; }

    // Enclosing named scopes at a byte offset, outermost first (for breadcrumbs).
    struct Scope {
        enum Kind { Namespace, Class, Function } kind;
        std::string name;
    };
    std::vector<Scope> scope_at(const TextBuffer& buffer, size_t offset) const;

    static std::string detect_language(const fs::path& path);

private:
    TSParser* parser_ = nullptr;
    TSTree* tree_ = nullptr;
    std::future<TSTree*> pending_;
    void discard_pending();
    std::string lang_id_;

    struct NodeTypeMap {
        std::unordered_map<std::string, TokenType> map;
    };
    NodeTypeMap type_map_;

    void build_type_map();
    TokenType classify_node(TSNode node) const;
};

} // namespace oh
