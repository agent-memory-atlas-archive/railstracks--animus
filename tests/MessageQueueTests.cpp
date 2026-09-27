// MessageQueueTests — aggregation-window debunching semantics
//
// Covers the aggregation_window_ms pre-chain debounce (the #rpg fix):
// burst arrivals within the window flush as ONE concatenated turn, the
// window is fixed (later messages do not extend it), a message arriving
// after expiry starts a new window, and composition with the response
// cooldown takes the later deadline.
//
// Timing: windows used here are 150-300ms — fast enough for a test run,
// slow enough that scheduling jitter can't flip a case. Failure prints
// elapsed times for diagnosis.

#include "animus_kernel/MessageQueue.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using animus::kernel::MessageQueue;

namespace {

int g_failures = 0;

void Assert(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "  ASSERT FAILED: %s\n", msg);
        ++g_failures;
    }
}

struct Capture {
    std::mutex mu;
    std::vector<std::pair<std::string, std::string>> flushes;  // (sessionKey, text)
    std::atomic<bool> fired{false};

    MessageQueue::FlushCallback Callback() {
        return [this](const std::string& key, const std::string& msg) {
            std::lock_guard<std::mutex> lock(mu);
            flushes.emplace_back(key, msg);
            fired.store(true);
        };
    }

    std::vector<std::pair<std::string, std::string>> Take() {
        std::lock_guard<std::mutex> lock(mu);
        return flushes;
    }
};

bool WaitFor(std::atomic<bool>& flag, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return flag.load();
}

void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ---------------------------------------------------------------------------
// Test 1: burst within the window flushes as ONE turn
// ---------------------------------------------------------------------------
int TestBurstBatchesIntoOneTurn() {
    std::fprintf(stderr, "  [message-queue] burst within window batches into one turn...\n");
    Capture cap;
    {
        MessageQueue q(cap.Callback());
        const auto t0 = std::chrono::steady_clock::now();
        // Simulate a 4-chunk PRIVMSG split arriving over ~60ms
        q.Push("s1", "", "chunk one", 1000, 0, 50, 150);
        SleepMs(20);
        q.Push("s1", "", "chunk two", 1001, 0, 50, 150);
        SleepMs(20);
        q.Push("s1", "", "chunk three", 1002, 0, 50, 150);
        SleepMs(20);
        q.Push("s1", "", "chunk four", 1003, 0, 50, 150);

        (void)WaitFor(cap.fired, 2000);
        auto flushes = cap.Take();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        Assert(flushes.size() == 1, "burst flushes exactly once");
        if (flushes.size() == 1) {
            Assert(flushes[0].second.find("chunk one") != std::string::npos
                && flushes[0].second.find("chunk two") != std::string::npos
                && flushes[0].second.find("chunk three") != std::string::npos
                && flushes[0].second.find("chunk four") != std::string::npos,
                "all four chunks in the single flushed turn");
        }
        Assert(elapsed >= 140, "flush happened no earlier than the window");
        Assert(elapsed < 1000, "flush happened promptly after window expiry");
        Assert(q.PendingCount("s1") == 0, "queue drained after flush");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Test 2: window is FIXED — later messages do not extend it
// ---------------------------------------------------------------------------
int TestWindowIsFixedNotSliding() {
    std::fprintf(stderr, "  [message-queue] window is fixed, not sliding...\n");
    Capture cap;
    {
        MessageQueue q(cap.Callback());
        const auto t0 = std::chrono::steady_clock::now();
        q.Push("s2", "", "first", 2000, 0, 50, 200);
        SleepMs(120);  // inside the first window
        q.Push("s2", "", "second", 2001, 0, 50, 200);  // must NOT extend

        (void)WaitFor(cap.fired, 2000);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        auto flushes = cap.Take();

        Assert(flushes.size() == 1, "fixed window flushes once");
        Assert(elapsed < 320,
               "flush fired at the ORIGINAL deadline (~200ms), not 120+200");
        if (flushes.size() == 1) {
            Assert(flushes[0].second.find("first") != std::string::npos
                && flushes[0].second.find("second") != std::string::npos,
                "both messages in the fixed-window turn");
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Test 3: message after expiry starts a NEW window (two bursts → two turns)
// ---------------------------------------------------------------------------
int TestPostExpiryMessageStartsNewWindow() {
    std::fprintf(stderr, "  [message-queue] post-expiry message starts a new window...\n");
    Capture cap;
    {
        MessageQueue q(cap.Callback());
        q.Push("s3", "", "burst A", 3000, 0, 50, 150);
        bool ok = WaitFor(cap.fired, 2000);
        Assert(ok, "first burst flushed");
        auto flushes = cap.Take();
        Assert(flushes.size() == 1, "one flush for burst A");

        // Well past the window — starts a fresh one
        SleepMs(250);
        cap.fired.store(false);
        q.Push("s3", "", "burst B", 3500, 0, 50, 150);
        ok = WaitFor(cap.fired, 2000);
        Assert(ok, "second burst flushed");
        flushes = cap.Take();
        Assert(flushes.size() == 2, "burst B flushed as its own turn");
        if (flushes.size() == 2) {
            Assert(flushes[1].second.find("burst B") != std::string::npos,
                "second turn carries burst B");
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Test 4: window composes with cooldown — later deadline wins
// ---------------------------------------------------------------------------
int TestWindowComposesWithCooldown() {
    std::fprintf(stderr, "  [message-queue] window composes with cooldown...\n");
    Capture cap;
    {
        MessageQueue q(cap.Callback());
        // Prime: chain ran and ended now → cooldown of 1 second applies
        q.NotifyChainStart("s4");
        q.NotifyChainEnd("s4", 0);  // has_responded = true, chain_end = now

        // A message arrives with a 150ms window + 1s cooldown — the flush
        // must wait for the COOLDOWN (later deadline), not fire at +150ms.
        q.Push("s4", "", "post-chain message", 4000, 1, 50, 150);

        // At +400ms the window has long expired but the cooldown has not —
        // nothing must have flushed yet.
        SleepMs(400);
        Assert(cap.Take().empty(), "window expiry does not short-circuit cooldown");

        const bool ok = WaitFor(cap.fired, 3000);
        Assert(ok, "flushed after cooldown expiry");
        auto flushes = cap.Take();
        Assert(flushes.size() == 1, "single flush after cooldown");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Test 5: window=0, interval=0 — queue-only semantics unchanged
// (the kernel only routes to the queue when a knob is set; a direct Push
// with both zero accumulates pending, exactly as before this change)
// ---------------------------------------------------------------------------
int TestNoKnobAccumulates() {
    std::fprintf(stderr, "  [message-queue] window=0 interval=0 accumulates (unchanged)...\n");
    Capture cap;
    {
        MessageQueue q(cap.Callback());
        q.Push("s5", "", "no knobs", 5000, 0, 50, 0);
        SleepMs(100);
        Assert(cap.Take().empty(), "no flush without window/interval");
        Assert(q.PendingCount("s5") == 1, "message stays pending");
        // Safety valve still works without knobs: maxQueued forces a flush
        q.Push("s5", "", "over the line", 5001, 0, 2, 0);
        const bool ok = WaitFor(cap.fired, 500);
        Assert(ok, "maxQueued force-flush still fires");
        auto flushes = cap.Take();
        Assert(flushes.size() == 1, "exactly one flush (both messages)");
        if (flushes.size() == 1) {
            Assert(flushes[0].second.find("no knobs") != std::string::npos
                && flushes[0].second.find("over the line") != std::string::npos,
                "both messages in the forced flush");
        }
    }
    return 0;
}

}  // namespace

int main() {
    std::fprintf(stderr, "[MessageQueueTests]\n");
    TestBurstBatchesIntoOneTurn();
    TestWindowIsFixedNotSliding();
    TestPostExpiryMessageStartsNewWindow();
    TestWindowComposesWithCooldown();
    TestNoKnobAccumulates();
    if (g_failures == 0) {
        std::fprintf(stderr, "  ALL PASSED\n");
        return 0;
    }
    std::fprintf(stderr, "  %d FAILURES\n", g_failures);
    return 1;
}