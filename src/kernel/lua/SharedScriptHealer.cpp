#include "animus_kernel/lua/SharedScriptHealer.h"

#include "animus_kernel/Log.h"

#include <openssl/evp.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace animus::kernel {

namespace {

namespace fs = std::filesystem;

// Real md5 (ops parity: log lines can be cross-checked with md5sum on the
// box). OpenSSL::Crypto is already linked into animus_kernel.
std::string Md5Hex(const std::string& data) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(data.data(), data.size(), digest, &len,
                   EVP_md5(), nullptr) != 1) {
        return {};
    }
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(static_cast<size_t>(len) * 2);
    for (unsigned int i = 0; i < len; ++i) {
        out += kHex[digest[i] >> 4];
        out += kHex[digest[i] & 0x0F];
    }
    return out;
}

bool ReadFileBytes(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Temp-file + rename: a half-written script must never be visible to the
// loader. rename() over an existing target is an atomic replace on POSIX.
bool WriteFileAtomic(const fs::path& target, const std::string& content) {
    fs::path tmp = target;
    tmp += ".heal-tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!f.good()) {
            f.close();
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, target, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::vector<std::string> ListLuaFiles(const fs::path& dir) {
    std::vector<std::string> names;
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return names;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".lua") continue;
        names.push_back(entry.path().filename().string());
    }
    std::sort(names.begin(), names.end());
    return names;
}

// Pin marker: ".pin-" + stem — deliberately NOT *.lua so the loader's
// ScanDirectory (extension filter) never enumerates it.
fs::path PinPathFor(const fs::path& sharedDir, const std::string& luaFilename) {
    return sharedDir / (".pin-" + fs::path(luaFilename).stem().string());
}

} // namespace

SharedScriptHealReport ReconcileSharedScripts(const std::string& stagingRoot,
                                              const std::string& scriptRoot) {
    SharedScriptHealReport report;

    const fs::path stagingShared = fs::path(stagingRoot) / "_shared";
    const fs::path volumeShared = fs::path(scriptRoot) / "_shared";
    report.staging_dir = stagingShared.string();
    report.shared_dir = volumeShared.string();

    std::error_code ec;
    if (!fs::exists(stagingShared, ec) || !fs::is_directory(stagingShared, ec)) {
        report.staging_present = false;
        ALOG_INFO("lua:heal",
                  "staging dir '" << stagingShared.string()
                  << "' absent — _shared heal disabled (dev/bare boot, not an error)");
        return report;
    }
    report.staging_present = true;

    // Fresh-volume path: create the target before seeding (main.cpp already
    // does this, but the heal must not depend on that ordering).
    fs::create_directories(volumeShared, ec);

    const auto staged = ListLuaFiles(stagingShared);

    int seeded = 0, healed = 0, equal = 0, pinned = 0, orphans = 0;

    for (const auto& name : staged) {
        const fs::path src = stagingShared / name;
        const fs::path dst = volumeShared / name;

        ScriptHealOutcome o;
        o.filename = name;

        // Pin guard first — pin wins over everything, including seeding
        // (an admin pinned it; maybe they deleted or rewrote it deliberately)
        if (fs::exists(PinPathFor(volumeShared, name), ec)) {
            o.action = ScriptHealOutcome::Action::Pinned;
            std::string current;
            if (ReadFileBytes(dst, current)) o.old_hash = Md5Hex(current);
            ALOG_INFO("lua:heal",
                      "'" << name << "' PINNED by admin marker — left untouched (volume md5="
                      << (o.old_hash.empty() ? "<absent>" : o.old_hash) << ")");
            ++pinned;
            report.outcomes.push_back(std::move(o));
            continue;
        }

        std::string stagedBytes;
        if (!ReadFileBytes(src, stagedBytes)) {
            ALOG_ERROR("lua:heal",
                       "FAILED to read staged '" << src.string()
                       << "' — this file skipped (boot continues)");
            continue;
        }
        const std::string stagedHash = Md5Hex(stagedBytes);

        std::string volumeBytes;
        const bool hasVolumeCopy = ReadFileBytes(dst, volumeBytes);

        if (hasVolumeCopy) {
            const std::string volumeHash = Md5Hex(volumeBytes);
            if (volumeHash == stagedHash) {
                o.action = ScriptHealOutcome::Action::Equal;
                o.old_hash = o.new_hash = volumeHash;
                ALOG_DEBUG("lua:heal",
                           "'" << name << "' already current (md5=" << volumeHash << ")");
                ++equal;
                report.outcomes.push_back(std::move(o));
                continue;
            }
            o.action = ScriptHealOutcome::Action::Healed;
            o.old_hash = volumeHash;
            o.new_hash = stagedHash;
        } else {
            o.action = ScriptHealOutcome::Action::Seeded;
            o.new_hash = stagedHash;
        }

        if (!WriteFileAtomic(dst, stagedBytes)) {
            ALOG_ERROR("lua:heal",
                       "FAILED to write '" << dst.string()
                       << "' — heal for this file aborted (boot continues)");
            continue;
        }

        if (o.action == ScriptHealOutcome::Action::Healed) {
            ALOG_INFO("lua:heal",
                      "HEALED '" << name << "' md5 " << o.old_hash
                      << " -> " << o.new_hash);
            ++healed;
        } else {
            ALOG_INFO("lua:heal",
                      "SEEDED '" << name << "' (was absent on volume) md5="
                      << o.new_hash);
            ++seeded;
        }
        report.outcomes.push_back(std::move(o));
    }

    // Orphan scan: volume files the image no longer ships — log loudly,
    // leave in place (renamed/removed adapters keep serving until an
    // explicit cleanup decision; that policy is deliberately not this pass).
    const auto volumeNames = ListLuaFiles(volumeShared);
    std::unordered_set<std::string> stagedSet(staged.begin(), staged.end());
    for (const auto& name : volumeNames) {
        if (stagedSet.count(name)) continue;
        ScriptHealOutcome o;
        o.filename = name;
        o.action = ScriptHealOutcome::Action::Orphaned;
        std::string current;
        if (ReadFileBytes(volumeShared / name, current)) {
            o.old_hash = Md5Hex(current);
        }
        ALOG_WARNING("lua:heal",
                     "ORPHAN '" << name << "' — not in image defaults (md5="
                     << (o.old_hash.empty() ? "<unreadable>" : o.old_hash)
                     << "), left in place; remove manually if retired");
        ++orphans;
        report.outcomes.push_back(std::move(o));
    }

    ALOG_INFO("lua:heal",
              "volume heal complete: seeded=" << seeded << " healed=" << healed
              << " equal=" << equal << " pinned=" << pinned
              << " orphans=" << orphans
              << " (staging=" << stagingShared.string() << ")");
    return report;
}

} // namespace animus::kernel
