#pragma once

#include "animus_kernel/scheduler/ScheduleStore.h"
#include "animus_kernel/scheduler/TaskRunStore.h"
#include "animus_kernel/scheduler/ScheduleLeaseStore.h"
#include "animus_kernel/IncomingEvent.h"

#include <json/json.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace animus::kernel {

// ============================================================================
// Scheduler — polling loop that fires due schedules as IncomingEvent objects
// ============================================================================

class Scheduler {
public:
    // Returns a short status token recorded in task_runs
    // ("dispatched:...", "skipped:...", "error:...").
    using FireCallback = std::function<std::string(const IncomingEvent& event)>;

    explicit Scheduler(IDataStore* dataStore);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Lifecycle
    bool Start(std::string* error);
    void Stop();
    bool IsRunning() const { return m_running.load(); }

    // Schedule management (delegates to ScheduleStore)
    std::string Create(const ScheduleDescriptor& sched, std::string* error);
    std::optional<ScheduleDescriptor> Get(const std::string& id) const;
    std::vector<ScheduleDescriptor> List(
        const std::string& agentId, const std::string& tag = "") const;
    bool Cancel(const std::string& id, std::string* error);
    bool Update(const ScheduleDescriptor& sched, std::string* error);

    // ── #78 P2b: leases with epoch fencing ─────────────────────────────
    // What the peer layer reports about a lease epoch check.
    struct LeasePeerState {
        int peersConfigured{0};     // configured peer count
        bool anyAcked{false};       // >=1 peer's effective lease == (epoch, me)
        bool allDown{false};        // every peer is state down/incompatible
        int64_t lastExchangeOkMs{0};// last ok HTTP exchange with any peer
    };
    // Wired by AgentKernel to PeerSyncService. Called at most once per
    // lease attempt per schedule (throttled).
    using LeaseAckFn = std::function<LeasePeerState(
        const std::string& scheduleId, int64_t epoch)>;
    void SetLeaseAckFn(LeaseAckFn fn) { m_leaseAckFn = std::move(fn); }

    // Claim visibility check (#78 P2b two-phase dispatch): what peers say
    // about a run_uuid's SURVIVOR claim row.
    struct ClaimPeerState {
        int peersConfigured{0};
        bool confirmedMine{false};   // a peer's survivor claim = mine
        bool showsOther{false};      // a peer's survivor claim = other's
        bool allDown{false};
    };
    using ClaimAckFn = std::function<ClaimPeerState(const std::string& runUuid)>;
    void SetClaimAckFn(ClaimAckFn fn) { m_claimAckFn = std::move(fn); }
    // How long a fresh claim must age before dispatch (replication round).
    void SetClaimVisibilityMs(int64_t ms) { m_claimVisibilityMs = ms; }

    void SetLeaseTtlMs(int64_t ms) {
        m_leaseTtlMs = ms;
        m_leaseRenewEveryMs = ms > 3 ? ms / 3 : 1;
    }
    void SetLeaseGraceMs(int64_t ms) { m_leaseGraceMs = ms; }

    // Ops: force-release a schedule's lease (expires it; epoch floor stays).
    bool ReleaseLease(const std::string& scheduleId);
    // Ops/status: effective leases for all schedules with any lease rows.
    Json::Value LeaseStatusJson() const;

    // Configuration
    void SetPollIntervalMs(uint32_t ms);
    // #78 P2a: identity recorded on every task_run claim row (who ran it).
    // Empty = single-node/legacy (stays '').
    void SetNodeId(const std::string& id) { m_nodeId = id; }
    void SetMaxSchedulesPerAgent(uint32_t limit);
    void SetFireCallback(FireCallback cb);

    // Access to stores (for tests / admin routes)
    ScheduleStore& Store() { return m_store; }
    TaskRunStore& RunStore() { return m_runStore; }
    ScheduleLeaseStore& LeaseStore() { return m_leaseStore; }

    // Utility: current time as ISO-8601 string (UTC).
    static std::string IsoNow();

private:
    void PollLoop();

public:
    // Single due-batch pass (tests drive it deterministically; the loop
    // calls it every poll interval).
    void ProcessDueSchedules();

    // #78 P2b: lease gate for lease_required schedules. Returns the epoch
    // to claim under, or nullopt = paused (no valid lease; fail-safe is
    // NOT running). Acquires/renews/takes over per the lease rules.
    std::optional<int64_t> LeaseEpochForDispatch(const ScheduleDescriptor& sched);

    // Time helpers
    static std::string ComputeNextFire(
        const std::string& cronExpr,
        const std::string& timezone,
        const std::string& lastFireIso);

    // Cron field parsing
    static bool CronFieldMatches(int value, const std::string& field, int min, int max);
    static bool CronMatches(const std::string& expr, int minute,
                             int hour, int day, int month, int year,
                             int wday);

    ScheduleStore m_store;
    TaskRunStore m_runStore;
    ScheduleLeaseStore m_leaseStore;
    LeaseAckFn m_leaseAckFn;
    ClaimAckFn m_claimAckFn;
    int64_t m_claimVisibilityMs{25000};   // ~2 sync rounds
    int64_t m_leaseTtlMs{60000};
    int64_t m_leaseRenewEveryMs{20000};
    int64_t m_leaseGraceMs{30000};
    int64_t m_leaseRetryEveryMs{3000};
    std::unordered_map<std::string, int64_t> m_leaseAttemptMs;
    std::unordered_map<std::string, int64_t> m_pauseLogMs;
    FireCallback m_fireCallback;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    std::mutex m_configMutex;
    uint32_t m_pollIntervalMs{30000};
    uint32_t m_maxSchedulesPerAgent{50};
    std::string m_nodeId;
};

} // namespace animus::kernel