#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <json/json.h>

#include "animus_kernel/KernelConfig.h"
#include "animus_kernel/admin/ProviderState.h"
#include "animus_kernel/llm/LLMProviderConfig.h"

namespace animus::kernel {
namespace llm {
class LLMProviderRegistry;
}

class AgentConfigStore;

class ProviderManager {
public:
    void Configure(const KernelConfig::ProviderConfigStorage& storage);

    bool LoadFromDisk(std::string* error);
    bool SaveToDisk(std::string* error) const;

    std::vector<ProviderState> ListProviders() const;
    std::optional<ProviderState> GetProvider(const std::string& id) const;
    bool HasProvider(const std::string& id) const;

    std::string GetDefaultProviderId() const;
    bool SetDefaultProviderId(const std::string& id, std::string* error);

    bool CreateProvider(const ProviderState& state, std::string* error);
    bool UpdateProvider(const std::string& id, const ProviderState& state, std::string* error);
    bool DeleteProvider(const std::string& id, ProviderState* removed, std::string* error);

    bool UpdateProviderStatus(
        const std::string& id,
        const std::string& status,
        const std::string& lastError,
        std::uint64_t lastTestedUnixMs,
        ProviderState* updated,
        std::string* error);

    bool UpdateProviderCapabilities(
        const std::string& id,
        const llm::ProviderCapabilities& capabilities,
        ProviderState* updated,
        std::string* error,
        const std::string& modelId = "");

    int GetProviderConcurrency(const std::string& providerId) const;
    std::optional<llm::LLMProviderConfig> GetProviderConfig(const std::string& providerId) const;

    bool LoadAuthFromDisk(const std::string& providerId, Json::Value* out, std::string* error) const;
    bool SaveAuthToDisk(const std::string& providerId, const Json::Value& auth, std::string* error) const;

    // ── #93 P3 slice 3: store-backed persistence (replicated set) ────────
    // Provider config persists as agent_config kv rows under the reserved
    // agent id "__providers" and replicates with the rest of the config
    // set. Layout: "__default" = default provider id; "cfg.<id>" = config
    // JSON (no secrets); "cfg.<id>.api_key" = API key (auto-vaulted by the
    // credential-suffix interception, replicated as a secret_ref with the
    // ciphertext in api_package_secrets); "cfg.<id>.auth_secret" = the
    // auth.json blob (vaulted the same way). Runtime state (status,
    // lastError, lastTested, capabilities) is node-local memory only —
    // today's file format never persisted it either; health is a per-node
    // observation and must not become sync chatter.
    //
    // Boot: files win ONLY when the store has no provider rows (one-time
    // import); after that the store is the source of truth and
    // providers.json/auth.json are legacy inputs, ignored.

    /// Attach the replicated config store; switches persistence to store
    /// mode (file methods become legacy import inputs only).
    void ConfigureStore(AgentConfigStore* store);
    bool UsingStore() const { return m_configStore != nullptr; }

    /// Boot load. Store mode: rebuild from kv rows; on an empty store with
    /// legacy files present, import them once (config + api keys + auth
    /// blobs) and persist. File mode: LoadFromDisk.
    bool LoadProviders(std::string* error);

    /// Persist provider config (routes by mode). Store mode reconciles:
    /// upserts every provider's rows and deletes orphaned rows.
    bool SaveProviders(std::string* error) const;

    /// Store-mode auth blob access (routes by mode).
    bool LoadAuthProvider(const std::string& providerId, Json::Value* out, std::string* error) const;
    bool SaveAuthProvider(const std::string& providerId, const Json::Value& auth, std::string* error) const;

    /// Sync-apply hook: a remote agent_config apply under "__providers"
    /// changed provider rows — rebuild the in-memory model (runtime state
    /// of surviving providers is carried over). No-op in file mode.
    void ReloadFromStore();

    Json::Value BuildProviderJson(const ProviderState& provider, bool maskSecrets = true) const;
    bool ValidateProviderPayload(
        const Json::Value& body,
        bool requireProviderId,
        std::string* error) const;
    bool ParseProviderPayload(
        const Json::Value& body,
        const ProviderState& defaults,
        ProviderState* out,
        std::string* error) const;
    bool ValidateOAuthProvider(
        const std::string& id,
        bool requireOpenAICodex,
        ProviderState* stateOut,
        int* httpStatusCodeOut,
        std::string* error) const;
    bool BuildOAuthStatus(
        const std::string& id,
        Json::Value* out,
        int* httpStatusCodeOut,
        std::string* error) const;
    bool FetchProviderModels(
        const std::string& id,
        llm::LLMProviderRegistry* registry,
        Json::Value* modelsOut,
        bool* fetchedOut,
        std::string* error) const;
    bool RefreshProviderCapabilities(
        const std::string& id,
        llm::LLMProviderRegistry* registry,
        const std::string& modelOverride,
        llm::ProviderCapabilities* capabilitiesOut,
        bool* fetchedOut,
        std::string* error);
    bool TestProviderConnectivity(
        const std::string& id,
        bool* successOut,
        std::string* testErrorOut,
        std::string* statusOut,
        std::string* error);

private:
    std::unordered_map<std::string, ProviderState>::const_iterator
    FindProviderLocked(const std::string& id) const;
    std::unordered_map<std::string, ProviderState>::iterator
    FindProviderLocked(const std::string& id);

    std::vector<std::string> ListProviderIdsForAuthImport() const;

    bool ResolveProviderAuthFilePathLocked(
        const std::string& providerId,
        std::string* authFile,
        std::string* error) const;
    bool BuildRuntimeProviderConfigLocked(
        const std::string& providerId,
        llm::LLMProviderConfig* configOut,
        ProviderState* stateOut,
        std::string* error) const;

    // Static fallback table: model name -> context window size.
    // Loaded from model_context_sizes.json at startup.
    std::unordered_map<std::string, std::uint32_t> m_staticContextSizes;
    void LoadStaticContextSizes();
    std::uint32_t LookupStaticContextWindow(const std::string& modelId) const;

    KernelConfig::ProviderConfigStorage m_providerStorage{};
    AgentConfigStore* m_configStore{nullptr};   // #93 P3: store mode when set
    std::string m_defaultProvider;
    std::unordered_map<std::string, ProviderState> m_providersByName;
    mutable std::mutex m_providerMutex;
};

} // namespace animus::kernel
