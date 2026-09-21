#pragma once

#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

namespace animus::kernel {

class IDataStore;
class SecretsVault;

// ============================================================================
// AgentConfigStore — persistent key-value config for Lua agents
//
// Backed by SQLite via IDataStore. Each key is scoped to an agent_id.
// Used by LuaState's config.get/set for values that must survive restarts
// (e.g. social credentials, API tokens).
//
// Read operations use an in-memory cache; writes go to SQLite first, then
// update the cache. This avoids DB hits on every Lua config.get call while
// maintaining consistency.
// ============================================================================

class AgentConfigStore {
public:
    explicit AgentConfigStore(IDataStore* dataStore);
    ~AgentConfigStore() = default;

    // #93 P3a: vault hook — when set, Get() transparently resolves
    // {"secret_ref":...} values against the agent scope. Set() intercepts
    // credential-shaped values and vaults them instead of storing
    // plaintext. Raw row access (GetRaw) bypasses resolution.
    void SetVault(SecretsVault* vault) { m_vault = vault; }

    // #93 P3: replication coherence — the sync layer writes agent_config
    // rows directly (bypassing this store), so a warmed in-memory cache
    // would hide replicated changes. Wired to SyncStore's apply notifier.
    void OnSyncApplied(const std::string& table, const std::string& rowKey);

    AgentConfigStore(const AgentConfigStore&) = delete;
    AgentConfigStore& operator=(const AgentConfigStore&) = delete;

    // --- Single key operations ---

    std::string Get(const std::string& agentId, const std::string& key) const;
    void Set(const std::string& agentId, const std::string& key, const std::string& value);
    void Delete(const std::string& agentId, const std::string& key);
    /// If `rawValue` is a {"secret_ref":...} object, best-effort delete the
    /// referenced vault entry — the secret dies with its ref row (PR #112
    /// audit: deleting rows without cleaning api_package_secrets left
    /// orphaned ciphertext replicating to every peer).
    void DeleteVaultEntryIfRef(const std::string& agentId, const std::string& rawValue);

    // --- Bulk operations ---

    /// Get all config entries for an agent as key→value pairs.
    std::unordered_map<std::string, std::string> GetAll(const std::string& agentId) const;

    /// Get all keys for an agent.
    std::vector<std::string> ListKeys(const std::string& agentId) const;

    /// Delete all keys matching a prefix for a given agent.
    /// e.g. DeleteByPrefix("default", "social.") removes all social config.
    void DeleteByPrefix(const std::string& agentId, const std::string& prefix);

    // #93 P3a: raw read WITHOUT secret_ref resolution (replication payloads,
    // exports, admin surfaces that must see the ref object, not the secret).
    std::string GetRaw(const std::string& agentId, const std::string& key) const;

    // #93 P3a: does this key name look like a credential (the migration
    // heuristic, shared so write-side vaulting and boot sweep agree)?
    static bool IsCredentialKey(const std::string& key);

    /// Load all values for an agent from SQLite into the in-memory cache.
    /// Called at startup or when a new agent's state is initialized.
    void WarmCache(const std::string& agentId);

    /// Flush any pending writes (called during graceful shutdown).
    void Flush();

private:
    void EnsureSchema();

    IDataStore* m_store;
    SecretsVault* m_vault{nullptr};   // #93 P3a: optional secret_ref resolution

    // Cache: agent_id → (key → value)
    // Mutable because Get operations are logically const but may need cache access.
    mutable std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_cache;

    // Track which agents have been warmed.
    mutable std::unordered_map<std::string, bool> m_cacheWarmed;
};

} // namespace animus::kernel
