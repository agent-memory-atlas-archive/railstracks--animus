#include "animus_kernel/PeerSyncService.h"
#include "animus_kernel/Log.h"

#include <chrono>
#include <map>
#include <sstream>

namespace animus::kernel {

namespace {

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string TrimTrailingSlash(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

} // namespace

PeerSyncService::PeerSyncService(SyncStore* store,
                                 std::vector<std::string> peerUrls,
                                 std::string syncToken)
    : m_store(store), m_token(std::move(syncToken)) {
    for (auto& u : peerUrls) {
        PeerState p;
        p.url = TrimTrailingSlash(std::move(u));
        m_peers.push_back(std::move(p));
    }
    // Federation peers live on tailscale / wireguard / loopback — the
    // web-tool SSRF guard does not apply to configured infrastructure.
    m_http.SetAllowPrivateAddresses(true);
}

PeerSyncService::~PeerSyncService() { Stop(); }

bool PeerSyncService::Start(std::string* error) {
    if (m_running.exchange(true)) {
        if (error) *error = "already running";
        return false;
    }
    if (!m_store) {
        m_running = false;
        if (error) *error = "null sync store";
        return false;
    }
    try {
        m_thread = std::thread([this] { RunLoop(); });
    } catch (const std::system_error& e) {
        m_running = false;
        if (error) *error = std::string("thread spawn failed: ") + e.what();
        return false;
    }
    ALOG_INFO("sync", "peer sync service started (" << m_peers.size()
              << " peer(s), node " << m_store->LocalNodeId() << ")");
    return true;
}

void PeerSyncService::Stop() {
    if (!m_running.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lk(m_wakeMutex);
    }
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
    ALOG_INFO("sync", "peer sync service stopped");
}

void PeerSyncService::RunLoop() {
    int64_t nextRunMs = 0;
    while (m_running.load()) {
        const int64_t now = NowMs();
        if (now >= nextRunMs) {
            SyncOnce();
            // Re-scan quickly while any peer still has data for us, else
            // fall back to the poll interval.
            nextRunMs = now + static_cast<int64_t>(m_pollIntervalMs);
        }
        std::unique_lock<std::mutex> lk(m_wakeMutex);
        m_wake.wait_for(lk, std::chrono::milliseconds(m_pollIntervalMs),
                        [this] { return !m_running.load(); });
    }
}

bool PeerSyncService::ParseJson(const std::string& body, Json::Value* out) {
    Json::CharReaderBuilder rb;
    std::istringstream stream(body);
    std::string errors;
    if (!Json::parseFromStream(rb, stream, out, &errors)) return false;
    return out->isObject();
}

HttpClient::Response PeerSyncService::Get(const PeerState& p,
                                          const std::string& pathAndQuery) {
    HttpClient::Request req;
    req.method = "GET";
    req.url = p.url + pathAndQuery;
    if (!m_token.empty()) req.headers["Authorization"] = "Bearer " + m_token;
    req.headers["X-Animus-Node"] = std::to_string(m_store->LocalNodeId());
    req.timeout_seconds = 15;
    return m_http.Execute(req);
}

bool PeerSyncService::Handshake(PeerState& p, std::string* error) {
    auto resp = Get(p, "/api/v1/sync/handshake");
    if (resp.status_code != 200) {
        if (error) *error = resp.status_code == 0
            ? ("connection failed: " + (resp.error.empty() ? "no route" : resp.error))
            : ("HTTP " + std::to_string(resp.status_code));
        return false;
    }
    Json::Value body;
    if (!ParseJson(resp.body, &body) || !body.isMember("protocol")
        || !body.isMember("node_id")) {
        if (error) *error = "malformed handshake response";
        return false;
    }
    const int protocol = body["protocol"].asInt();
    if (protocol != kProtocol) {
        if (error) *error = "protocol mismatch (peer=" + std::to_string(protocol)
                          + ", local=" + std::to_string(kProtocol)
                          + ") — deferring sync with this peer";
        p.state = "incompatible";
        return false;
    }
    const uint64_t peerNode = body["node_id"].asUInt64();
    if (peerNode == 0) {
        if (error) *error = "peer reports node id 0 (single-node deployment?)";
        return false;
    }
    if (peerNode == m_store->LocalNodeId()) {
        if (error) *error = "peer reports OUR node id — configuration error "
                            "(two nodes with the same id?)";
        return false;
    }
    // A peer list naming the same node twice would double-pull one outbox;
    // survive it, but flag loudly.
    for (const auto& other : m_peers) {
        if (&other != &p && other.nodeId == peerNode) {
            if (error) *error = "duplicate peer node id " + std::to_string(peerNode)
                              + " (also configured via " + other.url + ")";
            p.state = "incompatible";
            return false;
        }
    }
    p.nodeId = peerNode;

    // Anti-entropy fields (optional: tolerated absent so older peers and
    // minimal fakes stay pullable — the heal check just won't run).
    p.peerMaxOutboxId = -1;
    p.peerDigests.clear();
    if (body.isMember("max_outbox_id") && body["max_outbox_id"].isNumeric()) {
        p.peerMaxOutboxId = body["max_outbox_id"].asInt64();
    }
    if (body.isMember("digests") && body["digests"].isArray()) {
        for (const auto& d : body["digests"]) {
            if (d.isMember("table") && d["table"].isString()
                    && d.isMember("count") && d["count"].isNumeric()) {
                p.peerDigests.emplace_back(d["table"].asString(),
                                           d["count"].asInt64());
            }
        }
    }
    return true;
}

int PeerSyncService::PullFromPeer(PeerState& p, std::string* error) {
    int applied = 0;
    // Bounded batch loop: cap at 50 batches per pass so one huge backlog
    // can't starve the other peers; the next pass continues from the cursor.
    for (int batch = 0; batch < 50; ++batch) {
        std::ostringstream q;
        q << "/api/v1/sync/outbox?since=" << p.cursor
          << "&limit=" << m_batchLimit;
        auto resp = Get(p, q.str());
        if (resp.status_code != 200) {
            if (error) *error = resp.status_code == 0
                ? ("connection failed: " + (resp.error.empty() ? "no route" : resp.error))
                : ("HTTP " + std::to_string(resp.status_code));
            return applied > 0 ? 0 : -1;
        }
        Json::Value body;
        if (!ParseJson(resp.body, &body) || !body.isMember("records")) {
            if (error) *error = "malformed outbox response";
            return applied > 0 ? 0 : -1;
        }
        const Json::Value& records = body["records"];
        if (!records.isArray()) {
            if (error) *error = "outbox records not an array";
            return applied > 0 ? 0 : -1;
        }
        for (Json::ArrayIndex i = 0; i < records.size(); ++i) {
            const Json::Value& r = records[i];
            if (!r.isMember("outbox_id") || !r.isMember("origin_node")
                || !r.isMember("table_name") || !r.isMember("row_id")
                || !r.isMember("op") || !r.isMember("payload")
                || !r.isMember("unix_ms")) {
                if (error) *error = "malformed outbox record";
                continue;  // skip the record, keep the cursor behind it
            }
            OutboxRecord rec;
            rec.outbox_id = r["outbox_id"].asInt64();
            rec.origin_node = r["origin_node"].asInt64();
            rec.table_name = r["table_name"].asString();
            rec.row_id = r["row_id"].asInt64();
            rec.op = r["op"].asString();
            rec.payload = r["payload"].asString();
            rec.unix_ms = r["unix_ms"].asInt64();

            // LWW apply: stale/echo/tie all return false — safe skip.
            // Unknown table (peer newer schema) also returns false; the
            // cursor still advances, which is the defer-don't-block policy.
            if (m_store->ApplyRemoteChange(rec)) applied++;
            p.pulledTotal++;

            p.cursor = rec.outbox_id;
            if (p.nodeId != 0) {
                m_store->SetPeerCursor(static_cast<int64_t>(p.nodeId), p.cursor);
            }
        }
        const bool hasMore = body.get("has_more", false).asBool();
        if (!hasMore || records.size() == 0) break;
    }
    return applied;
}

int PeerSyncService::SyncOnce() {
    int totalApplied = 0;
    for (auto& p : m_peers) {
        std::string error;
        bool ok = true;

        // Handshake runs every pass (not just first connect): one cheap
        // GET that refreshes the peer's anti-entropy digests and catches
        // protocol/config flips without a restart.
        const bool freshLearn = (p.nodeId == 0);
        ok = Handshake(p, &error);
        if (ok && freshLearn && p.nodeId != 0) {
            // Adopt any persisted cursor from a previous run (a wiped or
            // first-time node persists nothing and starts at 0 = the
            // initial full sync).
            p.cursor = m_store->GetPeerCursor(static_cast<int64_t>(p.nodeId));
        }

        int applied = 0;
        if (ok) {
            applied = PullFromPeer(p, &error);
            if (applied < 0) ok = false;
        }

        if (ok) {
            p.state = "healthy";
            p.consecutiveFailures = 0;
            p.lastContactOkMs = NowMs();
            p.lastError.clear();
            totalApplied += applied;
            p.appliedTotal += static_cast<uint64_t>(applied > 0 ? applied : 0);

            // Anti-entropy heal (P1d): fully consumed the peer's outbox,
            // yet its handshake digests still report rows we don't have?
            // The outbox can't explain the gap - classic case is a datadir
            // restored from a partial backup holding a stale cursor past
            // rows that vanished locally. Cure: replay from cursor 0.
            // Every record applies idempotently under LWW, so a full
            // replay is always safe; one reset heals every table.
            if (!p.peerDigests.empty() && p.cursor > 0
                    && p.peerMaxOutboxId >= 0
                    && p.cursor >= p.peerMaxOutboxId) {
                auto ours = m_store->TableDigests();
                std::map<std::string, int64_t> oursByName;
                for (const auto& d : ours) oursByName.emplace(d.table, d.count);
                std::vector<std::string> shortTables;
                for (const auto& [table, peerCount] : p.peerDigests) {
                    auto it = oursByName.find(table);
                    if (it != oursByName.end() && it->second < peerCount) {
                        shortTables.push_back(table);
                    }
                }
                if (!shortTables.empty()) {
                    // The version stamps for these tables outlived their
                    // rows and would tie-skip the replay forever - forget
                    // them so the replay can re-apply (rows that still
                    // exist just get re-stamped identically).
                    std::string names;
                    for (const auto& t : shortTables) {
                        m_store->ClearTableVersions(t);
                        names += (names.empty() ? "" : ", ") + t;
                    }
                    ALOG_WARNING("sync", "anti-entropy: tables short on "
                        "rows (" << names << ") with outbox fully consumed"
                        << " - clearing stale version stamps and replaying"
                        << " from cursor 0");
                    p.cursor = 0;
                    m_store->SetPeerCursor(
                        static_cast<int64_t>(p.nodeId), 0);
                    p.healTotal++;
                    p.lastHealMs = NowMs();
                }
            }
        } else {
            p.consecutiveFailures++;
            p.lastError = error;
            // "incompatible" is sticky — no point retrying until config
            // changes; anything else degrades to "down" after a streak.
            if (p.state != "incompatible"
                && p.consecutiveFailures >= m_failuresBeforeDown) {
                p.state = "down";
            }
        }
    }
    return totalApplied;
}

void PeerSyncService::RecordIncomingPull(uint64_t nodeId, const std::string& remoteUrl) {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& in : m_incoming) {
        if (in.nodeId == nodeId) {
            in.lastPullMs = NowMs();
            in.pulls++;
            if (!remoteUrl.empty()) in.url = remoteUrl;
            return;
        }
    }
    IncomingPull in;
    in.nodeId = nodeId;
    in.url = remoteUrl;
    in.lastPullMs = NowMs();
    in.pulls = 1;
    m_incoming.push_back(std::move(in));
}

Json::Value PeerSyncService::StatusJson() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    Json::Value out(Json::objectValue);
    out["node_id"] = Json::UInt64(m_store ? m_store->LocalNodeId() : 0);
    out["protocol"] = kProtocol;
    out["running"] = m_running.load();

    Json::Value peers(Json::arrayValue);
    for (const auto& p : m_peers) {
        Json::Value j(Json::objectValue);
        j["url"] = p.url;
        j["node_id"] = Json::UInt64(p.nodeId);
        j["state"] = p.state;
        j["cursor"] = Json::Int64(p.cursor);
        j["last_contact_ok_ms"] = Json::Int64(p.lastContactOkMs);
        j["consecutive_failures"] = p.consecutiveFailures;
        j["pulled_total"] = Json::UInt64(p.pulledTotal);
        j["applied_total"] = Json::UInt64(p.appliedTotal);
        j["heal_total"] = Json::UInt64(p.healTotal);
        if (p.lastHealMs > 0) j["last_heal_ms"] = Json::Int64(p.lastHealMs);
        if (!p.lastError.empty()) j["last_error"] = p.lastError;
        peers.append(j);
    }
    out["peers"] = peers;

    Json::Value incoming(Json::arrayValue);
    for (const auto& in : m_incoming) {
        Json::Value j(Json::objectValue);
        j["node_id"] = Json::UInt64(in.nodeId);
        j["url"] = in.url;
        j["last_pull_ms"] = Json::Int64(in.lastPullMs);
        j["pulls"] = Json::UInt64(in.pulls);
        incoming.append(j);
    }
    out["incoming"] = incoming;
    return out;
}

} // namespace animus::kernel
