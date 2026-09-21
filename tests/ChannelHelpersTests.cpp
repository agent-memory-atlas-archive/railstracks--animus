#include "animus_kernel/ChannelHelpers.h"

#include <iostream>
#include <string>
#include <vector>

using namespace animus::kernel;
using channel_detail::SplitForLimit;

namespace {

int g_failures = 0;

void Assert(bool condition, const std::string& msg) {
    if (!condition) {
        std::cerr << "  ASSERT FAILED: " << msg << "\n";
        g_failures++;
    }
}

bool AllWithinLimit(const std::vector<std::string>& chunks, size_t limit) {
    for (const auto& c : chunks) if (c.size() > limit) return false;
    return true;
}

size_t CountFences(const std::string& s) {
    size_t n = 0;
    for (size_t f = s.find("```"); f != std::string::npos; f = s.find("```", f + 3)) ++n;
    return n;
}

std::string Join(const std::vector<std::string>& chunks) {
    std::string out;
    for (const auto& c : chunks) out += c;
    return out;
}

// Reassembly invariant: the only bytes the splitter ADDS are fence-surgery
// markers, and a synthetic close ("\n```", chunk-final) is always immediately
// followed in the join by a synthetic reopen ("```\n", chunk-initial) — the
// exact 8-byte pair "\n``````\n". Removing those pairs from the joined
// chunks must reproduce the original byte-for-byte (nothing dropped, nothing
// invented). Test corpora avoid legitimate empty-line code blocks so the pair
// is unambiguous.
std::string JoinMinusSurgery(const std::vector<std::string>& chunks) {
    std::string out = Join(chunks);
    const std::string pair = "\n``````\n";
    for (size_t f = out.find(pair); f != std::string::npos; f = out.find(pair, f)) {
        out.erase(f, pair.size());
    }
    return out;
}

std::string Paragraphs(int count, int paraSize) {
    std::string out;
    for (int p = 0; p < count; ++p) {
        for (int i = 0; i < paraSize; ++i) out += "word ";
        out += "\n\n";
    }
    return out;
}

} // anonymous namespace

// ============================================================================
// Short text passes through untouched
// ============================================================================
void TestPassthrough() {
    std::cerr << "  TestPassthrough...";
    auto r = SplitForLimit("hello world", 2000);
    Assert(r.size() == 1 && r[0] == "hello world", "short text must pass through");
    std::cerr << " done\n";
}

// ============================================================================
// Exact-limit text is a single chunk
// ============================================================================
void TestExactLimit() {
    std::cerr << "  TestExactLimit...";
    std::string text(2000, 'a');
    auto r = SplitForLimit(text, 2000);
    Assert(r.size() == 1, "exact-limit text must not split, got " + std::to_string(r.size()));
    std::cerr << " done\n";
}

// ============================================================================
// Breaks at paragraph boundaries when available
// ============================================================================
void TestParagraphBoundary() {
    std::cerr << "  TestParagraphBoundary...";
    const std::string text = Paragraphs(3, 5) + "tail paragraph that will not fit\n\n";
    // ~27 bytes per paragraph: boundaries at ~27/54/81; limit 60 breaks at 54.
    auto r = SplitForLimit(text, 60);
    Assert(r.size() >= 2, "should split");
    Assert(AllWithinLimit(r, 60), "all chunks within limit");
    Assert(r[0].size() >= 2 && r[0].substr(r[0].size() - 2) == "\n\n",
           "first chunk should end at a paragraph boundary");
    Assert(Join(r) == text, "prose reassembly must be byte-identical");
    std::cerr << " done\n";
}

// ============================================================================
// No boundaries: hard cuts, all within limit, always advances
// ============================================================================
void TestHardCutUnbroken() {
    std::cerr << "  TestHardCutUnbroken...";
    const std::string text(5000, 'x');  // no whitespace at all
    auto r = SplitForLimit(text, 2000);
    Assert(r.size() >= 3, "5000 unbroken bytes at limit 2000 need >=3 chunks, got " + std::to_string(r.size()));
    Assert(AllWithinLimit(r, 2000), "all chunks within limit");
    Assert(Join(r) == text, "reassembly must be byte-identical even for hard cuts");
    std::cerr << " done\n";
}

// ============================================================================
// Code fence spanning a chunk boundary is closed and reopened
// ============================================================================
void TestFenceSpanningChunks() {
    std::cerr << "  TestFenceSpanningChunks...";
    // Fence at the very start so the split is forced inside it (no earlier
    // boundary to prefer).
    const std::string fenceBody(3500, 'c');
    const std::string text = "```rust\n" + fenceBody + "\n```\n\noutro";
    auto r = SplitForLimit(text, 2000);
    Assert(r.size() >= 2, "must split");
    Assert(AllWithinLimit(r, 2000), "all chunks within limit despite fence surgery");
    Assert(r[0].size() >= 4 && r[0].substr(r[0].size() - 4) == "\n```",
           "chunk 1 must close the fence when split mid-fence");
    Assert(r[1].rfind("```", 0) == 0, "chunk 2 must reopen the fence");
    Assert(JoinMinusSurgery(r) == text, "reassembly minus fence surgery must equal original");
    std::cerr << " done\n";
}

// ============================================================================
// A break BEFORE a whole fence is preferred over mid-fence surgery when a
// paragraph boundary makes it available.
// ============================================================================
void TestPrefersBreakBeforeFence() {
    std::cerr << "  TestPrefersBreakBeforeFence...";
    // Prose sized so the natural break lands right after the opener line of a
    // SHORT fence: the guard pulls the break back to before the opener, the
    // whole fence rides in the next chunk, no surgery anywhere.
    const std::string text = Paragraphs(47, 8)          // 1974 bytes ending "\n\n"
        + "```rust\n" + std::string(100, 'c') + "\n```\n\noutro";
    auto r = SplitForLimit(text, 2000);
    Assert(AllWithinLimit(r, 2000), "all chunks within limit");
    Assert(Join(r) == text, "no surgery needed: reassembly identical");
    Assert(r.size() == 2, "expected exactly 2 chunks, got " + std::to_string(r.size()));
    Assert(r[0].find("```") == std::string::npos,
           "chunk 1 should end before the fence opens");
    Assert(r[1].rfind("```rust", 0) == 0,
           "chunk 2 should start with the whole fence");
    std::cerr << " done\n";
}

// ============================================================================
// Fence opened in a later chunk keeps state across boundaries
// ============================================================================
void TestFenceStateAcrossChunks() {
    std::cerr << "  TestFenceStateAcrossChunks...";
    // Prose chunk 1, fence starts late in chunk 1 and runs into chunk 2,
    // closes in chunk 2, more prose after.
    std::string text = Paragraphs(6, 30);            // ~960 bytes of prose
    text += "```\n";                                  // fence opens near the end
    text += std::string(2500, 'k');                  // fence body crosses the boundary
    text += "\n```\n\nfinal prose paragraph here\n\n";
    auto r = SplitForLimit(text, 1000);
    Assert(AllWithinLimit(r, 1000), "all chunks within limit");
    bool sawReopen = false;
    for (size_t i = 1; i < r.size(); ++i) {
        if (r[i].rfind("```\n", 0) == 0) { sawReopen = true; break; }
    }
    Assert(sawReopen, "a later chunk must reopen a fence carried across a boundary");
    Assert(JoinMinusSurgery(r) == text, "reassembly minus surgery equals original");
    // Every chunk except possibly the last must have balanced fences.
    for (size_t i = 0; i + 1 < r.size(); ++i) {
        Assert(CountFences(r[i]) % 2 == 0,
               "chunk " + std::to_string(i) + " must have balanced fences");
    }
    std::cerr << " done\n";
}

// ============================================================================
// The #30 acceptance shape: 5k Discord message, paragraphs + code block,
// arrives as ~3 sequential chunks, nothing dropped, code intact.
// ============================================================================
void TestAcceptanceFiveKDiscord() {
    std::cerr << "  TestAcceptanceFiveKDiscord...";
    std::string text = Paragraphs(8, 40);                   // ~1616 bytes prose
    text += "```cpp\nint main() {\n    return 0;\n}\n```\n\n";  // small fence
    text += Paragraphs(8, 40);                              // ~1616 bytes prose
    text += "```\n" + std::string(1400, 'z') + "\n```\n\n";  // big fence, forces split inside it
    text += "closing prose\n\n";
    Assert(text.size() > 4600, "test corpus must actually be ~5k, got " + std::to_string(text.size()));

    auto r = SplitForLimit(text, 2000);
    Assert(r.size() >= 3, "5k message should yield ~3+ chunks, got " + std::to_string(r.size()));
    Assert(AllWithinLimit(r, 2000), "all chunks within Discord's 2000 cap");
    Assert(JoinMinusSurgery(r) == text, "nothing dropped: reassembly equals original");
    for (size_t i = 0; i + 1 < r.size(); ++i) {
        Assert(CountFences(r[i]) % 2 == 0, "non-final chunks must be fence-balanced");
    }
    std::cerr << " done\n";
}

// ============================================================================
// Small limits are respected down to the 16-byte floor
// ============================================================================
void TestSmallLimitFloor() {
    std::cerr << "  TestSmallLimitFloor...";
    const std::string text(300, 'q');
    auto r = SplitForLimit(text, 40);
    Assert(r.size() >= 7, "300 bytes at limit 40 need >=7 chunks");
    Assert(AllWithinLimit(r, 40), "all chunks within the small limit");
    Assert(Join(r) == text, "reassembly identical");

    auto tiny = SplitForLimit(text, 10);  // below floor: clamped to 16
    Assert(AllWithinLimit(tiny, 16), "sub-floor limit clamps to 16");
    Assert(Join(tiny) == text, "reassembly identical even below the floor");
    std::cerr << " done\n";
}

// ============================================================================
// Fence that never closes (author's unterminated fence) — no surgery on the
// final chunk, bytes preserved verbatim
// ============================================================================
void TestUnterminatedFenceFinalChunk() {
    std::cerr << "  TestUnterminatedFenceFinalChunk...";
    std::string text = Paragraphs(4, 30);
    text += "```\n" + std::string(900, 'u');  // unterminated, near the end
    auto r = SplitForLimit(text, 500);
    Assert(AllWithinLimit(r, 500), "all chunks within limit");
    const std::string last = r.back();
    Assert(last.substr(last.size() - 4) != "\n```",
           "final chunk must not gain a synthetic close");
    Assert(JoinMinusSurgery(r) == text, "bytes preserved verbatim");
    std::cerr << " done\n";
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::cerr << "ChannelHelpers tests:\n";

    TestPassthrough();
    TestExactLimit();
    TestParagraphBoundary();
    TestHardCutUnbroken();
    TestFenceSpanningChunks();
    TestPrefersBreakBeforeFence();
    TestFenceStateAcrossChunks();
    TestAcceptanceFiveKDiscord();
    TestSmallLimitFloor();
    TestUnterminatedFenceFinalChunk();

    std::cerr << "\n";
    if (g_failures > 0) {
        std::cerr << "FAILED: " << g_failures << " assertion(s)\n";
        return 1;
    }
    std::cerr << "All ChannelHelpers tests passed!\n";
    return 0;
}
