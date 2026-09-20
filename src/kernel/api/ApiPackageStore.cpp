#include "animus_kernel/ApiPackageStore.h"
#include "animus_kernel/SchemaHelpers.h"
#include "animus_kernel/Log.h"

#include <json/json.h>

#include <atomic>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>

namespace animus::kernel {

namespace {

// Package names may never shadow management verbs (docs/api/TOOL.md).
bool IsReservedFirstWord(const std::string& name) {
    static const char* kReserved[] = {
        "package", "command", "connection", "files",
        "download", "upload", "enable", "disable", "status"};
    for (const char* r : kReserved)
        if (name == r) return true;
    return false;
}

bool IsValidSlug(const std::string& s) {
    if (s.empty() || s.size() > 63) return false;
    if (!(isalnum(static_cast<unsigned char>(s[0])) && islower(static_cast<unsigned char>(s[0]))))
        return false;
    for (char c : s) {
        if (!(islower(static_cast<unsigned char>(c)) || isdigit(static_cast<unsigned char>(c)) ||
              c == '-'))
            return false;
    }
    return true;
}

// Command names: lowercase, spaces allowed, no leading/trailing/double space.
bool IsValidCommandName(const std::string& s) {
    if (s.empty() || s.size() > 100) return false;
    if (s.front() == ' ' || s.back() == ' ') return false;
    bool prevSpace = false;
    for (char c : s) {
        bool space = (c == ' ');
        if (space && prevSpace) return false;
        if (!(space || islower(static_cast<unsigned char>(c)) ||
              isdigit(static_cast<unsigned char>(c)) || c == '-'))
            return false;
        prevSpace = space;
    }
    return true;
}

std::string GenerateIdHex() {
    static thread_local std::mt19937_64 rng(static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    static std::atomic<std::uint64_t> counter{0};
    std::uniform_int_distribution<std::uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << dist(rng)
        << std::setw(8) << counter.fetch_add(1);
    return oss.str();
}

std::string JsonCompact(const Json::Value& v) {
    Json::StreamWriterBuilder b;
    b["indentation"] = "";
    b["commentStyle"] = "None";
    return Json::writeString(b, v);
}

bool ParseJson(const std::string& text, Json::Value& out, std::string& err) {
    Json::CharReaderBuilder b;
    std::istringstream iss(text);
    return Json::parseFromStream(b, iss, &out, &err);
}

}  // namespace

ApiPackageStore::ApiPackageStore(IDataStore* store) : m_store(store) {
    EnsureSchema();
}

namespace {

// Host-pattern shape: lowercase host possibly with one leading "*." wildcard.
// No scheme, port, path, userinfo — scope is a DOMAIN, not a URL.
bool IsValidHostPattern(const std::string& p) {
    if (p.empty() || p.size() > 253) return false;
    for (char c : p)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' ||
              c == '*'))
            return false;
    if (p.find('*') != std::string::npos && p.substr(0, 2) != "*.")
        return false;  // wildcard only as a whole leading label
    if (p.front() == '.' || p.back() == '.' || p.find("..") != std::string::npos)
        return false;  // no empty DNS labels (leading/trailing/double dots)
    return true;
}

// Extract the host from a (possibly templated) URL after substituting
// {{state.<key>}} tokens with their schema string defaults. Returns "" when
// the host is not statically derivable.
std::string ExtractHostFromTemplate(const std::string& tmpl, const Json::Value& stateSchema) {
    std::string s = tmpl;
    if (stateSchema.isObject()) {
        for (const std::string& k : stateSchema.getMemberNames()) {
            const Json::Value& def = stateSchema[k];
            if (!def.isObject() || !def.isMember("default") || !def["default"].isString())
                continue;
            const std::string token = "{{state." + k + "}}";
            size_t pos;
            while ((pos = s.find(token)) != std::string::npos)
                s.replace(pos, token.size(), def["default"].asString());
        }
    }
    const size_t sep = s.find("://");
    if (sep == std::string::npos) return "";
    size_t hostStart = sep + 3;
    size_t hostEnd = hostStart;
    while (hostEnd < s.size() && s[hostEnd] != '/' && s[hostEnd] != '?' && s[hostEnd] != '#')
        ++hostEnd;
    std::string host = s.substr(hostStart, hostEnd - hostStart);
    const size_t at = host.rfind('@');   // strip userinfo if present
    if (at != std::string::npos) host = host.substr(at + 1);
    const size_t colon = host.rfind(':'); // strip port (naive; [::1]:80 handled)
    if (!host.empty() && host[0] != '[' && colon != std::string::npos)
        host = host.substr(0, colon);
    if (host.find('{') != std::string::npos || host.find('}') != std::string::npos)
        return "";  // still templated beyond schema defaults
    std::transform(host.begin(), host.end(), host.begin(), ::tolower);
    return IsValidHostPattern(host) ? host : "";
}

std::string JsonHostArray(const std::vector<std::string>& hosts) {
    Json::Value arr(Json::arrayValue);
    for (const auto& h : hosts) arr.append(h);
    return JsonCompact(arr);
}

} // namespace

void ApiPackageStore::EnsureSchema() {
    if (!m_store) return;

    schema::CreateTable(m_store, R"(
        CREATE TABLE IF NOT EXISTS api_packages (
            id TEXT PRIMARY KEY,
            name TEXT UNIQUE NOT NULL,
            display_name TEXT NOT NULL DEFAULT '',
            description TEXT NOT NULL,
            keywords TEXT NOT NULL DEFAULT '[]',
            version TEXT NOT NULL,
            registry_source TEXT NOT NULL DEFAULT '',
            registry_version TEXT NOT NULL DEFAULT '',
            locally_modified INTEGER NOT NULL DEFAULT 0,
            enabled INTEGER NOT NULL DEFAULT 0,
            dispatch_cooldown_ms INTEGER NOT NULL DEFAULT 10000,
            files_quota_mb INTEGER NOT NULL DEFAULT 256,
            state_schema TEXT NOT NULL DEFAULT '{}',
            state TEXT NOT NULL DEFAULT '{}',
            egress_hosts TEXT,
            created_at_unix_ms INTEGER NOT NULL,
            updated_at_unix_ms INTEGER NOT NULL
        );
    )");

    // #25 migration: legacy rows keep NULL (the sweep sentinel — an explicit
    // manifest "egress_hosts": [] must never be confused with a pre-scope
    // row). See MigrateEgressScopes below for the one-shot backfill pass.
    if (!schema::ColumnExists(m_store, "api_packages", "egress_hosts"))
        schema::CreateTable(m_store,
            "ALTER TABLE api_packages ADD COLUMN egress_hosts TEXT");

    schema::CreateTable(m_store, R"(
        CREATE TABLE IF NOT EXISTS api_package_agents (
            id TEXT PRIMARY KEY,
            package_id TEXT NOT NULL,
            agent_id TEXT NOT NULL,
            enabled INTEGER NOT NULL DEFAULT 1,
            created_at_unix_ms INTEGER NOT NULL,
            UNIQUE (package_id, agent_id)
        );
    )");

    schema::CreateTable(m_store, R"(
        CREATE TABLE IF NOT EXISTS api_package_commands (
            id TEXT PRIMARY KEY,
            package_id TEXT NOT NULL,
            name TEXT NOT NULL,
            kind TEXT NOT NULL,
            event TEXT NOT NULL DEFAULT '',
            description TEXT NOT NULL,
            parameters TEXT NOT NULL DEFAULT '{}',
            request TEXT NOT NULL DEFAULT '',
            script TEXT NOT NULL,
            created_at_unix_ms INTEGER NOT NULL,
            UNIQUE (package_id, name)
        );
    )");

    schema::CreateTable(m_store, R"(
        CREATE TABLE IF NOT EXISTS api_package_connections (
            id TEXT PRIMARY KEY,
            package_id TEXT NOT NULL,
            name TEXT NOT NULL,
            type TEXT NOT NULL,
            enabled INTEGER NOT NULL DEFAULT 1,
            url_template TEXT NOT NULL,
            headers_template TEXT NOT NULL DEFAULT '{}',
            poll TEXT NOT NULL DEFAULT '',
            hooks TEXT NOT NULL DEFAULT '{}',
            created_at_unix_ms INTEGER NOT NULL,
            UNIQUE (package_id, name)
        );
    )");

    // #25 sweep LAST: it reads api_package_commands/api_package_connections,
    // so every table it depends on must exist before it runs.
    MigrateEgressScopes();
}

void ApiPackageStore::MigrateEgressScopes() {
    // #25 backfill: rows predating egress scoping have NULL (empty string
    // here). Deny-by-default would hard-break them on upgrade, so derive each
    // scope from the STORED rows (commands' request url templates, connection
    // templates, state_schema defaults) — same derivation as fresh installs.
    // One-shot by construction: the sweep always writes a value (possibly
    // '[]' = derived-deny-all), so a row is NULL at most once. A manifest
    // that explicitly declares "egress_hosts": [] stores '[]' and is NEVER
    // re-derived — declared deny-all survives restarts.
    for (const auto& pkg : ListPackages()) {
        if (!pkg.egress_hosts.empty()) continue;
        Json::Value stateSchema;
        std::string schemaErr;
        ParseJson(pkg.state_schema.empty() ? "{}" : pkg.state_schema, stateSchema, schemaErr);
        if (!stateSchema.isObject()) stateSchema = Json::Value(Json::objectValue);
        std::vector<std::string> derived;
        auto pushUnique = [&](const std::string& url) {
            const std::string host = ExtractHostFromTemplate(url, stateSchema);
            if (host.empty()) return;
            for (const auto& d : derived)
                if (d == host) return;
            if (derived.size() < 32) derived.push_back(host);
        };
        for (const auto& cmd : ListCommands(pkg.id)) {
            if (cmd.request.empty()) continue;
            Json::Value rq;
            std::string reqErr;
            ParseJson(cmd.request, rq, reqErr);
            if (rq.isObject() && rq.isMember("url") && rq["url"].isString())
                pushUnique(rq["url"].asString());
        }
        for (const auto& conn : ListConnections(pkg.id))
            if (!conn.url_template.empty()) pushUnique(conn.url_template);
        auto stmt = m_store->Prepare(
            "UPDATE api_packages SET egress_hosts = ? WHERE id = ?");
        if (!stmt) continue;
        stmt->BindText(1, JsonHostArray(derived));
        stmt->BindText(2, pkg.id);
        stmt->ExecDML();
        ALOG_INFO("api", "[egress] derived scope for legacy package '" << pkg.name
                   << "': " << JsonHostArray(derived));
    }
}

int64_t ApiPackageStore::NowUnixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string ApiPackageStore::GenerateId() const { return GenerateIdHex(); }

// ---------------------------------------------------------------------------
// Packages
// ---------------------------------------------------------------------------

namespace {

ApiPackage RowToPackage(const std::unique_ptr<IStatement>& stmt) {
    ApiPackage p;
    p.id = stmt->ColumnText(0);
    p.name = stmt->ColumnText(1);
    p.display_name = stmt->ColumnText(2);
    p.description = stmt->ColumnText(3);
    p.keywords = stmt->ColumnText(4);
    p.version = stmt->ColumnText(5);
    p.registry_source = stmt->ColumnText(6);
    p.registry_version = stmt->ColumnText(7);
    p.locally_modified = stmt->ColumnInt64(8) != 0;
    p.enabled = stmt->ColumnInt64(9) != 0;
    p.dispatch_cooldown_ms = stmt->ColumnInt64(10);
    p.files_quota_mb = stmt->ColumnInt64(11);
    p.state_schema = stmt->ColumnText(12);
    p.state = stmt->ColumnText(13);
    p.egress_hosts = stmt->ColumnText(16);
    p.created_at_unix_ms = stmt->ColumnInt64(14);
    p.updated_at_unix_ms = stmt->ColumnInt64(15);
    return p;
}

const char* kPackageColumns =
    "id, name, display_name, description, keywords, version, registry_source, "
    "registry_version, locally_modified, enabled, dispatch_cooldown_ms, "
    "files_quota_mb, state_schema, state, created_at_unix_ms, updated_at_unix_ms, "
    "egress_hosts";

ApiPackageCommand RowToCommand(const std::unique_ptr<IStatement>& stmt) {
    ApiPackageCommand c;
    c.id = stmt->ColumnText(0);
    c.package_id = stmt->ColumnText(1);
    c.name = stmt->ColumnText(2);
    c.kind = stmt->ColumnText(3);
    c.event = stmt->ColumnText(4);
    c.description = stmt->ColumnText(5);
    c.parameters = stmt->ColumnText(6);
    c.request = stmt->ColumnText(7);
    c.script = stmt->ColumnText(8);
    return c;
}

const char* kCommandColumns =
    "id, package_id, name, kind, event, description, parameters, request, script";

ApiPackageConnection RowToConnection(const std::unique_ptr<IStatement>& stmt) {
    ApiPackageConnection c;
    c.id = stmt->ColumnText(0);
    c.package_id = stmt->ColumnText(1);
    c.name = stmt->ColumnText(2);
    c.type = stmt->ColumnText(3);
    c.enabled = stmt->ColumnInt64(4) != 0;
    c.url_template = stmt->ColumnText(5);
    c.headers_template = stmt->ColumnText(6);
    c.poll = stmt->ColumnText(7);
    c.hooks = stmt->ColumnText(8);
    return c;
}

const char* kConnectionColumns =
    "id, package_id, name, type, enabled, url_template, headers_template, poll, hooks";

}  // namespace

ApiPackage ApiPackageStore::CreatePackage(const ApiPackage& pkg) {
    ValidateName(pkg.name);
    if (GetPackageByName(pkg.name))
        throw std::runtime_error("api package '" + pkg.name + "' already exists");

    const int64_t now = NowUnixMs();
    const std::string id = pkg.id.empty() ? GenerateId() : pkg.id;

    auto stmt = m_store->Prepare(
        "INSERT INTO api_packages (id, name, display_name, description, keywords, "
        "version, registry_source, registry_version, locally_modified, enabled, "
        "dispatch_cooldown_ms, files_quota_mb, state_schema, state, "
        "created_at_unix_ms, updated_at_unix_ms, egress_hosts) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!stmt) throw std::runtime_error("api_packages insert prepare failed");
    stmt->BindText(1, id);
    stmt->BindText(2, pkg.name);
    stmt->BindText(3, pkg.display_name);
    stmt->BindText(4, pkg.description);
    stmt->BindText(5, pkg.keywords.empty() ? "[]" : pkg.keywords);
    stmt->BindText(6, pkg.version);
    stmt->BindText(7, pkg.registry_source);
    stmt->BindText(8, pkg.registry_version);
    stmt->BindInt64(9, pkg.locally_modified ? 1 : 0);
    stmt->BindInt64(10, pkg.enabled ? 1 : 0);
    stmt->BindInt64(11, pkg.dispatch_cooldown_ms);
    stmt->BindInt64(12, pkg.files_quota_mb);
    stmt->BindText(13, pkg.state_schema.empty() ? "{}" : pkg.state_schema);
    stmt->BindText(14, pkg.state.empty() ? "{}" : pkg.state);
    stmt->BindInt64(15, now);
    stmt->BindInt64(16, now);
    stmt->BindText(17, pkg.egress_hosts.empty() ? "[]" : pkg.egress_hosts);
    stmt->ExecDML();
    stmt->Finalize();

    ApiPackage stored = pkg;
    stored.id = id;
    stored.created_at_unix_ms = now;
    stored.updated_at_unix_ms = now;
    return stored;
}

std::optional<ApiPackage> ApiPackageStore::GetPackage(const std::string& id) const {
    auto stmt = m_store->Prepare(std::string("SELECT ") + kPackageColumns +
                                 " FROM api_packages WHERE id = ?");
    if (!stmt) return std::nullopt;
    stmt->BindText(1, id);
    if (!stmt->Step()) return std::nullopt;
    return RowToPackage(stmt);
}

std::optional<ApiPackage> ApiPackageStore::GetPackageByName(const std::string& name) const {
    auto stmt = m_store->Prepare(std::string("SELECT ") + kPackageColumns +
                                 " FROM api_packages WHERE name = ?");
    if (!stmt) return std::nullopt;
    stmt->BindText(1, name);
    if (!stmt->Step()) return std::nullopt;
    return RowToPackage(stmt);
}

std::vector<ApiPackage> ApiPackageStore::ListPackages() const {
    std::vector<ApiPackage> out;
    auto stmt = m_store->Prepare(std::string("SELECT ") + kPackageColumns +
                                 " FROM api_packages ORDER BY name ASC");
    if (!stmt) return out;
    while (stmt->Step()) out.push_back(RowToPackage(stmt));
    return out;
}

bool ApiPackageStore::UpdatePackageMeta(const ApiPackage& pkg) {
    if (!GetPackage(pkg.id)) return false;
    auto stmt = m_store->Prepare(
        "UPDATE api_packages SET display_name = ?, description = ?, keywords = ?, "
        "version = ?, registry_source = ?, registry_version = ?, locally_modified = ?, "
        "dispatch_cooldown_ms = ?, files_quota_mb = ?, state_schema = ?, "
        "egress_hosts = ?, updated_at_unix_ms = ? WHERE id = ?");
    if (!stmt) return false;
    stmt->BindText(1, pkg.display_name);
    stmt->BindText(2, pkg.description);
    stmt->BindText(3, pkg.keywords.empty() ? "[]" : pkg.keywords);
    stmt->BindText(4, pkg.version);
    stmt->BindText(5, pkg.registry_source);
    stmt->BindText(6, pkg.registry_version);
    stmt->BindInt64(7, pkg.locally_modified ? 1 : 0);
    stmt->BindInt64(8, pkg.dispatch_cooldown_ms);
    stmt->BindInt64(9, pkg.files_quota_mb);
    stmt->BindText(10, pkg.state_schema.empty() ? "{}" : pkg.state_schema);
    stmt->BindText(11, pkg.egress_hosts.empty() ? "[]" : pkg.egress_hosts);
    stmt->BindInt64(12, NowUnixMs());
    stmt->BindText(13, pkg.id);
    stmt->ExecDML();
    stmt->Finalize();
    return true;
}

bool ApiPackageStore::SetPackageEnabled(const std::string& id, bool enabled) {
    auto stmt = m_store->Prepare(
        "UPDATE api_packages SET enabled = ?, updated_at_unix_ms = ? WHERE id = ?");
    if (!stmt) return false;
    stmt->BindInt64(1, enabled ? 1 : 0);
    stmt->BindInt64(2, NowUnixMs());
    stmt->BindText(3, id);
    stmt->ExecDML();
    stmt->Finalize();
    return m_store->Changes() > 0;
}

bool ApiPackageStore::SetPackageState(const std::string& id, const std::string& stateJson) {
    Json::Value v;
    std::string err;
    if (!ParseJson(stateJson.empty() ? "{}" : stateJson, v, err) || !v.isObject())
        return false;
    if (!GetPackage(id)) return false;
    auto stmt = m_store->Prepare(
        "UPDATE api_packages SET state = ?, updated_at_unix_ms = ? WHERE id = ?");
    if (!stmt) return false;
    stmt->BindText(1, JsonCompact(v));
    stmt->BindInt64(2, NowUnixMs());
    stmt->BindText(3, id);
    stmt->ExecDML();
    stmt->Finalize();
    return true;
}

bool ApiPackageStore::DeleteRows(const char* table, const char* column,
                                const std::string& id) {
    auto stmt = m_store->Prepare(std::string("DELETE FROM ") + table +
                                 " WHERE " + column + " = ?");
    if (!stmt) return false;
    if (!stmt->BindText(1, id)) return false;
    if (!stmt->ExecDML()) return false;
    stmt->Finalize();
    return true;
}

bool ApiPackageStore::DeletePackage(const std::string& id) {
    if (!GetPackage(id)) return false;
    m_store->BeginTransaction();
    if (!DeleteRows("api_package_commands", "package_id", id) ||
        !DeleteRows("api_package_connections", "package_id", id) ||
        !DeleteRows("api_package_agents", "package_id", id) ||
        !DeleteRows("api_packages", "id", id)) {
        ALOG_ERROR("api", "[api-package-store] DeletePackage DELETE FAILED: "
                  << m_store->ErrMsg() << " (package '" << id << "')");
        m_store->Rollback();
        return false;
    }
    if (!m_store->Commit()) {
        ALOG_ERROR("api", "[api-package-store] DeletePackage COMMIT FAILED: "
                  << m_store->ErrMsg() << " (package '" << id << "' NOT deleted)");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

ApiPackageCommand ApiPackageStore::AddCommand(const ApiPackageCommand& cmd) {
    if (GetCommand(cmd.package_id, cmd.name))
        throw std::runtime_error("command '" + cmd.name + "' already exists in this package");

    auto stmt = m_store->Prepare(
        "INSERT INTO api_package_commands (id, package_id, name, kind, event, "
        "description, parameters, request, script, created_at_unix_ms) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!stmt) throw std::runtime_error("api_package_commands insert prepare failed");
    ApiPackageCommand stored = cmd;
    stored.id = cmd.id.empty() ? GenerateId() : cmd.id;
    stmt->BindText(1, stored.id);
    stmt->BindText(2, stored.package_id);
    stmt->BindText(3, stored.name);
    stmt->BindText(4, stored.kind);
    stmt->BindText(5, stored.event);
    stmt->BindText(6, stored.description);
    stmt->BindText(7, stored.parameters.empty() ? "{}" : stored.parameters);
    stmt->BindText(8, stored.request);
    stmt->BindText(9, stored.script);
    stmt->BindInt64(10, NowUnixMs());
    stmt->ExecDML();
    stmt->Finalize();
    return stored;
}

std::optional<ApiPackageCommand> ApiPackageStore::GetCommand(const std::string& packageId,
                                                             const std::string& name) const {
    auto stmt = m_store->Prepare(std::string("SELECT ") + kCommandColumns +
                                 " FROM api_package_commands "
                                 "WHERE package_id = ? AND name = ?");
    if (!stmt) return std::nullopt;
    stmt->BindText(1, packageId);
    stmt->BindText(2, name);
    if (!stmt->Step()) return std::nullopt;
    return RowToCommand(stmt);
}

std::vector<ApiPackageCommand> ApiPackageStore::ListCommands(const std::string& packageId) const {
    std::vector<ApiPackageCommand> out;
    auto stmt = m_store->Prepare(std::string("SELECT ") + kCommandColumns +
                                 " FROM api_package_commands WHERE package_id = ? "
                                 "ORDER BY kind ASC, name ASC");  // actions before hooks
    if (!stmt) return out;
    stmt->BindText(1, packageId);
    while (stmt->Step()) out.push_back(RowToCommand(stmt));
    return out;
}

bool ApiPackageStore::DeleteCommand(const std::string& id) {
    auto stmt = m_store->Prepare("DELETE FROM api_package_commands WHERE id = ?");
    if (!stmt) return false;
    stmt->BindText(1, id);
    stmt->ExecDML();
    stmt->Finalize();
    return m_store->Changes() > 0;
}

int ApiPackageStore::ReplaceCommands(const std::string& packageId,
                                     const std::vector<ApiPackageCommand>& cmds) {
    m_store->BeginTransaction();
    try {
        if (!DeleteRows("api_package_commands", "package_id", packageId))
            throw std::runtime_error("delete commands failed: " + m_store->ErrMsg());
        int inserted = 0;
        for (const auto& c : cmds) {
            ApiPackageCommand copy = c;
            copy.package_id = packageId;
            AddCommand(copy);
            inserted++;
        }
        if (!m_store->Commit()) {
            ALOG_ERROR("api", "[api-package-store] ReplaceCommands COMMIT FAILED: "
                      << m_store->ErrMsg() << " (commands for '" << packageId
                      << "' NOT replaced)");
            m_store->Rollback();
            throw std::runtime_error("commit failed: " + m_store->ErrMsg());
        }
        return inserted;
    } catch (...) {
        m_store->Rollback();
        throw;
    }
}

// ---------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------

ApiPackageConnection ApiPackageStore::AddConnection(const ApiPackageConnection& conn) {
    if (GetConnection(conn.package_id, conn.name))
        throw std::runtime_error("connection '" + conn.name + "' already exists in this package");

    auto stmt = m_store->Prepare(
        "INSERT INTO api_package_connections (id, package_id, name, type, enabled, "
        "url_template, headers_template, poll, hooks, created_at_unix_ms) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!stmt) throw std::runtime_error("api_package_connections insert prepare failed");
    ApiPackageConnection stored = conn;
    stored.id = conn.id.empty() ? GenerateId() : conn.id;
    stmt->BindText(1, stored.id);
    stmt->BindText(2, stored.package_id);
    stmt->BindText(3, stored.name);
    stmt->BindText(4, stored.type);
    stmt->BindInt64(5, stored.enabled ? 1 : 0);
    stmt->BindText(6, stored.url_template);
    stmt->BindText(7, stored.headers_template.empty() ? "{}" : stored.headers_template);
    stmt->BindText(8, stored.poll);
    stmt->BindText(9, stored.hooks.empty() ? "{}" : stored.hooks);
    stmt->BindInt64(10, NowUnixMs());
    stmt->ExecDML();
    stmt->Finalize();
    return stored;
}

std::optional<ApiPackageConnection> ApiPackageStore::GetConnection(const std::string& packageId,
                                                                   const std::string& name) const {
    auto stmt = m_store->Prepare(std::string("SELECT ") + kConnectionColumns +
                                 " FROM api_package_connections "
                                 "WHERE package_id = ? AND name = ?");
    if (!stmt) return std::nullopt;
    stmt->BindText(1, packageId);
    stmt->BindText(2, name);
    if (!stmt->Step()) return std::nullopt;
    return RowToConnection(stmt);
}

std::vector<ApiPackageConnection> ApiPackageStore::ListConnections(
    const std::string& packageId) const {
    std::vector<ApiPackageConnection> out;
    auto stmt = m_store->Prepare(std::string("SELECT ") + kConnectionColumns +
                                 " FROM api_package_connections WHERE package_id = ? "
                                 "ORDER BY name ASC");
    if (!stmt) return out;
    stmt->BindText(1, packageId);
    while (stmt->Step()) out.push_back(RowToConnection(stmt));
    return out;
}

bool ApiPackageStore::DeleteConnection(const std::string& id) {
    auto stmt = m_store->Prepare("DELETE FROM api_package_connections WHERE id = ?");
    if (!stmt) return false;
    stmt->BindText(1, id);
    stmt->ExecDML();
    stmt->Finalize();
    return m_store->Changes() > 0;
}

int ApiPackageStore::ReplaceConnections(const std::string& packageId,
                                        const std::vector<ApiPackageConnection>& conns) {
    m_store->BeginTransaction();
    try {
        if (!DeleteRows("api_package_connections", "package_id", packageId))
            throw std::runtime_error("delete connections failed: " + m_store->ErrMsg());
        int inserted = 0;
        for (const auto& c : conns) {
            ApiPackageConnection copy = c;
            copy.package_id = packageId;
            AddConnection(copy);
            inserted++;
        }
        if (!m_store->Commit()) {
            ALOG_ERROR("api", "[api-package-store] ReplaceCommands COMMIT FAILED: "
                      << m_store->ErrMsg() << " (commands for '" << packageId
                      << "' NOT replaced)");
            m_store->Rollback();
            throw std::runtime_error("commit failed: " + m_store->ErrMsg());
        }
        return inserted;
    } catch (...) {
        m_store->Rollback();
        throw;
    }
}

// ---------------------------------------------------------------------------
// Per-agent enablement overlay (D13)
// ---------------------------------------------------------------------------

void ApiPackageStore::SetAgentEnablement(const std::string& packageId,
                                         const std::string& agentId, bool enabled) {
    auto existing = m_store->Prepare(
        "SELECT id FROM api_package_agents WHERE package_id = ? AND agent_id = ?");
    std::string rowId;
    if (existing) {
        existing->BindText(1, packageId);
        existing->BindText(2, agentId);
        if (existing->Step()) rowId = existing->ColumnText(0);
        existing->Finalize();
    }

    if (!rowId.empty()) {
        auto upd = m_store->Prepare(
            "UPDATE api_package_agents SET enabled = ? WHERE id = ?");
        if (!upd) return;
        upd->BindInt64(1, enabled ? 1 : 0);
        upd->BindText(2, rowId);
        upd->ExecDML();
        upd->Finalize();
        return;
    }

    auto ins = m_store->Prepare(
        "INSERT INTO api_package_agents (id, package_id, agent_id, enabled, "
        "created_at_unix_ms) VALUES (?, ?, ?, ?, ?)");
    if (!ins) return;
    ins->BindText(1, GenerateId());
    ins->BindText(2, packageId);
    ins->BindText(3, agentId);
    ins->BindInt64(4, enabled ? 1 : 0);
    ins->BindInt64(5, NowUnixMs());
    ins->ExecDML();
    ins->Finalize();
}

std::optional<bool> ApiPackageStore::GetAgentEnablement(const std::string& packageId,
                                                        const std::string& agentId) const {
    auto stmt = m_store->Prepare(
        "SELECT enabled FROM api_package_agents WHERE package_id = ? AND agent_id = ?");
    if (!stmt) return std::nullopt;
    stmt->BindText(1, packageId);
    stmt->BindText(2, agentId);
    if (!stmt->Step()) return std::nullopt;
    return stmt->ColumnInt64(0) != 0;
}

bool ApiPackageStore::ClearAgentEnablement(const std::string& packageId,
                                           const std::string& agentId) {
    auto stmt = m_store->Prepare(
        "DELETE FROM api_package_agents WHERE package_id = ? AND agent_id = ?");
    if (!stmt) return false;
    stmt->BindText(1, packageId);
    stmt->BindText(2, agentId);
    stmt->ExecDML();
    stmt->Finalize();
    return m_store->Changes() > 0;
}

bool ApiPackageStore::EffectiveEnabled(const std::string& packageId,
                                       const std::string& agentId) const {
    auto pkg = GetPackage(packageId);
    if (!pkg || !pkg->enabled) return false;
    auto row = GetAgentEnablement(packageId, agentId);
    return !row.has_value() || row.value();
}

// ---------------------------------------------------------------------------
// Manifest install (manifest v1 — docs/api/SCHEMAS.md)
// ---------------------------------------------------------------------------

namespace {

struct LintError {
    std::vector<std::string> msgs;
    void Add(const std::string& m) { msgs.push_back(m); }
    bool Ok() const { return msgs.empty(); }
    [[noreturn]] void Throw(const std::string& context) const {
        std::string all = context + ":";
        for (size_t i = 0; i < msgs.size() && i < 5; ++i) all += "\n  - " + msgs[i];
        if (msgs.size() > 5) all += "\n  - (+" + std::to_string(msgs.size() - 5) + " more)";
        throw std::runtime_error(all);
    }
};

bool IsHookEvent(const std::string& e) {
    return e == "on_connect" || e == "on_disconnect" || e == "on_message" || e == "on_error";
}

// --- #23: credentials are forbidden as literals in package files -------------
// A literal token in an exchanged package is a leak by construction. Headers
// whose names mark them as credential carriers must reference state
// ({{state.…}}) so the value comes from the per-install vault at runtime.
bool IsCredentialHeaderName(const std::string& name) {
    std::string lower;
    for (char c : name)
        lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "authorization" || lower == "proxy-authorization" || lower == "cookie")
        return true;
    const std::array<std::string, 5> needles = {"api-key", "apikey", "token", "secret",
                                                "password"};
    for (const auto& n : needles)
        if (lower.find(n) != std::string::npos) return true;
    return false;
}

void LintCredentialHeaders(const Json::Value& headers, const std::string& where, LintError& lint) {
    if (!headers.isObject()) return;
    for (const std::string& h : headers.getMemberNames()) {
        if (!IsCredentialHeaderName(h)) continue;
        const Json::Value& v = headers[h];
        if (!v.isString()) continue;
        const std::string value = v.asString();
        // Extract every {{...}} token; a credential header must be templated
        // and every token must follow the runtime grammar (state.* / args.*
        // dotted paths) — unknown roots like {{env.X}} or malformed braces
        // fail here instead of exploding (or silently resolving) at runtime.
        std::vector<std::string> tokens;
        size_t pos = 0;
        bool malformed = false;
        while (true) {
            const size_t s = value.find("{{", pos);
            if (s == std::string::npos) break;
            const size_t e = value.find("}}", s + 2);
            if (e == std::string::npos) { malformed = true; break; }
            tokens.push_back(value.substr(s + 2, e - s - 2));
            pos = e + 2;
        }
        bool bad = malformed || tokens.empty();
        for (const auto& tok : tokens) {
            const auto dot = tok.find('.');
            if (dot == std::string::npos ||
                (tok.substr(0, dot) != "state" && tok.substr(0, dot) != "args")) {
                bad = true;
                break;
            }
        }
        if (bad) {
            lint.Add(where + ": header '" + h + "' must template its value from "
                     "state.* / args.* ({{state.…}}) — literal or malformed "
                     "credentials are forbidden in package files (#23)");
        }
    }
}

}  // namespace

void ApiPackageStore::ValidateName(const std::string& name) {
    if (!IsValidSlug(name))
        throw std::runtime_error("package name must be a lowercase slug [a-z0-9-], 1-63 chars (got '" +
                                 name + "')");
    if (IsReservedFirstWord(name))
        throw std::runtime_error("package name '" + name +
                                 "' is a reserved management word (package, command, connection, "
                                 "files, download, upload, enable, disable, status)");
}


ApiPackage ApiPackageStore::InstallFromManifest(const std::string& manifestJson,
                                               const std::string& registrySource,
                                               const std::string& registryVersion) {
    Json::Value m;
    std::string err;
    if (!ParseJson(manifestJson, m, err) || !m.isObject())
        throw std::runtime_error("manifest is not a JSON object" +
                                 (err.empty() ? "" : " (" + err + ")"));

    LintError lint;

    if (m.get("kind", Json::Value("")).asString() != "api_package")
        lint.Add("kind must be \"api_package\"");

    const std::string name = m.get("name", Json::Value("")).asString();
    if (!IsValidSlug(name))
        lint.Add("name must be a lowercase slug [a-z0-9-], 1-63 chars (got '" + name + "')");
    else if (IsReservedFirstWord(name))
        lint.Add("name '" + name + "' is a reserved management word");

    // (ValidateName path for direct API/tool creates)

    const std::string version = m.get("version", Json::Value("")).asString();
    if (version.empty()) lint.Add("version is required");

    const std::string description = m.get("description", Json::Value("")).asString();
    if (description.empty()) lint.Add("description is required");

    Json::Value keywords = m.get("keywords", Json::Value(Json::arrayValue));
    if (!keywords.isArray()) {
        lint.Add("keywords must be an array of strings");
        keywords = Json::Value(Json::arrayValue);
    } else {
        for (const auto& k : keywords)
            if (!k.isString()) { lint.Add("keywords must be strings"); break; }
    }

    const int64_t cooldown = m.get("dispatch_cooldown_ms", Json::Value(10000)).asInt64();
    if (cooldown <= 0) lint.Add("dispatch_cooldown_ms must be > 0");
    const int64_t quota = m.get("files_quota_mb", Json::Value(256)).asInt64();
    if (quota <= 0) lint.Add("files_quota_mb must be > 0");

    // state_schema: object; values are {type: string, default?: any, secret?: bool}
    Json::Value stateSchema = m.get("state_schema", Json::Value(Json::objectValue));
    if (!stateSchema.isObject()) {
        lint.Add("state_schema must be an object");
        stateSchema = Json::Value(Json::objectValue);
    } else {
        for (const std::string& key : stateSchema.getMemberNames()) {
            const Json::Value& def = stateSchema[key];
            if (!def.isObject() || !def.get("type", Json::Value::nullSingleton()).isString()) {
                lint.Add("state_schema." + key + " must be an object with a string 'type'");
            } else if (def.isMember("secret") && !def["secret"].isBool()) {
                lint.Add("state_schema." + key + ".secret must be a boolean");
            } else if (def.get("secret", Json::Value(false)).asBool() &&
                       def.get("type", Json::Value("")).asString() != "string") {
                // Secrets are credential strings: the vault stores strings, the
                // masked-state copy and redaction assume strings. Non-string
                // secret types are rejected at the door.
                lint.Add("state_schema." + key + ": secret keys must be of type string");
            } else if (def.get("secret", Json::Value(false)).asBool() && def.isMember("default")) {
                // #23: a default on a secret key is a literal credential in an
                // exchanged file — exactly the leak this ticket forbids.
                lint.Add("state_schema." + key + ": secret keys must not declare a default "
                         "(secrets live in the per-install vault, never in package files)");
            }
        }
    }

    // commands
    std::vector<ApiPackageCommand> cmds;
    Json::Value jcmds = m.get("commands", Json::Value(Json::arrayValue));
    if (!jcmds.isArray()) {
        lint.Add("commands must be an array");
    } else {
        std::vector<std::string> names;
        for (const auto& c : jcmds) {
            ApiPackageCommand cmd;
            if (!c.isObject()) { lint.Add("each command must be an object"); continue; }
            cmd.name = c.get("name", Json::Value("")).asString();
            if (!IsValidCommandName(cmd.name)) {
                lint.Add("command name '" + cmd.name + "' must be lowercase [a-z0-9 -], no leading/trailing/double spaces");
                continue;
            }
            for (const auto& n : names)
                if (n == cmd.name) lint.Add("duplicate command name '" + cmd.name + "'");
            names.push_back(cmd.name);

            cmd.kind = c.get("kind", Json::Value("")).asString();
            cmd.event = c.get("event", Json::Value("")).asString();
            cmd.description = c.get("description", Json::Value("")).asString();
            cmd.script = c.get("script", Json::Value("")).asString();

            // description optional-if-present per manifest v1 (registry parity);
            // if present it was already read as string — nothing else to check here
            (void)cmd.description;
            if (cmd.script.empty())
                lint.Add("command '" + cmd.name + "': script is required (defines run(ctx))");

            if (cmd.kind == "hook") {
                if (!IsHookEvent(cmd.event))
                    lint.Add("hook '" + cmd.name + "': event must be one of on_connect, on_disconnect, on_message, on_error");
                if (c.isMember("parameters"))
                    lint.Add("hook '" + cmd.name + "': hooks must not declare parameters");
                if (c.isMember("request"))
                    lint.Add("hook '" + cmd.name + "': hooks must not declare request");
                cmd.parameters = "{}";
                cmd.request = "";
            } else if (cmd.kind == "action") {
                Json::Value params = c.get("parameters", Json::Value(Json::objectValue));
                if (!params.isObject()) {
                    lint.Add("action '" + cmd.name + "': parameters must be an object");
                } else {
                    bool pOk = true;
                    for (const std::string& pn : params.getMemberNames()) {
                        const Json::Value& pd = params[pn];
                        if (!pd.isObject() || !pd.get("type", Json::Value::nullSingleton()).isString()) {
                            lint.Add("action '" + cmd.name + "': parameter '" + pn + "' must be an object with a string 'type'");
                            pOk = false;
                        }
                    }
                    if (pOk) cmd.parameters = JsonCompact(params);
                }
                if (c.isMember("request") && !c["request"].isNull()) {
                    if (!c["request"].isObject())
                        lint.Add("action '" + cmd.name + "': request must be an object");
                    else {
                        cmd.request = JsonCompact(c["request"]);
                        LintCredentialHeaders(c["request"].get("headers", Json::Value(Json::objectValue)),
                                              "action '" + cmd.name + "'", lint);
                    }
                }
                cmd.event = "";
            } else {
                lint.Add("command '" + cmd.name + "': kind must be 'action' or 'hook'");
            }
            cmds.push_back(std::move(cmd));
        }
    }

    // connections
    std::vector<ApiPackageConnection> conns;
    Json::Value jconns = m.get("connections", Json::Value(Json::arrayValue));
    if (!jconns.isArray()) {
        lint.Add("connections must be an array");
    } else {
        std::vector<std::string> names;
        for (const auto& c : jconns) {
            ApiPackageConnection conn;
            if (!c.isObject()) { lint.Add("each connection must be an object"); continue; }
            conn.name = c.get("name", Json::Value("")).asString();
            if (!IsValidSlug(conn.name)) {
                lint.Add("connection name '" + conn.name + "' must be a lowercase slug [a-z0-9-]");
                continue;
            }
            for (const auto& n : names)
                if (n == conn.name) lint.Add("duplicate connection name '" + conn.name + "'");
            names.push_back(conn.name);

            conn.type = c.get("type", Json::Value("")).asString();
            conn.enabled = c.get("enabled", Json::Value(true)).asBool();
            conn.url_template = c.get("url_template", Json::Value("")).asString();
            if (conn.url_template.empty())
                lint.Add("connection '" + conn.name + "': url_template is required");

            Json::Value headers = c.get("headers_template", Json::Value(Json::objectValue));
            if (!headers.isObject()) {
                lint.Add("connection '" + conn.name + "': headers_template must be an object");
            } else {
                conn.headers_template = JsonCompact(headers);
                LintCredentialHeaders(headers, "connection '" + conn.name + "'", lint);
            }

            Json::Value hooks = c.get("hooks", Json::Value(Json::objectValue));
            if (!hooks.isObject()) {
                lint.Add("connection '" + conn.name + "': hooks must be an object");
            } else {
                bool hOk = true;
                for (const std::string& ev : hooks.getMemberNames()) {
                    if (!IsHookEvent(ev)) {
                        lint.Add("connection '" + conn.name + "': hook key '" + ev + "' is not a valid event");
                        hOk = false;
                    } else if (!hooks[ev].isString()) {
                        lint.Add("connection '" + conn.name + "': hook '" + ev + "' must be a command name string");
                        hOk = false;
                    }
                }
                if (hOk) conn.hooks = JsonCompact(hooks);
            }

            if (conn.type == "websocket") {
                // url_template is the whole config
            } else if (conn.type == "longpoll") {
                Json::Value poll = c.get("poll", Json::Value(Json::Value::nullSingleton()));
                if (!poll.isObject()) {
                    lint.Add("longpoll connection '" + conn.name + "': poll object is required");
                } else {
                    const std::string cursor = poll.get("cursor_path", Json::Value("")).asString();
                    if (cursor.empty())
                        lint.Add("longpoll connection '" + conn.name + "': poll.cursor_path is required");
                    if (!poll.get("interval_s", Json::Value(0)).isNumeric() ||
                        poll.get("interval_s", Json::Value(0)).asDouble() <= 0)
                        lint.Add("longpoll connection '" + conn.name + "': poll.interval_s must be > 0");
                    if (!poll.get("params_template", Json::Value(Json::objectValue)).isObject())
                        lint.Add("longpoll connection '" + conn.name + "': poll.params_template must be an object");
                    conn.poll = JsonCompact(poll);
                }
            } else {
                lint.Add("connection '" + conn.name + "': type must be 'websocket' or 'longpoll'");
            }
            conns.push_back(std::move(conn));
        }
    }

    // #25 egress scope: declared egress_hosts win; otherwise derive from the
    // static parts of all url templates ({{state.*}} resolved from schema
    // string defaults). Deny-by-default at runtime; derivation keeps the
    // default safe without breaking template-based packages.
    Json::Value egressArr(Json::arrayValue);
    bool egressDeclared = m.isMember("egress_hosts");
    if (egressDeclared) {
        const Json::Value& eh = m["egress_hosts"];
        if (!eh.isArray()) {
            lint.Add("egress_hosts must be an array of host patterns");
        } else if (eh.size() > 32) {
            lint.Add("egress_hosts: at most 32 patterns");
        } else {
            for (const auto& h : eh) {
                if (!h.isString() || !IsValidHostPattern(h.asString())) {
                    lint.Add("egress_hosts: invalid host pattern '" +
                             (h.isString() ? h.asString() : std::string("<non-string>")) +
                             "' (lowercase host, optional leading '*.', no scheme/port/path)");
                    break;
                }
            }
            if (lint.Ok()) egressArr = eh;
        }
    }
    std::vector<std::string> derived;
    if (!egressDeclared) {
        auto pushUnique = [&](const std::string& url) {
            const std::string host = ExtractHostFromTemplate(url, stateSchema);
            if (host.empty()) return;
            for (const auto& d : derived)
                if (d == host) return;
            if (derived.size() < 32) derived.push_back(host);
        };
        if (jcmds.isArray()) {
            for (const auto& c : jcmds) {
                const Json::Value& rq = c.get("request", Json::Value(Json::Value::nullSingleton()));
                if (rq.isObject() && rq.isMember("url") && rq["url"].isString())
                    pushUnique(rq["url"].asString());
            }
        }
        if (jconns.isArray()) {
            for (const auto& c : jconns)
                if (c.isObject() && c.isMember("url_template") && c["url_template"].isString())
                    pushUnique(c["url_template"].asString());
        }
    }

    if (!lint.Ok()) lint.Throw("manifest lint failed");

    // Upgrade-in-place: same name -> replace commands/connections/meta while
    // PRESERVING operator-owned state and the enabled flag. ApiRuntime re-reads
    // package/command rows on every invocation, so new scripts apply on the
    // next call; connection template changes apply after a daemon restart.
    // Local modifications are never clobbered silently.
    std::optional<ApiPackage> existing = GetPackageByName(name);
    if (existing && existing->locally_modified)
        throw std::runtime_error(
            "api package '" + name + "' has local modifications - refusing registry "
            "upgrade (delete and reinstall to discard them)");

    ApiPackage pkg;
    pkg.name = name;
    pkg.display_name = m.get("display_name", Json::Value("")).asString();
    pkg.description = description;
    pkg.keywords = JsonCompact(keywords);
    pkg.version = version;
    pkg.registry_source = registrySource;
    pkg.registry_version = registryVersion;
    pkg.locally_modified = false;
    pkg.enabled = false;  // install != enable
    pkg.dispatch_cooldown_ms = cooldown;
    pkg.files_quota_mb = quota;
    pkg.state_schema = JsonCompact(stateSchema);
    pkg.egress_hosts = egressDeclared ? JsonCompact(egressArr) : JsonHostArray(derived);
    if (existing) {
        pkg.id = existing->id;
        pkg.created_at_unix_ms = existing->created_at_unix_ms;
        pkg.enabled = existing->enabled;  // operator choice survives upgrades
        // State is operator-owned: keep all values; overlay defaults for keys
        // the new version introduces (same overlay Execute applies at read).
        Json::Value liveState;
        std::string stateErr;
        ParseJson(existing->state.empty() ? "{}" : existing->state, liveState, stateErr);
        if (!liveState.isObject()) liveState = Json::Value(Json::objectValue);
        for (const std::string& k : stateSchema.getMemberNames()) {
            if (stateSchema[k].isMember("default") && !liveState.isMember(k))
                liveState[k] = stateSchema[k]["default"];
        }
        pkg.state = JsonCompact(liveState);
    } else {
        pkg.state = "{}";
    }

    m_store->BeginTransaction();
    ApiPackage stored;
    try {
        if (existing) {
            if (!UpdatePackageMeta(pkg))
                throw std::runtime_error("upgrade meta update failed: " + m_store->ErrMsg());
            stored = pkg;
        } else {
            stored = CreatePackage(pkg);
        }
        // Inline replaces (no nested transactions — Begin inside Begin is a
        // no-op returning false and the inner Commit would end the outer tx).
        if (!DeleteRows("api_package_commands", "package_id", stored.id))
            throw std::runtime_error("delete commands failed: " + m_store->ErrMsg());
        for (const auto& c : cmds) {
            ApiPackageCommand copy = c;
            copy.package_id = stored.id;
            AddCommand(copy);
        }
        if (!DeleteRows("api_package_connections", "package_id", stored.id))
            throw std::runtime_error("delete connections failed: " + m_store->ErrMsg());
        for (const auto& c : conns) {
            ApiPackageConnection copy = c;
            copy.package_id = stored.id;
            AddConnection(copy);
        }
    } catch (...) {
        m_store->Rollback();
        throw;
    }
    if (!m_store->Commit()) {
        ALOG_ERROR("api", "[api-package-store] InstallFromManifest COMMIT FAILED: "
                  << m_store->ErrMsg() << " (package '" << name
                  << "' NOT installed)");
        m_store->Rollback();
        throw std::runtime_error("commit failed: " + m_store->ErrMsg());
    }
    return stored;
}

}  // namespace animus::kernel
