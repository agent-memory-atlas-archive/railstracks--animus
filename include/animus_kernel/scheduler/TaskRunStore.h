#pragma once

#include "animus_kernel/IDataStore.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace animus::kernel {

// ============================================================================
// TaskRunStore — durable record + idempotency guard for scheduler fires.
//
// Every due-window fire is claimed BEFORE dispatch via
// INSERT ... ON CONFLICT(run_uuid) DO NOTHING RETURNING id. If the claim
// returns no row, that window was already processed (crash between fire and
// schedule-state update, double-poll, or — in the federation design — a peer
// node) and dispatch is skipped while the schedule still advances.
//
// This is the at-least-once -> exactly-once-effective mechanism from the
// node-federation design (#78): analysis tasks worst-case write twice and
// merge; here the common case is they fire once per window, provably.
//
// node_id is '' until node identity lands (P1a); triggered_by is 'schedule'
// until manual/failover triggers exist (P2).
// ============================================================================

struct TaskRun {
    int64_t id{0};
    std::string run_uuid;        // "<schedule_id>@<window>" — the idempotency key
    std::string schedule_id;
    std::string agent_id;
    std::string node_id;
    std::string triggered_by;
    std::string scheduled_for;   // window identity (the next_fire that made it due)
    int64_t started_at_unix_ms{0};
    int64_t finished_at_unix_ms{0};   // 0 while running
    std::string outcome;         // running | dispatched | skipped:* | error:* | no_callback
    std::string error;
};

class TaskRunStore {
public:
    explicit TaskRunStore(IDataStore* store);

    void EnsureSchema();

    // Atomically claim a fire window. Returns true iff THIS call inserted the
    // row (a RETURNING row came back). false = already claimed (running or
    // finished by this node, a past life, or a peer).
    bool TryClaim(const std::string& runUuid, const std::string& scheduleId,
                  const std::string& agentId, const std::string& scheduledFor,
                  int64_t startedAtUnixMs);

    // Record the dispatch outcome. Returns false if the uuid is unknown.
    bool Finish(const std::string& runUuid, const std::string& outcome,
                const std::string& error, int64_t finishedAtUnixMs);

    std::optional<TaskRun> GetByUuid(const std::string& runUuid) const;
    std::vector<TaskRun> ListForSchedule(const std::string& scheduleId, int limit) const;
    // agent_id "" = all agents. Newest first.
    std::vector<TaskRun> ListRecent(const std::string& agentId, int limit) const;

private:
    TaskRun RowFromStatement(IStatement* stmt) const;

    IDataStore* m_store;
};

} // namespace animus::kernel
