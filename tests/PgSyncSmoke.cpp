// PG-path smoke test for #78 P1b outbox sync (SQLite is covered by
// OutboxSyncTests). Runs ONLY when ANIMUS_PG_SYNC_SMOKE is set to
// "host:port:user:password" (two scratch databases are created+dropped:
// animus_p1b_a / animus_p1b_b). Skips (exit 0) otherwise.
#include "animus_kernel/PgDataStore.h"
#include "animus_kernel/IdRanges.h"
#include "animus_kernel/SyncStore.h"
#include "animus_kernel/AgentConfigStore.h"
#include "animus_kernel/api/SecretsVault.h"
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
    admin.Exec("DROP DATABASE IF EXISTS animus_p1b_c");
    admin.Exec("DROP DATABASE IF EXISTS animus_p1b_d");
    if (!admin.Exec("CREATE DATABASE animus_p1b_a") ||
        !admin.Exec("CREATE DATABASE animus_p1b_b") ||
        !admin.Exec("CREATE DATABASE animus_p1b_c") ||
        !admin.Exec("CREATE DATABASE animus_p1b_d")) {
        std::cerr << "scratch db create failed: " << admin.ErrMsg() << "\n";
        return 2;
    }

    PgDataStore dbA(ParseConn(conn, "animus_p1b_a"));
    PgDataStore dbB(ParseConn(conn, "animus_p1b_b"));
    PgDataStore dbC(ParseConn(conn, "animus_p1b_c"));
    PgDataStore dbD(ParseConn(conn, "animus_p1b_d"));

    // Minimal agent-global tables (BIGSERIAL = post-fix translator output).
    // Missing agent-global tables are skipped by EnsureSchema with a warning.
    for (auto* db : {&dbA, &dbB, &dbC, &dbD}) {
        db->Exec("CREATE TABLE memory_layers (id BIGSERIAL PRIMARY KEY, "
                 "agent_id TEXT NOT NULL DEFAULT 'default', name TEXT NOT NULL, "
                 "horizon TEXT NOT NULL DEFAULT '', sort_order INTEGER NOT NULL DEFAULT 0)");
        db->Exec("CREATE TABLE observations (id BIGSERIAL PRIMARY KEY, "
                 "layer_id BIGINT NOT NULL, agent_id TEXT NOT NULL DEFAULT 'default', "
                 "text TEXT NOT NULL DEFAULT '')");
        // #93 P3: composite-key table (pair shape) — the PG trigger
        // functions must take the shape argument and branch on it
        // (PR #109 audit F1: zero-arg functions + argumented call =
        // no triggers installed at all on PG).
        db->Exec("CREATE TABLE agent_config (agent_id TEXT NOT NULL, "
                 "key TEXT NOT NULL, value TEXT NOT NULL DEFAULT '', "
                 "updated_at TEXT NOT NULL DEFAULT (now()::text), "
                 "PRIMARY KEY (agent_id, key))");   // matches store schema: Set writes updated_at
        // #93 P3 slice 3: the vault table joins the fixture (ciphertext
        // replicates; the master key does NOT — verified nodes share it).
        db->Exec("CREATE TABLE api_package_secrets (id TEXT PRIMARY KEY, "
                 "package_id TEXT NOT NULL, name TEXT NOT NULL, "
                 "ciphertext TEXT NOT NULL, created_at_unix_ms BIGINT NOT NULL, "
                 "updated_at_unix_ms BIGINT NOT NULL, "
                 "UNIQUE (package_id, name))");
    }

    std::string err;
    // P1a + P1b together: node-scoped ids + trigger-based outbox.
    const int seededA = SeedAgentGlobalIdRanges(&dbA, 1, &err);
    Check(seededA >= 2, "node 1 ranges seeded (got " + std::to_string(seededA) + ")");
    const int seededB = SeedAgentGlobalIdRanges(&dbB, 2, &err);
    Check(seededB >= 2, "node 2 ranges seeded (got " + std::to_string(seededB) + ")");
    const int seededC = SeedAgentGlobalIdRanges(&dbC, 3, &err);
    Check(seededC >= 2, "node 3 ranges seeded (got " + std::to_string(seededC) + ")");
    const int seededD = SeedAgentGlobalIdRanges(&dbD, 4, &err);
    Check(seededD >= 2, "node 4 ranges seeded (got " + std::to_string(seededD) + ")");
    SyncStore syncA(&dbA, 1), syncB(&dbB, 2), syncC(&dbC, 3), syncD(&dbD, 4);
    Check(syncA.EnsureSchema(&err), "A sync schema: " + err);
    Check(syncB.EnsureSchema(&err), "B sync schema: " + err);
    Check(syncC.EnsureSchema(&err), "C sync schema: " + err);
    Check(syncD.EnsureSchema(&err), "D sync schema: " + err);

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

    // ── #93 P3 / PR #109 audit: composite-key table on PG ──────────────
    // agent_config uses the 'pair' shape. A fresh EnsureSchema with the
    // pre-fix zero-arg trigger functions installs NOTHING on PG — so this
    // block doubles as the boot-time trigger-installation regression test
    // for the audit's coverage gap.
    {
        std::cerr << "  [P3] agent_config pair-shape replication (PG)\n";
        dbA.Exec("INSERT INTO agent_config (agent_id, key, value) "
                 "VALUES ('default', 'channel.t.type', 'telegram')");
        dbA.Exec("INSERT INTO agent_config (agent_id, key, value) "
                 "VALUES ('default', 'channel.t.config', '{\"n\":1}')");
        auto out = syncA.FetchOutboxSince(syncA.MaxOutboxId() - 2, 10);
        Check(out.size() == 2, "pair upserts captured on A, got " +
                                   std::to_string(out.size()));
        if (!out.empty()) {
            Check(out.front().row_key == "default\x1f" "channel.t.type" ||
                  out.back().row_key == "default\x1f" "channel.t.type",
                  "pair row_key is escaped agent_id\\x1Fkey, got '" +
                      out.front().row_key + "'");
        }
        int applied = 0;
        for (const auto& r : out) if (syncB.ApplyRemoteChange(r)) applied++;
        Check(applied == 2, "B applied pair rows, got " +
                                std::to_string(applied));
        {
            auto q = dbB.Prepare("SELECT value FROM agent_config "
                                 "WHERE agent_id = 'default' AND key = 'channel.t.type'");
            Check(q && q->Step() && q->ColumnText(0) == "telegram",
                  "pair row replicated to B");
        }

        // Same-ms update + delete (the LWW tie defect class, PG path):
        // one statement updates, the next deletes. Even if both land in
        // the same millisecond, the delete must win on B (tie + delete +
        // origin != local applies).
        dbA.Exec("UPDATE agent_config SET value = 'irc' "
                 "WHERE agent_id = 'default' AND key = 'channel.t.type'");
        dbA.Exec("DELETE FROM agent_config "
                 "WHERE agent_id = 'default' AND key = 'channel.t.type'");
        auto tail = syncA.FetchOutboxSince(syncA.MaxOutboxId() - 2, 10);
        int tailApplied = 0;
        for (const auto& r : tail) if (syncB.ApplyRemoteChange(r)) tailApplied++;
        Check(tailApplied >= 1, "B applied at least the delete, got " +
                                     std::to_string(tailApplied));
        {
            auto q = dbB.Prepare("SELECT COUNT(*) FROM agent_config "
                                 "WHERE agent_id = 'default' AND key = 'channel.t.type'");
            Check(q && q->Step() && q->ColumnInt64(0) == 0,
                  "same-ms update+delete: row gone from B (delete wins)");
        }

        // Delete payload carries the pair, not a bogus id key.
        for (const auto& r : tail) {
            if (r.op == "delete") {
                Check(r.payload.find("agent_id") != std::string::npos &&
                          r.payload.find("channel.t.type") != std::string::npos,
                      "delete payload carries composite key, got '" + r.payload + "'");
            }
        }

        // Pair digests work on PG (chr(31) — char(31) is a type there).
        auto digests = syncA.TableDigests();
        bool sawConfig = false;
        for (const auto& d : digests) if (d.table == "agent_config") sawConfig = true;
        Check(sawConfig, "agent_config present in PG digests");
    }

    // ── #93 P3 slice 3: provider config + vaulted key replicate ────────
    // The scratch-node story in miniature: both nodes share one vault
    // master key (verified-node key distribution is manual, per #93); A
    // holds provider config with a vaulted api_key; a scratch B pulls
    // everything and resolves the key with the replicated ciphertext.
    {
        std::cerr << "  [P3-s3] provider config replicates (vaulted key)\n";
        const std::string sharedKeyPath = "/tmp/animus_pgsmoke_shared.key";
        SecretsVault vaultA(&dbA, sharedKeyPath);
        vaultA.EnsureSchema();
        SecretsVault vaultB(&dbB, sharedKeyPath);
        vaultB.EnsureSchema();   // loads the same key file
        AgentConfigStore cfgA(&dbA);
        cfgA.SetVault(&vaultA);
        AgentConfigStore cfgB(&dbB);
        cfgB.SetVault(&vaultB);

        cfgA.Set("__providers", "cfg.zai",
                 "{\"provider_type\":\"zai\",\"auth_type\":\"api_key\",\"concurrency\":2}");
        cfgA.Set("__providers", "cfg.zai.api_key", "sk-pg-live-42");
        cfgA.Set("__providers", "__default", "zai");

        auto out = syncA.FetchOutboxSince(0, 1000);
        int applied = 0, secretRows = 0;
        for (const auto& r : out) {
            if (!syncB.ApplyRemoteChange(r)) continue;
            applied++;
            if (r.table_name == "api_package_secrets") secretRows++;
        }
        Check(applied >= 3, "scratch join applied provider rows, got " +
                                   std::to_string(applied));
        Check(secretRows >= 1, "vault ciphertext replicated to B");
        Check(cfgB.Get("__providers", "cfg.zai.api_key") == "sk-pg-live-42",
              "api_key resolves on B (shared key decrypts replicated ciphertext)");
        Check(cfgB.GetRaw("__providers", "cfg.zai.api_key").find("secret_ref") !=
                  std::string::npos,
              "B's row still carries a ref, not plaintext");
    }

    // ── #93 wrap-up: N>2 validation + mixed-version grace ───────────────
    // Acceptance sketch in miniature on live PG, chain topology A→B→C→D:
    // echo-propagated fan-out, node-kill catch-up, scratch-join, and the
    // defer-don't-block policy for unknown shapes.
    {
        std::cerr << "  [P3-N3] three-node validation\n";

        // Phase 1 — fan-out A→B→C via B's echo (C never pulls A).
        const int64_t aBefore = syncA.MaxOutboxId();
        dbA.Exec("INSERT INTO agent_config (agent_id, key, value) "
                 "VALUES ('default', 'channel.n3.type', 'telegram')");
        int viaEcho = 0;
        for (const auto& r : syncA.FetchOutboxSince(aBefore, 10)) {
            if (syncB.ApplyRemoteChange(r) && r.origin_node == 1) viaEcho++;
        }
        Check(viaEcho == 1, "B applied A's fan-out write, got " +
                                std::to_string(viaEcho));
        int cGot = 0;
        for (const auto& r : syncB.FetchOutboxSince(0, 1000)) {
            if (syncC.ApplyRemoteChange(r) && r.table_name == "agent_config" &&
                r.row_key.find("channel.n3.type") != std::string::npos) cGot++;
        }
        Check(cGot >= 1, "C received A's row via B's echo (chain propagation)");
        {
            auto q = dbC.Prepare("SELECT value FROM agent_config "
                                 "WHERE agent_id='default' AND key='channel.n3.type'");
            Check(q && q->Step() && std::string(q->ColumnText(0)) == "telegram",
                  "fan-out row landed on C");
            if (q) q->Finalize();
        }

        // Phase 2 — node kill + catch-up from cursor.
        const int64_t bCursor = syncB.MaxOutboxId();  // B's tail; C consumed up to here
        // C goes dark; A and B keep working.
        const int64_t aBefore2 = syncA.MaxOutboxId();
        dbA.Exec("INSERT INTO agent_config (agent_id, key, value) "
                 "VALUES ('default', 'channel.n3.after_kill', '1')");
        for (const auto& r : syncA.FetchOutboxSince(aBefore2, 10))
            syncB.ApplyRemoteChange(r);
        {
            auto q = dbC.Prepare("SELECT COUNT(*) FROM agent_config "
                                 "WHERE key='channel.n3.after_kill'");
            long n = -1;
            if (q && q->Step()) n = q->ColumnInt64(0);
            Check(n == 0, "dark C misses the write, as it should");
            if (q) q->Finalize();
        }
        int caught = 0;
        for (const auto& r : syncB.FetchOutboxSince(bCursor, 100))
            if (syncC.ApplyRemoteChange(r)) caught++;
        Check(caught >= 1, "returned C caught up from its cursor, got " +
                               std::to_string(caught));
        {
            auto q = dbC.Prepare("SELECT value FROM agent_config "
                                 "WHERE agent_id='default' AND key='channel.n3.after_kill'");
            Check(q && q->Step() && std::string(q->ColumnText(0)) == "1",
                  "missed write replayed on C");
            if (q) q->Finalize();
        }

        // Phase 3 — scratch-join: empty node D pulls EVERYTHING from B
        // (echo carries channels, providers, vault ciphertext).
        int dApplied = 0, dFailed = 0, dSecrets = 0;
        for (const auto& r : syncB.FetchOutboxSince(0, 2000)) {
            if (syncD.ApplyRemoteChange(r)) { dApplied++; if (r.table_name == "api_package_secrets") dSecrets++; }
            else dFailed++;
        }
        Check(dFailed == 0, "scratch D applied everything cleanly, failed=" +
                                std::to_string(dFailed));
        Check(dSecrets >= 1, "vault ciphertext reached scratch D");
        {
            AgentConfigStore cfgD(&dbD);
            SecretsVault vaultD(&dbD, "/tmp/animus_pgsmoke_shared.key");
            vaultD.EnsureSchema();  // same shared key as A/B
            cfgD.SetVault(&vaultD);
            Check(cfgD.Get("default", "channel.n3.type") == "telegram",
                  "scratch D rebuilt channel config from peers");
            Check(cfgD.Get("__providers", "cfg.zai.api_key") == "sk-pg-live-42",
                  "scratch D rebuilt provider config + vault secret from peers");
        }

        // Phase 4 — mixed-version grace: unknown table + unknown column.
        {
            std::cerr << "  [P3-mixed] mixed-version grace\n";
            OutboxRecord future;
            future.table_name = "future_widgets";   // peer runs a newer schema
            future.row_key = "42";
            future.op = "upsert";
            future.payload = "{\"id\":\"42\",\"name\":\"widget\"}";
            future.unix_ms = 1;
            future.origin_node = 9;
            const bool rejected = !syncC.ApplyRemoteChange(future);
            Check(rejected, "unknown table rejected by apply");
            // Sync continues: a known record right after still applies.
            OutboxRecord known;
            known.table_name = "agent_config";
            known.row_key = "default" "\x1f" "channel.mixed";  // native pair format
            known.op = "upsert";
            known.payload = "{\"agent_id\":\"default\",\"key\":\"channel.mixed\","
                            "\"value\":\"ok\",\"future_field\":\"ignored\"}";
            known.unix_ms = 2;
            known.origin_node = 9;
            Check(syncC.ApplyRemoteChange(known),
                  "known record applies after unknown-table rejection (no poison)");
            auto qm = dbC.Prepare("SELECT value FROM agent_config "
                                  "WHERE agent_id='default' AND key='channel.mixed'");
            Check(qm && qm->Step() && std::string(qm->ColumnText(0)) == "ok",
                  "extra payload column dropped by column intersection");
            if (qm) qm->Finalize();
        }
    }

    // Tear down: kill pool connections first, then drop the scratch DBs.
    admin.Exec("SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
               "WHERE datname IN ('animus_p1b_a','animus_p1b_b','animus_p1b_c','animus_p1b_d')");
    admin.Exec("DROP DATABASE IF EXISTS animus_p1b_a");
    admin.Exec("DROP DATABASE IF EXISTS animus_p1b_b");
    admin.Exec("DROP DATABASE IF EXISTS animus_p1b_c");
    admin.Exec("DROP DATABASE IF EXISTS animus_p1b_d");
    std::cout << (g_failures ? "PG SYNC SMOKE FAILED\n" : "PG SYNC SMOKE PASSED\n");
    return g_failures ? 1 : 0;
}
