#pragma once
#include "types.h"
#include <tree_sitter/api.h>
#include <string>
#include <vector>
#include <unordered_map>

namespace oh {

class TextBuffer;

class SyntaxHighlighter {
public:
    SyntaxHighlighter();
    ~SyntaxHighlighter();

    void set_language(const std::string& lang_id);
    void update(const TextBuffer& buffer);
    void update_incremental(const TextBuffer& buffer, size_t start_byte, size_t old_end, size_t new_end);

    std::vector<Token> tokens_for_line(size_t line, size_t line_start_byte, size_t line_length) const;
    std::string language_id() const { return lang_id_; }

    static std::string detect_language(const fs::path& path);

private:
    TSParser* parser_ = nullptr;
    TSTree* tree_ = nullptr;
    std::string lang_id_;

    struct NodeTypeMap {
        std::unordered_map<std::string, TokenType> map;
    };
    NodeTypeMap type_map_;

    void build_type_map();
    TokenType classify_node(TSNode node) const;
};

} // namespace oh
