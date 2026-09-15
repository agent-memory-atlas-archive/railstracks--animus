// NodeManagerBindingTests — #73 node-token user binding.
//
// Covers:
//   - GenerateCredentials(description, userId) persists the binding
//   - GetUserIdForToken resolves raw token → user id
//   - SetTokenUser reassigns (and unbinds with "")
//   - unbound tokens report "" (the backfill default for existing rows)
//   - the legacy one-arg GenerateCredentials leaves tokens unbound

#include "animus_kernel/NodeManager.h"
#include "animus_kernel/SqliteDataStore.h"

#include <iostream>
#include <string>
#include <unistd.h>

using namespace animus::kernel;

namespace {

int g_failures = 0;

void Assert(bool condition, const std::string& msg) {
    if (!condition) {
        std::cerr << "  ASSERT FAILED: " << msg << "\n";
        g_failures++;
    }
}

std::string MakeTempDbPath() {
    char tmp[] = "/tmp/animus_nodebind_test_XXXXXX";
    int fd = mkstemp(tmp);
    if (fd >= 0) close(fd);
    return std::string(tmp) + ".db";
}

int TestBindingRoundTrip() {
    std::cerr << "  [node-bind] create with user binds and resolves...\n";
    const auto dbPath = MakeTempDbPath();
    SqliteDataStore store(dbPath);
    NodeManager mgr(&store);

    auto creds = mgr.GenerateCredentials("workstation node", "user-abc-123");
    Assert(!creds.token.empty(), "token generated");
    Assert(mgr.GetUserIdForToken(creds.token) == "user-abc-123",
           "token resolves to bound user id");

    // ListTokens surfaces the binding
    auto tokens = mgr.ListTokens();
    Assert(tokens.size() == 1, "one token listed");
    Assert(tokens[0].user_id == "user-abc-123", "list exposes user_id");
    Assert(tokens[0].description == "workstation node", "description persists");
    return 0;
}

int TestUnboundByDefault() {
    std::cerr << "  [node-bind] legacy create leaves token unbound...\n";
    const auto dbPath = MakeTempDbPath();
    SqliteDataStore store(dbPath);
    NodeManager mgr(&store);

    auto creds = mgr.GenerateCredentials("legacy form");
    Assert(!creds.token.empty(), "token generated");
    Assert(mgr.GetUserIdForToken(creds.token).empty(),
           "one-arg overload leaves token unbound");
    return 0;
}

int TestSetTokenUser() {
    std::cerr << "  [node-bind] reassign and unbind...\n";
    const auto dbPath = MakeTempDbPath();
    SqliteDataStore store(dbPath);
    NodeManager mgr(&store);

    auto creds = mgr.GenerateCredentials("node", "user-one");
    int64_t tokenId = mgr.ValidateToken(creds.token);
    Assert(tokenId > 0, "token validates");

    Assert(mgr.SetTokenUser(tokenId, "user-two"), "reassign ok");
    Assert(mgr.GetUserIdForToken(creds.token) == "user-two",
           "token now resolves to reassigned user");

    Assert(mgr.SetTokenUser(tokenId, ""), "unbind ok");
    Assert(mgr.GetUserIdForToken(creds.token).empty(), "token unbound");
    return 0;
}

int TestMigrationBackfill() {
    std::cerr << "  [node-bind] pre-existing table migrates with unbound rows...\n";
    const auto dbPath = MakeTempDbPath();
    {
        // Create the table WITHOUT user_id (the pre-#73 shape) and insert a
        // row directly, then let NodeManager's constructor run the migration.
        SqliteDataStore raw(dbPath);
        raw.Exec("CREATE TABLE IF NOT EXISTS node_tokens ("
                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "token_hash TEXT NOT NULL UNIQUE,"
                 "signing_key_hash TEXT NOT NULL DEFAULT '',"
                 "description TEXT NOT NULL DEFAULT '',"
                 "created_at_unix_ms INTEGER NOT NULL,"
                 "revoked INTEGER NOT NULL DEFAULT 0)");
        raw.Exec("INSERT INTO node_tokens (token_hash, description, "
                 "created_at_unix_ms, revoked) "
                 "VALUES ('legacy-hash-1', 'old token', 1, 0)");
    }
    SqliteDataStore store(dbPath);
    NodeManager mgr(&store);  // constructor runs EnsureSchema migration

    auto tokens = mgr.ListTokens();
    Assert(tokens.size() == 1, "legacy token loaded");
    Assert(tokens[0].user_id.empty(), "legacy row backfills unbound");
    Assert(mgr.SetTokenUser(tokens[0].id, "user-migrated"), "bind after migrate");
    Assert(mgr.ListTokens()[0].user_id == "user-migrated",
           "reassigned user visible on reload path");
    return 0;
}

} // namespace

int main() {
    std::cerr << "[NodeManagerBindingTests]\n";
    TestBindingRoundTrip();
    TestUnboundByDefault();
    TestSetTokenUser();
    TestMigrationBackfill();

    if (g_failures > 0) {
        std::cerr << "  " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cerr << "  all node binding tests passed\n";
    return 0;
}