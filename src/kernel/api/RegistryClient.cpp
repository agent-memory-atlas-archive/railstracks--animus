#include "animus_kernel/api/RegistryClient.h"

#include <json/json.h>

#include <sstream>

#include "animus_kernel/api/ManifestV1.h"

namespace animus::kernel::registry {

namespace {

std::string TrimTrailingSlash(const std::string& s) {
    std::string out = s;
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

// Shared status-code -> error translation for registry GETs.
// status_code 0 = transport failure (unreachable / timeout).
std::string RegistryHttpError(const std::string& what, const HttpClient::Response& resp) {
    if (resp.status_code == 0) return "registry unreachable: " + resp.error;
    if (resp.status_code == 404) return what + " not found on registry";
    return "registry returned HTTP " + std::to_string(resp.status_code);
}

Json::Value ParseJsonBody(const std::string& body, std::string& err) {
    Json::CharReaderBuilder rb;
    rb["collectComments"] = false;
    Json::Value root;
    std::istringstream ss(body);
    if (!Json::parseFromStream(rb, ss, &root, &err)) {
        err = "registry response is not valid JSON: " + err;
        return Json::Value();
    }
    return root;
}

}  // namespace

HttpClient::Response FetchPackageManifest(HttpClient& http,
                                          const std::string& registryBase,
                                          const std::string& name,
                                          const std::string& version) {
    HttpClient::Request req;
    req.method = "GET";
    std::string url = TrimTrailingSlash(registryBase) + "/api/v1/packages/" + name;
    if (!version.empty()) url += "/versions/" + version;
    req.url = url;
    req.headers["Accept"] = "application/json";
    req.timeout_seconds = 10;
    return http.Execute(req);
}

bool ParseRegistryPackageList(const std::string& body,
                               std::vector<RegistryPackageSummary>& out,
                               std::string& err) {
    out.clear();
    Json::Value root = ParseJsonBody(body, err);
    if (!err.empty()) return false;
    if (!root.isObject() || !root.isMember("packages") || !root["packages"].isArray()) {
        err = "registry list response missing packages[]";
        return false;
    }
    for (const Json::Value& row : root["packages"]) {
        if (!row.isObject() || !row.isMember("name") || !row["name"].isString()) {
            err = "registry list row missing name";
            return false;
        }
        RegistryPackageSummary s;
        s.name = row["name"].asString();
        s.version = row.get("version", 0).asInt();
        s.semantic_version = row.get("semantic_version", "").asString();
        s.description = row.get("description", "").asString();
        s.content_hash = row.get("content_hash", "").asString();
        s.official = row.get("official", false).asBool();
        s.published_at = row.get("published_at", "").asString();
        out.push_back(std::move(s));
    }
    return true;
}

bool ParseRegistryVersionList(const std::string& body,
                              std::string& name,
                              std::vector<RegistryPackageVersion>& out,
                              std::string& err) {
    out.clear();
    name.clear();
    Json::Value root = ParseJsonBody(body, err);
    if (!err.empty()) return false;
    if (!root.isObject() || !root.isMember("versions") || !root["versions"].isArray()) {
        err = "registry versions response missing versions[]";
        return false;
    }
    name = root.get("name", "").asString();
    for (const Json::Value& row : root["versions"]) {
        RegistryPackageVersion v;
        v.version = row.get("version", 0).asInt();
        v.semantic_version = row.get("semantic_version", "").asString();
        v.content_hash = row.get("content_hash", "").asString();
        v.published_at = row.get("published_at", "").asString();
        if (v.semantic_version.empty()) {
            err = "registry version row missing semantic_version";
            return false;
        }
        out.push_back(std::move(v));
    }
    return true;
}

RegistryListResult ListRegistryPackages(HttpClient& http,
                                        const std::string& registryBase,
                                        bool allowPrivate) {
    RegistryListResult result;
    result.registry = TrimTrailingSlash(registryBase);
    if (result.registry.empty()) {
        result.err = "registry base URL is empty";
        return result;
    }
    if (allowPrivate) http.SetAllowPrivateAddresses(true);
    HttpClient::Request req;
    req.method = "GET";
    req.url = result.registry + "/api/v1/packages";
    req.headers["Accept"] = "application/json";
    req.timeout_seconds = 10;
    HttpClient::Response resp = http.Execute(req);
    if (resp.status_code != 200) {
        result.err = RegistryHttpError("package list", resp);
        return result;
    }
    if (!ParseRegistryPackageList(resp.body, result.packages, result.err)) return result;
    result.ok = true;
    return result;
}

RegistryVersionsResult ListRegistryPackageVersions(HttpClient& http,
                                                    const std::string& registryBase,
                                                    const std::string& name,
                                                    bool allowPrivate) {
    RegistryVersionsResult result;
    result.registry = TrimTrailingSlash(registryBase);
    if (result.registry.empty()) {
        result.err = "registry base URL is empty";
        return result;
    }
    if (name.empty()) {
        result.err = "package name is empty";
        return result;
    }
    if (allowPrivate) http.SetAllowPrivateAddresses(true);
    HttpClient::Request req;
    req.method = "GET";
    req.url = result.registry + "/api/v1/packages/" + name + "/versions";
    req.headers["Accept"] = "application/json";
    req.timeout_seconds = 10;
    HttpClient::Response resp = http.Execute(req);
    if (resp.status_code != 200) {
        result.err = RegistryHttpError("package '" + name + "'", resp);
        return result;
    }
    if (!ParseRegistryVersionList(resp.body, result.name, result.versions, result.err)) {
        return result;
    }
    if (!result.name.empty() && result.name != name) {
        result.err = "registry name mismatch (asked '" + name + "', got '" + result.name + "')";
        return result;
    }
    result.ok = true;
    return result;
}

RegistryInstallResult InstallFromRegistry(ApiPackageStore& store,
                                           HttpClient& http,
                                           const std::string& registryBase,
                                           const std::string& name,
                                           const std::string& version,
                                           bool allowPrivate) {
    RegistryInstallResult result;

    // 1. Fetch
    if (allowPrivate) http.SetAllowPrivateAddresses(true);
    HttpClient::Response resp = FetchPackageManifest(http, registryBase, name, version);
    if (resp.status_code == 0) {
        result.err = "registry unreachable: " + resp.error;
        return result;
    }
    if (resp.status_code == 404) {
        result.err = "package not found on registry";
        return result;
    }
    if (resp.status_code != 200) {
        result.err = "registry returned HTTP " + std::to_string(resp.status_code);
        return result;
    }

    // 2. Parse envelope {name, semantic_version, content_hash, manifest}
    Json::CharReaderBuilder rb;
    rb["collectComments"] = false;
    Json::Value envelope;
    std::string parseErr;
    std::istringstream ss(resp.body);
    if (!Json::parseFromStream(rb, ss, &envelope, &parseErr)) {
        result.err = "registry response is not valid JSON: " + parseErr;
        return result;
    }
    if (!envelope.isObject() || !envelope.isMember("manifest") ||
        !envelope.isMember("content_hash")) {
        result.err = "registry response missing manifest/content_hash";
        return result;
    }
    const std::string advertisedHash = envelope["content_hash"].asString();
    const std::string semanticVersion = envelope.get("semantic_version", "").asString();
    const std::string registryName = envelope.get("name", "").asString();
    if (registryName != name) {
        result.err = "registry name mismatch (asked '" + name + "', got '" + registryName + "')";
        return result;
    }

    // 3. Re-canonicalize + re-hash locally; must match the advertised hash.
    Json::StreamWriterBuilder wb;
    const std::string manifestJson = Json::writeString(wb, envelope["manifest"]);
    manifest_v1::Processed processed;
    std::string manifestErr;
    if (!manifest_v1::Process(manifestJson, processed, manifestErr)) {
        result.err = "manifest failed local validation: " + manifestErr;
        return result;
    }
    if (processed.content_hash != advertisedHash) {
        result.err = "CONTENT HASH MISMATCH — registry advertised " + advertisedHash +
                     " but manifest hashes to " + processed.content_hash +
                     " (refusing to install)";
        return result;
    }

    // 4. Install (disabled by design; user enables after configuring state).
    try {
        result.pkg = store.InstallFromManifest(manifestJson, TrimTrailingSlash(registryBase),
                                               semanticVersion.empty() ? processed.manifest.get("version", "").asString()
                                                                       : semanticVersion);
    } catch (const std::exception& e) {
        result.err = std::string("install failed: ") + e.what();
        return result;
    }

    result.ok = true;
    result.content_hash = processed.content_hash;
    result.semantic_version = semanticVersion;
    return result;
}

}  // namespace animus::kernel::registry
