// AuthManagerTests — #73 prerequisites: the session-token resolution path
// the ws/chat identity threading depends on.
//
// Covers:
//   - login → session token → ValidateToken returns the user id
//   - static token still validates (no user id — the ws/chat fallback path)
//   - expired session tokens are rejected and cleaned up
//   - no token + auth required → NoTokenProvided
//   - auth not required → AuthNotRequired with empty user id

#include "animus_kernel/AuthManager.h"
#include "animus_kernel/AuthStore.h"
#include "animus_kernel/SqliteDataStore.h"

#include <iostream>
#include <memory>
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
    char tmp[] = "/tmp/animus_authmgr_test_XXXXXX";
    int fd = mkstemp(tmp);
    if (fd >= 0) close(fd);
    return std::string(tmp) + ".db";
}

// Auth fixture: owns the store + AuthStore + manager so lifetimes stay
// valid for the whole test (AuthManager is non-movable; SqliteDataStore and
// AuthStore must outlive it).
struct AuthFixture {
    std::unique_ptr<SqliteDataStore> dataStore;
    std::unique_ptr<AuthStore> authStore;
    std::unique_ptr<AuthManager> mgr;
};

AuthFixture MakeManagerWithUsers(const std::string& dbPath,
                                  const std::string& staticToken) {
    AuthFixture fx;
    fx.dataStore = std::make_unique<SqliteDataStore>(dbPath);
    fx.authStore = std::make_unique<AuthStore>(fx.dataStore.get());
    fx.authStore->EnsureSchema();

    fx.mgr = std::make_unique<AuthManager>();
    fx.mgr->SetAuthStore(fx.authStore.get());
    if (!staticToken.empty()) {
        fx.mgr->SetStaticToken(staticToken);
    } else {
        fx.mgr->SetRequireAuth(true);
    }
    fx.mgr.CreateUser("melvin", "hunter2", "admin");
    fx.mgr.CreateUser("thomas", "wachtwoord", "viewer");
    return fx;
}

int TestSessionTokenResolvesUser() {
    std::cerr << "  [auth] session token resolves user...\n";
    const auto dbPath = MakeTempDbPath();
    auto fx = MakeManagerWithUsers(dbPath, "");
    auto& mgr = *fx.mgr;

    auto user = mgr.GetUserByUsername("thomas");
    Assert(user.has_value(), "user exists");
    if (!user) return 1;

    std::string rawToken;
    Assert(mgr.CreateSessionToken(user->id, 3600 * 1000, rawToken),
           "session token created");
    Assert(!rawToken.empty(), "raw token non-empty");

    auto [result, userId] = mgr.ValidateToken(rawToken);
    Assert(result == AuthResult::Ok, "session token validates");
    Assert(userId == user->id, "token resolves to owning user id");

    // GetUserById round-trip — what the ws/chat controller calls
    auto resolved = mgr.GetUserById(userId);
    Assert(resolved.has_value(), "user resolvable by id");
    Assert(resolved->username == "thomas", "username round-trips");
    Assert(resolved->role == "viewer", "role round-trips");
    return 0;
}

int TestStaticTokenNoUserId() {
    std::cerr << "  [auth] static token carries no user id...\n";
    const auto dbPath = MakeTempDbPath();
    auto fx = MakeManagerWithUsers(dbPath, "static-op-token");
    auto& mgr = *fx.mgr;

    auto [result, userId] = mgr.ValidateToken("static-op-token");
    Assert(result == AuthResult::Ok, "static token validates");
    Assert(userId.empty(), "static token yields empty user id — "
           "ws/chat must treat this as unauthenticated identity");

    auto [badResult, _] = mgr.ValidateToken("wrong-token");
    Assert(badResult == AuthResult::InvalidToken, "wrong static token rejected");
    return 0;
}

int TestExpiredSessionToken() {
    std::cerr << "  [auth] expired session token rejected...\n";
    const auto dbPath = MakeTempDbPath();
    auto fx = MakeManagerWithUsers(dbPath, "");
    auto& mgr = *fx.mgr;

    auto user = mgr.GetUserByUsername("melvin");
    if (!user) { Assert(false, "user missing"); return 1; }

    std::string rawToken;
    Assert(mgr.CreateSessionToken(user->id, -1000, rawToken),
           "token created with negative ttl (already expired)");
    auto [result, userId] = mgr.ValidateToken(rawToken);
    Assert(result == AuthResult::InvalidToken, "expired token rejected");
    Assert(userId.empty(), "expired token yields no user id");
    return 0;
}

int TestNoTokenProvided() {
    std::cerr << "  [auth] no token + required auth...\n";
    const auto dbPath = MakeTempDbPath();
    auto fx = MakeManagerWithUsers(dbPath, "");
    auto& mgr = *fx.mgr;
    auto [result, userId] = mgr.ValidateToken("");
    Assert(result == AuthResult::NoTokenProvided, "empty token flagged");
    Assert(userId.empty(), "no user id");
    return 0;
}

int TestAuthNotRequired() {
    std::cerr << "  [auth] auth not required...\n";
    AuthManager mgr;
    // No static token, no store, never configured — auth off.
    Assert(!mgr.IsAuthRequired(), "auth not required by default");
    auto [result, userId] = mgr.ValidateToken("anything");
    Assert(result == AuthResult::AuthNotRequired, "auth not required result");
    Assert(userId.empty(), "no user id when auth off");
    return 0;
}

} // namespace

int main() {
    std::cerr << "[AuthManagerTests]\n";
    TestSessionTokenResolvesUser();
    TestStaticTokenNoUserId();
    TestExpiredSessionToken();
    TestNoTokenProvided();
    TestAuthNotRequired();

    if (g_failures > 0) {
        std::cerr << "  " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cerr << "  all auth manager tests passed\n";
    return 0;
}