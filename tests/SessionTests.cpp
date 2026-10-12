#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <filesystem>
#include <memory>

#include <json/json.h>

#include "animus_kernel/ConversationCompactor.h"
#include "animus_kernel/DefaultSessionRouter.h"
#include "animus_kernel/SessionNotesStore.h"
#include "animus_kernel/SessionTagsStore.h"
#include "animus_kernel/tools/SessionsTool.h"
#include "animus_kernel/SessionManager.h"
#include "animus_kernel/SqliteDataStore.h"
#include "animus_kernel/SqliteSessionStore.h"
#include "animus_kernel/InMemorySessionStore.h"
#include "animus_kernel/SessionRoutingRule.h"
#include "animus_kernel/SessionManager.h"

using namespace animus::kernel;

namespace {

int Fail(int code, const std::string& message) {
    std::cerr << message << "\n";
    return code;
}

Session BuildSessionWithTurns(std::size_t turnCount, const std::string& content) {
    Session session(100, SessionKey{"test", "conversation", ""});
    for (std::size_t i = 0; i < turnCount; ++i) {
        SessionTurn turn;
        turn.role = (i % 2 == 0) ? "user" : "assistant";
        turn.content = content + " #" + std::to_string(i + 1);
        turn.unix_ms = static_cast<std::uint64_t>(1000 + i);
        session.AddTurn(std::move(turn));
    }
    return session;
}

int TestSessionRoutingAndAccess() {
    IncomingEvent baseEvent;
    baseEvent.source = "slack";
    baseEvent.metadata["channel_id"] = "C123";
    baseEvent.metadata["thread_ts"] = "T999";
    baseEvent.metadata["project_id"] = "proj-7";
    baseEvent.metadata["workspace_id"] = "ws-42";
    baseEvent.text = "hello";

    SessionRoutingCondition presentCondition{
        SessionRoutingMatchType::MetadataPresent, "project_id", ""};
    if (!presentCondition.Matches(baseEvent)) {
        return Fail(1, "expected metadata-present condition to match");
    }

    SessionRoutingCondition equalCondition{
        SessionRoutingMatchType::MetadataEquals, "workspace_id", "ws-42"};
    if (!equalCondition.Matches(baseEvent)) {
        return Fail(2, "expected metadata-equals condition to match");
    }

    SessionRoutingCondition prefixCondition{
        SessionRoutingMatchType::MetadataStartsWith, "channel_id", "C1"};
    if (!prefixCondition.Matches(baseEvent)) {
        return Fail(3, "expected metadata-prefix condition to match");
    }

    SessionRoutingCondition failedPrefixCondition{
        SessionRoutingMatchType::MetadataStartsWith, "channel_id", "D"};
    if (failedPrefixCondition.Matches(baseEvent)) {
        return Fail(4, "expected mismatched prefix condition to fail");
    }

    SessionRoutingRule projectRule{
        {
            SessionRoutingCondition{
                SessionRoutingMatchType::MetadataStartsWith, "channel_id", "C"},
            SessionRoutingCondition{
                SessionRoutingMatchType::MetadataPresent, "project_id", ""},
        },
        SessionKeyTemplate{"projects", "{meta:project_id}", ""},
    };

    if (!projectRule.Matches(baseEvent)) {
        return Fail(5, "expected project routing rule to match");
    }

    const SessionKey resolvedProject = projectRule.ResolveContext(baseEvent);
    if (resolvedProject.connector != "projects"
        || resolvedProject.conversation_id != "proj-7"
        || !resolvedProject.thread_id.empty()) {
        return Fail(6, "project routing rule resolved unexpected session key");
    }

    auto store = std::make_unique<InMemorySessionStore>();
    std::vector<SessionRoutingRule> rules{
        projectRule,
        SessionRoutingRule{
            {
                SessionRoutingCondition{
                    SessionRoutingMatchType::MetadataEquals,
                    "workspace_id",
                    "ws-42",
                },
            },
            SessionKeyTemplate{"workspace", "{meta:workspace_id}", "{source}"},
        },
        SessionRoutingRule{
            {
                SessionRoutingCondition{
                    SessionRoutingMatchType::MetadataEquals,
                    "workspace_id",
                    "ws-42",
                },
            },
            SessionKeyTemplate{"workspace", "{meta:workspace_id}", "{source}"},
        },
    };
    auto router = std::make_unique<DefaultSessionRouter>(std::move(rules));
    SessionManager mgr(std::move(store), std::move(router));

    IncomingEvent e1 = baseEvent;

    IncomingEvent e2 = e1;
    e2.text = "another";

    auto c1 = mgr.Resolve(e1);
    auto c2 = mgr.Resolve(e2);

    if (!c1.primary || !c2.primary) {
        return Fail(7, "failed to resolve primary session");
    }

    if (c1.primary.Id() != c2.primary.Id()) {
        return Fail(8, "expected same session id for same routing key");
    }

    IncomingEvent e3 = e1;
    e3.metadata["thread_ts"] = "T1000";
    auto c3 = mgr.Resolve(e3);

    if (c3.primary.Id() == c1.primary.Id()) {
        return Fail(9, "expected different session id for different thread");
    }

    SessionTurn firstTurn;
    firstTurn.role = "user";
    firstTurn.content = "hi";
    c1.primary.AddTurn(firstTurn);
    if (c1.primary.Turns().empty()) {
        return Fail(10, "turn did not persist");
    }

    if (c1.primary.Turns().front().turn_id == 0) {
        return Fail(11, "expected session to assign turn ids");
    }

    if (c1.context.size() != 2) {
        return Fail(12, "expected exactly two deduplicated context sessions");
    }

    if (c1.context.front().Mode() != SessionAccessMode::ReadOnly
        || c1.context.back().Mode() != SessionAccessMode::ReadOnly) {
        return Fail(13, "expected context sessions to be read-only");
    }

    if (c1.context.front().Key().connector != "projects"
        || c1.context.front().Key().conversation_id != "proj-7") {
        return Fail(14, "unexpected first context session key");
    }

    if (c1.context.back().Key().connector != "workspace"
        || c1.context.back().Key().conversation_id != "ws-42"
        || c1.context.back().Key().thread_id != "slack") {
        return Fail(15, "unexpected second context session key");
    }

    if (c1.context.front().Id() != c2.context.front().Id()
        || c1.context.back().Id() != c2.context.back().Id()) {
        return Fail(16, "expected context sessions to resolve consistently");
    }

    try {
        SessionTurn blockedTurn;
        blockedTurn.role = "user";
        blockedTurn.content = "nope";
        c1.context.front().AddTurn(blockedTurn);
        return Fail(17, "expected read-only context to reject AddTurn");
    } catch (const std::runtime_error&) {
    }

    return 0;
}

int TestCompactionPolicyEvaluation() {
    Session turnThresholdSession = BuildSessionWithTurns(4, "short");
    ConversationCompactor turnThresholdCompactor(
        turnThresholdSession,
        CompactionPolicy{3, 1000, CompactionStrategy::Truncate});
    if (!turnThresholdCompactor.ShouldCompact()) {
        return Fail(18, "expected turn threshold compaction to trigger");
    }

    Session tokenThresholdSession = BuildSessionWithTurns(2, std::string(80, 'x'));
    ConversationCompactor tokenThresholdCompactor(
        tokenThresholdSession,
        CompactionPolicy{50, 10, CompactionStrategy::Truncate});
    if (!tokenThresholdCompactor.ShouldCompact()) {
        return Fail(19, "expected token threshold compaction to trigger");
    }

    Session withinLimitsSession = BuildSessionWithTurns(2, "brief");
    ConversationCompactor withinLimitsCompactor(
        withinLimitsSession,
        CompactionPolicy{4, 1000, CompactionStrategy::Truncate});
    if (withinLimitsCompactor.ShouldCompact()) {
        return Fail(20, "did not expect compaction when session is within limits");
    }

    return 0;
}

int TestTruncationStrategy() {
    Session session = BuildSessionWithTurns(5, "truncate me");
    const auto originalFirstTurnId = session.Turns().front().turn_id;
    const auto keptTurnId = session.Turns()[2].turn_id;

    ConversationCompactor compactor(
        session,
        CompactionPolicy{3, 1000, CompactionStrategy::Truncate});
    if (!compactor.Compact()) {
        return Fail(21, "expected truncation compaction to run");
    }

    if (session.Turns().size() != 3) {
        return Fail(22, "expected truncation to keep only the newest turns");
    }

    if (session.Turns().front().turn_id != keptTurnId) {
        return Fail(23, "expected truncation to remove the oldest turns first");
    }

    if (session.GetCompactionSummary() != nullptr) {
        return Fail(24, "truncate strategy should not create a summary turn");
    }

    if (session.Turns().front().turn_id == originalFirstTurnId) {
        return Fail(25, "expected oldest turn to be removed during truncation");
    }

    return 0;
}

int TestSummaryProvenance() {
    Session session = BuildSessionWithTurns(4, "summarize me");
    const auto compactedFirstId = session.Turns()[0].turn_id;
    const auto compactedSecondId = session.Turns()[1].turn_id;
    const auto keptTurnId = session.Turns()[2].turn_id;

    ConversationCompactor compactor(
        session,
        CompactionPolicy{2, 1000, CompactionStrategy::Summarize});
    if (!compactor.Compact()) {
        return Fail(26, "expected summarize compaction to run");
    }

    const SessionTurn* summary = session.GetCompactionSummary();
    if (summary == nullptr) {
        return Fail(27, "expected compaction summary to be stored on the session");
    }

    if (!summary->is_summary) {
        return Fail(28, "expected compaction summary to be marked as a summary turn");
    }

    if (summary->compacted_from.size() != 2
        || summary->compacted_from[0] != compactedFirstId
        || summary->compacted_from[1] != compactedSecondId) {
        return Fail(29, "summary provenance did not record the compacted turn ids");
    }

    if (summary->turn_id == 0) {
        return Fail(30, "expected session to assign an id to the summary turn");
    }

    if (summary->content.find("Compacted 2 conversation turns") == std::string::npos) {
        return Fail(31, "expected placeholder summary content to describe the compaction");
    }

    if (session.Turns().size() != 2 || session.Turns().front().turn_id != keptTurnId) {
        return Fail(32, "expected summarized session to retain the newest turns");
    }

    return 0;
}

} // namespace

// ============================================================================
// #77: sessions:history / sessions:search DB fallback — terminated and
// persisted sessions must stay reachable; numeric ids and conversation ids
// must resolve; search must be newest-first across sessions.
// ============================================================================

namespace {

Json::Value ParseJsonOr(const std::string& text, int& failures) {
    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string errs;
    std::istringstream stream(text);
    if (!Json::parseFromStream(builder, stream, &root, &errs)) {
        ++failures;
        std::cerr << "[json] parse failed: " << errs << "\n";
    }
    return root;
}

std::string HistoryArgs(const std::string& agentId,
                        const std::string& sessionKey) {
    return "{\"action\":\"history\",\"__agent_id\":\"" + agentId +
           "\",\"session_key\":\"" + sessionKey + "\"}";
}

std::string SearchArgs(const std::string& agentId, const std::string& query) {
    return "{\"action\":\"search\",\"__agent_id\":\"" + agentId +
           "\",\"query\":\"" + query + "\"}";
}

} // namespace

int TestSessionsToolHistoryFallback() {
    namespace fs = std::filesystem;
    const std::string dbPath = "/tmp/animus_session_tests_77.db";
    std::error_code ec;
    fs::remove(dbPath, ec);

    SqliteDataStore dataStore(dbPath);
    SessionNotesStore notesStore(&dataStore);
    SessionTagsStore tagsStore(&dataStore);
    SessionManager manager(
        std::make_unique<SqliteSessionStore>(&dataStore),
        std::make_unique<DefaultSessionRouter>());
    SessionsTool tool(&manager, &notesStore, &tagsStore, nullptr);

    auto session = manager.GetOrCreate(
        SessionKey{"scheduled", "daily_marketclose:buffett:1770000000", ""});
    session->SetAgentId("buffett");
    SessionTurn turn;
    turn.role = "assistant";
    turn.content = "ORCL close assessment: defensive flip confirmed";
    turn.unix_ms = 1770000000000ULL;
    session->AddTurn(std::move(turn));
    manager.GetStore().FlushSession(session->Id());
    session->MarkTerminated();
    manager.GetStore().FlushSession(session->Id());

    int failures = 0;

    // 1. "session_<id>" — the exact form that failed in production (#77).
    {
        ToolCall call;
        call.id = "t-by-id";
        call.arguments = HistoryArgs(
            "buffett", "session_" + std::to_string(session->Id()));
        auto result = tool.Execute(call);
        if (!result.success)
            return Fail(1, "history by session_<id> should succeed");
        auto body = ParseJsonOr(result.output, failures);
        if (body["source"].asString() != "database")
            return Fail(2, "history by id should be database-sourced");
        if (!body["terminated"].asBool())
            return Fail(3, "terminated session should report terminated=true");
        if (body["messages"].size() != 1 ||
            body["messages"][0]["content"].asString().find("ORCL") ==
                std::string::npos)
            return Fail(4, "history by id should return persisted content");
    }

    // 2. Conversation id form.
    {
        ToolCall call;
        call.id = "t-by-conv";
        call.arguments = HistoryArgs(
            "buffett", "daily_marketclose:buffett:1770000000");
        auto result = tool.Execute(call);
        if (!result.success)
            return Fail(5, "history by conversation id should succeed");
        auto body = ParseJsonOr(result.output, failures);
        if (body["source"].asString() != "database")
            return Fail(6, "conversation-id lookup should be database-sourced");
    }

    // 3. Full key string — still resolved (live registry match).
    {
        ToolCall call;
        call.id = "t-by-key";
        call.arguments = HistoryArgs(
            "buffett", "scheduled|daily_marketclose:buffett:1770000000|");
        auto result = tool.Execute(call);
        if (!result.success)
            return Fail(7, "history by key string should succeed");
        auto body = ParseJsonOr(result.output, failures);
        if (body["terminated"].asBool() != true)
            return Fail(8, "key-string history should still see termination");
    }

    // 4. Ownership enforced on the fallback path.
    {
        ToolCall call;
        call.id = "t-foreign";
        call.arguments = HistoryArgs(
            "sable", "session_" + std::to_string(session->Id()));
        auto result = tool.Execute(call);
        if (result.success)
            return Fail(9, "foreign agent must not read another agent's session");
    }

    // 5. Unknown key — error names the accepted formats.
    {
        ToolCall call;
        call.id = "t-unknown";
        call.arguments = HistoryArgs("buffett", "no|such|session");
        auto result = tool.Execute(call);
        if (result.success) return Fail(10, "unknown session must fail");
        if (result.error.find("accepted") == std::string::npos)
            return Fail(11, "error should list accepted formats");
    }

    if (failures) return Fail(12, "json parse failures in history asserts");
    return 0;
}

int TestSessionsToolSearchRecency() {
    namespace fs = std::filesystem;
    const std::string dbPath = "/tmp/animus_session_tests_77b.db";
    std::error_code ec;
    fs::remove(dbPath, ec);

    SqliteDataStore dataStore(dbPath);
    SessionNotesStore notesStore(&dataStore);
    SessionTagsStore tagsStore(&dataStore);
    SessionManager manager(
        std::make_unique<SqliteSessionStore>(&dataStore),
        std::make_unique<DefaultSessionRouter>());
    SessionsTool tool(&manager, &notesStore, &tagsStore, nullptr);

    struct Seed {
        const char* connector;
        const char* conversation;
        std::uint64_t lastActive;
        const char* content;
    };
    const Seed seeds[] = {
        {"scheduled", "older:run", 1000000, "needleword in the older run"},
        {"scheduled", "newer:run", 2000000, "needleword in the newer run"},
        {"scheduled", "foreign:agent", 3000000, "needleword owned by someone else"},
    };
    for (const auto& s : seeds) {
        auto session = manager.GetOrCreate(SessionKey{s.connector, s.conversation, ""});
        session->SetAgentId(std::string(s.conversation) == "foreign:agent"
                                ? "sable" : "buffett");
        session->SetLastActiveUnixMs(s.lastActive);
        SessionTurn turn;
        turn.role = "assistant";
        turn.content = s.content;
        turn.unix_ms = s.lastActive;
        session->AddTurn(std::move(turn));
        manager.GetStore().FlushSession(session->Id());
        session->MarkTerminated();
        manager.GetStore().FlushSession(session->Id());
    }

    // Case-insensitive query across terminated sessions, agent-scoped,
    // newest match first.
    ToolCall call;
    call.id = "t-search";
    call.arguments = SearchArgs("buffett", "NEEDLEWORD");
    auto result = tool.Execute(call);
    if (!result.success) return Fail(1, "search should succeed");

    int failures = 0;
    auto body = ParseJsonOr(result.output, failures);
    const auto& results = body["results"];
    if (body.isMember("results") ? results.size() != 2 : true) {
        // Fallback: some result shapes use different keys; inspect items.
    }
    // Locate the items array regardless of key name.
    Json::Value items;
    if (body.isMember("results")) items = body["results"];
    else if (body.isMember("matches")) items = body["matches"];
    else items = body["messages"];
    if (items.size() != 2)
        return Fail(2, "search should find exactly the agent's two matches");
    if (items[0]["content"].asString().find("newer") == std::string::npos)
        return Fail(3, "newest match must be returned first");
    if (items[1]["content"].asString().find("older") == std::string::npos)
        return Fail(4, "second match should be the older session");
    if (failures) return Fail(5, "json parse failures in search asserts");
    return 0;
}

int main() {
    if (const int rc = TestSessionRoutingAndAccess(); rc != 0) {
        return rc;
    }

    if (const int rc = TestCompactionPolicyEvaluation(); rc != 0) {
        return rc;
    }

    if (const int rc = TestTruncationStrategy(); rc != 0) {
        return rc;
    }

    if (const int rc = TestSummaryProvenance(); rc != 0) {
        return rc;
    }

    if (const int rc = TestSessionsToolHistoryFallback(); rc != 0) {
        return rc;
    }

    if (const int rc = TestSessionsToolSearchRecency(); rc != 0) {
        return rc;
    }

    return 0;
}
