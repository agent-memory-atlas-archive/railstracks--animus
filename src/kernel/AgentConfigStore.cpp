#include "animus_kernel/AgentConfigStore.h"
#include "animus_kernel/IDataStore.h"
#include "animus_kernel/SchemaHelpers.h"
#include "animus_kernel/Log.h"
#include "animus_kernel/api/SecretsVault.h"

#include <algorithm>
#include <cctype>

#include <iostream>
#include <json/json.h>
#include <sstream>

namespace animus::kernel {

// Schema: agent_config table
// DDL is dialect-dependent — datetime('now') is SQLite-specific.
// PostgreSQL uses now() or CURRENT_TIMESTAMP.

// ============================================================================
// Construction
// ============================================================================

AgentConfigStore::AgentConfigStore(IDataStore* dataStore)
    : m_store(dataStore)
{
    EnsureSchema();
}

void AgentConfigStore::EnsureSchema() {
    if (!m_store) return;
    if (m_store->Dialect() == DataStoreDialect::PostgreSQL) {
        schema::CreateTable(m_store, R"(
            CREATE TABLE IF NOT EXISTS agent_config (
              agent_id  TEXT NOT NULL,
              key       TEXT NOT NULL,
              value     TEXT NOT NULL DEFAULT '',
              updated_at TEXT NOT NULL DEFAULT (now()::text),
              PRIMARY KEY (agent_id, key)
            );
        )");
    } else {
        schema::CreateTable(m_store, R"(
            CREATE TABLE IF NOT EXISTS agent_config (
              agent_id  TEXT NOT NULL,
              key       TEXT NOT NULL,
              value     TEXT NOT NULL DEFAULT '',
              updated_at TEXT NOT NULL DEFAULT (datetime('now')),
              PRIMARY KEY (agent_id, key)
            );
        )");
    }
}

// ============================================================================
// Single key operations
// ============================================================================

std::string AgentConfigStore::GetRaw(const std::string& agentId,
                                   const std::string& key) const {
    // Check cache first
    auto agentIt = m_cache.find(agentId);
    if (agentIt != m_cache.end()) {
        auto keyIt = agentIt->second.find(key);
        if (keyIt != agentIt->second.end()) {
            return keyIt->second;
        }
        // Key not in cache means it doesn't exist
        return "";
    }

    // Cache not warmed for this agent — query DB and warm it
    if (!m_store) return "";

    auto stmt = m_store->Prepare(
        "SELECT value FROM agent_config WHERE agent_id = ? AND key = ?");
    if (!stmt) return "";

    stmt->BindText(1, agentId);
    stmt->BindText(2, key);

    std::string value;
    if (stmt->Step()) {
        value = stmt->ColumnText(0);
    }
    stmt->Finalize();
    return value;
}

std::string AgentConfigStore::Get(const std::string& agentId,
                                   const std::string& key) const {
    std::string raw = GetRaw(agentId, key);
    if (m_vault) return m_vault->ResolveAgentValue(agentId, raw);
    return raw;
}

void AgentConfigStore::OnSyncApplied(const std::string& table, const std::string& rowKey) {
    if (table != "agent_config") return;
    // rowKey is the escaped composite pair agent_id \x1F key — invalidate
    // that agent's whole cache slice (simplest correct granularity; the
    // next Get re-reads from the DB).
    const auto pos = rowKey.find('\x1F');
    if (pos == std::string::npos) return;
    const std::string agentId = rowKey.substr(0, pos);
    auto it = m_cache.find(agentId);
    if (it != m_cache.end()) m_cache.erase(it);
    // NOTE: m_cacheWarmed stays — a warmed-but-empty slice re-reads on Get
    // (the miss path queries the DB), which is the behavior we want.
}

bool AgentConfigStore::IsCredentialKey(const std::string& key) {
    static const std::vector<std::string> suffixes = {
        "api_key", "access_token", "bot_token", "app_token", "app_password",
        "client_secret", "refresh_token", "server_password", "access_jwt",
        "refresh_jwt", "api_secret", "secret", "password",
    };
    std::string lower;
    std::transform(key.begin(), key.end(), std::back_inserter(lower),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& s : suffixes) {
        std::string::size_type pos = lower.rfind(s);
        if (pos != std::string::npos && pos + s.size() == lower.size()) return true;
    }
    return false;
}

void AgentConfigStore::Set(const std::string& agentId,
                            const std::string& key,
                            const std::string& value) {
    // #93 P3a: credential-shaped values are vaulted, never stored raw. The
    // row carries a {"secret_ref":"<name>"} object string instead; Get()
    // resolves it transparently. Vault disabled -> plaintext passthrough
    // (single-box legacy behavior, loud at boot by the migration sweep).
    if (m_vault && !value.empty() && IsCredentialKey(key)) {
        std::string probeName;
        if (!SecretsVault::IsRefValue(value, probeName) && value.front() != '{') {
            std::string name;
            for (char c : key) {
                if (SecretsVault::IsValidSecretName(std::string(1, c))) name += c;
                else if (!name.empty() && name.back() != '_') name += '_';
            }
            while (!name.empty() && name.back() == '_') name.pop_back();
            std::string err;
            if (!name.empty() && name.size() <= 63 &&
                m_vault->VaultAgentValue(agentId, name, value, err)) {
                Json::Value ref(Json::objectValue);
                ref["secret_ref"] = name;
                Json::StreamWriterBuilder wb;
                wb["indentation"] = "";
                Set(agentId, key, Json::writeString(wb, ref));  // re-set as ref
                return;
            }
            ALOG_WARNING("config", "[vault] could not vault credential '" << key
                         << "' (" << err << ") — storing plaintext (legacy behavior)");
        }
    }
    // Write to DB first
    if (m_store) {
        const char* nowFunc = (m_store->Dialect() == DataStoreDialect::PostgreSQL)
            ? "now()::text" : "datetime('now')";
        std::string sql = std::string(
            "INSERT OR REPLACE INTO agent_config (agent_id, key, value, updated_at) "
            "VALUES (?, ?, ?, ") + nowFunc + ")";

        // PostgreSQL doesn't support OR REPLACE — use UPSERT instead
        if (m_store->Dialect() == DataStoreDialect::PostgreSQL) {
            sql = std::string(
                "INSERT INTO agent_config (agent_id, key, value, updated_at) "
                "VALUES (?, ?, ?, ") + nowFunc + ") "
                "ON CONFLICT (agent_id, key) DO UPDATE SET value = EXCLUDED.value, updated_at = EXCLUDED.updated_at";
        }

        auto stmt = m_store->Prepare(sql);
        if (stmt) {
            stmt->BindText(1, agentId);
            stmt->BindText(2, key);
            stmt->BindText(3, value);
            stmt->ExecDML();
            stmt->Finalize();
        }
    }

    // Update cache
    m_cache[agentId][key] = value;
    m_cacheWarmed[agentId] = true;
}

void AgentConfigStore::Delete(const std::string& agentId,
                               const std::string& key) {
    if (m_store) {
        auto stmt = m_store->Prepare(
            "DELETE FROM agent_config WHERE agent_id = ? AND key = ?");
        if (stmt) {
            stmt->BindText(1, agentId);
            stmt->BindText(2, key);
            stmt->ExecDML();
            stmt->Finalize();
        }
    }

    // Remove from cache
    auto agentIt = m_cache.find(agentId);
    if (agentIt != m_cache.end()) {
        agentIt->second.erase(key);
    }
}

// ============================================================================
// Bulk operations
// ============================================================================

std::unordered_map<std::string, std::string>
AgentConfigStore::GetAll(const std::string& agentId) const {
    // Return from cache if warmed
    auto warmedIt = m_cacheWarmed.find(agentId);
    if (warmedIt != m_cacheWarmed.end() && warmedIt->second) {
        auto agentIt = m_cache.find(agentId);
        if (agentIt != m_cache.end()) {
            return agentIt->second;
        }
        return {};
    }

    // Load from DB
    if (!m_store) { std::cerr << "[config-store] GetAll: no store!" << std::endl; return {}; }

    auto stmt = m_store->Prepare(
        "SELECT key, value FROM agent_config WHERE agent_id = ?");
    if (!stmt) { std::cerr << "[config-store] GetAll: prepare failed" << std::endl; return {}; }

    std::unordered_map<std::string, std::string> result;
    stmt->BindText(1, agentId);
    while (stmt->Step()) {
        result[stmt->ColumnText(0)] = stmt->ColumnText(1);
    }
    stmt->Finalize();

    std::cerr << "[config-store] GetAll(agentId='" << agentId << "'): " << result.size() << " entries" << std::endl;

    // Cache it
    m_cache[agentId] = result;
    m_cacheWarmed[agentId] = true;
    return result;
}

std::vector<std::string>
AgentConfigStore::ListKeys(const std::string& agentId) const {
    auto all = GetAll(agentId);
    std::vector<std::string> keys;
    keys.reserve(all.size());
    for (const auto& [k, _] : all) {
        keys.push_back(k);
    }
    return keys;
}

void AgentConfigStore::DeleteByPrefix(const std::string& agentId,
                                       const std::string& prefix) {
    if (m_store) {
        auto stmt = m_store->Prepare(
            "DELETE FROM agent_config WHERE agent_id = ? AND key LIKE ?");
        if (stmt) {
            stmt->BindText(1, agentId);
            stmt->BindText(2, prefix + "%");
            stmt->ExecDML();
            stmt->Finalize();
        }
    }

    // Remove from cache
    auto agentIt = m_cache.find(agentId);
    if (agentIt != m_cache.end()) {
        auto& map = agentIt->second;
        for (auto it = map.begin(); it != map.end(); ) {
            if (it->first.substr(0, prefix.size()) == prefix) {
                it = map.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void AgentConfigStore::WarmCache(const std::string& agentId) {
    // Load all from DB into cache
    (void)GetAll(agentId);
}

void AgentConfigStore::Flush() {
    // All writes are immediate (INSERT OR REPLACE).
    // This exists for symmetry with other stores and future batching.
}

} // namespace animus::kernel
