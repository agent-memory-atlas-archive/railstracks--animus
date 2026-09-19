#include "animus_kernel/api/SecretsVault.h"

#include "animus_kernel/ApiPackageStore.h"
#include "animus_kernel/CryptoUtils.h"
#include "animus_kernel/Log.h"
#include "animus_kernel/SchemaHelpers.h"

#include <chrono>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace animus::kernel {

namespace {

int64_t NowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string GenerateId() {
    return crypto::RandomHex(16);
}

}  // namespace

SecretsVault::SecretsVault(IDataStore* store, const std::string& keyPath)
    : m_store(store), m_keyPath(keyPath) {
    if (!m_keyPath.empty()) {
        std::string err;
        if (!LoadOrCreateKey(err)) {
            ALOG_ERROR("api", "[vault] key load failed: " << err << " — vault disabled; "
                       << "secret operations will fail closed until fixed");
            m_key.clear();
        }
    }
}

bool SecretsVault::LoadOrCreateKey(std::string& error) {
    std::ifstream in(m_keyPath);
    if (in.good()) {
        std::string hex((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // Trim whitespace/newlines.
        while (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r' || hex.back() == ' '))
            hex.pop_back();
        if (hex.size() != 64) {
            error = "key file '" + m_keyPath + "' must hold 64 hex chars (32 bytes), found " +
                    std::to_string(hex.size());
            return false;
        }
        m_key.reserve(32);
        for (size_t i = 0; i < 64; i += 2) {
            auto nib = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = nib(hex[i]), lo = nib(hex[i + 1]);
            if (hi < 0 || lo < 0) {
                error = "key file '" + m_keyPath + "' contains non-hex characters";
                return false;
            }
            m_key.push_back(static_cast<unsigned char>((hi << 4) | lo));
        }
        ALOG_INFO("api", "[vault] master key loaded from " << m_keyPath);
        return true;
    }

    // First use — generate.
    const std::string keyHex = crypto::RandomHex(32);  // 32 bytes -> 64 hex chars
    {
        std::ofstream out(m_keyPath, std::ios::trunc);
        if (!out.good()) {
            error = "cannot create key file '" + m_keyPath + "'";
            return false;
        }
        out << keyHex << "\n";
    }
    ::chmod(m_keyPath.c_str(), 0600);
    m_key.reserve(32);
    for (size_t i = 0; i < 64; i += 2) {
        m_key.push_back(static_cast<unsigned char>(
            (std::stoi(keyHex.substr(i, 2), nullptr, 16) & 0xFF)));
    }
    ALOG_INFO("api", "[vault] NEW master key generated at " << m_keyPath
              << " (mode 0600). Losing this file loses all vault secrets — "
              << "back it up separately from the database.");
    return true;
}

void SecretsVault::EnsureSchema() {
    if (!m_store) return;
    schema::CreateTable(m_store, R"(
        CREATE TABLE IF NOT EXISTS api_package_secrets (
            id TEXT PRIMARY KEY,
            package_id TEXT NOT NULL,
            name TEXT NOT NULL,
            ciphertext TEXT NOT NULL,
            created_at_unix_ms INTEGER NOT NULL,
            updated_at_unix_ms INTEGER NOT NULL,
            UNIQUE (package_id, name)
        );
    )");
}

bool SecretsVault::Seal(const std::string& plaintext, std::string& envelopeHex,
                        std::string& error) const {
    if (m_key.empty()) { error = "vault disabled (no master key)"; return false; }
    envelopeHex = crypto::Aes256GcmSeal(m_key, plaintext, error);
    return envelopeHex.empty() == false;
}

bool SecretsVault::Open(const std::string& envelopeHex, std::string& plaintext,
                        std::string& error) const {
    if (m_key.empty()) { error = "vault disabled (no master key)"; return false; }
    return crypto::Aes256GcmOpen(m_key, envelopeHex, plaintext, error);
}

bool SecretsVault::Set(const std::string& packageId, const std::string& name,
                       const std::string& value, std::string& error) {
    if (!m_store) { error = "vault has no store"; return false; }
    if (packageId.empty() || name.empty()) {
        error = "vault set requires package id and secret name";
        return false;
    }
    std::string envelope;
    if (!Seal(value, envelope, error)) return false;

    const int64_t now = NowUnixMs();
    if (Has(packageId, name)) {
        auto stmt = m_store->Prepare(
            "UPDATE api_package_secrets SET ciphertext = ?, updated_at_unix_ms = ? "
            "WHERE package_id = ? AND name = ?");
        if (!stmt) { error = "vault prepare failed: " + m_store->ErrMsg(); return false; }
        stmt->BindText(1, envelope);
        stmt->BindInt64(2, now);
        stmt->BindText(3, packageId);
        stmt->BindText(4, name);
        if (!stmt->ExecDML()) { error = "vault update failed: " + m_store->ErrMsg(); return false; }
        return true;
    }
    auto stmt = m_store->Prepare(
        "INSERT INTO api_package_secrets (id, package_id, name, ciphertext, "
        "created_at_unix_ms, updated_at_unix_ms) VALUES (?, ?, ?, ?, ?, ?)");
    if (!stmt) { error = "vault prepare failed: " + m_store->ErrMsg(); return false; }
    stmt->BindText(1, GenerateId());
    stmt->BindText(2, packageId);
    stmt->BindText(3, name);
    stmt->BindText(4, envelope);
    stmt->BindInt64(5, now);
    stmt->BindInt64(6, now);
    if (!stmt->ExecDML()) {
        // Lost a race with a concurrent insert of the same name: fall back
        // to update so Set() stays idempotent.
        auto upd = m_store->Prepare(
            "UPDATE api_package_secrets SET ciphertext = ?, updated_at_unix_ms = ? "
            "WHERE package_id = ? AND name = ?");
        if (!upd) { error = "vault prepare failed: " + m_store->ErrMsg(); return false; }
        upd->BindText(1, envelope);
        upd->BindInt64(2, now);
        upd->BindText(3, packageId);
        upd->BindText(4, name);
        if (!upd->ExecDML()) { error = "vault upsert failed: " + m_store->ErrMsg(); return false; }
    }
    return true;
}

std::optional<std::string> SecretsVault::Get(const std::string& packageId,
                                             const std::string& name) const {
    if (!m_store || m_key.empty()) return std::nullopt;
    auto stmt = m_store->Prepare(
        "SELECT ciphertext FROM api_package_secrets WHERE package_id = ? AND name = ?");
    if (!stmt) return std::nullopt;
    stmt->BindText(1, packageId);
    stmt->BindText(2, name);
    if (!stmt->Step()) return std::nullopt;
    const std::string envelope = stmt->ColumnText(0);
    stmt->Finalize();
    std::string plaintext, err;
    if (!Open(envelope, plaintext, err)) {
        ALOG_ERROR("api", "[vault] open failed for secret '" << name << "' of package "
                   << packageId << ": " << err << " (treating as unset — set it again)");
        return std::nullopt;
    }
    return plaintext;
}

bool SecretsVault::Has(const std::string& packageId, const std::string& name) const {
    if (!m_store) return false;
    auto stmt = m_store->Prepare(
        "SELECT 1 FROM api_package_secrets WHERE package_id = ? AND name = ?");
    if (!stmt) return false;
    stmt->BindText(1, packageId);
    stmt->BindText(2, name);
    const bool has = stmt->Step();
    stmt->Finalize();
    return has;
}

bool SecretsVault::Delete(const std::string& packageId, const std::string& name) {
    if (!m_store) return false;
    if (!Has(packageId, name)) return false;  // miss, not silent success
    auto stmt = m_store->Prepare(
        "DELETE FROM api_package_secrets WHERE package_id = ? AND name = ?");
    if (!stmt) return false;
    stmt->BindText(1, packageId);
    stmt->BindText(2, name);
    return stmt->ExecDML();
}

void SecretsVault::DeleteForPackage(const std::string& packageId) {
    if (!m_store) return;
    auto stmt = m_store->Prepare("DELETE FROM api_package_secrets WHERE package_id = ?");
    if (!stmt) return;
    stmt->BindText(1, packageId);
    stmt->ExecDML();
}

std::vector<SecretsVault::EntryInfo> SecretsVault::ListForPackage(
    const std::string& packageId) const {
    std::vector<EntryInfo> out;
    if (!m_store) return out;
    auto stmt = m_store->Prepare(
        "SELECT name, updated_at_unix_ms FROM api_package_secrets "
        "WHERE package_id = ? ORDER BY name");
    if (!stmt) return out;
    stmt->BindText(1, packageId);
    while (stmt->Step()) {
        EntryInfo e;
        e.name = stmt->ColumnText(0);
        e.updated_at_unix_ms = stmt->ColumnInt64(1);
        out.push_back(std::move(e));
    }
    stmt->Finalize();
    return out;
}

// ---------------------------------------------------------------------------
// State <-> vault helpers
// ---------------------------------------------------------------------------

bool SecretsVault::IsSecretRef(const Json::Value& v, std::string& name) {
    if (!v.isObject() || v.size() != 1 || !v.isMember("secret_ref")) return false;
    const Json::Value& ref = v["secret_ref"];
    if (!ref.isString() || ref.asString().empty()) return false;
    name = ref.asString();
    return true;
}

void SecretsVault::ResolveState(const std::string& packageId,
                                const Json::Value& stateSchema,
                                Json::Value& state) const {
    if (!state.isObject() || !stateSchema.isObject()) return;
    for (const std::string& k : stateSchema.getMemberNames()) {
        const Json::Value& def = stateSchema[k];
        if (!def.isObject() || !def.get("secret", false).asBool()) continue;

        // Explicit indirection: {"secret_ref": "<other-name>"} wins.
        std::string refName;
        if (state.isMember(k) && IsSecretRef(state[k], refName)) {
            auto value = Get(packageId, refName);
            if (value) {
                state[k] = *value;
            } else {
                ALOG_ERROR("api", "[vault] secret_ref '" << refName << "' (state key '" << k
                           << "', package " << packageId << ") is not set in the vault — "
                           << "key resolves empty; set it via admin");
                state.removeMember(k);
            }
            continue;
        }
        // Default: vault entry named exactly like the state key.
        if (Has(packageId, k)) {
            auto value = Get(packageId, k);
            if (value) state[k] = *value;
            else state.removeMember(k);
        } else if (state.isMember(k)) {
            // Pre-migration literal in stored state — treat as valid in-memory
            // (MigrateLegacyStateSecrets will move it to the vault at boot).
        }
    }
}

int SecretsVault::SplitStateSecrets(const std::string& packageId,
                                    const Json::Value& stateSchema,
                                    Json::Value& state,
                                    std::string& error) const {
    if (!state.isObject() || !stateSchema.isObject()) return 0;
    Json::Value next = state;
    int migrated = 0;
    for (const std::string& k : stateSchema.getMemberNames()) {
        const Json::Value& def = stateSchema[k];
        if (!def.isObject() || !def.get("secret", false).asBool()) continue;
        if (!next.isMember(k)) continue;
        Json::Value v = next[k];
        std::string refName;
        // Split path accepts the admin write-through form {"secret_ref": name,
        // "value": literal} — strict IsSecretRef (resolution) rejects extra
        // members, so detect the ref shape directly here.
        if (v.isObject() && v.isMember("secret_ref") && v["secret_ref"].isString()) {
            refName = v["secret_ref"].asString();
            // Admin write-through form: {"secret_ref": name, "value": literal}
            if (v.isMember("value") && v["value"].isString()) {
                const std::string literal = v["value"].asString();
                if (!literal.empty() && literal != "***") {
                    if (!const_cast<SecretsVault*>(this)->Set(packageId, refName, literal, error))
                        return -1;
                    migrated++;
                }
            }
            next[k] = Json::Value(Json::objectValue);
            next[k]["secret_ref"] = refName;
            continue;
        }
        if (v.isString()) {
            const std::string literal = v.asString();
            if (literal == "***") { next.removeMember(k); continue; }  // masked echo = keep
            if (!const_cast<SecretsVault*>(this)->Set(packageId, k, literal, error)) return -1;
            migrated++;
            next.removeMember(k);
        }
    }
    state = next;
    return migrated;
}

int SecretsVault::MigrateLegacyStateSecrets(ApiPackageStore& packages, std::string& error) {
    if (!m_store) { error = "vault has no store"; return -1; }
    if (m_key.empty()) {
        ALOG_WARNING("api", "[vault] migration skipped: vault disabled (no master key) — "
                     "legacy secrets remain in state");
        return 0;
    }
    int migrated = 0;
    for (const ApiPackage& pkg : packages.ListPackages()) {
        Json::CharReaderBuilder rb;
        Json::Value schema, state;
        std::string errs;
        std::stringstream ss1(pkg.state_schema.empty() ? "{}" : pkg.state_schema);
        Json::parseFromStream(rb, ss1, &schema, &errs);
        std::stringstream ss2(pkg.state.empty() ? "{}" : pkg.state);
        Json::parseFromStream(rb, ss2, &state, &errs);
        if (!state.isObject()) continue;

        Json::Value next = state;
        bool changed = false;
        if (schema.isObject()) {
            for (const std::string& k : schema.getMemberNames()) {
                const Json::Value& def = schema[k];
                if (!def.isObject() || !def.get("secret", false).asBool()) continue;
                if (!next.isMember(k)) continue;
                if (!next[k].isString()) continue;
                const std::string literal = next[k].asString();
                std::string setErr;
                if (!Set(pkg.id, k, literal, setErr)) {
                    error = "package '" + pkg.name + "' key '" + k + "': " + setErr;
                    return -1;
                }
                next.removeMember(k);
                changed = true;
                migrated++;
                ALOG_INFO("api", "[vault] migrated secret '" << k << "' of package '"
                          << pkg.name << "' into the vault (removed from state JSON)");
            }
        }
        if (changed) {
            Json::StreamWriterBuilder wb;
            wb["indentation"] = "";
            wb["commentStyle"] = "None";
            if (!packages.SetPackageState(pkg.id, Json::writeString(wb, next))) {
                error = "package '" + pkg.name + "': state rewrite failed";
                return -1;
            }
        }
    }
    if (migrated > 0) {
        ALOG_INFO("api", "[vault] migration complete: " << migrated
                  << " secret(s) moved from package state into the encrypted vault");
    }
    return migrated;
}

}  // namespace animus::kernel
