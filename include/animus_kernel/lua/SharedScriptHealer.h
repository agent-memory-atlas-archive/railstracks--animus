#pragma once

#include <string>
#include <vector>

namespace animus::kernel {

// Result of reconciling one file between the image-baked staging defaults
// and the persisted volume (#143).
struct ScriptHealOutcome {
    std::string filename;  // e.g. "mastodon.lua"

    enum class Action {
        Seeded,    // absent on the volume -> copied from staging
        Healed,    // content differed    -> overwritten with staged content
        Equal,     // identical content   -> untouched
        Pinned,    // .pin-<stem> marker present -> deliberately untouched
        Orphaned   // on the volume but not shipped by the image -> left in place
    };
    Action action{Action::Equal};

    // md5 hex of the volume copy BEFORE the heal (Seeded: empty).
    std::string old_hash;
    // md5 hex of the content AFTER the heal — equals the staged md5 for
    // Seeded/Healed/Equal; empty for Pinned/Orphaned (nothing written).
    std::string new_hash;
};

struct SharedScriptHealReport {
    // false => staging dir absent: heal disabled (dev/bare boot) — no-op.
    bool staging_present{false};
    std::string staging_dir;  // <stagingRoot>/_shared actually consulted
    std::string shared_dir;   // <scriptRoot>/_shared actually reconciled
    std::vector<ScriptHealOutcome> outcomes;
};

// Boot-time reconcile of <stagingRoot>/_shared/*.lua into <scriptRoot>/_shared/
// (#143 — "volume-heal-on-boot").
//
// The problem: persisted volumes mount over the image's lua dir, so image
// rebuilds silently never deliver Lua adapter changes to long-lived boxes.
// The image bakes pristine defaults at a staging root OUTSIDE the data dir
// (env ANIMUS_LUA_STAGING_DIR, e.g. /opt/animus/lua-defaults); at boot this
// pass reconciles them into the volume.
//
// Policy (documented contract):
//  - Content-hash gate (md5), never mtimes: deployment copies scramble
//    mtimes, so they cannot decide "image newer than volume".
//  - Pin guard: a sibling marker file ".pin-<stem>" (mastodon.lua ->
//    .pin-mastodon; deliberately NOT *.lua so the loader never enumerates
//    it) protects an admin-edited script from ANY overwrite, regardless of
//    content. Pin wins over heal AND over seeding.
//  - Orphans (volume files the image no longer ships) are logged loudly and
//    LEFT IN PLACE — deletion propagation is a later, explicit step.
//  - Agent-specific script dirs (<scriptRoot>/<agentId>/) and DB-stored
//    scripts (lua_scripts table) are never touched.
//  - Fail loud: every seeded/healed/pinned/orphan decision logs with hash
//    identity; write failures log as errors and abort that file's heal
//    (boot continues — a half-healed box still boots, visibly).
SharedScriptHealReport ReconcileSharedScripts(const std::string& stagingRoot,
                                              const std::string& scriptRoot);

} // namespace animus::kernel
