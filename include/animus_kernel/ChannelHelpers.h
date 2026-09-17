#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <json/json.h>
#include <json/reader.h>
#include <json/writer.h>

namespace animus::kernel {

namespace channel_detail {

inline Json::Value ParseJson(const std::string& json) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::istringstream stream(json);
    std::string errors;
    Json::parseFromStream(builder, stream, &root, &errors);
    return root;
}

inline std::string GetString(const Json::Value& v, const std::string& key,
                             const std::string& def = "") {
    if (v.isMember(key) && v[key].isString()) return v[key].asString();
    return def;
}

inline int64_t GetInt(const Json::Value& v, const std::string& key, int64_t def = 0) {
    if (v.isMember(key) && v[key].isInt64()) return v[key].asInt64();
    if (v.isMember(key) && v[key].isInt()) return v[key].asInt();
    return def;
}

inline std::string UrlEncode(const std::string& input) {
    std::string result;
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += c;
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
            result += buf;
        }
    }
    return result;
}

inline std::string Base64EncodeStr(const std::string& input) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((input.size() + 2) / 3) * 4);
    for (size_t i = 0; i < input.size(); i += 3) {
        uint32_t n = static_cast<uint8_t>(input[i]) << 16;
        if (i + 1 < input.size()) n |= static_cast<uint8_t>(input[i + 1]) << 8;
        if (i + 2 < input.size()) n |= static_cast<uint8_t>(input[i + 2]);
        result += table[(n >> 18) & 0x3F];
        result += table[(n >> 12) & 0x3F];
        result += (i + 1 < input.size()) ? table[(n >> 6) & 0x3F] : '=';
        result += (i + 2 < input.size()) ? table[n & 0x3F] : '=';
    }
    return result;
}

inline std::string JsonCompact(const Json::Value& v) {
    Json::StreamWriterBuilder wb;
    wb.settings_["indentation"] = "";
    return Json::writeString(wb, v);
}

// Split `text` into chunks of at most `limit` bytes, breaking at natural
// boundaries (paragraph > line > word > hard cut). Markdown-aware: when a
// break lands inside an open ``` fence, the chunk closes the fence and the
// next chunk reopens it, so rendered code blocks survive reassembly. Each
// chunk reserves 8 bytes of headroom for that close+reopen surgery.
// Returns {text} unchanged when it already fits. Heuristic note: fences are
// counted as literal ``` occurrences; inline triple backticks are rare
// enough that the trade is worth it (#30).
inline std::vector<std::string> SplitForLimit(const std::string& text, size_t limit) {
    if (limit < 16) limit = 16;  // floor: guarantees the loop always advances
    if (text.size() <= limit) return {text};

    const size_t budget = limit - 8;  // reserve close ("\n```") + reopen ("```\n") room
    std::vector<std::string> chunks;
    size_t pos = 0;
    bool fenceOpen = false;  // true when the previous chunk ended inside a fence

    auto countFences = [](const std::string& s) {
        size_t n = 0;
        for (size_t f = s.find("```"); f != std::string::npos; f = s.find("```", f + 3)) ++n;
        return n;
    };
    auto bodyHasOpenerTail = [&countFences](const std::string& body) {
        const size_t lf = body.rfind("```");
        if (lf == std::string::npos) return false;
        const std::string tail = body.substr(lf + 3);
        return tail.empty()
            || (tail.back() == '\n' && tail.substr(0, tail.size() - 1).find('\n') == std::string::npos);
    };

    while (pos < text.size()) {
        const size_t cap = (fenceOpen ? budget - 4 : budget);
        size_t end = (text.size() - pos <= cap) ? text.size() : pos + cap;

        // Find a natural break point at or before `end`, preferring
        // paragraph, then line, then word boundaries. Falls back to a hard cut.
        // Note: rfind requires the whole match within [0, pos], so multi-byte
        // delimiters get pos+1 to catch boundaries straddling the window edge.
        size_t breakPoint;
        if (end == text.size()) {
            breakPoint = end;
        } else {
            breakPoint = std::string::npos;
            size_t dd = text.rfind("\n\n", end + 1);
            if (dd != std::string::npos && dd >= pos) breakPoint = dd + 2;
            if (breakPoint == std::string::npos) {
                size_t sd = text.rfind('\n', end);
                if (sd != std::string::npos && sd + 1 > pos) breakPoint = sd + 1;
            }
            if (breakPoint == std::string::npos) {
                size_t sp = text.rfind(' ', end);
                if (sp != std::string::npos && sp + 1 > pos) breakPoint = sp + 1;
            }
            if (breakPoint == std::string::npos || breakPoint > end + 2) breakPoint = end;
        }
        if (breakPoint <= pos) breakPoint = pos + 1;  // always advance

        std::string body = text.substr(pos, breakPoint - pos);
        size_t fenceCount = countFences(body);
        bool openAfter = ((fenceCount % 2) == 1) ^ fenceOpen;
        const bool isLast = (breakPoint >= text.size());

        // Opener-straddle guard: if the body ends with a fence opener line
        // (``` plus optional language tag plus newline, no content line after
        // it in this chunk), pull the break back so the fence starts whole in
        // the next chunk instead of forming a degenerate open/close pair. If
        // the opener sits at the body start, extend forward (hard cut)
        // instead — mid-fence surgery handles it.
        if (openAfter && !isLast) {
            const size_t lastFence = body.rfind("```");
            if (lastFence != std::string::npos) {
                const std::string tail = body.substr(lastFence + 3);
                const bool openerTail = tail.empty()
                    || (tail.back() == '\n' && tail.substr(0, tail.size() - 1).find('\n') == std::string::npos);
                if (openerTail) {
                    if (lastFence > 0) {
                        const size_t cut = body.rfind('\n', lastFence - 1);
                        if (cut != std::string::npos) {
                            breakPoint = pos + cut + 1;
                            body = text.substr(pos, breakPoint - pos);
                            fenceCount = countFences(body);
                            openAfter = ((fenceCount % 2) == 1) ^ fenceOpen;
                        }
                    }
                    if (lastFence == 0 || bodyHasOpenerTail(body)) {
                        // Opener at body start: take the full cap instead.
                        const size_t hardEnd = pos + cap;
                        if (hardEnd > breakPoint && hardEnd <= text.size()) {
                            breakPoint = hardEnd;
                            body = text.substr(pos, breakPoint - pos);
                            fenceCount = countFences(body);
                            openAfter = ((fenceCount % 2) == 1) ^ fenceOpen;
                        }
                    }
                }
            }
        }
        pos = breakPoint;

        std::string chunk;
        if (fenceOpen) chunk += "```\n";  // reopen fence carried from previous chunk
        chunk += body;
        if (openAfter && pos < text.size()) {
            chunk += "\n```";  // close the fence; next chunk reopens it
            fenceOpen = true;
        } else {
            fenceOpen = openAfter;  // final chunk keeps the author's bytes verbatim
        }
        chunks.push_back(std::move(chunk));
    }
    return chunks;
}

inline std::string StripHtmlSimple(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    bool inTag = false;
    bool lastWasSpace = true;
    for (char c : html) {
        if (inTag) { if (c == '>') inTag = false; continue; }
        if (c == '<') { inTag = true; continue; }
        if (c == '\n') { if (!lastWasSpace) { out += '\n'; lastWasSpace = true; } continue; }
        if (c == ' ' || c == '\t' || c == '\r') { if (!lastWasSpace) { out += ' '; lastWasSpace = true; } continue; }
        out += c; lastWasSpace = false;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) out.pop_back();
    return out;
}

} // namespace channel_detail

} // namespace animus::kernel
