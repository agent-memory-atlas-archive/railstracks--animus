#include "animus_kernel/scheduler/TaskRunStore.h"

#include "animus_kernel/Log.h"

#include <algorithm>

namespace animus::kernel {

static const char* kTaskRunCols =
    "id, run_uuid, schedule_id, agent_id, node_id, triggered_by, scheduled_for, "
    "started_at_unix_ms, finished_at_unix_ms, outcome, error";

TaskRunStore::TaskRunStore(IDataStore* store) : m_store(store) {}

void TaskRunStore::EnsureSchema() {
    m_store->Exec(R"(
        CREATE TABLE IF NOT EXISTS task_runs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            run_uuid TEXT NOT NULL UNIQUE,
            schedule_id TEXT NOT NULL,
            agent_id TEXT NOT NULL DEFAULT '',
            node_id TEXT NOT NULL DEFAULT '',
            triggered_by TEXT NOT NULL DEFAULT 'schedule',
            scheduled_for TEXT NOT NULL DEFAULT '',
            started_at_unix_ms INTEGER NOT NULL,
            finished_at_unix_ms INTEGER,
            outcome TEXT NOT NULL DEFAULT 'running',
            error TEXT NOT NULL DEFAULT ''
        );
    )");
    m_store->Exec(
        "CREATE INDEX IF NOT EXISTS idx_task_runs_schedule "
        "ON task_runs(schedule_id, started_at_unix_ms DESC);");
    m_store->Exec(
        "CREATE INDEX IF NOT EXISTS idx_task_runs_agent "
        "ON task_runs(agent_id, started_at_unix_ms DESC);");
}

bool TaskRunStore::TryClaim(const std::string& runUuid, const std::string& scheduleId,
                            const std::string& agentId, const std::string& scheduledFor,
                            int64_t startedAtUnixMs) {
    // Claim detection without any store-global state (#76 lesson):
    // the RETURNING row IS the receipt — a conflict produces no row.
    auto stmt = m_store->Prepare(
        "INSERT INTO task_runs (run_uuid, schedule_id, agent_id, scheduled_for, "
        "started_at_unix_ms) VALUES (?,?,?,?,?) "
        "ON CONFLICT(run_uuid) DO NOTHING RETURNING id");
    if (!stmt) return false;

    stmt->BindText(1, runUuid);
    stmt->BindText(2, scheduleId);
    stmt->BindText(3, agentId);
    stmt->BindText(4, scheduledFor);
    stmt->BindInt64(5, startedAtUnixMs);

    if (!stmt->Step()) {
        ALOG_DEBUG("scheduler", "task_run claim lost (already processed): " << runUuid);
        return false;
    }
    return true;
}

bool TaskRunStore::Finish(const std::string& runUuid, const std::string& outcome,
                          const std::string& error, int64_t finishedAtUnixMs) {
    auto stmt = m_store->Prepare(
        "UPDATE task_runs SET outcome=?, error=?, finished_at_unix_ms=? "
        "WHERE run_uuid=?");
    if (!stmt) return false;
    stmt->BindText(1, outcome);
    stmt->BindText(2, error);
    stmt->BindInt64(3, finishedAtUnixMs);
    stmt->BindText(4, runUuid);
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
    return run;
}

std::optional<TaskRun> TaskRunStore::GetByUuid(const std::string& runUuid) const {
    auto stmt = m_store->Prepare(
        std::string("SELECT ") + kTaskRunCols +
        " FROM task_runs WHERE run_uuid=?");
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
