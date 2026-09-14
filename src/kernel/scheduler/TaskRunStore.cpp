#include "animus_kernel/scheduler/TaskRunStore.h"

#include "animus_kernel/Log.h"
#include "animus_kernel/SchemaHelpers.h"

#include <algorithm>

namespace animus::kernel {

static const char* kTaskRunCols =
    "id, run_uuid, schedule_id, agent_id, node_id, triggered_by, scheduled_for, "
    "started_at_unix_ms, finished_at_unix_ms, outcome, error, epoch, fenced";

TaskRunStore::TaskRunStore(IDataStore* store) : m_store(store) {}

void TaskRunStore::EnsureSchema() {
    // schema::CreateTable translates the SQLite DDL for PostgreSQL
    // (AUTOINCREMENT -> BIGSERIAL etc.). Raw Exec leaves PG without the
    // table and every TryClaim wedges the schedule "due" forever —
    // seen live on a PG-backed prod instance (2026-09-14).
    schema::CreateTable(m_store, R"(
        CREATE TABLE IF NOT EXISTS task_runs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            run_uuid TEXT NOT NULL,
            schedule_id TEXT NOT NULL,
            agent_id TEXT NOT NULL DEFAULT '',
            node_id TEXT NOT NULL DEFAULT '',
            triggered_by TEXT NOT NULL DEFAULT 'schedule',
            scheduled_for TEXT NOT NULL DEFAULT '',
            started_at_unix_ms INTEGER NOT NULL,
            finished_at_unix_ms INTEGER,
            outcome TEXT NOT NULL DEFAULT 'running',
            error TEXT NOT NULL DEFAULT '',
            epoch INTEGER NOT NULL DEFAULT 0,
            fenced INTEGER NOT NULL DEFAULT 0
        )");
    )");
    // P2b migration: run_uuid drops its UNIQUE constraint (replicated rows
    // from a partition double-claim share run_uuid with different node-scoped
    // ids — UNIQUE would wedge the sync heal with an apply conflict). Fresh
    // schema above has no constraint; existing tables are rebuilt via
    // create-copy-swap. Also adds epoch/fenced columns.
    if (m_store->Dialect() == DataStoreDialect::SQLite) {
        auto probe = m_store->Prepare(
            "SELECT sql FROM sqlite_master WHERE type='table' AND name='task_runs'");
        if (probe && probe->Step()) {
            const std::string ddl = probe->ColumnText(0);
            const bool hasUnique = ddl.find("UNIQUE") != std::string::npos;
            auto colEpoch = m_store->Prepare(
                "SELECT 1 FROM pragma_table_info('task_runs') WHERE name='epoch'");
            const bool hasEpoch = colEpoch && colEpoch->Step();
            if (hasUnique || !hasEpoch) {
                m_store->Exec(R"(
                    CREATE TABLE IF NOT EXISTS task_runs_migrate (
                        id INTEGER PRIMARY KEY AUTOINCREMENT,
                        run_uuid TEXT NOT NULL,
                        schedule_id TEXT NOT NULL,
                        agent_id TEXT NOT NULL DEFAULT '',
                        node_id TEXT NOT NULL DEFAULT '',
                        triggered_by TEXT NOT NULL DEFAULT 'schedule',
                        scheduled_for TEXT NOT NULL DEFAULT '',
                        started_at_unix_ms INTEGER NOT NULL,
                        finished_at_unix_ms INTEGER,
                        outcome TEXT NOT NULL DEFAULT 'running',
                        error TEXT NOT NULL DEFAULT '',
                        epoch INTEGER NOT NULL DEFAULT 0,
                        fenced INTEGER NOT NULL DEFAULT 0
                    );
                )");
                m_store->Exec(
                    "INSERT OR REPLACE INTO task_runs_migrate (id, run_uuid, schedule_id, "
                    "agent_id, node_id, triggered_by, scheduled_for, started_at_unix_ms, "
                    "finished_at_unix_ms, outcome, error, epoch, fenced) "
                    "SELECT id, run_uuid, schedule_id, agent_id, node_id, triggered_by, "
                    "scheduled_for, started_at_unix_ms, finished_at_unix_ms, outcome, error, "
                    "0, 0 FROM task_runs");
                m_store->Exec("DROP TABLE task_runs");
                m_store->Exec("ALTER TABLE task_runs_migrate RENAME TO task_runs");
                ALOG_INFO("scheduler", "task_runs migrated: run_uuid UNIQUE dropped, "
                          "epoch/fenced added (create-copy-swap)");
            }
        }
    } else if (m_store->Dialect() == DataStoreDialect::PostgreSQL) {
        m_store->Exec("ALTER TABLE task_runs DROP CONSTRAINT IF EXISTS task_runs_run_uuid_key");
        auto colEpoch = m_store->Prepare(
            "SELECT 1 FROM information_schema.columns "
            "WHERE table_name='task_runs' AND column_name='epoch'");
        const bool hasEpoch = colEpoch && colEpoch->Step();
        if (!hasEpoch) {
            m_store->Exec("ALTER TABLE task_runs ADD COLUMN epoch BIGINT NOT NULL DEFAULT 0");
            m_store->Exec("ALTER TABLE task_runs ADD COLUMN fenced INTEGER NOT NULL DEFAULT 0");
        }
    }
    m_store->Exec(
        "CREATE INDEX IF NOT EXISTS idx_task_runs_uuid ON task_runs(run_uuid)");
    m_store->Exec(
        "CREATE INDEX IF NOT EXISTS idx_task_runs_schedule "
        "ON task_runs(schedule_id, started_at_unix_ms DESC);");
    m_store->Exec(
        "CREATE INDEX IF NOT EXISTS idx_task_runs_agent "
        "ON task_runs(agent_id, started_at_unix_ms DESC);");
}

bool TaskRunStore::TryClaim(const std::string& runUuid, const std::string& scheduleId,
                            const std::string& agentId, const std::string& scheduledFor,
                            int64_t startedAtUnixMs,
                            const std::string& nodeId,
                            int64_t epoch) {
    // Claim detection without any store-global state (#76 lesson):
    // the RETURNING row IS the receipt — no row means the uuid existed.
    // run_uuid has no UNIQUE constraint (P2b — replicated double-claims
    // must not wedge applies); atomicity here comes from
    // WHERE NOT EXISTS, sound because the poll loop is single-threaded.
    auto stmt = m_store->Prepare(
        "INSERT INTO task_runs (run_uuid, schedule_id, agent_id, scheduled_for, "
        "node_id, started_at_unix_ms, epoch) "
        "SELECT ?,?,?,?,?,?,? WHERE NOT EXISTS "
        "(SELECT 1 FROM task_runs WHERE run_uuid = ?) RETURNING id");
    if (!stmt) return false;

    stmt->BindText(1, runUuid);
    stmt->BindText(2, scheduleId);
    stmt->BindText(3, agentId);
    stmt->BindText(4, scheduledFor);
    stmt->BindText(5, nodeId);
    stmt->BindInt64(6, startedAtUnixMs);
    stmt->BindInt64(7, epoch);
    stmt->BindText(8, runUuid);

    if (!stmt->Step()) {
        ALOG_DEBUG("scheduler", "task_run claim lost (already processed): " << runUuid);
        return false;
    }
    return true;
}

int64_t TaskRunStore::TakeOverStaleClaim(const std::string& runUuid,
                                         const std::string& scheduleId,
                                         const std::string& agentId,
                                         const std::string& scheduledFor,
                                         int64_t nowMs,
                                         const std::string& nodeId,
                                         int64_t staleBeforeMs) {
    // Only a PENDING (running, unfinished) claim older than the bound is
    // takeable — finished windows are history, fresh windows get their
    // visibility round.
    auto cur = GetByUuid(runUuid);
    if (!cur) return 0;
    if (cur->outcome != "running" || cur->started_at_unix_ms >= staleBeforeMs)
        return 0;
    const int64_t newEpoch = cur->epoch + 1;
    // Fence every row of the old generation, then claim at epoch+1.
    auto f = m_store->Prepare(
        "UPDATE task_runs SET fenced = 1 WHERE run_uuid = ?");
    if (!f) return 0;
    f->BindText(1, runUuid);
    if (!f->ExecDML()) return 0;
    if (!TryClaim(runUuid, scheduleId, agentId, scheduledFor,
                  nowMs, nodeId, newEpoch)) {
        return 0;   // raced with a concurrent takeover attempt
    }
    ALOG_WARNING("scheduler", "STALE-CLAIM TAKEOVER: " << runUuid
        << " previous holder left it pending since "
        << cur->started_at_unix_ms << " — re-claimed at epoch "
        << newEpoch << " by " << nodeId);
    return newEpoch;
}

int TaskRunStore::FenceRunUuid(const std::string& runUuid) {
    // Survivor: max epoch, tie max(id) — total order, identical on every
    // node. Everything else gets fenced (the duplicate-fire witness).
    auto q = m_store->Prepare(
        "UPDATE task_runs SET fenced = 1 WHERE run_uuid = ? AND fenced = 0 "
        "AND id != (SELECT id FROM task_runs WHERE run_uuid = ? "
        "ORDER BY epoch DESC, id DESC LIMIT 1)");
    if (!q) return 0;
    q->BindText(1, runUuid);
    q->BindText(2, runUuid);
    if (!q->ExecDML()) return 0;
    auto v = m_store->Prepare("SELECT changes()");
    int n = (v && v->Step()) ? static_cast<int>(v->ColumnInt64(0)) : 0;
    if (n > 0) {
        ALOG_WARNING("scheduler", "DOUBLE-FIRE WITNESSED: " << n
            << " duplicate claim row(s) fenced for " << runUuid
            << " (partition double-claim reconciled by epoch)");
    }
    return n;
}

bool TaskRunStore::Finish(const std::string& runUuid, const std::string& outcome,
                          const std::string& error, int64_t finishedAtUnixMs,
                          const std::string& nodeId) {
    // Empty nodeId = legacy unscoped form (unit tests on single-node
    // stores); production callers pass their node id so a finish touches
    // only the caller's own row of the window.
    const std::string scope = nodeId.empty() ? "" : " AND node_id=?";
    auto stmt = m_store->Prepare(
        "UPDATE task_runs SET outcome=?, error=?, finished_at_unix_ms=? "
        "WHERE run_uuid=?" + scope);
    if (!stmt) return false;
    stmt->BindText(1, outcome);
    stmt->BindText(2, error);
    stmt->BindInt64(3, finishedAtUnixMs);
    stmt->BindText(4, runUuid);
    if (!nodeId.empty()) stmt->BindText(5, nodeId);
    stmt->ExecDML();
    return true;
}

TaskRun TaskRunStore::RowFromStatement(IStatement* stmt) const {
    TaskRun run;
    run.id = stmt->ColumnInt64(0);
    run.run_uuid = stmt->ColumnText(1);
    run.schedule_id = stmt->ColumnText(2);
    run.agent_id = stmt->ColumnText(3);
    run.node_id = stmt->ColumnText(4);
    run.triggered_by = stmt->ColumnText(5);
    run.scheduled_for = stmt->ColumnText(6);
    run.started_at_unix_ms = stmt->ColumnInt64(7);
    run.finished_at_unix_ms = stmt->IsColumnNull(8) ? 0 : stmt->ColumnInt64(8);
    run.outcome = stmt->ColumnText(9);
    run.error = stmt->ColumnText(10);
    run.epoch = stmt->IsColumnNull(11) ? 0 : stmt->ColumnInt64(11);
    run.fenced = !stmt->IsColumnNull(12) && stmt->ColumnInt64(12) != 0;
    return run;
}

std::optional<TaskRun> TaskRunStore::GetByUuid(const std::string& runUuid) const {
    auto stmt = m_store->Prepare(
        std::string("SELECT ") + kTaskRunCols +
        " FROM task_runs WHERE run_uuid=? "
        "ORDER BY epoch DESC, id DESC LIMIT 1");
    if (!stmt) return std::nullopt;
    stmt->BindText(1, runUuid);
    if (!stmt->Step()) return std::nullopt;
    return RowFromStatement(stmt.get());
}

std::vector<TaskRun> TaskRunStore::ListForSchedule(const std::string& scheduleId, int limit) const {
    std::vector<TaskRun> runs;
    if (limit <= 0) limit = 50;
    auto stmt = m_store->Prepare(
        std::string("SELECT ") + kTaskRunCols +
        " FROM task_runs WHERE schedule_id=? ORDER BY started_at_unix_ms DESC LIMIT ?");
    if (!stmt) return runs;
    stmt->BindText(1, scheduleId);
    stmt->BindInt64(2, limit);
    while (stmt->Step()) runs.push_back(RowFromStatement(stmt.get()));
    return runs;
}

std::vector<TaskRun> TaskRunStore::ListRecent(const std::string& agentId, int limit) const {
    std::vector<TaskRun> runs;
    if (limit <= 0) limit = 50;
    if (limit > 500) limit = 500;
    std::string sql = std::string("SELECT ") + kTaskRunCols + " FROM task_runs";
    if (!agentId.empty()) sql += " WHERE agent_id=?";
    sql += " ORDER BY started_at_unix_ms DESC LIMIT ?";
    auto stmt = m_store->Prepare(sql);
    if (!stmt) return runs;
    int bind = 1;
    if (!agentId.empty()) stmt->BindText(bind++, agentId);
    stmt->BindInt64(bind, limit);
    while (stmt->Step()) runs.push_back(RowFromStatement(stmt.get()));
    return runs;
}

} // namespace animus::kernel
