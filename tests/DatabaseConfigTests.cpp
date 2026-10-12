// DatabaseConfigTests — #96: db.json application precedence. Covers the
// explicit-flag contract (CLI wins), the sqlite.path application that the
// old "state/memory.db" sentinel silently dropped (ResolvePaths had already
// resolved the default to <dataDir>/memory.db before ApplyTo ran), and the
// key-absent no-stomp behavior.

#include "animus_kernel/DatabaseConfig.h"
#include "animus_kernel/KernelConfig.h"

#include <iostream>
#include <string>

using namespace animus::kernel;

namespace {

int g_failures = 0;

void Assert(bool condition, const std::string& msg) {
    if (!condition) {
        std::cerr << "  ASSERT FAILED: " << msg << "\n";
        g_failures++;
    }
}

int TestSqlitePathPrecedence() {
    std::cerr << "  [dbconfig] sqlite path precedence (the #96 regression)...\n";

    // The reported failure shape: ResolvePaths already resolved the default
    // to an absolute path; db.json specifies a scratch path — it must apply.
    {
        KernelConfig cfg;
        cfg.sqlite_path = (cfg.dataDir / "memory.db").string();  // post-ResolvePaths
        DatabaseConfig db;
        db.sqlitePath = "/tmp/scratch-96.db";
        db.ApplyTo(cfg);
        Assert(cfg.sqlite_path == "/tmp/scratch-96.db",
               "db.json sqlite.path applies over the resolved default");
    }

    // CLI --sqlite-path wins over db.json.
    {
        KernelConfig cfg;
        cfg.sqlite_path = "/cli/path.db";
        cfg.sqlite_path_explicit = true;
        DatabaseConfig db;
        db.sqlitePath = "/tmp/scratch-96.db";
        db.ApplyTo(cfg);
        Assert(cfg.sqlite_path == "/cli/path.db",
               "explicit CLI sqlite path survives db.json");
    }

    // db.json without a sqlite.path key leaves the resolved path alone
    // (LoadFromFile keeps the field at its default — ApplyTo must not stomp
    // the correctly-resolved dataDir path with the relative default).
    {
        KernelConfig cfg;
        cfg.sqlite_path = "/data/memory.db";
        DatabaseConfig db;  // sqlitePath stays "state/memory.db" (key absent)
        db.ApplyTo(cfg);
        Assert(cfg.sqlite_path == "/data/memory.db",
               "key-absent db.json does not stomp the resolved path");
    }
    return 0;
}

int TestBackendPrecedence() {
    std::cerr << "  [dbconfig] backend precedence...\n";
    {
        KernelConfig cfg;
        DatabaseConfig db;
        db.backend = "postgresql";
        db.ApplyTo(cfg);
        Assert(cfg.storage_backend == "postgresql",
               "db.json backend applies when not explicit");
    }
    {
        KernelConfig cfg;
        cfg.storage_backend = "postgresql";
        cfg.storage_backend_explicit = true;
        DatabaseConfig db;
        db.backend = "sqlite";
        db.ApplyTo(cfg);
        Assert(cfg.storage_backend == "postgresql",
               "explicit CLI backend survives db.json");
    }
    {
        KernelConfig cfg;   // backend "sqlite" (default)
        DatabaseConfig db;  // backend "sqlite" (default)
        db.ApplyTo(cfg);
        Assert(cfg.storage_backend == "sqlite", "default/default is a no-op");
    }
    return 0;
}

}  // namespace

int main() {
    std::cerr << "DatabaseConfigTests:\n";
    TestSqlitePathPrecedence();
    TestBackendPrecedence();
    if (g_failures > 0) {
        std::cerr << g_failures << " assertion(s) failed.\n";
        return 1;
    }
    std::cerr << "All tests passed.\n";
    return 0;
}
