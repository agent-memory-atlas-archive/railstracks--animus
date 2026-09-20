#include "animus_kernel/admin/ProviderManager.h"
#include "animus_kernel/AgentConfigStore.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

#include "animus_kernel/llm/LLMProviderBase.h"
#include "animus_kernel/llm/LLMProviderRegistry.h"

namespace animus::kernel {
namespace {

std::string MaskSecret(const std::string& value) {
    if (value.empty()) return {};
    if (value.size() <= 8) {
        return std::string(value.size(), '*');
    }
    return value.substr(0, 4) + "..." + value.substr(value.size() - 4);
}

} // namespace

void ProviderManager::Configure(const KernelConfig::ProviderConfigStorage& storage) {
    std::lock_guard<std::mutex> lock(m_providerMutex);
    m_providerStorage = storage;
    LoadStaticContextSizes();
}

void ProviderManager::LoadStaticContextSizes() {
    m_staticContextSizes.clear();
    if (m_providerStorage.modelContextSizesPath.empty()) return;

    std::ifstream file(m_providerStorage.modelContextSizesPath);
    if (!file.is_open()) {
        std::cerr << "[provider] No static context sizes file at "
                  << m_providerStorage.modelContextSizesPath << " (ok, skipping)\n";
        return;
    }

    Json::Value root;
    Json::CharReaderBuilder builder;
    std::string parseErrors;
    if (!Json::parseFromStream(builder, file, &root, &parseErrors)) {
        std::cerr << "[provider] Failed to parse model_context_sizes.json: " << parseErrors << "\n";
        return;
    }

    for (const auto& modelId : root.getMemberNames()) {
        const auto& v = root[modelId];
        if ((v.isUInt() || v.isInt()) && v.asUInt() > 0) {
            m_staticContextSizes[modelId] = v.asUInt();
        }
    }
    std::cerr << "[provider] Loaded " << m_staticContextSizes.size()
              << " static context size entries\n";
}

std::uint32_t ProviderManager::LookupStaticContextWindow(
    const std::string& modelId) const {
    if (modelId.empty()) return 0;

    // Direct lookup
    auto it = m_staticContextSizes.find(modelId);
    if (it != m_staticContextSizes.end()) return it->second;

    // Try suffix matching for vendor-prefixed names.
    // e.g. provider returns "deepseek/deepseek-v4-flash" -> try "deepseek-v4-flash"
    auto slashPos = modelId.find('/');
    if (slashPos != std::string::npos && slashPos + 1 < modelId.size()) {
        std::string suffix = modelId.substr(slashPos + 1);
        it = m_staticContextSizes.find(suffix);
        if (it != m_staticContextSizes.end()) return it->second;
    }

    return 0;
}

bool ProviderManager::LoadFromDisk(std::string* error) {
    std::lock_guard<std::mutex> lock(m_providerMutex);
    m_providersByName.clear();
    m_defaultProvider.clear();

    if (!m_providerStorage.persistToDisk) {
        return true;
    }
    if (m_providerStorage.providersFilePath.empty()) {
        if (error) *error = "providers file path must not be empty when persistence is enabled";
        return false;
    }

    const std::filesystem::path path(m_providerStorage.providersFilePath);
    if (!std::filesystem::exists(path)) {
        return true;
    }

    std::ifstream in(path);
    if (!in.is_open()) {
        if (error) *error = "failed to open providers file for reading: " + path.string();
        return false;
    }

    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string parseErrors;
    if (!Json::parseFromStream(builder, in, &root, &parseErrors)) {
        if (error) *error = "failed to parse providers file: " + parseErrors;
        return false;
    }

    m_defaultProvider = root.get("default_provider", "").asString();

    if (root.isMember("providers") && root["providers"].isObject()) {
        const auto& providers = root["providers"];
        for (const auto& id : providers.getMemberNames()) {
            const auto& entry = providers[id];
            if (!entry.isObject()) continue;

            ProviderState state;
            state.providerId = id;
            state.providerType = entry.get("provider_type", id).asString();
            state.baseUrl = entry.get("base_url", "").asString();
            state.apiKey = entry.get("api_key", "").asString();
            state.defaultModel = entry.get("default_model", "").asString();
            state.defaultContextWindow = entry.get("default_context_window", 128000).asUInt();
            state.authType = entry.get("auth_type", "api_key").asString();
            state.authFile = entry.get("auth_file", "").asString();
            state.concurrency = entry.get("concurrency", 1).asInt();
            state.status = "untested";
            if (state.defaultContextWindow == 0U) {
                state.defaultContextWindow = 128000U;
            }

            if (state.apiKey.empty() && state.authType == "api_key") {
                if (id == "ollama") {
                    state.authType = "none";
                }
            }

            if (entry.isMember("extra") && entry["extra"].isObject()) {
                const auto& extra = entry["extra"];
                for (const auto& key : extra.getMemberNames()) {
                    if (extra[key].isString()) {
                        state.extra[key] = extra[key].asString();
                    }
                }
            }

            if (entry.isMember("model_context_windows") && entry["model_context_windows"].isObject()) {
                const auto& windows = entry["model_context_windows"];
                for (const auto& modelId : windows.getMemberNames()) {
                    const Json::Value& value = windows[modelId];
                    if ((value.isUInt() || value.isInt()) && value.asUInt() > 0U) {
                        state.modelContextWindows[modelId] = value.asUInt();
                    }
                }
            }

            m_providersByName[id] = std::move(state);
        }
    }

    return true;
}

bool ProviderManager::SaveToDisk(std::string* error) const {
    std::lock_guard<std::mutex> lock(m_providerMutex);
    if (!m_providerStorage.persistToDisk) {
        return true;
    }
    if (m_providerStorage.providersFilePath.empty()) {
        if (error) *error = "providers file path must not be empty when persistence is enabled";
        return false;
    }

    const std::filesystem::path path(m_providerStorage.providersFilePath);
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error) *error = "failed to create providers config directory: " + parent.string();
            return false;
        }
    }

    Json::Value root(Json::objectValue);
    root["default_provider"] = m_defaultProvider;

    Json::Value providers(Json::objectValue);
    for (const auto& [id, state] : m_providersByName) {
        Json::Value entry(Json::objectValue);
        entry["provider_type"] = state.providerType;
        entry["base_url"] = state.baseUrl;
        if (state.authType == "api_key" && !state.apiKey.empty()) {
            entry["api_key"] = state.apiKey;
        }
        entry["default_model"] = state.defaultModel;
        entry["default_context_window"] = static_cast<Json::UInt>(state.defaultContextWindow);
        entry["auth_type"] = state.authType;
        entry["concurrency"] = state.concurrency;
        if (!state.authFile.empty()) {
            entry["auth_file"] = state.authFile;
        }
        if (!state.extra.empty()) {
            Json::Value extra(Json::objectValue);
            for (const auto& [k, v] : state.extra) {
                extra[k] = v;
            }
            entry["extra"] = extra;
        }
        if (!state.modelContextWindows.empty()) {
            Json::Value modelContextWindows(Json::objectValue);
            for (const auto& [modelId, contextWindow] : state.modelContextWindows) {
                modelContextWindows[modelId] = static_cast<Json::UInt>(contextWindow);
            }
            entry["model_context_windows"] = modelContextWindows;
        }
        providers[id] = entry;
    }
    root["providers"] = providers;

    Json::StreamWriterBuilder writerBuilder;
    writerBuilder["indentation"] = "  ";

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        if (error) *error = "failed to open providers file for writing: " + path.string();
        return false;
    }

    std::unique_ptr<Json::StreamWriter> writer(writerBuilder.newStreamWriter());
    writer->write(root, &out);
    out << "\n";
    if (!out.good()) {
        if (error) *error = "failed to write providers file: " + path.string();
        return false;
    }
    return true;
}

std::vector<ProviderState> ProviderManager::ListProviders() const {
    std::vector<ProviderState> providers;
    std::lock_guard<std::mutex> lock(m_providerMutex);
    providers.reserve(m_providersByName.size());
    for (const auto& pair : m_providersByName) {
        providers.push_back(pair.second);
    }
    return providers;
}

std::optional<ProviderState> ProviderManager::GetProvider(const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_providerMutex);
    auto it = FindProviderLocked(id);
    if (it == m_providersByName.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool ProviderManager::HasProvider(const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_providerMutex);
    return FindProviderLocked(id) != m_providersByName.end();
}

std::string ProviderManager::GetDefaultProviderId() const {
    std::lock_guard<std::mutex> lock(m_providerMutex);
    return m_defaultProvider;
}

bool ProviderManager::SetDefaultProviderId(const std::string& id, std::string* error) {
    std::lock_guard<std::mutex> lock(m_providerMutex);
    if (id.empty()) {
        m_defaultProvider.clear();
        return true;
    }
    auto it = FindProviderLocked(id);
    if (it == m_providersByName.end()) {
        if (error) *error = "provider not found: " + id;
        return false;
    }
    m_defaultProvider = it->first;
    return true;
}

bool ProviderManager::CreateProvider(const ProviderState& state, std::string* error) {
    std::lock_guard<std::mutex> lock(m_providerMutex);
    if (FindProviderLocked(state.providerId) != m_providersByName.end()) {
        if (error) *error = "provider already exists: " + state.providerId;
        return false;
    }
    m_providersByName[state.providerId] = state;
    if (m_defaultProvider.empty()) {
        m_defaultProvider = state.providerId;
    }
    return true;
}

bool ProviderManager::UpdateProvider(const std::string& id, const ProviderState& state, std::string* error) {
    std::lock_guard<std::mutex> lock(m_providerMutex);
    auto it = FindProviderLocked(id);
    if (it == m_providersByName.end()) {
        if (error) *error = "provider not found: " + id;
        return false;
    }
    it->second = state;
    return true;
}

bool ProviderManager::DeleteProvider(const std::string& id, ProviderState* removed, std::string* error) {
    std::lock_guard<std::mutex> lock(m_providerMutex);
    auto it = FindProviderLocked(id);
    if (it == m_providersByName.end()) {
        if (error) *error = "provider not found: " + id;
        return false;
    }
    if (removed) {
        *removed = it->second;
    }
    m_providersByName.erase(it);
    if (m_defaultProvider == id) {
        m_defaultProvider = m_providersByName.empty() ? "" : m_providersByName.begin()->first;
    }
    return true;
}

bool ProviderManager::UpdateProviderStatus(
    const std::string& id,
    const std::string& status,
    const std::string& lastError,
    std::uint64_t lastTestedUnixMs,
    ProviderState* updated,
    std::string* error) {
    std::lock_guard<std::mutex> lock(m_providerMutex);
    auto it = FindProviderLocked(id);
    if (it == m_providersByName.end()) {
        if (error) *error = "provider not found: " + id;
        return false;
    }
    it->second.status = status;
    it->second.lastError = lastError;
    it->second.lastTestedUnixMs = lastTestedUnixMs;
    if (updated) {
        *updated = it->second;
    }
    return true;
}

bool ProviderManager::UpdateProviderCapabilities(
    const std::string& id,
    const llm::ProviderCapabilities& capabilities,
    ProviderState* updated,
    std::string* error,
    const std::string& modelId) {
    std::lock_guard<std::mutex> lock(m_providerMutex);
    auto it = FindProviderLocked(id);
    if (it == m_providersByName.end()) {
        if (error) *error = "provider not found: " + id;
        return false;
    }
    it->second.capabilities = capabilities;

    // Populate per-model context window from capability discovery,
    // but never overwrite a manual value the user may have set.
    if (!modelId.empty() && capabilities.context_length > 0U) {
        auto mcwIt = it->second.modelContextWindows.find(modelId);
        if (mcwIt == it->second.modelContextWindows.end() || mcwIt->second == 0U) {
            it->second.modelContextWindows[modelId] =
                static_cast<std::uint32_t>(capabilities.context_length);
        }
    }

    if (updated) {
        *updated = it->second;
    }
    return true;
}

int ProviderManager::GetProviderConcurrency(const std::string& providerId) const {
    std::lock_guard<std::mutex> lock(m_providerMutex);
    auto it = FindProviderLocked(providerId);
    if (it != m_providersByName.end()) return it->second.concurrency;
    return 1;
}

std::optional<llm::LLMProviderConfig> ProviderManager::GetProviderConfig(const std::string& providerId) const {
    std::lock_guard<std::mutex> lock(m_providerMutex);
    auto it = FindProviderLocked(providerId);
    if (it == m_providersByName.end()) {
        std::cerr << "[config] provider not found: '" << providerId << "' available: ";
        for (const auto& [k, _] : m_providersByName) std::cerr << "'" << k << "' ";
        std::cerr << std::endl;
        return std::nullopt;
    }
    const auto& state = it->second;

    llm::LLMProviderConfig config;
    config.provider_id = state.providerType.empty() ? state.providerId : state.providerType;
    config.base_url = state.baseUrl;
    config.api_key = state.apiKey;
    config.default_model = state.defaultModel;
    config.extra = state.extra;
    if (state.authType == "oauth" && !state.authFile.empty()) {
        config.extra["auth_file"] = state.authFile;
    }
    return config;
}

bool ProviderManager::ResolveProviderAuthFilePathLocked(
    const std::string& providerId,
    std::string* authFile,
    std::string* error) const {
    if (!authFile) {
        if (error) *error = "auth file output is null";
        return false;
    }
    auto it = FindProviderLocked(providerId);
    if (it == m_providersByName.end()) {
        if (error) *error = "provider not found: " + providerId;
        return false;
    }
    *authFile = it->second.authFile;
    if (authFile->empty()) {
        *authFile = m_providerStorage.authFilePath;
    }
    return true;
}

bool ProviderManager::BuildRuntimeProviderConfigLocked(
    const std::string& providerId,
    llm::LLMProviderConfig* configOut,
    ProviderState* stateOut,
    std::string* error) const {
    if (!configOut) {
        if (error) *error = "provider config output is null";
        return false;
    }

    auto it = FindProviderLocked(providerId);
    if (it == m_providersByName.end()) {
        if (error) *error = "provider not found: " + providerId;
        return false;
    }

    const ProviderState& state = it->second;
    llm::LLMProviderConfig config;
    config.provider_id = state.providerType.empty() ? state.providerId : state.providerType;
    config.base_url = state.baseUrl;
    config.api_key = state.apiKey;
    config.default_model = state.defaultModel;
    config.extra = state.extra;
    if (state.authType == "oauth" && !state.authFile.empty()) {
        config.extra["auth_file"] = state.authFile;
    }

    *configOut = std::move(config);
    if (stateOut) {
        *stateOut = state;
    }
    return true;
}

std::unordered_map<std::string, ProviderState>::const_iterator
ProviderManager::FindProviderLocked(const std::string& id) const {
    auto it = m_providersByName.find(id);
    if (it != m_providersByName.end()) {
        return it;
    }
    std::string lowerId = id;
    std::transform(lowerId.begin(), lowerId.end(), lowerId.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    for (it = m_providersByName.begin(); it != m_providersByName.end(); ++it) {
        std::string lowerKey = it->first;
        std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lowerKey == lowerId) {
            return it;
        }
    }
    return m_providersByName.end();
}

std::unordered_map<std::string, ProviderState>::iterator
ProviderManager::FindProviderLocked(const std::string& id) {
    auto it = m_providersByName.find(id);
    if (it != m_providersByName.end()) {
        return it;
    }
    std::string lowerId = id;
    std::transform(lowerId.begin(), lowerId.end(), lowerId.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    for (it = m_providersByName.begin(); it != m_providersByName.end(); ++it) {
        std::string lowerKey = it->first;
        std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (lowerKey == lowerId) {
            return it;
        }
    }
    return m_providersByName.end();
}

bool ProviderManager::LoadAuthFromDisk(
    const std::string& providerId,
    Json::Value* out,
    std::string* error) const {
    if (!out) {
        if (error) *error = "output is null";
        return false;
    }

    std::string authFile;
    {
        std::lock_guard<std::mutex> lock(m_providerMutex);
        if (!ResolveProviderAuthFilePathLocked(providerId, &authFile, error)) {
            return false;
        }
    }

    if (authFile.empty()) {
        *out = Json::Value(Json::objectValue);
        return true;
    }

    const std::filesystem::path path(authFile);
    if (!std::filesystem::exists(path)) {
        *out = Json::Value(Json::objectValue);
        return true;
    }

    std::ifstream in(path);
    if (!in.is_open()) {
        if (error) *error = "failed to open auth file: " + path.string();
        return false;
    }

    Json::CharReaderBuilder builder;
    std::string parseErrors;
    if (!Json::parseFromStream(builder, in, out, &parseErrors)) {
        if (error) *error = "failed to parse auth file: " + parseErrors;
        return false;
    }

    return true;
}

bool ProviderManager::SaveAuthToDisk(
    const std::string& providerId,
    const Json::Value& auth,
    std::string* error) const {
    std::string authFile;
    {
        std::lock_guard<std::mutex> lock(m_providerMutex);
        if (!ResolveProviderAuthFilePathLocked(providerId, &authFile, error)) {
            return false;
        }
    }

    if (authFile.empty()) {
        if (error) *error = "no auth file path configured";
        return false;
    }

    const std::filesystem::path path(authFile);
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error) *error = "failed to create auth directory: " + parent.string();
            return false;
        }
    }

    Json::Value root(Json::objectValue);
    if (std::filesystem::exists(path)) {
        std::ifstream in(path);
        if (in.is_open()) {
            Json::CharReaderBuilder builder;
            std::string parseErrors;
            Json::parseFromStream(builder, in, &root, &parseErrors);
        }
    }

    root[providerId] = auth;

    Json::StreamWriterBuilder writerBuilder;
    writerBuilder["indentation"] = "  ";

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        if (error) *error = "failed to open auth file for writing: " + path.string();
        return false;
    }

    std::unique_ptr<Json::StreamWriter> writer(writerBuilder.newStreamWriter());
    writer->write(root, &out);
    out << "\n";
    return true;
}

Json::Value ProviderManager::BuildProviderJson(const ProviderState& provider, bool maskSecrets) const {
    Json::Value out(Json::objectValue);
    out["provider_id"] = provider.providerId;
    out["provider_type"] = provider.providerType;
    out["base_url"] = provider.baseUrl;
    out["default_model"] = provider.defaultModel;
    out["default_context_window"] = static_cast<Json::UInt>(provider.defaultContextWindow);
    out["auth_type"] = provider.authType;
    out["status"] = provider.status;
    out["last_error"] = provider.lastError;
    out["last_tested_unix_ms"] = static_cast<Json::UInt64>(provider.lastTestedUnixMs);
    out["concurrency"] = provider.concurrency;

    if (maskSecrets && provider.authType == "api_key") {
        out["api_key"] = MaskSecret(provider.apiKey);
    } else if (!maskSecrets) {
        out["api_key"] = provider.apiKey;
    } else {
        out["api_key"] = "";
    }

    if (!provider.authFile.empty()) {
        out["auth_file"] = provider.authFile;
    }

    Json::Value extra(Json::objectValue);
    for (const auto& [k, v] : provider.extra) {
        extra[k] = v;
    }
    out["extra"] = extra;

    Json::Value modelContextWindows(Json::objectValue);
    for (const auto& [modelId, contextWindow] : provider.modelContextWindows) {
        modelContextWindows[modelId] = static_cast<Json::UInt>(contextWindow);
    }
    out["model_context_windows"] = modelContextWindows;

    Json::Value caps(Json::objectValue);
    caps["model_id"] = provider.capabilities.model_id;
    caps["context_length"] = provider.capabilities.context_length;
    caps["supports_tools"] = provider.capabilities.supports_tools;
    caps["supports_tool_choice"] = provider.capabilities.supports_tool_choice;
    caps["supports_reasoning"] = provider.capabilities.supports_reasoning;
    caps["supports_streaming"] = provider.capabilities.supports_streaming;
    caps["supports_json_mode"] = provider.capabilities.supports_json_mode;
    caps["supports_vision"] = provider.capabilities.supports_vision;
    Json::Value rawFeatures(Json::arrayValue);
    for (const auto& f : provider.capabilities.raw_features) {
        rawFeatures.append(f);
    }
    caps["raw_features"] = rawFeatures;
    out["capabilities"] = caps;

    return out;
}

bool ProviderManager::ValidateProviderPayload(
    const Json::Value& body,
    bool requireProviderId,
    std::string* error) const {
    if (!body.isObject()) {
        if (error) *error = "request body must be a JSON object";
        return false;
    }
    if (requireProviderId) {
        if (!body.isMember("provider_id") || !body["provider_id"].isString()) {
            if (error) *error = "provider_id is required";
            return false;
        }
        const std::string id = body["provider_id"].asString();
        if (id.empty() || id.size() > 64) {
            if (error) *error = "provider_id must be 1-64 characters";
            return false;
        }
        // "__" is the reserved config-store namespace (__providers rows,
        // __kernel__ rows) — a provider id starting with it would collide
        // with structural keys in the replicated agent_config space.
        if (id.rfind("__", 0) == 0) {
            if (error) *error = "provider_id must not start with '__' (reserved namespace)";
            return false;
        }
        for (char c : id) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != ' ') {
                if (error) {
                    *error =
                        "provider_id contains invalid characters (alphanumeric, dash, underscore, space)";
                }
                return false;
            }
        }
    } else if (body.isMember("provider_id") && !body["provider_id"].isString()) {
        if (error) *error = "provider_id must be a string";
        return false;
    }

    if (body.isMember("provider_type") && !body["provider_type"].isString()) {
        if (error) *error = "provider_type must be a string";
        return false;
    }
    if (body.isMember("base_url") && !body["base_url"].isString()) {
        if (error) *error = "base_url must be a string";
        return false;
    }
    if (body.isMember("default_model") && !body["default_model"].isString()) {
        if (error) *error = "default_model must be a string";
        return false;
    }
    if (body.isMember("default_context_window")) {
        const Json::Value& context = body["default_context_window"];
        if (!context.isUInt() && !context.isInt()) {
            if (error) *error = "default_context_window must be an integer";
            return false;
        }
        const std::uint32_t value = context.asUInt();
        if (value == 0U || value > 50000000U) {
            if (error) *error = "default_context_window must be between 1 and 50000000";
            return false;
        }
    }
    if (body.isMember("model_context_windows")) {
        const Json::Value& windows = body["model_context_windows"];
        if (!windows.isObject()) {
            if (error) *error = "model_context_windows must be an object";
            return false;
        }
        for (const auto& modelId : windows.getMemberNames()) {
            const Json::Value& value = windows[modelId];
            if (!value.isUInt() && !value.isInt()) {
                if (error) *error = "model_context_windows values must be integers";
                return false;
            }
            const std::uint32_t parsed = value.asUInt();
            if (parsed == 0U || parsed > 50000000U) {
                if (error) *error = "model_context_windows values must be between 1 and 50000000";
                return false;
            }
        }
    }
    if (body.isMember("auth_type")) {
        if (!body["auth_type"].isString()) {
            if (error) *error = "auth_type must be a string";
            return false;
        }
        const std::string at = body["auth_type"].asString();
        if (at != "api_key" && at != "oauth" && at != "none") {
            if (error) *error = "auth_type must be 'api_key', 'oauth', or 'none'";
            return false;
        }
    }
    return true;
}

bool ProviderManager::ParseProviderPayload(
    const Json::Value& body,
    const ProviderState& defaults,
    ProviderState* out,
    std::string* error) const {
    if (!out) {
        if (error) *error = "provider output is null";
        return false;
    }
    if (!body.isObject()) {
        if (error) *error = "request body must be a JSON object";
        return false;
    }

    ProviderState p;
    p.providerId = body.get("provider_id", defaults.providerId).asString();
    p.providerType = body.get("provider_type", defaults.providerType).asString();
    p.baseUrl = body.get("base_url", defaults.baseUrl).asString();
    p.apiKey = body.isMember("api_key") && body["api_key"].isString() && !body["api_key"].asString().empty()
        ? body["api_key"].asString()
        : defaults.apiKey;
    p.defaultModel = body.get("default_model", defaults.defaultModel).asString();
    p.defaultContextWindow = body.get(
        "default_context_window",
        Json::Value(static_cast<Json::UInt>(defaults.defaultContextWindow))).asUInt();
    if (p.defaultContextWindow == 0U) {
        p.defaultContextWindow = defaults.defaultContextWindow == 0U ? 128000U : defaults.defaultContextWindow;
    }
    p.authType = body.get("auth_type", defaults.authType).asString();
    p.authFile = body.get("auth_file", defaults.authFile).asString();
    p.status = defaults.status;
    p.lastError = defaults.lastError;
    p.lastTestedUnixMs = defaults.lastTestedUnixMs;
    p.concurrency = body.get("concurrency", defaults.concurrency).asInt();
    p.capabilities = defaults.capabilities;

    p.extra = defaults.extra;
    if (body.isMember("extra")) {
        if (!body["extra"].isObject()) {
            if (error) *error = "extra must be an object";
            return false;
        }
        p.extra.clear();
        const auto& extra = body["extra"];
        for (const auto& key : extra.getMemberNames()) {
            if (extra[key].isString()) {
                p.extra[key] = extra[key].asString();
            }
        }
    }

    p.modelContextWindows = defaults.modelContextWindows;
    if (body.isMember("model_context_windows")) {
        if (!body["model_context_windows"].isObject()) {
            if (error) *error = "model_context_windows must be an object";
            return false;
        }
        p.modelContextWindows.clear();
        const Json::Value& windows = body["model_context_windows"];
        for (const auto& modelId : windows.getMemberNames()) {
            const Json::Value& value = windows[modelId];
            if ((value.isUInt() || value.isInt()) && value.asUInt() > 0U) {
                p.modelContextWindows[modelId] = value.asUInt();
            }
        }
    }

    *out = std::move(p);
    return true;
}

bool ProviderManager::ValidateOAuthProvider(
    const std::string& id,
    bool requireOpenAICodex,
    ProviderState* stateOut,
    int* httpStatusCodeOut,
    std::string* error) const {
    auto stateOpt = GetProvider(id);
    if (!stateOpt.has_value()) {
        if (httpStatusCodeOut) {
            *httpStatusCodeOut = 404;
        }
        if (error) *error = "provider not found: " + id;
        return false;
    }
    const ProviderState& state = *stateOpt;
    if (state.authType != "oauth") {
        if (httpStatusCodeOut) {
            *httpStatusCodeOut = 400;
        }
        if (error) *error = "provider is not configured for oauth auth_type";
        return false;
    }
    if (requireOpenAICodex && state.providerType != "openai-codex" && state.providerId != "openai-codex") {
        if (httpStatusCodeOut) {
            *httpStatusCodeOut = 400;
        }
        if (error) *error = "oauth browser flow currently supported for openai-codex only";
        return false;
    }
    if (stateOut) {
        *stateOut = state;
    }
    if (httpStatusCodeOut) {
        *httpStatusCodeOut = 200;
    }
    return true;
}

bool ProviderManager::BuildOAuthStatus(
    const std::string& id,
    Json::Value* out,
    int* httpStatusCodeOut,
    std::string* error) const {
    if (!out) {
        if (httpStatusCodeOut) {
            *httpStatusCodeOut = 500;
        }
        if (error) *error = "output is null";
        return false;
    }

    ProviderState state;
    if (!ValidateOAuthProvider(id, false, &state, httpStatusCodeOut, error)) {
        return false;
    }

    Json::Value authRoot;
    std::string authErr;
    if (!LoadAuthFromDisk(id, &authRoot, &authErr)) {
        if (httpStatusCodeOut) {
            *httpStatusCodeOut = 500;
        }
        if (error) *error = authErr;
        return false;
    }

    Json::Value status(Json::objectValue);
    status["provider_id"] = id;
    status["auth_type"] = state.authType;
    status["auth_file"] = state.authFile;
    status["connected"] = false;

    if (authRoot.isObject() && authRoot.isMember(id) && authRoot[id].isObject()) {
        const Json::Value& entry = authRoot[id];
        const std::string access = entry.get("access", "").asString();
        const Json::Int64 expires = entry.get("expires", 0).asInt64();
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
        status["connected"] = !access.empty();
        status["expires"] = static_cast<Json::Int64>(expires);
        status["expired"] = expires > 0 ? (expires <= nowMs) : true;
        status["has_refresh"] = !entry.get("refresh", "").asString().empty();
    }

    *out = std::move(status);
    if (httpStatusCodeOut) {
        *httpStatusCodeOut = 200;
    }
    return true;
}

bool ProviderManager::FetchProviderModels(
    const std::string& id,
    llm::LLMProviderRegistry* registry,
    Json::Value* modelsOut,
    bool* fetchedOut,
    std::string* error) const {
    if (!registry) {
        if (error) *error = "provider registry not available";
        return false;
    }
    if (!modelsOut) {
        if (error) *error = "models output is null";
        return false;
    }

    llm::LLMProviderConfig config;
    ProviderState state;
    {
        std::lock_guard<std::mutex> lock(m_providerMutex);
        if (!BuildRuntimeProviderConfigLocked(id, &config, &state, error)) {
            return false;
        }
    }

    auto provider = registry->Create(config);
    if (!provider) {
        if (error) *error = "failed to create provider: " + config.provider_id;
        return false;
    }

    auto* baseProvider = dynamic_cast<llm::LLMProviderBase*>(provider.get());
    if (!baseProvider) {
        if (error) *error = "provider does not support model listing";
        return false;
    }

    const std::string modelId = state.defaultModel.empty() ? "_list" : state.defaultModel;
    const bool ok = baseProvider->FetchCapabilities(modelId);
    if (fetchedOut) {
        *fetchedOut = ok;
    }

    *modelsOut = Json::Value(Json::arrayValue);
    for (const auto& model : baseProvider->GetCapabilities().raw_features) {
        modelsOut->append(model);
    }
    return true;
}

bool ProviderManager::RefreshProviderCapabilities(
    const std::string& id,
    llm::LLMProviderRegistry* registry,
    const std::string& modelOverride,
    llm::ProviderCapabilities* capabilitiesOut,
    bool* fetchedOut,
    std::string* error) {
    if (!registry) {
        if (error) *error = "provider registry not available";
        return false;
    }
    if (!capabilitiesOut) {
        if (error) *error = "capabilities output is null";
        return false;
    }

    llm::LLMProviderConfig config;
    ProviderState state;
    {
        std::lock_guard<std::mutex> lock(m_providerMutex);
        if (!BuildRuntimeProviderConfigLocked(id, &config, &state, error)) {
            return false;
        }
    }

    auto provider = registry->Create(config);
    if (!provider) {
        if (error) *error = "failed to create provider: " + config.provider_id;
        return false;
    }

    auto* baseProvider = dynamic_cast<llm::LLMProviderBase*>(provider.get());
    if (!baseProvider) {
        if (error) *error = "provider does not support capabilities";
        return false;
    }

    std::string modelId = modelOverride;
    if (modelId.empty()) {
        modelId = state.defaultModel;
    }
    if (modelId.empty()) {
        if (error) *error = "no default model configured for this provider";
        return false;
    }

    const bool ok = baseProvider->FetchCapabilities(modelId);
    if (fetchedOut) {
        *fetchedOut = ok;
    }
    auto caps = baseProvider->GetCapabilities();  // copy — may mutate

    // If the provider didn't return a context_length, fall back to the
    // static lookup table (built from OpenRouter's published model data).
    if (caps.context_length == 0U) {
        std::uint32_t fallback = LookupStaticContextWindow(modelId);
        if (fallback > 0U) {
            caps.context_length = fallback;
            std::cerr << "[provider] Context window for '" << modelId
                      << "' from static lookup: " << fallback << "\n";
        }
    }

    *capabilitiesOut = caps;

    ProviderState updated;
    std::string capErr;
    if (!UpdateProviderCapabilities(id, caps, &updated, &capErr, modelId)) {
        if (error) *error = capErr.empty() ? "failed to update provider capabilities" : capErr;
        return false;
    }
    return true;
}

bool ProviderManager::TestProviderConnectivity(
    const std::string& id,
    bool* successOut,
    std::string* testErrorOut,
    std::string* statusOut,
    std::string* error) {
    auto stateOpt = GetProvider(id);
    if (!stateOpt.has_value()) {
        if (error) *error = "provider not found: " + id;
        return false;
    }
    ProviderState state = *stateOpt;

    bool success = false;
    std::string testError;
    if (state.baseUrl.empty()) {
        testError = "base_url is not configured";
    } else if (state.authType == "api_key" && state.apiKey.empty()) {
        testError = "api_key is not configured";
    } else if (state.defaultModel.empty()) {
        testError = "default_model is not configured";
    } else {
        success = true;
    }

    const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());

    ProviderState updated;
    std::string statusErr;
    if (UpdateProviderStatus(
            id,
            success ? "available" : "error",
            testError,
            nowMs,
            &updated,
            &statusErr)) {
        state = updated;
    }

    if (successOut) {
        *successOut = success;
    }
    if (testErrorOut) {
        *testErrorOut = testError;
    }
    if (statusOut) {
        *statusOut = state.status;
    }
    return true;
}




// ── #93 P3 slice 3: store-backed persistence ─────────────────────────────
// See ProviderManager.h for the kv layout and boot-import contract.

namespace {
constexpr const char* kProviderConfigAgent = "__providers";

std::string CfgRow(const std::string& id) { return "cfg." + id; }
std::string ApiKeyRow(const std::string& id) { return "cfg." + id + ".api_key"; }
std::string AuthRow(const std::string& id) { return "cfg." + id + ".auth_secret"; }
constexpr const char* kDefaultRow = "__default";
}  // namespace

void ProviderManager::ConfigureStore(AgentConfigStore* store) {
    m_configStore = store;
}

bool ProviderManager::LoadProviders(std::string* error) {
    if (!m_configStore) return LoadFromDisk(error);

    // Store mode. The empty-check reads through the (thread-safe) config
    // store without our mutex; the import path calls LoadFromDisk and
    // SaveProviders, which take the mutex themselves.
    const auto rows = m_configStore->GetAll(kProviderConfigAgent);
    bool sawProviderRow = false;
    for (const auto& [k, v] : rows) {
        if (k == kDefaultRow) continue;
        if (k.rfind("cfg.", 0) == 0) { sawProviderRow = true; break; }
    }

    if (!sawProviderRow) {
        // Empty store. Legacy files present? -> one-time import; after
        // this the store is the source of truth and files are ignored.
        const std::filesystem::path p(m_providerStorage.providersFilePath);
        if (m_providerStorage.persistToDisk && !m_providerStorage.providersFilePath.empty() &&
            std::filesystem::exists(p)) {
            if (!LoadFromDisk(error)) return false;
            if (!SaveProviders(error)) return false;
            // Import auth blobs for providers that carry one (non-api_key
            // auth: oauth etc.). Failures are loud but non-fatal — the
            // config is in; auth can be re-supplied via the admin UI.
            for (const auto& id : ListProviderIdsForAuthImport()) {
                Json::Value auth;
                std::string authErr;
                // LoadAuthFromDisk returns the whole-file ROOT (callers
                // index by provider id); extract this provider's slice.
                if (LoadAuthFromDisk(id, &auth, &authErr) && auth.isObject() &&
                    auth.isMember(id)) {
                    std::string saveErr;
                    if (!SaveAuthProvider(id, auth[id], &saveErr)) {
                        std::cerr << "[provider-manager] auth import failed for '"
                                  << id << "': " << saveErr << std::endl;
                    }
                }
            }
            std::cerr << "[provider-manager] imported provider config from "
                      << p.string() << " into the replicated store" << std::endl;
            return true;
        }
        // Empty store, no legacy file: fresh install.
        std::lock_guard<std::mutex> lock(m_providerMutex);
        m_providersByName.clear();
        m_defaultProvider.clear();
        return true;
    }

    // Rebuild from rows (carrying over runtime state of survivors).
    std::lock_guard<std::mutex> lock(m_providerMutex);

    std::unordered_map<std::string, ProviderState> next;
    std::unordered_map<std::string, std::string> apiKeys;
    for (const auto& [k, v] : rows) {
        if (k == kDefaultRow || k.rfind("cfg.", 0) != 0) continue;
        if (k.size() > 8 && k.compare(k.size() - 8, 8, ".api_key") == 0) {
            apiKeys[k.substr(4, k.size() - 4 - 8)] = v;
            continue;
        }
        if (k.size() > 12 && k.compare(k.size() - 12, 12, ".auth_secret") == 0) {
            continue;  // loaded on demand via LoadAuthProvider
        }
        const std::string id = k.substr(4);
        ProviderState state;
        Json::CharReaderBuilder rb;
        std::string errs;
        std::istringstream ss(v);
        Json::Value entry;
        if (!Json::parseFromStream(rb, ss, &entry, &errs) || !entry.isObject()) {
            std::cerr << "[provider-manager] skipping malformed provider row '"
                      << id << "': " << errs << std::endl;
            continue;
        }
        state.providerId = id;
        state.providerType = entry.get("provider_type", "").asString();
        state.baseUrl = entry.get("base_url", "").asString();
        state.defaultModel = entry.get("default_model", "").asString();
        state.defaultContextWindow = entry.get("default_context_window", 128000).asUInt();
        state.authType = entry.get("auth_type", "api_key").asString();
        state.concurrency = entry.get("concurrency", 1).asInt();
        state.authFile = entry.get("auth_file", "").asString();
        if (entry.isMember("extra") && entry["extra"].isObject()) {
            for (const auto& ek : entry["extra"].getMemberNames()) {
                state.extra[ek] = entry["extra"][ek].asString();
            }
        }
        if (entry.isMember("model_context_windows") && entry["model_context_windows"].isObject()) {
            for (const auto& mk : entry["model_context_windows"].getMemberNames()) {
                state.modelContextWindows[mk] =
                    entry["model_context_windows"][mk].asUInt();
            }
        }
        // Runtime state carries over from the previous in-memory model
        // when the provider survived (health is node-local).
        auto prev = m_providersByName.find(id);
        if (prev != m_providersByName.end()) {
            state.status = prev->second.status;
            state.lastError = prev->second.lastError;
            state.lastTestedUnixMs = prev->second.lastTestedUnixMs;
            state.capabilities = prev->second.capabilities;
        }
        next[id] = std::move(state);
    }
    for (auto& [id, key] : apiKeys) {
        auto it = next.find(id);
        if (it != next.end() && it->second.authType == "api_key") {
            it->second.apiKey = key;   // GetAll already resolved the ref
        }
    }
    m_providersByName = std::move(next);
    m_defaultProvider = rows.count(kDefaultRow) ? rows.at(kDefaultRow) : std::string();
    if (!m_providersByName.empty() &&
        m_providersByName.find(m_defaultProvider) == m_providersByName.end()) {
        m_defaultProvider = m_providersByName.begin()->first;
    }
    return true;
}

std::vector<std::string> ProviderManager::ListProviderIdsForAuthImport() const {
    std::vector<std::string> ids;
    std::lock_guard<std::mutex> lock(m_providerMutex);
    for (const auto& [id, state] : m_providersByName) {
        if (state.authType != "api_key") ids.push_back(id);
    }
    return ids;
}

bool ProviderManager::SaveProviders(std::string* error) const {
    if (!m_configStore) return SaveToDisk(error);
    std::lock_guard<std::mutex> lock(m_providerMutex);

    std::vector<std::string> liveRowPrefixes;  // "cfg.<id>" per provider
    for (const auto& [id, state] : m_providersByName) {
        Json::Value entry(Json::objectValue);
        entry["provider_type"] = state.providerType;
        entry["base_url"] = state.baseUrl;
        entry["default_model"] = state.defaultModel;
        entry["default_context_window"] = static_cast<Json::UInt>(state.defaultContextWindow);
        entry["auth_type"] = state.authType;
        entry["concurrency"] = state.concurrency;
        if (!state.authFile.empty()) entry["auth_file"] = state.authFile;
        if (!state.extra.empty()) {
            Json::Value extra(Json::objectValue);
            for (const auto& [k, v] : state.extra) extra[k] = v;
            entry["extra"] = extra;
        }
        if (!state.modelContextWindows.empty()) {
            Json::Value mcw(Json::objectValue);
            for (const auto& [m, w] : state.modelContextWindows) mcw[m] = static_cast<Json::UInt>(w);
            entry["model_context_windows"] = mcw;
        }
        // NOTE: no api_key here — it lives in its own row so the
        // credential-suffix interception vaults it ("cfg.<id>.api_key").
        Json::StreamWriterBuilder wb;
        wb["indentation"] = "";
        m_configStore->Set(kProviderConfigAgent, CfgRow(id), Json::writeString(wb, entry));

        if (state.authType == "api_key" && !state.apiKey.empty()) {
            m_configStore->Set(kProviderConfigAgent, ApiKeyRow(id), state.apiKey);
        } else {
            m_configStore->Delete(kProviderConfigAgent, ApiKeyRow(id));
        }
        liveRowPrefixes.push_back(CfgRow(id));
    }
    m_configStore->Set(kProviderConfigAgent, kDefaultRow, m_defaultProvider);

    // Reconcile: delete rows of providers no longer in memory (handles
    // DeleteProvider -> SaveProviders, which orphans cfg.<id>* rows).
    for (const auto& k : m_configStore->ListKeys(kProviderConfigAgent)) {
        if (k == kDefaultRow || k.rfind("cfg.", 0) != 0) continue;
        const std::string id = (k.find(".api_key") == std::string::npos &&
                                k.find(".auth_secret") == std::string::npos)
                                   ? k.substr(4)
                                   : std::string();
        if (id.empty()) continue;
        if (m_providersByName.find(id) == m_providersByName.end()) {
            m_configStore->DeleteByPrefix(kProviderConfigAgent, CfgRow(id) + ".");
            m_configStore->Delete(kProviderConfigAgent, CfgRow(id));
        }
    }
    return true;
}

bool ProviderManager::LoadAuthProvider(const std::string& providerId, Json::Value* out, std::string* error) const {
    if (!m_configStore) return LoadAuthFromDisk(providerId, out, error);
    const std::string raw = m_configStore->Get(kProviderConfigAgent, AuthRow(providerId));
    if (raw.empty()) {
        if (error) *error = "no auth stored for provider: " + providerId;
        return false;
    }
    Json::CharReaderBuilder rb;
    std::string errs;
    std::istringstream ss(raw);
    Json::Value stored;
    if (!Json::parseFromStream(rb, ss, &stored, &errs)) {
        if (error) *error = "auth payload malformed for provider " + providerId + ": " + errs;
        return false;
    }
    // The file API is asymmetric: saves take the per-provider object,
    // loads return the whole-file root (callers index by provider id).
    // Rows store the per-provider slice; reassemble the root shape here
    // so every caller is unchanged.
    if (out) {
        *out = Json::Value(Json::objectValue);
        (*out)[providerId] = std::move(stored);
    }
    return true;
}

bool ProviderManager::SaveAuthProvider(const std::string& providerId, const Json::Value& auth, std::string* error) const {
    if (!m_configStore) return SaveAuthToDisk(providerId, auth, error);
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    // ".auth_secret" suffix -> the credential-suffix interception vaults
    // the whole serialized blob (field names like a bare "token" would
    // slip past any per-field heuristic — the suffix makes it structural).
    m_configStore->Set(kProviderConfigAgent, AuthRow(providerId), Json::writeString(wb, auth));
    return true;
}

void ProviderManager::ReloadFromStore() {
    if (!m_configStore) return;
    std::string err;
    if (!LoadProviders(&err)) {
        std::cerr << "[provider-manager] reload from store failed: " << err << std::endl;
    }
}

} // namespace animus::kernel
