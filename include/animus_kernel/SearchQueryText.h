#pragma once

#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace animus::kernel::memory {

// Shared natural-language query tokenization for search tools (#71).
// diary search and memory search must interpret queries identically:
// words are OR-matched with stop-word removal for broad recall.

// Split on whitespace and non-alphanumerics, lowercase, drop stop words,
// drop single-character tokens, deduplicate — preserving first-seen order.
inline std::vector<std::string> TokenizeSearchTerms(const std::string& query) {
    // Common stop words to exclude
    static const std::set<std::string> stopWords = {
        "a", "an", "and", "are", "as", "at", "be", "by", "for", "from",
        "has", "have", "he", "in", "is", "it", "its", "of", "on", "or",
        "that", "the", "to", "was", "were", "will", "with", "about",
        "into", "than", "then", "them", "these", "they", "this", "what",
        "when", "where", "which", "who", "how", "all", "any", "can",
        "do", "not", "but", "if", "so", "up", "out", "no", "just",
        "recent", "new", "latest"
    };

    std::vector<std::string> tokens;
    std::string current;
    for (char c : query) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            current += std::tolower(static_cast<unsigned char>(c));
        } else if (!current.empty()) {
            tokens.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) tokens.push_back(current);

    std::vector<std::string> terms;
    std::set<std::string> seen;
    for (auto& tok : tokens) {
        if (tok.size() < 2) continue;
        if (stopWords.count(tok)) continue;
        if (seen.insert(tok).second) terms.push_back(std::move(tok));
    }
    return terms;
}

// Convert a natural language query into an FTS5 OR query.
// FTS5 MATCH with space-separated words is implicit AND (all must match).
// We want OR (any word matches) for broader recall, ranked by relevance.
// Degenerate input (no indexable terms) returns the raw query.
inline std::string FtsQueryFromNaturalLanguage(const std::string& query) {
    const auto terms = TokenizeSearchTerms(query);
    if (terms.empty()) return query;

    std::string result;
    for (size_t i = 0; i < terms.size(); ++i) {
        if (i > 0) result += " OR ";
        result += terms[i];
    }
    return result;
}

// Convert a natural language query into a PostgreSQL tsquery with OR
// semantics (plainto_tsquery produces implicit AND). Degenerate input
// returns the raw query.
inline std::string PgTsQueryFromNaturalLanguage(const std::string& query) {
    const auto terms = TokenizeSearchTerms(query);
    if (terms.empty()) return query;

    std::string result;
    for (size_t i = 0; i < terms.size(); ++i) {
        if (i > 0) result += " | ";
        result += terms[i];
    }
    return result;
}

} // namespace animus::kernel::memory
