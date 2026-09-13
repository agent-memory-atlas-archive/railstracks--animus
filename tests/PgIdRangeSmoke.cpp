// PG-path smoke test for #78 P1a id-range seeding (SQLite is covered by
// NodeIdRangeTests). Runs ONLY when ANIMUS_PG_SMOKE is set to
// "host:port:database:user:password" and points at a THROWAWAY database —
// the test creates/drops tables freely. Skips (exit 0) otherwise, so CI
// and PG-less environments are unaffected.
#include "animus_kernel/PgDataStore.h"
#include "animus_kernel/IdRanges.h"
#include "animus_kernel/Log.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int main() {
    const char* conn = std::getenv("ANIMUS_PG_SMOKE");
    if (!conn || !*conn) {
        std::cout << "ANIMUS_PG_SMOKE unset — skipping PG smoke\n";
        return 0;
    }
    std::vector<std::string> parts;
    std::istringstream ss(conn);
    std::string p;
    while (std::getline(ss, p, ':')) parts.push_back(p);
    if (parts.size() < 5) {
        std::cerr << "ANIMUS_PG_SMOKE must be host:port:db:user:pass\n";
        return 2;
    }

    animus::kernel::PgDataStore::Config cfg;
    cfg.host = parts[0];
    cfg.port = std::stoi(parts[1]);
    cfg.database = parts[2];
    cfg.username = parts[3];
    cfg.password = parts[4];
    animus::kernel::PgDataStore store(cfg);

    // Legacy-schema simulation: create ONE table with int4 SERIAL (what the
    // pre-BIGSERIAL translator produced) to prove the widening path; the
    // rest are born BIGSERIAL (post-fix translator output).
    if (!store.Exec("CREATE TABLE IF NOT EXISTS observations "
                    "(id SERIAL PRIMARY KEY, payload TEXT NOT NULL DEFAULT 'x')")) {
        std::cerr << "create legacy table failed: " << store.ErrMsg() << "\n";
        return 1;
    }
    for (const auto& t : animus::kernel::AgentGlobalTables()) {
        if (t == "observations") continue;
        if (!store.Exec("CREATE TABLE IF NOT EXISTS " + t +
                        " (id BIGSERIAL PRIMARY KEY, payload TEXT NOT NULL DEFAULT 'x')")) {
            std::cerr << "create table failed: " << t << ": " << store.ErrMsg() << "\n";
            return 1;
        }
    }
    // single-node-era data at small id
    if (!store.Exec("INSERT INTO observations (payload) VALUES ('legacy')")) return 1;

    int failures = 0;
    auto check = [&](bool ok, const std::string& msg) {
        if (!ok) { std::cerr << "  ASSERT FAILED: " << msg << "\n"; failures++; }
    };

    std::string err;
    const int seeded = animus::kernel::SeedAgentGlobalIdRanges(&store, 7, &err);
    check(seeded == (int)animus::kernel::AgentGlobalTables().size(),
          "all tables seeded, got " + std::to_string(seeded) + " err=" + err);

    auto q = store.Prepare(
        "INSERT INTO observations (payload) VALUES ('fed') RETURNING id");
    check(q && q->Step(), "fed insert has RETURNING row");
    const int64_t id1 = q ? q->ColumnInt64(0) : 0;
    check(id1 > ((int64_t)7 << 40),
          "post-seed id in node-7 range, got " + std::to_string(id1));

    animus::kernel::SeedAgentGlobalIdRanges(&store, 2, &err);  // lower — must not regress
    auto q2 = store.Prepare(
        "INSERT INTO observations (payload) VALUES ('fed2') RETURNING id");
    check(q2 && q2->Step(), "second insert has RETURNING row");
    const int64_t id2 = q2 ? q2->ColumnInt64(0) : 0;
    check(id2 == id1 + 1,
          "raise-only: contiguous ids, got " + std::to_string(id1) + " then " +
          std::to_string(id2));

    std::cout << (failures ? "PG SMOKE FAILED\n" : "PG SMOKE PASSED\n");
    return failures ? 1 : 0;
}
