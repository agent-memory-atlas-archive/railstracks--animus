// SharedScriptHealerTests — #143 volume-heal-on-boot
// Exercises ReconcileSharedScripts against temp-dir staging/volume pairs.
// Golden hash pins the md5 implementation to the ops tool (md5sum):
//   echo "hello" | md5sum  ->  b1946ac92492d2347c6235b4d2611184

#include "animus_kernel/lua/SharedScriptHealer.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace animus::kernel;

namespace {

int g_failures = 0;
std::uint64_t g_dirCounter = 0;

void Assert(bool condition, const std::string& msg) {
    if (!condition) {
        std::cerr << "  ASSERT FAILED: " << msg << "\n";
        g_failures++;
    }
}

namespace fs = std::filesystem;

fs::path TempRoot() {
    ++g_dirCounter;
    return fs::temp_directory_path() /
           ("animus_heal_test_" + std::to_string(g_dirCounter));
}

void WriteFile(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << content;
}

std::string ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Stages a file under <root>/staging/_shared/
void Stage(fs::path root, const std::string& name, const std::string& content) {
    WriteFile(root / "staging" / "_shared" / name, content);
}

// Places a file under <root>/volume/_shared/
void Vol(fs::path root, const std::string& name, const std::string& content) {
    WriteFile(root / "volume" / "_shared" / name, content);
}

SharedScriptHealReport Run(fs::path root) {
    return ReconcileSharedScripts((root / "staging").string(),
                                  (root / "volume").string());
}

const ScriptHealOutcome* Find(const SharedScriptHealReport& r,
                              const std::string& name) {
    for (const auto& o : r.outcomes) {
        if (o.filename == name) return &o;
    }
    return nullptr;
}

constexpr const char* kGoldenHello = "b1946ac92492d2347c6235b4d2611184";

} // anonymous namespace

// ============================================================================
void TestStagingAbsentNoop() {
    std::cerr << "  TestStagingAbsentNoop...";
    auto root = TempRoot();
    Vol(root, "keep.lua", "volume content\n");
    fs::remove_all(root / "staging");  // staging entirely absent

    auto r = Run(root);
    Assert(!r.staging_present, "staging_present should be false");
    Assert(r.outcomes.empty(), "no outcomes expected on absent staging");
    Assert(ReadFile(root / "volume" / "_shared" / "keep.lua") == "volume content\n",
           "volume file must be untouched");
    fs::remove_all(root);
    std::cerr << " done\n";
}

// ============================================================================
void TestSeedsAbsentFile() {
    std::cerr << "  TestSeedsAbsentFile...";
    auto root = TempRoot();
    Stage(root, "hello.lua", "hello\n");
    fs::create_directories(root / "volume" / "_shared");  // empty volume _shared

    auto r = Run(root);
    Assert(r.staging_present, "staging_present should be true");
    const auto* o = Find(r, "hello.lua");
    Assert(o != nullptr, "hello.lua should have an outcome");
    Assert(o->action == ScriptHealOutcome::Action::Seeded,
           "absent file should be Seeded");
    Assert(o->old_hash.empty(), "Seeded old_hash should be empty");
    Assert(o->new_hash == kGoldenHello,
           "Seeded new_hash should be md5('hello\\n') = " + std::string(kGoldenHello) +
               ", got: " + o->new_hash);
    Assert(ReadFile(root / "volume" / "_shared" / "hello.lua") == "hello\n",
           "seeded content should match staging");
    fs::remove_all(root);
    std::cerr << " done\n";
}

// ============================================================================
void TestHealsDifferingFile() {
    std::cerr << "  TestHealsDifferingFile...";
    auto root = TempRoot();
    Stage(root, "hello.lua", "hello\n");
    Vol(root, "hello.lua", "stale\n");

    auto r = Run(root);
    const auto* o = Find(r, "hello.lua");
    Assert(o != nullptr, "hello.lua should have an outcome");
    Assert(o->action == ScriptHealOutcome::Action::Healed,
           "differing file should be Healed");
    Assert(!o->old_hash.empty() && o->old_hash != o->new_hash,
           "Healed old/new hashes should differ");
    Assert(o->new_hash == kGoldenHello, "new_hash should be the staged md5");
    Assert(ReadFile(root / "volume" / "_shared" / "hello.lua") == "hello\n",
           "volume content should now match staging");
    fs::remove_all(root);
    std::cerr << " done\n";
}

// ============================================================================
void TestEqualUntouched() {
    std::cerr << "  TestEqualUntouched...";
    auto root = TempRoot();
    Stage(root, "hello.lua", "hello\n");
    Vol(root, "hello.lua", "hello\n");

    auto r = Run(root);
    const auto* o = Find(r, "hello.lua");
    Assert(o != nullptr, "hello.lua should have an outcome");
    Assert(o->action == ScriptHealOutcome::Action::Equal,
           "identical file should be Equal");
    Assert(o->old_hash == kGoldenHello && o->new_hash == kGoldenHello,
           "Equal hashes should both be the content md5");
    Assert(ReadFile(root / "volume" / "_shared" / "hello.lua") == "hello\n",
           "content unchanged");
    fs::remove_all(root);
    std::cerr << " done\n";
}

// ============================================================================
void TestPinnedNotHealed() {
    std::cerr << "  TestPinnedNotHealed...";
    auto root = TempRoot();
    Stage(root, "hello.lua", "hello\n");
    Vol(root, "hello.lua", "admin edit\n");
    WriteFile(root / "volume" / "_shared" / ".pin-hello", "");  // pin marker

    auto r = Run(root);
    const auto* o = Find(r, "hello.lua");
    Assert(o != nullptr, "hello.lua should have an outcome");
    Assert(o->action == ScriptHealOutcome::Action::Pinned,
           "pinned file should be Pinned");
    Assert(o->new_hash.empty(), "Pinned new_hash should be empty");
    Assert(!o->old_hash.empty(), "Pinned old_hash should record current md5");
    Assert(ReadFile(root / "volume" / "_shared" / "hello.lua") == "admin edit\n",
           "pinned content must NOT be overwritten");
    fs::remove_all(root);
    std::cerr << " done\n";
}

// ============================================================================
void TestPinBlocksSeeding() {
    std::cerr << "  TestPinBlocksSeeding...";
    auto root = TempRoot();
    Stage(root, "hello.lua", "hello\n");
    // Pin present, but NO volume copy — pin wins over seeding too
    WriteFile(root / "volume" / "_shared" / ".pin-hello", "");

    auto r = Run(root);
    const auto* o = Find(r, "hello.lua");
    Assert(o != nullptr, "hello.lua should have an outcome");
    Assert(o->action == ScriptHealOutcome::Action::Pinned,
           "pin should block seeding");
    Assert(!fs::exists(root / "volume" / "_shared" / "hello.lua"),
           "pinned-absent file must NOT be seeded");
    fs::remove_all(root);
    std::cerr << " done\n";
}

// ============================================================================
void TestOrphanLeftInPlace() {
    std::cerr << "  TestOrphanLeftInPlace...";
    auto root = TempRoot();
    Stage(root, "hello.lua", "hello\n");
    Vol(root, "hello.lua", "hello\n");
    Vol(root, "legacy.lua", "retired adapter\n");  // not in image

    auto r = Run(root);
    const auto* o = Find(r, "legacy.lua");
    Assert(o != nullptr, "legacy.lua should have an outcome");
    Assert(o->action == ScriptHealOutcome::Action::Orphaned,
           "volume-only file should be Orphaned");
    Assert(o->new_hash.empty(), "Orphaned new_hash should be empty");
    Assert(!o->old_hash.empty(), "Orphaned old_hash should record current md5");
    Assert(ReadFile(root / "volume" / "_shared" / "legacy.lua") == "retired adapter\n",
           "orphan must be LEFT IN PLACE (log-only policy)");
    fs::remove_all(root);
    std::cerr << " done\n";
}

// ============================================================================
void TestFreshVolumeSeeded() {
    std::cerr << "  TestFreshVolumeSeeded...";
    auto root = TempRoot();
    Stage(root, "hello.lua", "hello\n");
    Stage(root, "other.lua", "other\n");
    // volume/ does not exist at all

    auto r = Run(root);
    Assert(r.staging_present, "staging_present should be true");
    Assert(Find(r, "hello.lua") != nullptr &&
               Find(r, "hello.lua")->action == ScriptHealOutcome::Action::Seeded,
           "hello.lua seeded on fresh volume");
    Assert(Find(r, "other.lua") != nullptr &&
               Find(r, "other.lua")->action == ScriptHealOutcome::Action::Seeded,
           "other.lua seeded on fresh volume");
    Assert(fs::exists(root / "volume" / "_shared" / "hello.lua") &&
               fs::exists(root / "volume" / "_shared" / "other.lua"),
           "seeded files must exist on volume");
    fs::remove_all(root);
    std::cerr << " done\n";
}

// ============================================================================
void TestAgentDirsUntouched() {
    std::cerr << "  TestAgentDirsUntouched...";
    auto root = TempRoot();
    Stage(root, "hello.lua", "hello\n");
    // Same-named file in an agent dir, differing content — heal must ignore
    WriteFile(root / "staging" / "agent1" / "hello.lua", "agent staging\n");
    WriteFile(root / "volume" / "agent1" / "hello.lua", "agent volume\n");

    auto r = Run(root);
    for (const auto& o : r.outcomes) {
        Assert(o.filename == "hello.lua",
               "only _shared/hello.lua expected in outcomes, got: " + o.filename);
    }
    Assert(ReadFile(root / "volume" / "agent1" / "hello.lua") == "agent volume\n",
           "agent-specific dirs are deliberate state — never touched");
    fs::remove_all(root);
    std::cerr << " done\n";
}

// ============================================================================
void TestMixedOutcomes() {
    std::cerr << "  TestMixedOutcomes...";
    auto root = TempRoot();
    Stage(root, "seedme.lua", "fresh\n");
    Stage(root, "healme.lua", "new content\n");
    Stage(root, "sameme.lua", "same\n");
    Vol(root, "healme.lua", "old content\n");
    Vol(root, "sameme.lua", "same\n");
    Vol(root, "orphan.lua", "orphaned\n");

    auto r = Run(root);
    int seeded = 0, healed = 0, equal = 0, pinned = 0, orphaned = 0;
    for (const auto& o : r.outcomes) {
        switch (o.action) {
            case ScriptHealOutcome::Action::Seeded: ++seeded; break;
            case ScriptHealOutcome::Action::Healed: ++healed; break;
            case ScriptHealOutcome::Action::Equal: ++equal; break;
            case ScriptHealOutcome::Action::Pinned: ++pinned; break;
            case ScriptHealOutcome::Action::Orphaned: ++orphaned; break;
        }
    }
    Assert(seeded == 1, "expected 1 Seeded");
    Assert(healed == 1, "expected 1 Healed");
    Assert(equal == 1, "expected 1 Equal");
    Assert(pinned == 0, "expected 0 Pinned");
    Assert(orphaned == 1, "expected 1 Orphaned");
    fs::remove_all(root);
    std::cerr << " done\n";
}

// ============================================================================
int main() {
    std::cerr << "SharedScriptHealerTests:\n";

    TestStagingAbsentNoop();
    TestSeedsAbsentFile();
    TestHealsDifferingFile();
    TestEqualUntouched();
    TestPinnedNotHealed();
    TestPinBlocksSeeding();
    TestOrphanLeftInPlace();
    TestFreshVolumeSeeded();
    TestAgentDirsUntouched();
    TestMixedOutcomes();

    if (g_failures > 0) {
        std::cerr << "\nFAILED: " << g_failures << " assertions failed\n";
        return 1;
    }

    std::cerr << "\nAll SharedScriptHealer tests passed!\n";
    return 0;
}
