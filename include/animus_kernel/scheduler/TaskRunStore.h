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
// INSERT ... SELECT WHERE NOT EXISTS(run_uuid) RETURNING id. No row returned
// = that window was already processed (crash between fire and schedule-state
// update, double-poll, or a peer node) and dispatch is skipped while the
// schedule still advances.
//
// run_uuid is NOT a UNIQUE constraint (P2b): rows replicate with node-scoped
// integer ids, and a partition double-claim leaves BOTH nodes holding the
// same run_uuid with different ids — a UNIQUE index would wedge the heal
// (apply conflict). Uniqueness is enforced at claim time (single-threaded
// poll loop; the local race the constraint guarded does not exist) and
// cross-node double claims are reconciled by EPOCH FENCING: survivor =
// max(epoch), tie max(id); losers keep their rows with fenced=1 — the
// double-fire tripwire (loud WARN at apply, visible in listings).
//
// epoch: the lease epoch the claim was made under (P2b). 0 = at-least-once
// semantics (no lease).
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
    int64_t epoch{0};            // lease epoch at claim time (0 = no lease)
    bool fenced{false};          // true = lost the epoch fence (a duplicate)
};

class TaskRunStore {
public:
    explicit TaskRunStore(IDataStore* store);

    void EnsureSchema();

    // Atomically claim a fire window. Returns true iff THIS call inserted the
    // row (a RETURNING row came back). false = already claimed locally.
    bool TryClaim(const std::string& runUuid, const std::string& scheduleId,
                  const std::string& agentId, const std::string& scheduledFor,
                  int64_t startedAtUnixMs,
                  const std::string& nodeId = "",
                  int64_t epoch = 0);

    // Record the dispatch outcome. Returns false if the uuid is unknown.
    // Finishes THIS node's claim row for the window. Scoped by node_id:
    // one node's finish must not smear its outcome onto peer rows of the
    // same run_uuid (the fenced loser keeps its own state — witnessed as
    // identical-ms finished stamps on both rows during chaos S1).
    bool Finish(const std::string& runUuid, const std::string& outcome,
                const std::string& error, int64_t finishedAtUnixMs,
                const std::string& nodeId = "");

    // Window takeover for a stale pending claim (#78 P2b): the claiming
    // node died between claim and dispatch (outcome still 'running' while
    // provably down + aged past the visibility bound). Fences the stale
    // row and inserts a fresh claim at epoch+1 for this node. Returns the
    // new claim's epoch (0 = takeover refused: row not stale or missing).
    int64_t TakeOverStaleClaim(const std::string& runUuid,
                               const std::string& scheduleId,
                               const std::string& agentId,
                               const std::string& scheduledFor,
                               int64_t nowMs,
                               const std::string& nodeId,
                               int64_t staleBeforeMs);

    // Epoch-fence reconciliation for a run_uuid: keeps the winner
    // (max epoch, tie max id) unfenced, marks every other row fenced.
    // Returns the number of rows fenced (0 = no collision). Called after
    // remote applies and after local claims — the double-fire tripwire
    // fires (WARN) whenever a collision is witnessed.
    int FenceRunUuid(const std::string& runUuid);

    std::optional<TaskRun> GetByUuid(const std::string& runUuid) const;
    std::vector<TaskRun> ListForSchedule(const std::string& scheduleId, int limit) const;
    // agent_id "" = all agents. Newest first.
    std::vector<TaskRun> ListRecent(const std::string& agentId, int limit) const;

private:
    TaskRun RowFromStatement(IStatement* stmt) const;

    IDataStore* m_store;
};

} // namespace animus::kernel
