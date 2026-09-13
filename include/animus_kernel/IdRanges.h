#pragma once

#include "animus_kernel/IDataStore.h"

#include <cstdint>
#include <string>
#include <vector>

namespace animus::kernel {

// ============================================================================
// #78 P1a: node-scoped id ranges for agent-global tables.
//
// In a federated (multi-master) deployment each node seeds its agent-global
// id sequences at nodeId << 40, so ids allocated locally can never collide
// with ids allocated on any other node (node ids are < 2^20; per-node
// capacity is 2^40 rows). Seeding is RAISE-ONLY: a sequence is never moved
// backward, so re-seeding, renumbering onto already-seeded data, or booting
// an old single-node backup against a new node id cannot regress ids into a
// used range.
//
// Node-local tables (sessions, session_turns, prompt_logs, consolidation
// runs, task_runs) are deliberately NOT seeded — their ids never leave the
// node that allocated them.
//
// Call once at boot, AFTER all agent-global stores have run EnsureSchema
// (tables and their sequences must exist). nodeId 0 = single-node default:
// no-op, ids start at 1 exactly as before.
// ============================================================================

// Tables whose integer row ids are agent-global (replicated in a federated
// deployment). memory_layers_v2 is migration scaffolding (renamed away) and
// is excluded; virtual/FTS tables have no sequence.
const std::vector<std::string>& AgentGlobalTables();

// Returns the number of sequences raised, or -1 with *error set on failure
// (nodeId out of range). Missing tables/sequences are skipped with a debug
// log, not treated as errors — the store set varies by install state.
int SeedAgentGlobalIdRanges(IDataStore* store, uint64_t nodeId, std::string* error);

} // namespace animus::kernel
