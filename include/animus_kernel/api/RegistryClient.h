#pragma once
// ============================================================================
// RegistryClient — install API packages from an Animus Registry
// (https://animus-registry.steadyfort.com or any compatible server).
//
// Wire format (registry API v1):
//   GET {base}/api/v1/packages/{name}                  -> latest
//   GET {base}/api/v1/packages/{name}/versions/{v}     -> pinned (int or semver)
//   Response: { name, version, semantic_version, content_hash, manifest: {...} }
//
// Security: the fetched manifest is re-canonicalized and re-hashed locally
// (manifest_v1::Process) and MUST match the registry's content_hash before
// install. Hash mismatch = reject loudly. A registry can serve a manifest,
// but not a different one than the hash it advertises.
// ============================================================================

#include <string>
#include <vector>

#include "animus_kernel/ApiPackageStore.h"
#include "animus_kernel/tools/HttpClient.h"

namespace animus::kernel::registry {

struct RegistryInstallResult {
    bool ok{false};
    ApiPackage pkg;
    std::string err;
    std::string content_hash;   // verified hash (on success)
    std::string semantic_version;
};

// Remote package summary as listed by a registry (GET /api/v1/packages).
struct RegistryPackageSummary {
    std::string name;
    int version{0};                 // registry-internal int version
    std::string semantic_version;   // e.g. "1.4.2"
    std::string description;
    std::string content_hash;
    bool official{false};
    std::string published_at;       // ISO-8601 as served
};

struct RegistryPackageVersion {
    int version{0};
    std::string semantic_version;
    std::string content_hash;
    std::string published_at;
};

struct RegistryListResult {
    bool ok{false};
    std::string err;
    std::string registry;                       // normalized base (trailing slash trimmed)
    std::vector<RegistryPackageSummary> packages;
};

struct RegistryVersionsResult {
    bool ok{false};
    std::string err;
    std::string registry;
    std::string name;                           // as confirmed by the registry
    std::vector<RegistryPackageVersion> versions;
};

// Fetch + verify + install. Blocking (bounded by the HTTP timeout, ~10s).
// allowPrivate: opt-in for localhost/LAN registries (operator-configured target,
// admin-authenticated call — not reachable from model-controlled surfaces).
RegistryInstallResult InstallFromRegistry(ApiPackageStore& store,
                                           HttpClient& http,
                                           const std::string& registryBase,
                                           const std::string& name,
                                           const std::string& version = "" /* latest */,
                                           bool allowPrivate = false);

// Fetch only (list/detail probes, UI "what's available"): returns raw body.
HttpClient::Response FetchPackageManifest(HttpClient& http,
                                          const std::string& registryBase,
                                          const std::string& name,
                                          const std::string& version = "");

// Browse: list available packages on a registry (GET /api/v1/packages).
// Blocking (bounded by the HTTP timeout, ~10s). SSRF posture matches install:
// public URLs only unless allowPrivate is explicitly set (operator-configured
// target, admin-authenticated call).
RegistryListResult ListRegistryPackages(HttpClient& http,
                                        const std::string& registryBase,
                                        bool allowPrivate = false);

// Browse detail: all published versions of one package
// (GET /api/v1/packages/{name}/versions). Versions are immutable on the
// registry; rows arrive newest-first.
RegistryVersionsResult ListRegistryPackageVersions(HttpClient& http,
                                                    const std::string& registryBase,
                                                    const std::string& name,
                                                    bool allowPrivate = false);

// Parse helpers (exported for tests — no network involved).
bool ParseRegistryPackageList(const std::string& body,
                               std::vector<RegistryPackageSummary>& out,
                               std::string& err);
bool ParseRegistryVersionList(const std::string& body,
                              std::string& name,
                              std::vector<RegistryPackageVersion>& out,
                              std::string& err);

}  // namespace animus::kernel::registry
