#include "animus_kernel/SyncStore.h"
#include "animus_kernel/IdRanges.h"
#include "animus_kernel/Log.h"
#include "animus_kernel/SchemaHelpers.h"

#include <algorithm>
#include <chrono>
#include <json/json.h>
#include <json/reader.h>
#include <sstream>

namespace animus::kernel {

using schema::CreateTable;

namespace {

constexpr const char* kUpsert = "upsert";
constexpr const char* kDelete = "delete";

bool IsAgentGlobalTable(const std::string& t) {
    for (const auto& x : AgentGlobalTables()) if (x == t) return true;
    return false;
}

// SQLite ms expression (millisecond precision; strftime('%s') is seconds).
std::string SqliteNowMs() {
    return "CAST((julianday('now') - 2440587.5) * 86400000 AS INTEGER)";
}

// The (ms, origin) stamp used by both version row and outbox row:
// - ms: max(local clock, apply_ms when applying a newer-stamped remote)
// - origin: the remote origin while applying, else the local node
// While a remote change is being applied to THIS EXACT row, stamp the
// incoming (ms, origin) EXACTLY — equal pairs are what kill echoes at their
// origin. Any other row (concurrent local writes) stamps (now, local node).
// The match is scoped to (table, row_id) so a sync batch never mis-stamps
// unrelated rows.
std::string SqliteMatchCond(const std::string& table, const std::string& rowExpr) {
    return "(SELECT apply_table FROM sync_control) = '" + table + "' "
           "AND (SELECT apply_row_id FROM sync_control) = " + rowExpr + " "
           "AND (SELECT apply_origin FROM sync_control) IS NOT NULL";
}

std::string SqliteStampMs(const std::string& table, const std::string& rowExpr) {
    return "CASE WHEN " + SqliteMatchCond(table, rowExpr) + " "
           "THEN (SELECT apply_ms FROM sync_control) "
           "ELSE " + SqliteNowMs() + " END";
}

std::string SqliteStampOrigin(const std::string& table, const std::string& rowExpr) {
    return "CASE WHEN " + SqliteMatchCond(table, rowExpr) + " "
           "THEN (SELECT apply_origin FROM sync_control) "
           "ELSE (SELECT node_id FROM sync_control) END";
}

} // namespace

SyncStore::SyncStore(IDataStore* store, uint64_t localNodeId)
    : m_store(store), m_nodeId(localNodeId) {}

bool SyncStore::EnsureSchema(std::string* error) {
    if (!m_store) { if (error) *error = "null store"; return false; }

    CreateTable(m_store, R"(
        CREATE TABLE IF NOT EXISTS sync_control (
            node_id INTEGER PRIMARY KEY,
            apply_table TEXT,
            apply_row_id INTEGER,
            apply_origin INTEGER,
            apply_ms INTEGER
        );
    )");
    CreateTable(m_store, R"(
        CREATE TABLE IF NOT EXISTS sync_outbox (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            origin_node INTEGER NOT NULL,
            table_name TEXT NOT NULL,
            row_id INTEGER NOT NULL,
            op TEXT NOT NULL,
            payload TEXT NOT NULL,
            unix_ms INTEGER NOT NULL
        );
    )");
    CreateTable(m_store, R"(
        CREATE TABLE IF NOT EXISTS sync_row_versions (
            table_name TEXT NOT NULL,
            row_id INTEGER NOT NULL,
            last_ms INTEGER NOT NULL,
            last_node INTEGER NOT NULL,
            PRIMARY KEY (table_name, row_id)
        );
    )");
    CreateTable(m_store, R"(
        CREATE TABLE IF NOT EXISTS sync_peer_state (
            peer_node INTEGER PRIMARY KEY,
            last_outbox_id INTEGER NOT NULL,
            updated_ms INTEGER NOT NULL
        );
    )");

    // Control row: local identity. Insert-if-missing (ON CONFLICT DO
    // NOTHING — native upsert on both dialects): never clobbers an in-flight
    // apply state, and node identity is expected to be stable anyway.
    {
        auto q = m_store->Prepare(
            "INSERT INTO sync_control (node_id, apply_table, apply_row_id, "
            "apply_origin, apply_ms) "
            "VALUES (?, NULL, NULL, NULL, NULL) ON CONFLICT (node_id) DO NOTHING");
        if (q) { q->BindInt64(1, (int64_t)m_nodeId); q->ExecDML(); }
    }

    const bool isPg = m_store->Dialect() == DataStoreDialect::PostgreSQL;

    if (isPg) {
        // Shared stamping functions (one per OLD/NEW shape).
        m_store->Exec(R"(
CREATE OR REPLACE FUNCTION animus_sync_upsert() RETURNS trigger AS $$
DECLARE
    ctl RECORD;
    ms BIGINT;
    origin BIGINT;
BEGIN
    SELECT * INTO ctl FROM sync_control;
    IF ctl.apply_table = TG_TABLE_NAME AND ctl.apply_row_id = NEW.id
       AND ctl.apply_origin IS NOT NULL THEN
        ms := ctl.apply_ms;
        origin := ctl.apply_origin;
    ELSE
        ms := (extract(epoch FROM clock_timestamp()) * 1000)::BIGINT;
        origin := ctl.node_id;
    END IF;
    INSERT INTO sync_row_versions (table_name, row_id, last_ms, last_node)
        VALUES (TG_TABLE_NAME, NEW.id, ms, origin)
        ON CONFLICT (table_name, row_id) DO UPDATE SET
            last_ms = EXCLUDED.last_ms, last_node = EXCLUDED.last_node;
    INSERT INTO sync_outbox (origin_node, table_name, row_id, op, payload, unix_ms)
        VALUES (origin, TG_TABLE_NAME, NEW.id, 'upsert', to_jsonb(NEW)::text, ms);
    RETURN NEW;
END $$ LANGUAGE plpgsql;)");
        m_store->Exec(R"(
CREATE OR REPLACE FUNCTION animus_sync_delete() RETURNS trigger AS $$
DECLARE
    ctl RECORD;
    ms BIGINT;
    origin BIGINT;
BEGIN
    SELECT * INTO ctl FROM sync_control;
    IF ctl.apply_table = TG_TABLE_NAME AND ctl.apply_row_id = OLD.id
       AND ctl.apply_origin IS NOT NULL THEN
        ms := ctl.apply_ms;
        origin := ctl.apply_origin;
    ELSE
        ms := (extract(epoch FROM clock_timestamp()) * 1000)::BIGINT;
        origin := ctl.node_id;
    END IF;
    INSERT INTO sync_row_versions (table_name, row_id, last_ms, last_node)
        VALUES (TG_TABLE_NAME, OLD.id, ms, origin)
        ON CONFLICT (table_name, row_id) DO UPDATE SET
            last_ms = EXCLUDED.last_ms, last_node = EXCLUDED.last_node;
    INSERT INTO sync_outbox (origin_node, table_name, row_id, op, payload, unix_ms)
        VALUES (origin, TG_TABLE_NAME, OLD.id, 'delete',
                json_build_object('id', OLD.id)::text, ms);
    RETURN OLD;
END $$ LANGUAGE plpgsql;)");
    }

    int installed = 0;
    for (const auto& table : AgentGlobalTables()) {
        std::string err;
        if (!InstallTriggersFor(table, &err)) {
            ALOG_WARNING("sync", "trigger install skipped for " << table
                         << ": " << err);
        } else {
            installed++;
        }
    }
    if (installed == 0) {
        if (error) *error = "no agent-global tables found — install after stores";
        return false;
    }
    return true;
}

std::vector<std::string> SyncStore::ReadTableColumns(const std::string& table) {
    std::vector<std::string> cols;
    if (m_store->Dialect() == DataStoreDialect::SQLite) {
        auto q = m_store->Prepare("PRAGMA table_info(" + table + ")");
        if (!q) return cols;
        while (q->Step()) cols.push_back(q->ColumnText(1));
    } else {
        auto q = m_store->Prepare(
            "SELECT column_name FROM information_schema.columns "
            "WHERE table_name = ? ORDER BY ordinal_position");
        if (!q) return cols;
        q->BindText(1, table);
        while (q->Step()) cols.push_back(q->ColumnText(0));
    }
    return cols;
}

bool SyncStore::InstallTriggersFor(const std::string& table, std::string* error) {
    if (!IsAgentGlobalTable(table)) {
        if (error) *error = "not an agent-global table";
        return false;
    }
    const auto cols = ReadTableColumns(table);
    if (cols.empty()) {
        if (error) *error = "table does not exist yet";
        return false;
    }

    if (m_store->Dialect() == DataStoreDialect::SQLite) {
        // Full-row JSON payload from the live column list.
        std::ostringstream jo;
        jo << "json_object(";
        for (size_t i = 0; i < cols.size(); ++i) {
            if (i) jo << ", ";
            jo << "'" << cols[i] << "', NEW." << cols[i];
        }
        jo << ")";
        const std::string payload = jo.str();

        // ONE timestamp per trigger firing: the outbox row is written first,
        // and the version row SELECTS its exact unix_ms back via
        // last_insert_rowid(). Two separate julianday('now') evaluations
        // (one per statement) straddle ms boundaries and break the
        // (ms, origin) tie that kills echoes at their origin.
        //
        // NOTE: INSERT ... SELECT ... ON CONFLICT requires a WHERE clause on
        // the SELECT to disambiguate the upsert clause (SQLite parser rule).
        const std::string outboxInsert =
            "INSERT INTO sync_outbox (origin_node, table_name, row_id, op, payload, unix_ms) "
            "SELECT " + SqliteStampOrigin(table, "NEW.id") + ", '" + table +
            "', NEW.id, 'upsert', " + payload + ", " +
            SqliteStampMs(table, "NEW.id") + "; ";
        const std::string versionUpsert =
            "INSERT INTO sync_row_versions (table_name, row_id, last_ms, last_node) "
            "SELECT '" + table + "', NEW.id, "
            "(SELECT unix_ms FROM sync_outbox WHERE id = last_insert_rowid()), " +
            SqliteStampOrigin(table, "NEW.id") + " WHERE NEW.id IS NOT NULL "
            "ON CONFLICT (table_name, row_id) DO UPDATE SET "
            "last_ms = excluded.last_ms, last_node = excluded.last_node; ";
        const std::string outboxInsertDel =
            "INSERT INTO sync_outbox (origin_node, table_name, row_id, op, payload, unix_ms) "
            "SELECT " + SqliteStampOrigin(table, "OLD.id") + ", '" + table +
            "', OLD.id, 'delete', json_object('id', OLD.id), " +
            SqliteStampMs(table, "OLD.id") + "; ";
        const std::string versionUpsertDel =
            "INSERT INTO sync_row_versions (table_name, row_id, last_ms, last_node) "
            "SELECT '" + table + "', OLD.id, "
            "(SELECT unix_ms FROM sync_outbox WHERE id = last_insert_rowid()), " +
            SqliteStampOrigin(table, "OLD.id") + " WHERE OLD.id IS NOT NULL "
            "ON CONFLICT (table_name, row_id) DO UPDATE SET "
            "last_ms = excluded.last_ms, last_node = excluded.last_node; ";

        // DROP+CREATE (not IF NOT EXISTS): a column added by a later
        // migration must refresh the payload column list on next boot.
        // Mirrors the PG branch.
        const std::string prefix =
            "DROP TRIGGER IF EXISTS sync_" + table + "_"; 
        const std::string ai =
            "CREATE TRIGGER sync_" + table + "_ai AFTER INSERT ON " +
            table + " BEGIN " + outboxInsert + versionUpsert + "END;";
        const std::string au =
            "CREATE TRIGGER sync_" + table + "_au AFTER UPDATE ON " +
            table + " BEGIN " + outboxInsert + versionUpsert + "END;";
        const std::string ad =
            "CREATE TRIGGER sync_" + table + "_ad AFTER DELETE ON " +
            table + " BEGIN " + outboxInsertDel + versionUpsertDel + "END;";

        if (!m_store->Exec(prefix + "ai") || !m_store->Exec(ai) ||
            !m_store->Exec(prefix + "au") || !m_store->Exec(au) ||
            !m_store->Exec(prefix + "ad") || !m_store->Exec(ad)) {
            if (error) *error = m_store->ErrMsg();
            return false;
        }
        return true;
    }

    // PostgreSQL
    if (!m_store->Exec("DROP TRIGGER IF EXISTS sync_" + table + "_ai ON " + table) ||
        !m_store->Exec("CREATE TRIGGER sync_" + table + "_ai AFTER INSERT OR UPDATE ON " +
                       table + " FOR EACH ROW EXECUTE FUNCTION animus_sync_upsert()") ||
        !m_store->Exec("DROP TRIGGER IF EXISTS sync_" + table + "_ad ON " + table) ||
        !m_store->Exec("CREATE TRIGGER sync_" + table + "_ad AFTER DELETE ON " +
                       table + " FOR EACH ROW EXECUTE FUNCTION animus_sync_delete()")) {
        if (error) *error = m_store->ErrMsg();
        return false;
    }
    return true;
}

std::vector<OutboxRecord> SyncStore::FetchOutboxSince(int64_t sinceOutboxId, int64_t limit) {
    std::vector<OutboxRecord> out;
    auto q = m_store->Prepare(
        "SELECT id, origin_node, table_name, row_id, op, payload, unix_ms "
        "FROM sync_outbox WHERE id > ? ORDER BY id ASC LIMIT ?");
    if (!q) return out;
    q->BindInt64(1, sinceOutboxId);
    q->BindInt64(2, limit);
    while (q->Step()) {
        OutboxRecord r;
        r.outbox_id = q->ColumnInt64(0);
        r.origin_node = q->ColumnInt64(1);
        r.table_name = q->ColumnText(2);
        r.row_id = q->ColumnInt64(3);
        r.op = q->ColumnText(4);
        r.payload = q->ColumnText(5);
        r.unix_ms = q->ColumnInt64(6);
        out.push_back(std::move(r));
    }
    return out;
}

int64_t SyncStore::MaxOutboxId() {
    auto q = m_store->Prepare("SELECT COALESCE(MAX(id), 0) FROM sync_outbox");
    if (!q || !q->Step()) return 0;
    return q->ColumnInt64(0);
}

int64_t SyncStore::GetPeerCursor(int64_t peerNode) {
    auto q = m_store->Prepare(
        "SELECT last_outbox_id FROM sync_peer_state WHERE peer_node = ?");
    if (!q) return 0;
    q->BindInt64(1, peerNode);
    if (!q->Step()) return 0;
    return q->ColumnInt64(0);
}

bool SyncStore::SetPeerCursor(int64_t peerNode, int64_t outboxId) {
    const std::string sql =
        "INSERT INTO sync_peer_state (peer_node, last_outbox_id, updated_ms) "
        "VALUES (?,?,?) ON CONFLICT (peer_node) DO UPDATE SET "
        "last_outbox_id = excluded.last_outbox_id, updated_ms = excluded.updated_ms";
    auto q = m_store->Prepare(sql);
    if (!q) return false;
    q->BindInt64(1, peerNode);
    q->BindInt64(2, outboxId);
    using namespace std::chrono;
    q->BindInt64(3, duration_cast<milliseconds>(
        system_clock::now().time_since_epoch()).count());
    return q->ExecDML();
}

bool SyncStore::ApplyRemoteChange(const OutboxRecord& rec) {
    // Whitelist the table name (identifier injection guard).
    if (!IsAgentGlobalTable(rec.table_name)) {
        ALOG_WARNING("sync", "apply rejected — unknown table: " << rec.table_name);
        return false;
    }
    if (rec.op != kUpsert && rec.op != kDelete) {
        ALOG_WARNING("sync", "apply rejected — unknown op: " << rec.op);
        return false;
    }

    // LWW: (ms, node) pair-compare against the row's current version.
    int64_t localMs = -1, localNode = -1;
    {
        auto q = m_store->Prepare(
            "SELECT last_ms, last_node FROM sync_row_versions "
            "WHERE table_name = ? AND row_id = ?");
        if (q) {
            q->BindText(1, rec.table_name);
            q->BindInt64(2, rec.row_id);
            if (q->Step()) {
                localMs = q->ColumnInt64(0);
                localNode = q->ColumnInt64(1);
            }
        }
    }
    const std::pair<int64_t, int64_t> incoming(rec.unix_ms, rec.origin_node);
    const std::pair<int64_t, int64_t> local(localMs, localNode);
    if (incoming <= local) {
        ALOG_DEBUG("sync", "skip " << rec.table_name << "/" << rec.row_id
                   << " — stale/echo (in " << rec.unix_ms << "/" << rec.origin_node
                   << " vs local " << localMs << "/" << localNode << ")");
        return false;   // stale, echo, or exact tie — all safe to skip
    }

    // Publish apply context so triggers stamp the incoming (ms, origin)
    // EXACTLY on this row — echoes then tie everywhere and die.
    {
        auto q = m_store->Prepare(
            "UPDATE sync_control SET apply_table = ?, apply_row_id = ?, "
            "apply_origin = ?, apply_ms = ?");
        if (q) {
            q->BindText(1, rec.table_name);
            q->BindInt64(2, rec.row_id);
            q->BindInt64(3, rec.origin_node);
            q->BindInt64(4, rec.unix_ms);
            q->ExecDML();
        }
    }
    bool ok = (rec.op == kDelete) ? ApplyDelete(rec) : ApplyUpsert(rec);
    {
        auto q = m_store->Prepare(
            "UPDATE sync_control SET apply_table = NULL, apply_row_id = NULL, "
            "apply_origin = NULL, apply_ms = NULL");
        if (q) q->ExecDML();
    }
    if (!ok) ALOG_WARNING("sync", "apply FAILED for " << rec.table_name
                          << "/" << rec.row_id << ": " << m_store->ErrMsg());
    return ok;
}

bool SyncStore::ApplyDelete(const OutboxRecord& rec) {
    auto q = m_store->Prepare("DELETE FROM " + rec.table_name + " WHERE id = ?");
    if (!q) return false;
    q->BindInt64(1, rec.row_id);
    return q->ExecDML();
}

bool SyncStore::ApplyUpsert(const OutboxRecord& rec) {
    Json::Value root;
    Json::CharReaderBuilder rb;
    std::string parseErr;
    std::istringstream ss(rec.payload);
    if (!Json::parseFromStream(rb, ss, &root, &parseErr)) {
        ALOG_WARNING("sync", "payload parse failed for " << rec.table_name
                     << "/" << rec.row_id << ": " << parseErr);
        return false;
    }
    if (!root.isObject() || !root.isMember("id")) {
        ALOG_WARNING("sync", "payload missing id for " << rec.table_name);
        return false;
    }

    // Bind only columns that exist in BOTH the payload and the live table —
    // mixed-version networks with extra/missing columns degrade gracefully.
    const auto tableCols = ReadTableColumns(rec.table_name);
    std::vector<std::string> cols;
    for (const auto& c : tableCols) if (root.isMember(c)) cols.push_back(c);
    if (cols.empty()) return false;

    std::ostringstream ins, ph, upd;
    ins << "INSERT INTO " << rec.table_name << " (";
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i) { ins << ", "; ph << ", "; }
        ins << cols[i];
        ph << "?";
    }
    ins << ") VALUES (" << ph.str() << ") ON CONFLICT (id) DO UPDATE SET ";
    bool first = true;
    for (const auto& c : cols) {
        if (c == "id") continue;   // conflict key is never updatable (SQLite rejects)
        if (!first) upd << ", ";
        first = false;
        upd << c << " = excluded." << c;
    }
    ins << upd.str();

    auto q = m_store->Prepare(ins.str());
    if (!q) return false;
    for (size_t i = 0; i < cols.size(); ++i) {
        const Json::Value& v = root[cols[i]];
        const int idx = (int)i + 1;
        if (v.isNull()) q->BindNull(idx);
        else if (v.isIntegral()) q->BindInt64(idx, v.asInt64());
        else if (v.isNumeric()) q->BindDouble(idx, v.asDouble());
        else if (v.isString()) q->BindText(idx, v.asString());
        else if (v.isBool()) q->BindInt64(idx, v.asBool() ? 1 : 0);
        else q->BindText(idx, v.asString());
    }
    return q->ExecDML();
}

} // namespace animus::kernel
