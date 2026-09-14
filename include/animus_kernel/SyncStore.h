#pragma once
// #78 P1b: replication outbox + LWW apply for agent-global tables.
//
// The pattern (generalizing memory_mutations, per #78's design): every write
// to an agent-global table lands in a node-local, append-only outbox via
// DATABASE TRIGGERS — not write-path instrumentation. Triggers cannot be
// forgotten when a new write path appears; background jobs (consolidation,
// scheduler) are captured for free.
//
// Echo suppression without session flags: while a remote change is being
// applied, sync_control carries (apply_origin, apply_ms); triggers stamp the
// ORIGIN's identity (not the local node's) onto the version row and the
// outbox echo. When the echo reaches any node that already has that exact
// (ms, origin) version — including the origin itself — the pair-compare
// rejects it. Echoes die at birth, concurrent local writes during a sync
// batch are never lost (no "applying" flag suppressing outbox rows), and
// clock skew is bounded by stamping max(local_now, apply_ms).
//
// LWW: a row's truth is the (unix_ms, origin_node) pair; strictly greater
// wins, ties (equal pair) are skipped — identical on every node, so the
// network converges without coordination.

#include "animus_kernel/IDataStore.h"

#include <cstdint>
#include <string>
#include <vector>

namespace animus::kernel {

struct OutboxRecord {
    int64_t outbox_id{0};
    int64_t origin_node{0};
    std::string table_name;
    std::string row_key;    // TEXT row identity: int ids stringify, TEXT ids as-is
    std::string op;        // "upsert" | "delete"
    std::string payload;   // full-row JSON (upsert) or {"id":N} (delete)
    int64_t unix_ms{0};
};

struct SyncStore {
public:
    SyncStore(IDataStore* store, uint64_t localNodeId);

    // SQLite table-rebuild helper (P2a): recreates a sync table with a
    // changed column type, preserving rows. Used for the INTEGER -> TEXT
    // row-id migration (schedules/task_runs joined the synced set with
    // TEXT / node-scoped int ids expressed as text).
    static bool RebuildSqliteTableWide(IDataStore* store, const std::string& ddl,
                                       const std::string& table, std::string* error);

    // Creates the sync tables and installs change triggers on EVERY
    // agent-global table (live schema read at install time). Call after all
    // agent-global stores exist. Idempotent — safe on every boot.
    bool EnsureSchema(std::string* error = nullptr);

    // Outbox: ordered, append-only. Peers pull with ?since=<their cursor>.
    std::vector<OutboxRecord> FetchOutboxSince(int64_t sinceOutboxId, int64_t limit);
    int64_t MaxOutboxId();

    // Per-peer pull cursors (the peer's progress through OUR outbox).
    // Per-table digest over the agent-global tables for anti-entropy:
    // row count + max id from the table itself, sum of version stamps from
    // sync_row_versions. Used by the handshake to let peers detect
    // divergence the outbox alone can't explain (e.g. a datadir restored
    // from a partial backup carries a stale cursor past missing rows).
    struct TableDigest {
        std::string table;
        int64_t count{0};
        int64_t maxId{0};
        int64_t sumLastMs{0};
    };
    std::vector<TableDigest> TableDigests();

    // Anti-entropy support: drop the version stamps for one table so a
    // full outbox replay can re-apply rows whose stamps survived the
    // disappearance of the rows themselves (partial-restore corruption —
    // stamps claim knowledge the table no longer has).
    void ClearTableVersions(const std::string& table);

    int64_t GetPeerCursor(int64_t peerNode);   // 0 = from the beginning
    bool SetPeerCursor(int64_t peerNode, int64_t outboxId);

    // LWW apply of one remote change. Returns true if applied; false if
    // skipped (stale, echo, or tie — all safe). Records the remote's
    // (ms, origin) as the row's version.
    bool ApplyRemoteChange(const OutboxRecord& rec);

    IDataStore* Store() const { return m_store; }
    uint64_t LocalNodeId() const { return m_nodeId; }

    // Tables triggers were actually installed on (subset of agent-global;
    // missing stores are skipped). Handshake reports this list.
    const std::vector<std::string>& SyncedTables() const { return m_syncedTables; }

private:
    std::vector<std::string> ReadTableColumns(const std::string& table);
    bool InstallTriggersFor(const std::string& table, std::string* error);
    bool ApplyUpsert(const OutboxRecord& rec);
    bool ApplyDelete(const OutboxRecord& rec);
    // P2b: epoch-fence a run window after a remote task_runs apply.
    void FenceTaskRun(const std::string& payloadJson);

    IDataStore* m_store;
    uint64_t m_nodeId;
    std::vector<std::string> m_syncedTables;
};

} // namespace animus::kernel
