// ============================================================================
// API Packages — admin-facing CRUD + debug execute
//
// GET    /api/v1/api/packages              — list (no manifest bodies)
// GET    /api/v1/api/packages/{id}         — detail (commands, connections, state masked)
// POST   /api/v1/api/packages              — install from manifest v1 JSON (installs disabled)
// PUT    /api/v1/api/packages/{id}/enabled — {enabled: bool}
// PUT    /api/v1/api/packages/{id}/state   — {state: {...}} full state write (secrets write-only)
// DELETE /api/v1/api/packages/{id}         — uninstall (cascades)
// POST   /api/v1/api/packages/{id}/execute — {command, args, agent_id} — debug surface
// POST   /api/v1/api/packages/install-from-registry — {registry, name, version?}
// GET    /api/v1/api/registry/packages?registry=<base> — browse available (#63)
// GET    /api/v1/api/registry/packages/{name}/versions?registry=<base> — version picker (#63)
// GET    /api/v1/api/packages/{id}/secrets — declared + stored secret names (#23)
// PUT    /api/v1/api/packages/{id}/secrets/{name} — set {"value"} (write-only ack)
// DELETE /api/v1/api/packages/{id}/secrets/{name} — remove vault entry (#23)
// ============================================================================

        // Helper lambda: package summary row
        auto pkgSummary = [](const ApiPackage& p) {
            Json::Value v(Json::objectValue);
            v["id"] = p.id;
            v["name"] = p.name;
            v["display_name"] = p.display_name;
            v["description"] = p.description;
            v["version"] = p.version;
            v["registry_source"] = p.registry_source;
            v["registry_version"] = p.registry_version;
            v["locally_modified"] = p.locally_modified;
            v["enabled"] = p.enabled;
            v["keywords"] = p.keywords;
            v["created_at"] = static_cast<Json::Int64>(p.created_at_unix_ms);
            v["updated_at"] = static_cast<Json::Int64>(p.updated_at_unix_ms);
            return v;
        };

        // Helper lambda: masked state for display (secret values never leave the box)
        auto maskedState = [](const ApiPackage& p) {
            Json::Value schema, state;
            Json::CharReaderBuilder rb;
            std::string errs;
            std::stringstream ss1(p.state_schema.empty() ? "{}" : p.state_schema);
            Json::parseFromStream(rb, ss1, &schema, &errs);
            std::stringstream ss2(p.state.empty() ? "{}" : p.state);
            Json::parseFromStream(rb, ss2, &state, &errs);
            if (state.isObject()) {
                for (const std::string& k : state.getMemberNames()) {
                    const Json::Value* def = schema.isObject() ? schema.find(k.data(), k.data() + k.size()) : nullptr;
                    bool secret = def && def->isObject() && (*def).get("secret", false).asBool();
                    bool hasValue = state[k] != Json::Value::null && (state[k].isString() ? !state[k].asString().empty() : true);
                    if (secret && hasValue) state[k] = "***";
                }
            }
            return state;
        };

        // GET /api/v1/api/packages
        drogon::app().registerHandler(
            "/api/v1/api/packages",
            [pkgSummary](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                (void)req;
                AdminServer* server = g_activeAdminServer.load();
                if (!server) { SendServerNotActive(callback); return; }
                if (!server->m_apiPackageStore) {
                    SendJsonError(callback, "api package store not available",
                        drogon::k503ServiceUnavailable);
                    return;
                }
                auto pkgs = server->m_apiPackageStore->ListPackages();
                Json::Value items(Json::arrayValue);
                for (const auto& p : pkgs) items.append(pkgSummary(p));
                Json::Value body(Json::objectValue);
                body["packages"] = items;
                body["count"] = static_cast<Json::Int>(pkgs.size());
                SendJsonResponse(callback, body);
            },
            {drogon::Get});

        // GET /api/v1/api/packages/{id}
        drogon::app().registerHandler(
            "/api/v1/api/packages/{id}",
            [pkgSummary, maskedState](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback,
               const std::string& id) {
                AdminServer* server = g_activeAdminServer.load();
                if (!server) { SendServerNotActive(callback); return; }
                if (!server->m_apiPackageStore) {
                    SendJsonError(callback, "api package store not available",
                        drogon::k503ServiceUnavailable);
                    return;
                }
                auto pkg = server->m_apiPackageStore->GetPackage(id);
                if (!pkg) { SendJsonError(callback, "package not found", drogon::k404NotFound); return; }

                Json::Value body = pkgSummary(*pkg);
                body["state_schema"] = pkg->state_schema.empty() ? "{}" : pkg->state_schema;
                body["state"] = maskedState(*pkg);

                // #23: declared secrets + vault status (never values).
                {
                    Json::Value schema, stateObj;
                    Json::CharReaderBuilder rb2;
                    std::string e2;
                    std::stringstream s1(pkg->state_schema.empty() ? "{}" : pkg->state_schema);
                    Json::parseFromStream(rb2, s1, &schema, &e2);
                    std::stringstream s2(pkg->state.empty() ? "{}" : pkg->state);
                    Json::parseFromStream(rb2, s2, &stateObj, &e2);
                    Json::Value secrets(Json::objectValue);
                    if (schema.isObject()) {
                        for (const std::string& k : schema.getMemberNames()) {
                            if (!schema[k].isObject() ||
                                !schema[k].get("secret", false).asBool()) continue;
                            Json::Value sinfo(Json::objectValue);
                            std::string refName;
                            if (stateObj.isMember(k) &&
                                SecretsVault::IsSecretRef(stateObj[k], refName)) {
                                sinfo["ref"] = refName;
                                sinfo["set"] = server->m_apiRuntime && server->m_apiRuntime->vault() &&
                                                server->m_apiRuntime->vault()->Has(id, refName);
                            } else {
                                sinfo["set"] = server->m_apiRuntime && server->m_apiRuntime->vault() &&
                                                server->m_apiRuntime->vault()->Has(id, k);
                            }
                            secrets[k] = sinfo;
                        }
                    }
                    body["secrets"] = secrets;
                }

                Json::Value cmds(Json::arrayValue);
                for (const auto& c : server->m_apiPackageStore->ListCommands(id)) {
                    Json::Value cv(Json::objectValue);
                    cv["id"] = c.id;
                    cv["name"] = c.name;
                    cv["kind"] = c.kind;
                    cv["event"] = c.event;
                    cv["description"] = c.description;
                    cv["parameters"] = c.parameters;
                    cv["request"] = c.request;
                    cv["script"] = c.script;
                    cmds.append(cv);
                }
                body["commands"] = cmds;

                Json::Value conns(Json::arrayValue);
                for (const auto& c : server->m_apiPackageStore->ListConnections(id)) {
                    Json::Value cc(Json::objectValue);
                    cc["id"] = c.id;
                    cc["name"] = c.name;
                    cc["type"] = c.type;
                    cc["enabled"] = c.enabled;
                    cc["url_template"] = c.url_template;
                    cc["headers_template"] = c.headers_template;
                    cc["poll"] = c.poll;
                    cc["hooks"] = c.hooks;
                    conns.append(cc);
                }
                body["connections"] = conns;
                SendJsonResponse(callback, body);
            },
            {drogon::Get});

        // POST /api/v1/api/packages — install from manifest
        drogon::app().registerHandler(
            "/api/v1/api/packages",
            [pkgSummary](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                AdminServer* server = g_activeAdminServer.load();
                if (!server) { SendServerNotActive(callback); return; }
                if (!server->m_apiPackageStore) {
                    SendJsonError(callback, "api package store not available",
                        drogon::k503ServiceUnavailable);
                    return;
                }
                Json::Value body;
                std::string parseError;
                if (!ParseJsonPayload(std::string(req->body()), &body, &parseError)) {
                    SendJsonError(callback, "invalid JSON body: " + parseError,
                        drogon::k400BadRequest);
                    return;
                }
                Json::StreamWriterBuilder wb;
                std::string manifest = Json::writeString(wb, body);
                try {
                    ApiPackage pkg = server->m_apiPackageStore->InstallFromManifest(manifest);
                    Json::Value out = pkgSummary(pkg);
                    out["installed_disabled"] = true;
                    SendJsonResponse(callback, out);
                } catch (const std::exception& e) {
                    SendJsonError(callback, std::string("install failed: ") + e.what(),
                        drogon::k400BadRequest);
                }
            },
            {drogon::Post});

        // PUT /api/v1/api/packages/{id}/enabled
        drogon::app().registerHandler(
            "/api/v1/api/packages/{id}/enabled",
            [](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback,
               const std::string& id) {
                AdminServer* server = g_activeAdminServer.load();
                if (!server) { SendServerNotActive(callback); return; }
                if (!server->m_apiPackageStore) {
                    SendJsonError(callback, "api package store not available",
                        drogon::k503ServiceUnavailable);
                    return;
                }
                Json::Value body;
                std::string parseError;
                if (!ParseJsonPayload(std::string(req->body()), &body, &parseError) ||
                    !body.isObject() || !body.isMember("enabled")) {
                    SendJsonError(callback, "body must be {\"enabled\": bool}",
                        drogon::k400BadRequest);
                    return;
                }
                if (!server->m_apiPackageStore->SetPackageEnabled(id, body["enabled"].asBool())) {
                    SendJsonError(callback, "package not found", drogon::k404NotFound);
                    return;
                }
                Json::Value out(Json::objectValue);
                out["ok"] = true;
                SendJsonResponse(callback, out);
            },
            {drogon::Put});

        // PUT /api/v1/api/packages/{id}/state
        drogon::app().registerHandler(
            "/api/v1/api/packages/{id}/state",
            [](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback,
               const std::string& id) {
                AdminServer* server = g_activeAdminServer.load();
                if (!server) { SendServerNotActive(callback); return; }
                if (!server->m_apiPackageStore) {
                    SendJsonError(callback, "api package store not available",
                        drogon::k503ServiceUnavailable);
                    return;
                }
                Json::Value body;
                std::string parseError;
                if (!ParseJsonPayload(std::string(req->body()), &body, &parseError) ||
                    !body.isObject() || !body["state"].isObject()) {
                    SendJsonError(callback, "body must be {\"state\": {...}}",
                        drogon::k400BadRequest);
                    return;
                }
                // write-only secrets: merge semantics — existing state overlaid by payload
                auto pkg = server->m_apiPackageStore->GetPackage(id);
                if (!pkg) { SendJsonError(callback, "package not found", drogon::k404NotFound); return; }

                Json::Value current;
                Json::CharReaderBuilder rb;
                std::string errs;
                std::stringstream ss(pkg->state.empty() ? "{}" : pkg->state);
                Json::parseFromStream(rb, ss, &current, &errs);

                Json::Value next = current;
                for (const std::string& k : body["state"].getMemberNames()) {
                    const Json::Value& v = body["state"][k];
                    if (v.isString() && v.asString() == "***") continue;  // masked echo = keep existing
                    next[k] = v;
                }
                // #23: literal values on secret-typed keys route into the encrypted
                // vault and never persist into state JSON.
                int vaulted = 0;
                if (server->m_apiRuntime && server->m_apiRuntime->vault()) {
                    Json::Value schema;
                    std::stringstream sss(pkg->state_schema.empty() ? "{}" : pkg->state_schema);
                    Json::parseFromStream(rb, sss, &schema, &errs);
                    std::string splitErr;
                    vaulted = server->m_apiRuntime->vault()->SplitStateSecrets(id, schema, next, splitErr);
                    if (vaulted < 0) {
                        SendJsonError(callback, "secret vault write failed: " + splitErr,
                            drogon::k400BadRequest);
                        return;
                    }
                }
                Json::StreamWriterBuilder wb;
                if (!server->m_apiPackageStore->SetPackageState(id, Json::writeString(wb, next))) {
                    SendJsonError(callback, "state write rejected", drogon::k400BadRequest);
                    return;
                }
                Json::Value out(Json::objectValue);
                out["ok"] = true;
                out["secrets_vaulted"] = static_cast<Json::Int>(vaulted);
                SendJsonResponse(callback, out);
            },
            {drogon::Put});

        // DELETE /api/v1/api/packages/{id}
        drogon::app().registerHandler(
            "/api/v1/api/packages/{id}",
            [](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback,
               const std::string& id) {
                (void)req;
                AdminServer* server = g_activeAdminServer.load();
                if (!server) { SendServerNotActive(callback); return; }
                if (!server->m_apiPackageStore) {
                    SendJsonError(callback, "api package store not available",
                        drogon::k503ServiceUnavailable);
                    return;
                }
                if (!server->m_apiPackageStore->DeletePackage(id)) {
                    SendJsonError(callback, "package not found", drogon::k404NotFound);
                    return;
                }
                // #23: secrets cascade with the package — nothing orphaned in the vault.
                if (server->m_apiRuntime && server->m_apiRuntime->vault())
                    server->m_apiRuntime->vault()->DeleteForPackage(id);
                Json::Value out(Json::objectValue);
                out["ok"] = true;
                SendJsonResponse(callback, out);
            },
            {drogon::Delete});

        // POST /api/v1/api/packages/{id}/execute — debug surface
        drogon::app().registerHandler(
            "/api/v1/api/packages/{id}/execute",
            [](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback,
               const std::string& id) {
                AdminServer* server = g_activeAdminServer.load();
                if (!server) { SendServerNotActive(callback); return; }
                if (!server->m_apiPackageStore || !server->m_apiRuntime) {
                    SendJsonError(callback, "api subsystem not available",
                        drogon::k503ServiceUnavailable);
                    return;
                }
                auto pkg = server->m_apiPackageStore->GetPackage(id);
                if (!pkg) { SendJsonError(callback, "package not found", drogon::k404NotFound); return; }

                Json::Value body;
                std::string parseError;
                if (!ParseJsonPayload(std::string(req->body()), &body, &parseError) ||
                    !body.isObject() || !body.isMember("command")) {
                    SendJsonError(callback, "body must be {\"command\": str, \"args\": {}, \"agent_id\": str}",
                        drogon::k400BadRequest);
                    return;
                }
                std::string agentId = body.get("agent_id", "default").asString();
                Json::Value args = body.get("args", Json::Value(Json::objectValue));
                Json::Value result = server->m_apiRuntime->ExecuteAction(
                    pkg->name, body["command"].asString(), agentId, args);
                SendJsonResponse(callback, result);
            },
            {drogon::Post});

        // POST /api/v1/api/packages/install-from-registry
        // body: {registry: str, name: str, version?: str}
        // Blocking (HTTP timeout ~10s). Verifies content hash before install.
        drogon::app().registerHandler(
            "/api/v1/api/packages/install-from-registry",
            [pkgSummary](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                AdminServer* server = g_activeAdminServer.load();
                if (!server) { SendServerNotActive(callback); return; }
                if (!server->m_apiPackageStore) {
                    SendJsonError(callback, "api package store not available",
                        drogon::k503ServiceUnavailable);
                    return;
                }
                Json::Value body;
                std::string parseError;
                if (!ParseJsonPayload(std::string(req->body()), &body, &parseError) ||
                    !body.isObject() || !body.isMember("registry") || !body.isMember("name")) {
                    SendJsonError(callback, "body must be {\"registry\": str, \"name\": str, \"version\"?: str}",
                        drogon::k400BadRequest);
                    return;
                }
                HttpClient http;  // fresh instance: public URLs only (SSRF defaults)
                auto result = registry::InstallFromRegistry(
                    *server->m_apiPackageStore, http,
                    body["registry"].asString(), body["name"].asString(),
                    body.get("version", "").asString(),
                    body.get("allow_private", false).asBool());
                if (!result.ok) {
                    SendJsonError(callback, result.err, drogon::k502BadGateway);
                    return;
                }
                Json::Value out = pkgSummary(result.pkg);
                out["content_hash"] = result.content_hash;
                out["semantic_version"] = result.semantic_version;
                out["hash_verified"] = true;
                SendJsonResponse(callback, out);
            },
            {drogon::Post});

        // ------------------------------------------------------------------
        // Registry browse (issue #63) — list available packages + versions
        //
        // GET /api/v1/api/registry/packages?registry=<base>&allow_private=<0|1>
        //   -> {registry, source, count, packages: [summary rows]}
        // GET /api/v1/api/registry/packages/{name}/versions?registry=<base>&allow_private=<0|1>
        //   -> {registry, source, name, count, versions: [version rows]}
        //
        // Read-only probes of the configured registry. SSRF posture matches
        // install-from-registry: public URLs only unless allow_private is
        // explicitly set (operator-configured target, admin-authenticated).
        // ------------------------------------------------------------------

        // Helper lambda: registry rows as JSON
        auto registrySummaryJson = [](const registry::RegistryPackageSummary& s) {
            Json::Value v(Json::objectValue);
            v["name"] = s.name;
            v["version"] = s.version;
            v["semantic_version"] = s.semantic_version;
            v["description"] = s.description;
            v["content_hash"] = s.content_hash;
            v["official"] = s.official;
            v["published_at"] = s.published_at;
            return v;
        };
        auto registryVersionJson = [](const registry::RegistryPackageVersion& ver) {
            Json::Value v(Json::objectValue);
            v["version"] = ver.version;
            v["semantic_version"] = ver.semantic_version;
            v["content_hash"] = ver.content_hash;
            v["published_at"] = ver.published_at;
            return v;
        };
        // Helper lambda: host label for the source chip (federation-ready)
        auto registrySourceLabel = [](const std::string& base) {
            std::string s = base;
            const auto scheme = s.find("://");
            if (scheme != std::string::npos) s = s.substr(scheme + 3);
            const auto slash = s.find('/');
            if (slash != std::string::npos) s = s.substr(0, slash);
            return s;
        };

        // GET /api/v1/api/registry/packages
        drogon::app().registerHandler(
            "/api/v1/api/registry/packages",
            [registrySummaryJson, registrySourceLabel](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                AdminServer* server = g_activeAdminServer.load();
                if (!server) { SendServerNotActive(callback); return; }
                const std::string registryBase = req->getParameter("registry");
                if (registryBase.empty()) {
                    SendJsonError(callback, "missing required query param: registry",
                        drogon::k400BadRequest);
                    return;
                }
                const bool allowPrivate = req->getParameter("allow_private") == "1";
                HttpClient http;  // fresh instance: public URLs only (SSRF defaults)
                auto result = registry::ListRegistryPackages(http, registryBase, allowPrivate);
                if (!result.ok) {
                    SendJsonError(callback, result.err, drogon::k502BadGateway);
                    return;
                }
                Json::Value items(Json::arrayValue);
                for (const auto& s : result.packages) items.append(registrySummaryJson(s));
                Json::Value body(Json::objectValue);
                body["registry"] = result.registry;
                body["source"] = registrySourceLabel(result.registry);
                body["count"] = static_cast<Json::Int>(result.packages.size());
                body["packages"] = items;
                SendJsonResponse(callback, body);
            },
            {drogon::Get});

        // GET /api/v1/api/registry/packages/{name}/versions
        drogon::app().registerHandler(
            "/api/v1/api/registry/packages/{name}/versions",
            [registryVersionJson, registrySourceLabel](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback,
               const std::string& name) {
                AdminServer* server = g_activeAdminServer.load();
                if (!server) { SendServerNotActive(callback); return; }
                const std::string registryBase = req->getParameter("registry");
                if (registryBase.empty()) {
                    SendJsonError(callback, "missing required query param: registry",
                        drogon::k400BadRequest);
                    return;
                }
                const bool allowPrivate = req->getParameter("allow_private") == "1";
                HttpClient http;  // fresh instance: public URLs only (SSRF defaults)
                auto result = registry::ListRegistryPackageVersions(http, registryBase, name, allowPrivate);
                if (!result.ok) {
                    SendJsonError(callback, result.err, drogon::k502BadGateway);
                    return;
                }
                Json::Value items(Json::arrayValue);
                for (const auto& v : result.versions) items.append(registryVersionJson(v));
                Json::Value body(Json::objectValue);
                body["registry"] = result.registry;
                body["source"] = registrySourceLabel(result.registry);
                body["name"] = result.name;
                body["count"] = static_cast<Json::Int>(result.versions.size());
                body["versions"] = items;
                SendJsonResponse(callback, body);
            },
            {drogon::Get});

        // ------------------------------------------------------------------
        // Secrets vault (#23) — per-package secret management.
        //
        // GET    /api/v1/api/packages/{id}/secrets          -> names + status
        // PUT    /api/v1/api/packages/{id}/secrets/{name}   -> set {"value": str}
        // DELETE /api/v1/api/packages/{id}/secrets/{name}   -> remove
        //
        // Values are write-only: acknowledged by name, never echoed. The
        // vault stores them AES-256-GCM sealed under a key file outside the DB.
        // ------------------------------------------------------------------

        auto requireVault = [](AdminServer* server,
                               const std::function<void(const drogon::HttpResponsePtr&)>& callback)
            -> SecretsVault* {
            if (!server->m_apiRuntime || !server->m_apiRuntime->vault()) {
                SendJsonError(callback, "secrets vault not available",
                    drogon::k503ServiceUnavailable);
                return nullptr;
            }
            return server->m_apiRuntime->vault();
        };

        // GET /api/v1/api/packages/{id}/secrets
        drogon::app().registerHandler(
            "/api/v1/api/packages/{id}/secrets",
            [requireVault](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback,
               const std::string& id) {
                (void)req;
                AdminServer* server = g_activeAdminServer.load();
                if (!server) { SendServerNotActive(callback); return; }
                if (!server->m_apiPackageStore) {
                    SendJsonError(callback, "api package store not available",
                        drogon::k503ServiceUnavailable);
                    return;
                }
                auto pkg = server->m_apiPackageStore->GetPackage(id);
                if (!pkg) { SendJsonError(callback, "package not found", drogon::k404NotFound); return; }
                SecretsVault* vault = requireVault(server, callback);
                if (!vault) return;

                // Declared (schema) + stored (vault rows) — names only, never values.
                Json::Value schema;
                Json::CharReaderBuilder rb;
                std::string errs;
                std::stringstream ss(pkg->state_schema.empty() ? "{}" : pkg->state_schema);
                Json::parseFromStream(rb, ss, &schema, &errs);
                std::set<std::string> declared;
                if (schema.isObject()) {
                    for (const std::string& k : schema.getMemberNames())
                        if (schema[k].isObject() && schema[k].get("secret", false).asBool())
                            declared.insert(k);
                }
                auto entries = vault->ListForPackage(id);
                std::set<std::string> stored;
                for (const auto& e : entries) stored.insert(e.name);

                Json::Value items(Json::arrayValue);
                for (const auto& name : declared) {
                    Json::Value s(Json::objectValue);
                    s["name"] = name;
                    s["declared"] = true;
                    s["set"] = stored.count(name) > 0;
                    items.append(s);
                }
                for (const auto& e : entries) {
                    if (declared.count(e.name)) continue;  // already listed
                    Json::Value s(Json::objectValue);
                    s["name"] = e.name;
                    s["declared"] = false;
                    s["set"] = true;
                    s["updated_at"] = static_cast<Json::Int64>(e.updated_at_unix_ms);
                    items.append(s);
                }
                Json::Value body(Json::objectValue);
                body["secrets"] = items;
                body["count"] = static_cast<Json::Int>(items.size());
                SendJsonResponse(callback, body);
            },
            {drogon::Get});

        // PUT /api/v1/api/packages/{id}/secrets/{name}
        drogon::app().registerHandler(
            "/api/v1/api/packages/{id}/secrets/{name}",
            [requireVault](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback,
               const std::string& id, const std::string& name) {
                AdminServer* server = g_activeAdminServer.load();
                if (!server) { SendServerNotActive(callback); return; }
                if (!server->m_apiPackageStore) {
                    SendJsonError(callback, "api package store not available",
                        drogon::k503ServiceUnavailable);
                    return;
                }
                auto pkg = server->m_apiPackageStore->GetPackage(id);
                if (!pkg) { SendJsonError(callback, "package not found", drogon::k404NotFound); return; }
                SecretsVault* vault = requireVault(server, callback);
                if (!vault) return;

                Json::Value body;
                std::string parseError;
                if (!ParseJsonPayload(std::string(req->body()), &body, &parseError) ||
                    !body.isObject() || !body.isMember("value") || !body["value"].isString()) {
                    SendJsonError(callback, "body must be {\"value\": str}",
                        drogon::k400BadRequest);
                    return;
                }
                const std::string value = body["value"].asString();
                if (value.empty() || value == "***") {
                    SendJsonError(callback, "refusing to store an empty/masked value",
                        drogon::k400BadRequest);
                    return;
                }
                std::string setErr;
                if (!vault->Set(id, name, value, setErr)) {
                    SendJsonError(callback, "vault write failed: " + setErr,
                        drogon::k500InternalServerError);
                    return;
                }
                Json::Value out(Json::objectValue);
                out["ok"] = true;
                out["name"] = name;  // acknowledgment by name only — never the value
                SendJsonResponse(callback, out);
            },
            {drogon::Put});

        // DELETE /api/v1/api/packages/{id}/secrets/{name}
        drogon::app().registerHandler(
            "/api/v1/api/packages/{id}/secrets/{name}",
            [requireVault](const drogon::HttpRequestPtr& req,
               std::function<void(const drogon::HttpResponsePtr&)>&& callback,
               const std::string& id, const std::string& name) {
                (void)req;
                AdminServer* server = g_activeAdminServer.load();
                if (!server) { SendServerNotActive(callback); return; }
                SecretsVault* vault = requireVault(server, callback);
                if (!vault) return;
                if (!vault->Delete(id, name)) {
                    SendJsonError(callback, "secret not found", drogon::k404NotFound);
                    return;
                }
                Json::Value out(Json::objectValue);
                out["ok"] = true;
                SendJsonResponse(callback, out);
            },
            {drogon::Delete});
