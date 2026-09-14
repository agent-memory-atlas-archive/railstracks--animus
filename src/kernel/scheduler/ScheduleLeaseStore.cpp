#include "animus_kernel/scheduler/ScheduleLeaseStore.h"
#include "animus_kernel/Log.h"

#include <chrono>
#include "animus_kernel/SchemaHelpers.h"

namespace animus::kernel {

ScheduleLeaseStore::ScheduleLeaseStore(IDataStore* store)
        : m_store(store) {}

void ScheduleLeaseStore::EnsureSchema() {
    // schema::CreateTable: PG translation (AUTOINCREMENT -> BIGSERIAL).
    schema::CreateTable(m_store, R"(
        CREATE TABLE IF NOT EXISTS schedule_leases (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            schedule_id TEXT NOT NULL,
            holder_node_id TEXT NOT NULL,
            epoch INTEGER NOT NULL,
            acquired_ms INTEGER NOT NULL,
            expires_ms INTEGER NOT NULL,
            last_renew_ms INTEGER NOT NULL
        )");
    )");
    m_store->Exec(
        "CREATE INDEX IF NOT EXISTS idx_schedule_leases_sched "
        "ON schedule_leases(schedule_id, epoch DESC, expires_ms DESC);");
}

ScheduleLease ScheduleLeaseStore::Acquire(const std::string& scheduleId,
                                          const std::string& holderNodeId,
                                          int64_t epoch, int64_t nowMs,
                                          int64_t ttlMs) {
    ScheduleLease out;
    out.schedule_id = scheduleId;
    out.holder_node_id = holderNodeId;
    out.epoch = epoch;
    out.acquired_ms = nowMs;
    out.last_renew_ms = nowMs;
    out.expires_ms = nowMs + ttlMs;

    auto q = m_store->Prepare(
        "INSERT INTO schedule_leases "
        "(schedule_id, holder_node_id, epoch, acquired_ms, expires_ms, last_renew_ms) "
        "VALUES (?,?,?,?,?,?) RETURNING id");
    if (!q) return out;
    q->BindText(1, scheduleId);
    q->BindText(2, holderNodeId);
    q->BindInt64(3, epoch);
    q->BindInt64(4, nowMs);
    q->BindInt64(5, out.expires_ms);
    q->BindInt64(6, nowMs);
    if (q->Step()) out.id = q->ColumnInt64(0);
    return out;
}

bool ScheduleLeaseStore::Renew(int64_t leaseId, int64_t nowMs, int64_t ttlMs) {
    auto q = m_store->Prepare(
        "UPDATE schedule_leases SET expires_ms = ?, last_renew_ms = ? "
        "WHERE id = ?");
    if (!q) return false;
    q->BindInt64(1, nowMs + ttlMs);
    q->BindInt64(2, nowMs);
    q->BindInt64(3, leaseId);
    if (!q->ExecDML()) return false;
    // UPDATE ... WHERE id matched nothing = row gone (replaced/wiped).
    auto v = m_store->Prepare("SELECT changes()");
    if (v && v->Step()) return v->ColumnInt64(0) > 0;
    return true;
}

bool ScheduleLeaseStore::Expire(const std::string& scheduleId) {
    // Expire every live row for the schedule at once (ops force-release):
    // keeps epochs as floors, frees the schedule for immediate takeover.
    auto q = m_store->Prepare(
        "UPDATE schedule_leases SET expires_ms = ? WHERE schedule_id = ? "
        "AND expires_ms > ?");
    if (!q) return false;
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    q->BindInt64(1, nowMs);
    q->BindText(2, scheduleId);
    q->BindInt64(3, nowMs);
    return q->ExecDML();
}

ScheduleLease ScheduleLeaseStore::RowFromStatement(IStatement* stmt) const {
    ScheduleLease l;
    l.id = stmt->ColumnInt64(0);
    l.schedule_id = stmt->ColumnText(1);
    l.holder_node_id = stmt->ColumnText(2);
    l.epoch = stmt->ColumnInt64(3);
    l.acquired_ms = stmt->ColumnInt64(4);
    l.expires_ms = stmt->ColumnInt64(5);
    l.last_renew_ms = stmt->ColumnInt64(6);
    return l;
}

std::optional<ScheduleLease> ScheduleLeaseStore::EffectiveLease(
        const std::string& scheduleId) const {
    auto q = m_store->Prepare(
        "SELECT id, schedule_id, holder_node_id, epoch, acquired_ms, "
        "expires_ms, last_renew_ms FROM schedule_leases "
        "WHERE schedule_id = ? ORDER BY epoch DESC, expires_ms DESC, id DESC "
        "LIMIT 1");
    if (!q) return std::nullopt;
    q->BindText(1, scheduleId);
    if (!q->Step()) return std::nullopt;
    return RowFromStatement(q.get());
}

std::vector<ScheduleLease> ScheduleLeaseStore::ListRows(
        const std::string& scheduleId) const {
    std::vector<ScheduleLease> out;
    std::string sql =
        "SELECT id, schedule_id, holder_node_id, epoch, acquired_ms, "
        "expires_ms, last_renew_ms FROM schedule_leases";
    if (!scheduleId.empty()) sql += " WHERE schedule_id = ?";
    sql += " ORDER BY schedule_id, epoch DESC, expires_ms DESC";
    auto q = m_store->Prepare(sql);
    if (!q) return out;
    if (!scheduleId.empty()) q->BindText(1, scheduleId);
    while (q->Step()) out.push_back(RowFromStatement(q.get()));
    return out;
}

} // namespace animus::kernel
