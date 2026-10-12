#include "animus_kernel/PromptAssembler.h"
#include <ctime>
#include <cstdio>
#include <cstdint>
#include "animus_kernel/Session.h"
#include "animus_kernel/TokenEstimate.h"

#include <chrono>

namespace animus::kernel {

std::size_t TokenEstimator::Estimate(const std::string& text) const {
    return TokenEstimate::Estimate(text);
}

std::size_t TokenEstimator::EstimateTurns(const std::vector<SessionTurn>& turns) const {
    std::size_t total = 0;
    for (const auto& turn : turns) {
        // Use cached token count if available (set during compaction or previous estimation)
        if (turn.token_count > 0) {
            total += turn.token_count + 4;  // +4 for message overhead
        } else {
            total += Estimate(turn.content) + 4;
        }
    }
    return total;
}

std::size_t TokenEstimator::EstimateMessages(const std::vector<llm::LLMMessage>& messages) const {
    std::size_t total = 0;
    for (const auto& msg : messages) {
        total += Estimate(msg.content) + 4;  // +4 for message overhead
    }
    return total;
}

PromptAssemblyResult PromptAssembler::BuildFromAccess(
    const SessionAccess& session,
    const std::string& userMessage,
    const std::string& systemPrompt,
    const std::string& modelId,
    std::size_t contextWindowTokens,
    float compactionThreshold) const {

    return Build(
        session.Turns(),
        session.GetCompactionSummary(),
        userMessage,
        systemPrompt,
        modelId,
        contextWindowTokens,
        compactionThreshold);
}

PromptAssemblyResult PromptAssembler::Build(
    const std::vector<SessionTurn>& turns,
    const SessionTurn* compactionSummary,
    const std::string& userMessage,
    const std::string& systemPrompt,
    const std::string& modelId,
    std::size_t contextWindowTokens,
    float compactionThreshold) const {

    PromptAssemblyResult result;
    result.request.model = modelId;
    result.request.temperature = 0.7f;
    result.request.stream = true;
    result.request.max_tokens = -1;

    // 1. System prompt
    if (!systemPrompt.empty()) {
        result.request.messages.push_back({"system", systemPrompt});
        result.system_prompt_tokens = m_estimator.Estimate(systemPrompt) + 4;
    }

    // 2. Compaction summary
    if (compactionSummary && !compactionSummary->content.empty()) {
        const std::string summaryText =
            "[Previous conversation summary]\n" + compactionSummary->content;
        result.request.messages.push_back({"system", summaryText});
        result.compaction_summary_tokens = m_estimator.Estimate(summaryText) + 4;
    }

    // 3. Session history
    // #133: block-level attribution headers — speaker header fires when a
    // user turn's author differs from the previous rendered turn's speaker;
    // timestamp header fires when the time gap exceeds the threshold
    // (10 min). Assistant/tool/system turns are agent-native: no speaker
    // header, but they still anchor time on long gaps.
    auto stampMs = [](std::uint64_t ms) -> std::string {
        std::time_t secs = static_cast<std::time_t>(ms / 1000);
        std::tm tm{};
        gmtime_r(&secs, &tm);
        char buf[24];
        std::snprintf(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d]",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min);
        return std::string(buf);
    };
    std::string lastSpeaker;
    std::uint64_t lastRenderedMs = 0;
    std::size_t headerTokens = 0;  // #133: header cost, folded into accounting below
    constexpr std::uint64_t kAttributionGapMs = 600'000;  // 10 min
    for (const auto& turn : turns) {
        if (turn.is_summary) continue;
        if (turn.is_compacted) continue;

        // Skip thinking-only turns (turns with no content and no tool_calls).
        // thinking_content is a display-only property — never sent to the LLM.
        if (turn.content.empty() && turn.tool_calls.empty()) continue;

        // #133: attribution header (speaker on change, timestamp on gap)
        const bool attributedUser =
            (turn.role == "user" && !turn.author.empty());
        std::string header;
        if (attributedUser && turn.author != lastSpeaker) {
            header = stampMs(turn.unix_ms) + " " + turn.author + ":";
        } else if (lastRenderedMs != 0 && turn.unix_ms != 0 &&
                   turn.unix_ms > lastRenderedMs + kAttributionGapMs) {
            header = stampMs(turn.unix_ms);
        }
        if (!header.empty()) {
            header += "\n";
            headerTokens += m_estimator.Estimate(header);
        }
        if (attributedUser) lastSpeaker = turn.author;
        if (turn.unix_ms != 0) lastRenderedMs = turn.unix_ms;

        llm::LLMMessage msg;
        msg.role = turn.role;
        msg.content = header + turn.content;

        // Carry tool result metadata for proper API formatting
        if (turn.role == "tool") {
            msg.tool_call_id = turn.tool_call_id;
            msg.name = turn.tool_name;
        }

        // Carry tool calls for assistant turns (needed for API compliance)
        if (turn.role == "assistant" && !turn.tool_calls.empty()) {
            for (const auto& tc : turn.tool_calls) {
                msg.tool_calls.push_back(llm::LLMToolCall{
                    tc.id, tc.name, tc.arguments
                });
            }
        }

        result.request.messages.push_back(std::move(msg));
    }

    // 4. User message (only if non-empty — avoids duplicate from session turns)
    if (!userMessage.empty()) {
        result.request.messages.push_back({"user", userMessage});
        result.draft_message_tokens = m_estimator.Estimate(userMessage) + 4;
    }

    // Compute session turn tokens (already assembled above)
    for (const auto& turn : turns) {
        if (turn.is_summary) continue;
        if (turn.is_compacted) continue;
        if (turn.content.empty() && turn.tool_calls.empty()) continue;
        if (turn.token_count > 0) {
            result.session_turns_tokens += turn.token_count + 4;
        } else {
            result.session_turns_tokens += m_estimator.Estimate(turn.content) + 4;
        }
    }
    // #133: attribution headers ride inside message content — cached
    // token_count and raw-content estimates both exclude them.
    result.session_turns_tokens += headerTokens;

    // Token budget check
    result.total_tokens = result.system_prompt_tokens
                        + result.compaction_summary_tokens
                        + result.session_turns_tokens
                        + result.draft_message_tokens;

    if (contextWindowTokens > 0) {
        const std::size_t budget = static_cast<std::size_t>(
            static_cast<float>(contextWindowTokens) * compactionThreshold);

        if (result.total_tokens > budget && turns.size() > 4) {
            result.needs_compaction = true;
        }
    }

    return result;
}

} // namespace animus::kernel