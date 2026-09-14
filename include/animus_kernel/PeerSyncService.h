#pragma once
// #78 P1c: federation transport — the pull loop.
//
// A background service that keeps this node's agent-global tables converged
// with its configured peers. Pull-based (each node fetches peer outboxes
// past its per-peer cursor and applies through SyncStore's LWW path), so
// there is no inbound push channel to firewall: the only listener is the
// existing admin API.
//
// Heartbeat rides the pull: every successful fetch is bidirectional
// liveness (we learn the peer is up; the peer learns we are up — the
// outbox handler records incoming pulls via RecordIncomingPull, and the
// X-Animus-Node header carries our identity). A dedicated heartbeat frame
// would carry no information the pull doesn't already transport.
//
// Failure model: a peer being down degrades to "no sync with that peer"
// — flagged in status, retried at a slower interval, never fatal. Dead
// peers don't block live ones (each peer is synced independently).
//
// Mixed-version policy (ticket #78): a handshake reporting a different
// protocol major marks the peer "incompatible" and skips it — defer what
// you don't understand rather than guess at payloads.

#include "animus_kernel/SyncStore.h"
#include "animus_kernel/tools/HttpClient.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <json/json.h>

namespace animus::kernel {

class PeerSyncService {
public:
    PeerSyncService(SyncStore* store,
                    std::vector<std::string> peerUrls,
                    std::string syncToken);
    ~PeerSyncService();

    PeerSyncService(const PeerSyncService&) = delete;
    PeerSyncService& operator=(const PeerSyncService&) = delete;

    // Lifecycle
    bool Start(std::string* error);
    void Stop();
    bool IsRunning() const { return m_running.load(); }

    // Tuning (defaults: poll 10s, down-retry 60s, batch 500)
    void SetPollIntervalMs(uint32_t ms) { m_pollIntervalMs = ms; }
    void SetDownRetryIntervalMs(uint32_t ms) { m_downRetryIntervalMs = ms; }
    void SetBatchLimit(uint32_t limit) { m_batchLimit = limit; }
    // Test hook: allow addressing a peer that never completes handshake in
    // N passes before flipping to "down" (default 3).
    void SetFailuresBeforeDown(int n) { m_failuresBeforeDown = n; }

    // One pass over every peer. Returns the number of remote changes
    // applied this pass. Public so tests drive it synchronously and the
    // admin API can expose a manual kick later.
    int SyncOnce();

    // Diagnostics snapshot (thread-safe copy).
    Json::Value StatusJson() const;

    // Called by the admin server when a peer pulls our outbox —
    // the "they are alive" half of the heartbeat.
    void RecordIncomingPull(uint64_t nodeId, const std::string& remoteUrl);

    // ── #78 P2b: lease acknowledgement ────────────────────────────────
    // What a lease-epoch check learned from the peer set.
    struct LeasePeerState {
        int peersConfigured{0};
        bool anyAcked{false};        // a peer's effective lease == (epoch, localNode)
        bool allDown{false};         // every peer is down/incompatible
        int64_t lastExchangeOkMs{0};
    };
    // Queries each reachable peer for its effective lease on a schedule.
    // anyAcked == true proves this node's lease writes are visible on a
    // peer (renewal-safe). Never throws; unreachable peers simply don't
    // ack. Must NOT be called from the sync thread (blocking HTTP).
    LeasePeerState CheckLeaseAck(const std::string& scheduleId, int64_t epoch);

    // #78 P2b two-phase dispatch: peers' SURVIVOR view of a claim window.
    struct ClaimPeerState {
        int peersConfigured{0};
        bool confirmedMine{false};   // a peer's survivor claim row = mine
        bool showsOther{false};      // a peer's survivor claim row = other's
        bool allDown{false};
    };
    ClaimPeerState CheckClaimAck(const std::string& runUuid);

    static constexpr int kProtocol = 1;

private:
    struct PeerState {
        std::string url;
        uint64_t nodeId{0};        // learned from handshake
        std::string state{"connecting"};  // connecting|healthy|down|incompatible
        int64_t lastContactOkMs{0};       // last successful HTTP exchange
        int64_t cursor{0};                // our progress through THEIR outbox
        int consecutiveFailures{0};
        uint64_t appliedTotal{0};
        uint64_t pulledTotal{0};
        std::string lastError;
        // Anti-entropy (P1d): digests as reported by the peer's latest
        // handshake, plus its outbox high-water mark. Empty digests =
        // peer predates them (or a minimal fake peer) — no heal checks.
        int64_t peerMaxOutboxId{-1};
        std::vector<std::pair<std::string, int64_t>> peerDigests;
        uint64_t healTotal{0};
        int64_t lastHealMs{0};
    };

    struct IncomingPull {
        uint64_t nodeId{0};
        std::string url;
        int64_t lastPullMs{0};
        uint64_t pulls{0};
    };

    void RunLoop();
    bool Handshake(PeerState& p, std::string* error);
    int PullFromPeer(PeerState& p, std::string* error);
    HttpClient::Response Get(const PeerState& p, const std::string& pathAndQuery);

    // Parse a JSON body; false on malformed input.
    static bool ParseJson(const std::string& body, Json::Value* out);

    SyncStore* m_store;
    std::string m_token;
    std::vector<PeerState> m_peers;          // worker-owned; status copies under mutex
    std::vector<IncomingPull> m_incoming;    // mutex-guarded
    mutable std::mutex m_mutex;

    HttpClient m_http;  // allow-private: federation peers are on tailscale/wireguard/lan
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::condition_variable m_wake;
    std::mutex m_wakeMutex;

    uint32_t m_pollIntervalMs{10000};
    uint32_t m_downRetryIntervalMs{60000};
    uint32_t m_batchLimit{500};
    int m_failuresBeforeDown{3};
};

} // namespace animus::kernel
