#include "animus_kernel/SyncStore.h"
#include "animus_kernel/IdRanges.h"
#include "animus_kernel/SqliteDataStore.h"
#include "animus_kernel/MemoryStore.h"
#include "animus_kernel/MemoryFileStore.h"
#include "animus_kernel/OntologyStore.h"
#include "animus_kernel/admin/DiaryManager.h"
#include "animus_kernel/IDataStore.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

using namespace animus::kernel;

namespace {

int g_failures = 0;

void Assert(bool condition, const std::string& msg) {
    if (!condition) {
        std::cerr << "  ASSERT FAILED: " << msg << "\n";
        g_failures++;
    }
}

std::string MakeTempDbPath() {
    char tmp[] = "/tmp/animus_sync_test_XXXXXX";
    mktemp(tmp);
    return std::string(tmp) + ".db";
}

// A full agent-global store set + sync store = one "node".
struct Node {
    std::string dbPath;
    SqliteDataStore dataStore;
    memory::MemoryStore memory;
    memory::MemoryFileStore files;
    ontology::OntologyStore ontology;
    DiaryStore diary;
    SyncStore sync;

    Node(uint64_t nodeId)
        : dbPath(MakeTempDbPath()), dataStore(dbPath), memory(&dataStore),
          files(&dataStore), ontology(&dataStore), diary(&dataStore),
          sync(&dataStore, nodeId) {
        std::string err;
        if (!sync.EnsureSchema(&err)) {
            std::cerr << "  FATAL: node " << nodeId << " sync init: " << err << "\n";
        }
    }
    ~Node() { std::filesystem::remove(dbPath); }

    // Pull everything new from `from` and apply it here. Returns applied count.
    int PullFrom(Node& from) {
        const int64_t cursor = sync.GetPeerCursor(from.sync.LocalNodeId());
        auto records = from.sync.FetchOutboxSince(cursor, 1000);
        int applied = 0;
        for (const auto& r : records) {
            if (sync.ApplyRemoteChange(r)) applied++;
        }
        sync.SetPeerCursor(from.sync.LocalNodeId(), from.sync.MaxOutboxId());
        return applied;
    }

    int64_t CountRows(const std::string& table) {
        auto q = dataStore.Prepare("SELECT COUNT(*) FROM " + table);
        if (!q || !q->Step()) return -1;
        return q->ColumnInt64(0);
    }

    bool HasRow(const std::string& table, int64_t id) {
        auto q = dataStore.Prepare(
            "SELECT 1 FROM " + table + " WHERE id = ?");
        if (!q) return false;
        q->BindInt64(1, id);
        return q->Step();
    }

    std::string TextOf(const std::string& table, int64_t id, const std::string& col) {
        auto q = dataStore.Prepare(
            "SELECT " + col + " FROM " + table + " WHERE id = ?");
        if (!q) return "<no-prepare>";
        q->BindInt64(1, id);
        if (!q->Step()) return "<no-row>";
        return q->ColumnText(0);
    }
};

} // namespace

// Writes land in the outbox via triggers — every agent-global table, both ops.
int TestTriggerCoverage() {
    std::cerr << "  [P1b] trigger coverage (all 10 tables + delete)...\n";
    Node a(1);

    memory::Observation o;
    a.memory.CreateDefaultLayersForAgent("ag");
    auto layers = a.memory.ListLayersForAgent("ag");
    Assert(!layers.empty(), "layers created");
    o.layer_id = layers[0].id;
    o.agent_id = "ag";
    o.text = "coverage probe";
    auto obs = a.memory.CreateObservationForAgent("ag", o);
    Assert(obs.id > 0, "observation created");

    ontology::OntologyEntity e;
    e.name = "Cov";
    e.full_path = "projects/Cov";
    e.root_category = ontology::RootCategory::Projects;
    e.agent_id = "ag";
    a.ontology.CreateEntity(e, "coverage");

    DiaryEntry de;
    de.id = "cov-diary";
    de.agent_id = "ag";
    de.content = "coverage";
    de.timestamp_unix_ms = 42;
    a.diary.Create(de);

    // Direct minimal rows for the remaining tables (payload-heavy stores).
    // (layer_perspectives rows come from CreateDefaultLayersForAgent itself)
    a.dataStore.Exec("INSERT INTO memory_mutations (mutation_type, target_type, target_id, unix_ms) "
                     "VALUES ('m', 't', 1, 1)");
    a.dataStore.Exec("INSERT INTO memory_files (source_path, file_type, created_at_unix_ms, imported_at_unix_ms) "
                     "VALUES ('/x', 1, 1, 1)");
    a.dataStore.Exec("INSERT INTO memory_file_chunks (file_id, source_path, content, created_at_unix_ms) "
                     "VALUES (1, '/x', 'c', 1)");
    a.dataStore.Exec("INSERT INTO ontology_properties (entity_id, key, created_at_unix_ms, updated_at_unix_ms) "
                     "VALUES (1, 'k', 1, 1)");
    a.dataStore.Exec("INSERT INTO ontology_mutations (mutation_type, target_type, target_id, unix_ms) "
                     "VALUES ('m', 't', 1, 1)");

    auto records = a.sync.FetchOutboxSince(0, 1000);
    Assert(records.size() >= 10, "10+ outbox records, got " +
           std::to_string(records.size()));
    // distinct agent-global tables captured
    bool seen[10] = {false};
    const auto tables = AgentGlobalTables();
    for (const auto& r : records) {
        for (size_t i = 0; i < tables.size(); ++i)
            if (r.table_name == tables[i]) seen[i] = true;
    }
    for (size_t i = 0; i < tables.size(); ++i)
        Assert(seen[i], "outbox captured " + tables[i]);
    Assert(records[0].origin_node == 1, "origin = local node");
    for (const auto& r : records)
        Assert(r.unix_ms > 0 && r.payload.size() > 2,
               "record has ms + payload: " + r.table_name);

    // UPDATE + DELETE fire too
    a.dataStore.Exec("UPDATE memory_mutations SET motivation = 'x' WHERE id = 1");
    a.dataStore.Exec("DELETE FROM memory_mutations WHERE id = 1");
    auto after = a.sync.FetchOutboxSince(records.back().outbox_id, 100);
    Assert(after.size() == 2, "update+delete recorded, got " +
           std::to_string(after.size()));
    Assert(after.size() == 2 && after[1].op == "delete", "delete op recorded");
    return 0;
}

// Full A→B replication, echo death at A, then convergence on update + delete.
int TestTwoNodeReplication() {
    std::cerr << "  [P1b] two-node replication + echo death...\n";
    Node a(1), b(2);

    // A writes a layer + observation
    a.memory.CreateDefaultLayersForAgent("ag");
    auto layers = a.memory.ListLayersForAgent("ag");
    memory::Observation o;
    o.layer_id = layers[0].id;
    o.agent_id = "ag";
    o.text = "v1";
    auto obs = a.memory.CreateObservationForAgent("ag", o);

    // B pulls A
    int applied = b.PullFrom(a);
    Assert(applied >= 2, "B applied layer+observation, got " +
           std::to_string(applied));
    Assert(b.HasRow("memory_layers", layers[0].id), "layer replicated");
    Assert(b.HasRow("observations", obs.id), "observation replicated");
    Assert(b.TextOf("observations", obs.id, "text") == "v1", "text intact");

    // B's outbox now holds ECHOES stamped with A's origin (not B's).
    auto bEchoes = b.sync.FetchOutboxSince(0, 1000);
    Assert(!bEchoes.empty(), "B produced echo records");
    Assert(bEchoes[0].origin_node == 1,
           "echo stamped with ORIGIN node 1, got " +
           std::to_string(bEchoes[0].origin_node));

    // ECHO DEATH: A pulls B's echoes back — all skipped, A's outbox unchanged.
    const int64_t aBefore = a.sync.MaxOutboxId();
    const int aApplied = a.PullFrom(b);
    Assert(aApplied == 0, "echoes all skipped at origin, applied " +
           std::to_string(aApplied));
    Assert(a.sync.MaxOutboxId() == aBefore,
           "A's outbox unchanged after echo round-trip");

    // UPDATE on A → B applies the newer version
    a.dataStore.Exec("UPDATE observations SET text = 'v2' WHERE id = " +
                     std::to_string(obs.id));
    applied = b.PullFrom(a);
    Assert(applied == 1, "B applied the update, got " + std::to_string(applied));
    Assert(b.TextOf("observations", obs.id, "text") == "v2", "v2 replicated");

    // Stale record rejected: pair-lesser (older ms, same origin)
    OutboxRecord stale;
    stale.table_name = "observations";
    stale.row_id = obs.id;
    stale.op = "upsert";
    stale.origin_node = 1;
    stale.unix_ms = 1;   // ancient
    stale.payload = R"({"id":)" + std::to_string(obs.id) + R"(,"text":"ancient","layer_id":)" +
                    std::to_string(layers[0].id) + R"(,"agent_id":"ag"})";
    Assert(!b.sync.ApplyRemoteChange(stale), "stale rejected");
    Assert(b.TextOf("observations", obs.id, "text") == "v2", "v2 survived stale");

    // DELETE on A → B removes the row
    a.dataStore.Exec("DELETE FROM observations WHERE id = " + std::to_string(obs.id));
    applied = b.PullFrom(a);
    Assert(applied == 1, "B applied the delete, got " + std::to_string(applied));
    Assert(!b.HasRow("observations", obs.id), "delete replicated");

    // B's delete-echo dies at A too
    Assert(a.PullFrom(b) == 0, "delete echo skipped at origin");
    return 0;
}

// LWW pair semantics: (ms, node) tie-break, strictly-greater-only.
int TestLwwPairSemantics() {
    std::cerr << "  [P1b] LWW (ms, node) pair semantics...\n";
    Node a(1), b(2);

    a.memory.CreateDefaultLayersForAgent("ag");
    auto layers = a.memory.ListLayersForAgent("ag");
    memory::Observation o;
    o.layer_id = layers[0].id;
    o.agent_id = "ag";
    o.text = "node1-v1";
    auto obs = a.memory.CreateObservationForAgent("ag", o);
    b.PullFrom(a);
    Assert(b.HasRow("observations", obs.id), "seeded");

    // Craft from the REAL payload (full row — NOT NULL columns intact).
    auto current = a.sync.FetchOutboxSince(0, 1000);
    int64_t writeMs = 0; std::string realPayload;
    for (const auto& r : current)
        if (r.table_name == "observations" && r.row_id == obs.id) {
            writeMs = r.unix_ms; realPayload = r.payload;
        }
    Assert(writeMs > 0, "found write ms");
    Assert(!realPayload.empty(), "captured real payload");
    // tweak only the text value inside the JSON
    const std::string marker = "\"text\":\"node1-v1\"";
    const size_t mpos = realPayload.find(marker);
    Assert(mpos != std::string::npos, "payload carries text field");
    const std::string pay = realPayload.substr(0, mpos) +
                            "\"text\":\"X\"" +
                            realPayload.substr(mpos + marker.size());

    // Equal ms, HIGHER node: version at b is (ms_of_write, 1);
    // send (same ms, node 2) → pair-greater → wins.
    OutboxRecord tie;
    tie.table_name = "observations"; tie.row_id = obs.id; tie.op = "upsert";
    tie.origin_node = 2; tie.unix_ms = writeMs; tie.payload = pay;
    Assert(b.sync.ApplyRemoteChange(tie), "tie broken by higher node id");
    Assert(b.TextOf("observations", obs.id, "text") == "X", "tie-winner applied");

    // Exact equal pair (same ms, same node) → skipped
    OutboxRecord echo = tie;   // exact same pair + payload value
    Assert(!b.sync.ApplyRemoteChange(echo), "exact equal pair skipped");
    Assert(b.TextOf("observations", obs.id, "text") == "X", "echo did not apply");

    // Unknown table / op rejected (identifier whitelist)
    OutboxRecord evil;
    evil.table_name = "sync_outbox; DROP TABLE observations;--";
    evil.row_id = 1; evil.op = "upsert"; evil.origin_node = 9;
    evil.unix_ms = 99999999999999; evil.payload = "{}";
    Assert(!b.sync.ApplyRemoteChange(evil), "unknown table rejected");
    OutboxRecord badOp = tie;
    badOp.op = "truncate";
    Assert(!b.sync.ApplyRemoteChange(badOp), "unknown op rejected");
    return 0;
}

// Concurrency safety: local writes during a "sync batch" still hit the outbox.
int TestLocalWritesDuringApply() {
    std::cerr << "  [P1b] local writes during apply stay captured...\n";
    Node a(1), b(2);

    b.memory.CreateDefaultLayersForAgent("ag");
    auto layers = b.memory.ListLayersForAgent("ag");
    memory::Observation o;
    o.layer_id = layers[0].id;
    o.agent_id = "ag";
    o.text = "b-local";
    auto obsB = b.memory.CreateObservationForAgent("ag", o);

    // simulate mid-batch: apply context set, then a local write happens
    OutboxRecord from;
    from.table_name = "diary_entries"; from.row_id = 777; from.op = "delete";
    from.origin_node = 1; from.unix_ms = 1; from.payload = R"({"id":777})";
    // (set apply context the same way ApplyRemoteChange does)
    auto q = b.dataStore.Prepare(
        "UPDATE sync_control SET apply_table = 'diary_entries', apply_row_id = 777, "
        "apply_origin = 1, apply_ms = 2");
    q->ExecDML();

    memory::Observation mid;
    mid.layer_id = layers[0].id;
    mid.agent_id = "ag";
    mid.text = "mid-batch";
    auto obsMid = b.memory.CreateObservationForAgent("ag", mid);
    auto midRec = b.sync.FetchOutboxSince(0, 1000);
    bool midCaptured = false; int64_t midMs = 0;
    for (const auto& r : midRec)
        if (r.table_name == "observations" && r.row_id == obsMid.id) {
            midCaptured = true; midMs = r.unix_ms;
        }
    Assert(midCaptured, "mid-batch local write captured in outbox");
    // mid-batch write is a DIFFERENT row than the applying one -> local stamp
    Assert(midMs >= 2, "mid-batch stamp is real local time");

    // clear apply context (ApplyRemoteChange's finally-path)
    auto q2 = b.dataStore.Prepare(
        "UPDATE sync_control SET apply_table = NULL, apply_row_id = NULL, "
        "apply_origin = NULL, apply_ms = NULL");
    q2->ExecDML();

    // A can pull B's local writes
    Assert(a.HasRow("memory_layers", layers[0].id) == false, "not yet");
    const int applied = a.PullFrom(b);
    Assert(a.HasRow("observations", obsMid.id), "mid-batch write replicated to A");
    (void)obsB; (void)applied;
    return 0;
}

// Cursors: incremental pull returns only new records.
int TestPeerCursors() {
    std::cerr << "  [P1b] peer cursors...\n";
    Node a(1), b(2);
    a.memory.CreateDefaultLayersForAgent("ag");
    const int64_t c0 = b.sync.GetPeerCursor(1);
    Assert(c0 == 0, "cursor starts at 0");

    b.PullFrom(a);   // consumes everything
    const int64_t c1 = b.sync.GetPeerCursor(1);
    Assert(c1 == a.sync.MaxOutboxId(), "cursor = max outbox id");

    memory::Observation o;
    auto layers = a.memory.ListLayersForAgent("ag");
    o.layer_id = layers[0].id; o.agent_id = "ag"; o.text = "second";
    a.memory.CreateObservationForAgent("ag", o);

    // Incremental fetch reads A's outbox (b's cursor into it). The second
    // observation write lands as TWO records — the observation itself plus
    // its memory_mutations audit row (agent-global, captured by design):
    auto fresh = a.sync.FetchOutboxSince(b.sync.GetPeerCursor(1), 1000);
    Assert(fresh.size() == 2, "incremental fetch = 2 new records, got " +
           std::to_string(fresh.size()));
    bool sawObs = false, sawMutation = false;
    for (const auto& r : fresh) {
        if (r.table_name == "observations") sawObs = true;
        if (r.table_name == "memory_mutations") sawMutation = true;
    }
    Assert(sawObs && sawMutation, "observation + its audit row captured");
    return 0;
}

// Idempotent re-boot: EnsureSchema twice, triggers still single-fire.
int TestIdempotentBoot() {
    std::cerr << "  [P1b] idempotent double boot...\n";
    Node a(1);
    std::string err;
    Assert(a.sync.EnsureSchema(&err), "second EnsureSchema ok: " + err);
    a.memory.CreateDefaultLayersForAgent("ag");
    auto recs = a.sync.FetchOutboxSince(0, 1000);
    // default layers = multiple layer rows; each must appear exactly once
    int layerRecords = 0;
    for (const auto& r : recs)
        if (r.table_name == "memory_layers") layerRecords++;
    Assert(layerRecords == (int)a.memory.ListLayersForAgent("ag").size(),
           "no double-triggering after re-boot");
    return 0;
}

int main() {
    std::cerr << "\n=== Outbox Sync Tests (#78 P1b) ===\n\n";
    TestTriggerCoverage();
    TestTwoNodeReplication();
    TestLwwPairSemantics();
    TestLocalWritesDuringApply();
    TestPeerCursors();
    TestIdempotentBoot();
    if (g_failures == 0) std::cerr << "\nAll outbox sync tests passed.\n";
    else std::cerr << "\n" << g_failures << " test assertion(s) FAILED.\n";
    return g_failures == 0 ? 0 : 1;
}
