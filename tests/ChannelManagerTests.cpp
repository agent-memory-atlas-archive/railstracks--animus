// ChannelManagerTests — #144: WARN loudly when a configured channel type has
// neither a C++ connector nor a registered Lua plugin.
//
// Origin (Oct 11 Mastodon trial): a configured channel whose type had no
// implementation anywhere was silently skipped at start — the entire signal
// was one DEBUG line; the failure only surfaced later as
// adapter_registered:false and "unknown platform" dispatch errors. "That's a
// config error masquerading as a missing feature."
//
// These tests pin the start-path behavior for all three states:
//   (a) made-up type, no implementation anywhere          -> WARN
//   (b) plugin-backed type, Lua plugin registered         -> no WARN (INFO)
//   (c) plugin-backed type, plugin absent (the trial)     -> WARN
//
// Test seam: a fresh ChannelManager has m_running == false, so RestartChannel
// takes its synchronous branch (StopChannel + StartChannel inline, no restart
// thread) — a deterministic, single-threaded path to the StartChannel
// fall-through under test. CreateChannel registers the channel without
// starting it (auto-start requires m_running, which only Initialize() sets).
// ALOG_* output is captured by swapping std::cerr's streambuf — the logger
// facade writes straight to std::cerr, so the buffer swap intercepts it
// without a production-code sink.

#include "animus_kernel/ChannelManager.h"
#include "animus_kernel/ChannelState.h"
#include "animus_kernel/tools/HttpClient.h"

#include <json/value.h>

#include <iostream>
#include <sstream>
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

class StderrCapture {
public:
    StderrCapture() : m_old(std::cerr.rdbuf(m_buffer.rdbuf())) {}
    ~StderrCapture() { std::cerr.rdbuf(m_old); }
    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;

    std::string Take() {
        std::string out = m_buffer.str();
        m_buffer.str("");
        m_buffer.clear();
        return out;
    }

private:
    std::stringstream m_buffer;
    std::streambuf* m_old;
};

ChannelState MakeChannel(const std::string& name, const std::string& type) {
    ChannelState state;
    state.name = name;
    state.type = type;
    state.enabled = true;
    state.config = Json::Value(Json::objectValue);
    return state;
}

// Register the channel, then drive StartChannel synchronously via
// RestartChannel's m_running==false branch (see file header).
std::string StartAndGetLog(ChannelManager& manager, const ChannelState& state) {
    std::string error;
    if (!manager.CreateChannel(state, &error)) {
        Assert(false, "CreateChannel failed: " + error);
        return {};
    }
    StderrCapture capture;
    if (!manager.RestartChannel(state.name, &error)) {
        Assert(false, "RestartChannel failed: " + error);
    }
    return capture.Take();
}

// (a) configured type with no implementation anywhere (made-up type) -> WARN
void TestMadeUpTypeWithoutImplementationWarns() {
    std::cout << "[1/4] made-up type, no implementation anywhere -> WARN\n";
    HttpClient http;
    ChannelManager manager(http, nullptr, {}, {});
    manager.SetSocialPluginProbe(
        [](const std::string&) { return false; });

    const std::string log = StartAndGetLog(
        manager, MakeChannel("ghost", "definitely-not-a-platform"));

    Assert(log.find("WARN") != std::string::npos,
           "expected a WARN for a type with no implementation anywhere");
    Assert(log.find("definitely-not-a-platform") != std::string::npos,
           "WARN must name the channel type");
    Assert(log.find("C++ connector") != std::string::npos &&
               log.find("Lua") != std::string::npos,
           "WARN must say what to check (C++ connector vs Lua script)");
}

// (b) configured mastodon + Lua plugin registered -> no WARN
void TestPluginBackedTypeWithPluginRegisteredIsQuiet() {
    std::cout << "[2/4] plugin-backed type, plugin registered -> no WARN\n";
    HttpClient http;
    ChannelManager manager(http, nullptr, {}, {});

    // The probe is the kernel's live view of the Lua plugin registry —
    // here "mastodon" is registered (plugin.id == channel type).
    manager.SetSocialPluginProbe([](const std::string& type) {
        return type == "mastodon";
    });

    const std::string log = StartAndGetLog(
        manager, MakeChannel("mastodon-main", "mastodon"));

    Assert(log.find("WARN") == std::string::npos,
           "plugin-backed channel must not WARN at start");
    Assert(log.find("handled by a registered Lua plugin") != std::string::npos,
           "expected an INFO line naming the Lua plugin path");
}

// (c) configured mastodon, plugin absent — the Oct 11 trial condition -> WARN
void TestPluginBackedTypeWithPluginAbsentWarns() {
    std::cout << "[3/4] plugin-backed type, plugin absent (the trial) -> WARN\n";
    HttpClient http;
    ChannelManager manager(http, nullptr, {}, {});

    // Plugin registry answers "nothing registered".
    manager.SetSocialPluginProbe(
        [](const std::string&) { return false; });

    const std::string log = StartAndGetLog(
        manager, MakeChannel("mastodon-main", "mastodon"));

    Assert(log.find("WARN") != std::string::npos,
           "the trial condition (plugin absent) must WARN at start");
    Assert(log.find("mastodon") != std::string::npos,
           "WARN must name the channel type");
}

// A manager wired without any probe must keep the loud behavior for unknown
// types rather than regressing to silence.
void TestProbelessManagerWarnsOnUnknownType() {
    std::cout << "[4/4] no probe set, unknown type -> WARN (no silent skip)\n";
    HttpClient http;
    ChannelManager manager(http, nullptr, {}, {});

    const std::string log = StartAndGetLog(
        manager, MakeChannel("ghost2", "no-such-connector"));

    Assert(log.find("WARN") != std::string::npos,
           "without a probe, unknown types must WARN, not stay silent");
    Assert(log.find("no-such-connector") != std::string::npos,
           "WARN must name the channel type");
}

} // namespace

int main() {
    TestMadeUpTypeWithoutImplementationWarns();
    TestPluginBackedTypeWithPluginRegisteredIsQuiet();
    TestPluginBackedTypeWithPluginAbsentWarns();
    TestProbelessManagerWarnsOnUnknownType();

    if (g_failures == 0) {
        std::cout << "ChannelManagerTests: ALL PASSED\n";
        return 0;
    }
    std::cout << "ChannelManagerTests: " << g_failures << " FAILURE(S)\n";
    return 1;
}
