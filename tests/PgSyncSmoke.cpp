// PG-path smoke test for #78 P1b outbox sync (SQLite is covered by
// OutboxSyncTests). Runs ONLY when ANIMUS_PG_SYNC_SMOKE is set to
// "host:port:user:password" (two scratch databases are created+dropped:
// animus_p1b_a / animus_p1b_b). Skips (exit 0) otherwise.
#include "animus_kernel/PgDataStore.h"
#include "animus_kernel/IdRanges.h"
#include "animus_kernel/SyncStore.h"
#include "animus_kernel/Log.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace animus::kernel;

namespace {

int g_failures = 0;
void Check(bool ok, const std::string& msg) {
    if (!ok) { std::cerr << "  ASSERT FAILED: " << msg << "\n"; g_failures++; }
}

PgDataStore::Config ParseConn(const std::string& spec, const std::string& db) {
    std::vector<std::string> parts;
    std::istringstream ss(spec);
    std::string p;
    while (std::getline(ss, p, ':')) parts.push_back(p);
    PgDataStore::Config cfg;
    cfg.host = parts[0];
    cfg.port = std::stoi(parts[1]);
    cfg.username = parts[2];
    cfg.password = parts[3];
    cfg.database = db;
    return cfg;
}

} // namespace

int main() {
    const char* conn = std::getenv("ANIMUS_PG_SYNC_SMOKE");
    if (!conn || !*conn) {
        std::cout << "ANIMUS_PG_SYNC_SMOKE unset — skipping PG sync smoke\n";
        return 0;
    }

    PgDataStore admin(ParseConn(conn, "postgres"));
    admin.Exec("DROP DATABASE IF EXISTS animus_p1b_a");
    admin.Exec("DROP DATABASE IF EXISTS animus_p1b_b");
    if (!admin.Exec("CREATE DATABASE animus_p1b_a") ||
        !admin.Exec("CREATE DATABASE animus_p1b_b")) {
        std::cerr << "scratch db create failed: " << admin.ErrMsg() << "\n";
        return 2;
    }

    PgDataStore dbA(ParseConn(conn, "animus_p1b_a"));
    PgDataStore dbB(ParseConn(conn, "animus_p1b_b"));

    // Minimal agent-global tables (BIGSERIAL = post-fix translator output).
    // Missing agent-global tables are skipped by EnsureSchema with a warning.
    for (auto* db : {&dbA, &dbB}) {
        db->Exec("CREATE TABLE memory_layers (id BIGSERIAL PRIMARY KEY, "
                 "agent_id TEXT NOT NULL DEFAULT 'default', name TEXT NOT NULL, "
                 "horizon TEXT NOT NULL DEFAULT '', sort_order INTEGER NOT NULL DEFAULT 0)");
        db->Exec("CREATE TABLE observations (id BIGSERIAL PRIMARY KEY, "
                 "layer_id BIGINT NOT NULL, agent_id TEXT NOT NULL DEFAULT 'default', "
                 "text TEXT NOT NULL DEFAULT '')");
    }

    std::string err;
    // P1a + P1b together: node-scoped ids + trigger-based outbox.
    Check(SeedAgentGlobalIdRanges(&dbA, 1, &err) == 10, "node 1 ranges seeded");
    Check(SeedAgentGlobalIdRanges(&dbB, 2, &err) == 10, "node 2 ranges seeded");
    SyncStore syncA(&dbA, 1), syncB(&dbB, 2);
    Check(syncA.EnsureSchema(&err), "A sync schema: " + err);
    Check(syncB.EnsureSchema(&err), "B sync schema: " + err);

    // Node A writes.
    {
        auto q = dbA.Prepare("INSERT INTO memory_layers (name) VALUES ('pg-layer') RETURNING id");
        Check(q && q->Step(), "A layer insert");
        const int64_t layerId = q ? q->ColumnInt64(0) : 0;
        Check(layerId > ((int64_t)1 << 40), "A id in node-1 range");
        auto q2 = dbA.Prepare(
            "INSERT INTO observations (layer_id, agent_id, text) "
            "VALUES (?, 'ag', 'pg-v1') RETURNING id");
        q2->BindInt64(1, layerId);
        Check(q2 && q2->Step(), "A observation insert");
        const int64_t obsId = q2 ? q2->ColumnInt64(0) : 0;
        Check(obsId > ((int64_t)1 << 40), "obs id in node-1 range");

        auto out = syncA.FetchOutboxSince(0, 1000);
        Check(out.size() == 2, "A outbox has 2 records, got " + std::to_string(out.size()));
        Check(!out.empty() && out.back().origin_node == 1, "origin node 1");
        Check(!out.empty() && out.back().payload.find("pg-v1") != std::string::npos,
              "payload carries to_jsonb row");

        // B pulls and applies both.
        int applied = 0;
        for (const auto& r : out) if (syncB.ApplyRemoteChange(r)) applied++;
        Check(applied == 2, "B applied both records, got " + std::to_string(applied));
        auto qb = dbB.Prepare("SELECT text FROM observations WHERE id = ?");
        qb->BindInt64(1, obsId);
        Check(qb && qb->Step() && qb->ColumnText(0) == "pg-v1", "row replicated to B");

        // Echo death: B's outbox echoes carry origin 1; A skips them all.
        auto echoes = syncB.FetchOutboxSince(0, 1000);
        Check(!echoes.empty(), "B produced echoes");
        int aApplied = 0;
        for (const auto& r : echoes) if (syncA.ApplyRemoteChange(r)) aApplied++;
        Check(aApplied == 0, "echoes die at origin (PG), applied " + std::to_string(aApplied));

        // Update + delete propagate.
        dbA.Exec("UPDATE observations SET text = 'pg-v2' WHERE id = " + std::to_string(obsId));
        auto upd = syncA.FetchOutboxSince(syncA.MaxOutboxId() - 1, 10);
        Check(!upd.empty(), "update captured");
        bool updApplied = false;
        for (const auto& r : upd) if (syncB.ApplyRemoteChange(r)) updApplied = true;
        Check(updApplied, "B applied update");
        {
            auto qv = dbB.Prepare("SELECT text FROM observations WHERE id = ?");
            qv->BindInt64(1, obsId);
            Check(qv && qv->Step() && qv->ColumnText(0) == "pg-v2", "v2 on B");
        }
        dbA.Exec("DELETE FROM observations WHERE id = " + std::to_string(obsId));
        auto del = syncA.FetchOutboxSince(syncA.MaxOutboxId() - 1, 10);
        bool delApplied = false;
        for (const auto& r : del) if (syncB.ApplyRemoteChange(r)) delApplied = true;
        Check(delApplied, "B applied delete");
        {
            auto qd = dbB.Prepare("SELECT 1 FROM observations WHERE id = ?");
            qd->BindInt64(1, obsId);
            Check(qd && !qd->Step(), "row gone from B");
        }
    }

    // Tear down: kill pool connections first, then drop the scratch DBs.
    admin.Exec("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
               "WHERE datname IN ('animus_p1b_a','animus_p1b_b')");
    admin.Exec("DROP DATABASE IF EXISTS animus_p1b_a");
    admin.Exec("DROP DATABASE IF EXISTS animus_p1b_b");
    std::cout << (g_failures ? "PG SYNC SMOKE FAILED\n" : "PG SYNC SMOKE PASSED\n");
    return g_failures ? 1 : 0;
}
