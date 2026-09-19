#pragma once
// ============================================================================
// SecretsVault (#23) — encrypted secret storage for API packages.
//
// Model:
//   - Secret-typed state keys NEVER hold values in package state JSON.
//     The vault is the only home for secret bytes; state stays pure config.
//   - Entries are namespaced per package. Resolution order for a secret-typed
//     schema key `k` of package `p`:
//       1. state[p][k] == {"secret_ref": "<name>"}  -> vault entry <name>
//          (explicit indirection: sharing / renaming; rare by design)
//       2. vault entry named exactly `k`
//       3. unset -> key stays absent; the existing missing-key hard error
//          at the use site names it. No silent empty strings.
//   - Values are sealed with AES-256-GCM under a master key kept OUTSIDE the
//     database (key file next to the data dir; auto-generated, 0600). This
//     protects against DB-only exfiltration (backups, dumps, state exports);
//     a full-host compromise reads the key file too — honest boundary.
//
// Egress discipline: values are never logged, never returned by list APIs,
// never included in exports or registry payloads. Admin set/get surfaces
// acknowledge by name only.
// ============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <json/json.h>

#include "animus_kernel/IDataStore.h"

namespace animus::kernel {

class ApiPackageStore;

class SecretsVault {
public:
    // keyPath: master key file (64-char hex = 32 bytes). Created with random
    // bytes on first use if missing — loudly logged. Empty keyPath = vault
    // disabled (tests / legacy embed); Set/Get fail closed with an error.
    SecretsVault(IDataStore* store, const std::string& keyPath);

    void EnsureSchema();

    bool Set(const std::string& packageId, const std::string& name,
             const std::string& value, std::string& error);
    // Opens the sealed value. Never logged by callers.
    std::optional<std::string> Get(const std::string& packageId,
                                   const std::string& name) const;
    bool Has(const std::string& packageId, const std::string& name) const;
    bool Delete(const std::string& packageId, const std::string& name);
    void DeleteForPackage(const std::string& packageId);

    struct EntryInfo {
        std::string name;
        int64_t updated_at_unix_ms{0};
    };
    // Names + timestamps only — never values.
    std::vector<EntryInfo> ListForPackage(const std::string& packageId) const;

    bool enabled() const { return !m_key.empty(); }

    // --- state <-> vault helpers (shared by runtime, admin, connections) ----

    // True when v is {"secret_ref": "<name>"} (single-member object).
    static bool IsSecretRef(const Json::Value& v, std::string& name);

    // Vault entry names: [A-Za-z0-9_-], 1-63 chars — shared by runtime writes,
    // split, and the admin routes so indirection targets can never smuggle
    // path separators, whitespace, or unbounded strings into storage keys.
    static bool IsValidSecretName(const std::string& name);

    // In-place resolution of a live state object: for every secret-typed
    // schema key, replace {"secret_ref":...} or vault-backed absence with the
    // opened value. Unset secret -> key REMOVED from `state` (missing-key
    // errors then fire naturally at use sites). Exception: a stale literal in
    // stored state (pre-migration) stays valid in-memory until the boot sweep
    // moves it to the vault. Never writes back to storage.
    // Returns an error string (no values) only on vault misuse (ref to an
    // unset entry) — caller decides severity.
    void ResolveState(const std::string& packageId,
                      const Json::Value& stateSchema,
                      Json::Value& state) const;

    // Split-write: moves literal values on secret-typed schema keys into the
    // vault (dropping them from `state`) and rewrites existing {"secret_ref"}
    // objects only when they carry a new literal under "value" (admin UI
    // write-through form). Returns the number of secrets vaulted; on failure
    // returns -1 with `error` set and `state` left unmodified.
    int SplitStateSecrets(const std::string& packageId,
                          const Json::Value& stateSchema,
                          Json::Value& state,
                          std::string& error) const;

    // One-time, idempotent migration sweep over all installed packages:
    // literal secret values in stored state -> vault entries (named by key),
    // state rewritten without them. Loud log line per migrated secret.
    // Returns migrated count or -1 on error.
    int MigrateLegacyStateSecrets(ApiPackageStore& packages, std::string& error);

    // Seal/open exposure for tests.
    bool Seal(const std::string& plaintext, std::string& envelopeHex, std::string& error) const;
    bool Open(const std::string& envelopeHex, std::string& plaintext, std::string& error) const;

private:
    bool LoadOrCreateKey(std::string& error);

    IDataStore* m_store;
    std::string m_keyPath;
    std::vector<unsigned char> m_key;
};

}  // namespace animus::kernel
