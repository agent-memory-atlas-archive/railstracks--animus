#include "animus_kernel/api/SecretsVault.h"

#include "animus_kernel/ApiPackageStore.h"
#include "animus_kernel/CryptoUtils.h"
#include "animus_kernel/Log.h"
#include "animus_kernel/SchemaHelpers.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

bool SecretsVault::IsValidSecretName(const std::string& name) {
    if (name.empty() || name.size() > 63) return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool SecretsVault::LoadOrCreateKey(std::string& error) {
    std::ifstream in(m_keyPath);
    if (in.good()) {
        std::string hex((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // Trim exactly one trailing newline (CRLF tolerated); anything else is
        // a malformed file and gets rejected below — not silently eaten.
        if (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r')) {
            hex.pop_back();
            if (!hex.empty() && (hex.back() == '\n' || hex.back() == '\r')) hex.pop_back();
        }
        if (hex.size() != 64) {
            error = "key file '" + m_keyPath + "' must hold 64 hex chars (32 bytes), found " +
                    std::to_string(hex.size());
            return false;
        }
        // Parse into a temporary; commit to m_key only after full validation.
        std::vector<unsigned char> parsed;
        parsed.reserve(32);
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
            parsed.push_back(static_cast<unsigned char>((hi << 4) | lo));
        }
        m_key = std::move(parsed);
        ALOG_INFO("api", "[vault] master key loaded from " << m_keyPath);
        return true;
    }

    // First use — generate. Exclusive, no-follow create with tight mode from
    // the first syscall: no symlink pre-placement, no umask window.
    const std::string keyHex = crypto::RandomHex(32);  // 32 bytes -> 64 hex chars
    {
        const int fd = ::open(m_keyPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd < 0) {
            if (errno == EEXIST) {
                // Lost a creation race (or a pre-existing regular file appeared
                // between the load attempt and now) — reload instead of clobber.
                in.close();
                in.clear();
                in.open(m_keyPath);
                if (in.good()) return LoadOrCreateKey(error);
                error = "key file '" + m_keyPath + "' appeared but cannot be read";
            } else {
                error = "cannot create key file '" + m_keyPath + "' (" + std::strerror(errno) + ")";
            }
            return false;
        }
        const std::string blob = keyHex + "\n";
        size_t off = 0;
        while (off < blob.size()) {
            const ssize_t n = ::write(fd, blob.data() + off, blob.size() - off);
            if (n <= 0) {
                ::close(fd);
                ::unlink(m_keyPath.c_str());  // never leave a partial key
                error = "short write creating key file '" + m_keyPath + "'";
                return false;
            }
            off += static_cast<size_t>(n);
        }
        ::fsync(fd);  // losing this file loses every vault secret — make it durable
        ::close(fd);
    }
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
        if (v.isObject() && v.isMember("secret_ref")) {
            // Strict shape: {"secret_ref"} or {"secret_ref", "value"} — nothing
            // else. Malformed objects are an error, never silently normalized.
            if (!v["secret_ref"].isString() || v.size() > 2 ||
                (v.size() == 2 && !v.isMember("value"))) {
                error = "state key '" + k + "': malformed secret_ref object "
                        "(expected {secret_ref: name} or {secret_ref: name, value: literal})";
                return -1;
            }
            refName = v["secret_ref"].asString();
            if (!IsValidSecretName(refName)) {
                error = "state key '" + k + "': secret_ref name '" + refName +
                        "' is not a valid vault entry name ([A-Za-z0-9_-], 1-63)";
                return -1;
            }
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
                // This package keeps its literals (still valid in-memory) and its
                // vault copies (identical values) — consistent, retried next boot.
                // Continue with the other packages; one bad row must not stall
                // the whole sweep.
                ALOG_ERROR("api", "[vault] migration: state rewrite failed for package '"
                           << pkg.name << "' — secrets already vaulted, state keeps literals; "
                           << "will retry next boot");
                continue;
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
