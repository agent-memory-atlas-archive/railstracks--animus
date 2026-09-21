#pragma once

#include "animus_kernel/tools/HttpClient.h"
#include <json/json.h>

#include <set>
#include <string>
#include <vector>

namespace animus::kernel {

class ApiPackageStore;
class HttpClient;
class SecretsVault;

// ============================================================================
// ApiRuntime — executes api package commands (build order c)
//
// Action path: validate args against parameters → interpolate the declared
// request template (missing key = hard error naming it) → C++ transport →
// Lua sandbox run(ctx) → result (files verified, secrets masked at every
// egress surface). Spec: docs/api/SANDBOX.md + TOOL.md.
//
// The sandbox is a dedicated lua_State per invocation — package scripts never
// share an agent VM. Globals are whitelisted (no io/require/loadfile), the
// instruction-count hook is active, secondary HTTP is capped at 5 calls,
// ctx.fs writes are quota-checked under <filesRoot>/<package>/.
// ============================================================================

class ApiRuntime {
public:
    struct Config {
        std::string filesRoot;   // e.g. <dataDir>/api-files
        int httpBudgetPerCall{5};
        size_t fsReadCapBytes{1024 * 1024};       // 1 MB per read
        size_t stringTruncateBytes{16 * 1024};    // ~16 KB truncation rule
        size_t instructionLimit{10'000'000};
    };

    ApiRuntime(ApiPackageStore* store, HttpClient* http, Config cfg,
              SecretsVault* vault = nullptr);

    // #25 egress scope helpers shared by action transport, sandbox secondary
    // fetches, and connection polls.
    // Parse the stored egress_hosts JSON array into patterns.
    static std::vector<std::string> ParseEgressHosts(const std::string& json);
    // Extract the host of a RESOLVED url and match it against the patterns
    // (exact, or "*.<domain>" subdomain wildcard). hostOut carries the
    // extracted host for audit lines. Empty pattern set denies everything.
    static bool EgressAllowed(const std::vector<std::string>& hostPatterns,
                              const std::string& resolvedUrl, std::string& hostOut);

    // #25: manual redirect following under the package egress scope. curl's
    // CURLOPT_FOLLOWLOCATION moves the conversation without re-checking the
    // package allowlist, so redirects are followed here — every hop passes
    // EgressAllowed, credentials are dropped on cross-host hops, chains cap
    // at kMaxRedirectHops. Denied hops return {status 0, egress-denied error}.
    static constexpr int kMaxRedirectHops = 5;
    static HttpClient::Response ExecuteScoped(HttpClient* http,
                                               const std::vector<std::string>& hostPatterns,
                                               HttpClient::Request req,
                                               const std::string& auditPrefix);
    // Resolves a Location header against the request URL. "" when the
    // location cannot be resolved (caller surfaces the raw redirect).
    static std::string ResolveRedirectUrl(const std::string& requestUrl,
                                          const std::string& location);

    // Executes an action command for an agent. argsJson must be a JSON
    // object (empty object for no-arg commands). Returns the agent-facing
    // result object: {success, output, data?, files?} on success, or
    // {success:false, error:"..."} — errors always name the missing key /
    // unknown package / failed constraint and list what is available.
    Json::Value ExecuteAction(const std::string& packageName,
                              const std::string& commandName,
                              const std::string& agentId,
                              const Json::Value& args);

    // Lists a package's action commands (name + description) for the
    // introspection affordance (`api <package>`).
    struct CommandSummary {
        std::string name;
        std::string description;
        bool hasRequest{false};
    };
    std::vector<CommandSummary> ListActions(const std::string& packageName,
                                            const std::string& agentId) const;

    // Runs a hook script against an event table (used by the connection
    // supervisor once (d) lands; sandboxed identically). eventJson is the
    // ctx.event table as JSON. No args/request, no primary transport.
    Json::Value RunHook(const std::string& packageName,
                        const std::string& commandName,
                        const std::string& agentId,
                        const Json::Value& eventJson);

    // --- interpolation (public for tests) ---------------------------------
    // Resolves {{state.x}} / {{args.y}} (dotted paths) against state + args.
    // Missing key or non-scalar value sets `error` (naming the path and the
    // masked template) and returns "". secretKeys are masked in errors.
    static std::string Interpolate(const std::string& tmpl,
                                   const Json::Value& state,
                                   const Json::Value& args,
                                   const std::set<std::string>& secretStateKeys,
                                   std::string& error);

    // Replaces every occurrence of each secret value with "***". Used on
    // outputs, errors, and log lines. Short/empty values are skipped
    // (masking "" would corrupt everything).
    static std::string MaskSecrets(const std::string& text,
                                   const std::vector<std::string>& secretValues);

    const Config& config() const { return m_cfg; }
    ApiPackageStore* store() const { return m_store; }
    SecretsVault* vault() const { return m_vault; }

private:
    Json::Value ExecuteInternal(const std::string& packageName,
                                const std::string& commandName,
                                const std::string& agentId,
                                Json::Value args,
                                bool isHook,
                                const Json::Value& eventJson);

    ApiPackageStore* m_store;
    HttpClient* m_http;
    Config m_cfg;
    SecretsVault* m_vault{nullptr};
};

}  // namespace animus::kernel
