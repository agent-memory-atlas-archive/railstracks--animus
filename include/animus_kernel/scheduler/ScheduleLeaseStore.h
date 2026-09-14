#pragma once

#include "animus_kernel/IDataStore.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace animus::kernel {

// ============================================================================
// ScheduleLeaseStore — durable leases with epoch fencing (#78 P2b).
//
// One lease ROW per acquisition: acquire INSERTs (new node-scoped id, new
// epoch); renew UPDATEs expires_ms/last_renew_ms in place (the P1b outbox
// trigger captures the update; row identity is stable). Cross-node the
// "effective" lease for a schedule is the merged view over all rows:
//   winner = max(epoch), then max(expires_ms), then max(id).
// Two rows with the same epoch can only exist after a symmetric takeover
// (two-node partition — the documented residual risk); the merge rule is
// deterministic on every node, and epoch-fenced task_runs surface any
// double dispatch loudly at reconciliation time.
//
// Lease semantics (enforced by Scheduler, not here):
//   - valid to dispatch iff holder == me AND expires_ms > now
//   - renewal requires peer ack (see Scheduler); an unrenewed lease simply
//     runs out — self-fencing. The holder may not re-acquire without a
//     successful peer exchange afterwards (its writes were invisible while
//     partitioned; a survivor may already hold a higher epoch).
//   - a NON-holder may take over once the effective lease is expired
//     beyond grace and the holder peer is known-down: epoch+1, no ack
//     required (time-gated fencing — the kill-A acceptance line).
// ============================================================================

struct ScheduleLease {
    int64_t id{0};
    std::string schedule_id;
    std::string holder_node_id;
    int64_t epoch{0};
    int64_t acquired_ms{0};
    int64_t expires_ms{0};
    int64_t last_renew_ms{0};
};

class ScheduleLeaseStore {
public:
    explicit ScheduleLeaseStore(IDataStore* store);

    void EnsureSchema();

    // Insert a fresh lease row for this schedule (epoch = given). Returns
    // the created lease (id filled). Overwrites nothing — append-only.
    ScheduleLease Acquire(const std::string& scheduleId,
                          const std::string& holderNodeId,
                          int64_t epoch, int64_t nowMs, int64_t ttlMs);

    // Extend MY row (matched by id) to now+ttl. Returns false if the row
    // vanished (wiped/replaced) — caller should treat the lease as lost.
    bool Renew(int64_t leaseId, int64_t nowMs, int64_t ttlMs);

    // Expire a lease immediately (ops force-release). Keeps the epoch as a
    // floor for any future acquisition.
    bool Expire(const std::string& scheduleId);

    // Effective lease for a schedule (merged view over all rows for it).
    std::optional<ScheduleLease> EffectiveLease(const std::string& scheduleId) const;

    // All rows (ops/debug — includes superseded epochs).
    std::vector<ScheduleLease> ListRows(const std::string& scheduleId) const;

private:
    ScheduleLease RowFromStatement(IStatement* stmt) const;

    IDataStore* m_store;
};

} // namespace animus::kernel
