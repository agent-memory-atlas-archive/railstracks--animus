// RegistryClientTests — registry browse parsing (issue #63)
//
// Covers the parse layer for GET /api/v1/packages and
// GET /api/v1/packages/{name}/versions envelopes: happy paths captured from
// the live registry (animus-registry.steadyfort.com, 2026-09-19), optional
// field defaults, and every malformed-shape rejection. Network layer
// (ListRegistryPackages / ListRegistryPackageVersions) is a thin wrapper over
// the same HttpClient path InstallFromRegistry uses and stays untested here —
// parse coverage is where the shape contract lives.

#include "animus_kernel/api/RegistryClient.h"

#include <iostream>
#include <string>
#include <vector>

using namespace animus::kernel;
using registry::ParseRegistryPackageList;
using registry::ParseRegistryVersionList;
using registry::RegistryPackageSummary;
using registry::RegistryPackageVersion;

namespace {

int g_failures = 0;

void Assert(bool condition, const std::string& msg) {
    if (!condition) {
        std::cerr << "  ASSERT FAILED: " << msg << "\n";
        g_failures++;
    }
}

// Live-shape list envelope (single entry, alpaca) captured 2026-09-19.
const char* kListOne = R"JSON({"packages":[{"name":"alpaca","version":3,"semantic_version":"1.4.2","description":"Alpaca brokerage API - paper/live trading: portfolio, orders, watchlist, one-shot price triggers.","content_hash":"8641071970e22cdf478dab40042eb45b537544d1cea955c050f3c48624bc58ba","official":false,"published_at":"2026-09-14T20:49:26.678Z"}]})JSON";

// Live-shape versions envelope (3 releases, newest first) captured 2026-09-19.
const char* kVersionsThree = R"JSON({"name":"alpaca","versions":[{"version":3,"semantic_version":"1.4.2","content_hash":"8641071970e22cdf478dab40042eb45b537544d1cea955c050f3c48624bc58ba","published_at":"2026-09-14T20:49:26.678Z"},{"version":2,"semantic_version":"1.4.1","content_hash":"4828d9d109cc960570cd2696788d0d7d20c480e28ca2a28c64e7b04757609ed8","published_at":"2026-09-12T15:44:42.659Z"},{"version":1,"semantic_version":"1.4.0","content_hash":"a3d0644d0a5bc57a635031d2e8a673c825b2c02fb19cd14a91908717a740e5de","published_at":"2026-09-07T12:05:47.324Z"}]})JSON";

void TestListHappyPath() {
    std::cerr << "  list: happy path (live envelope)\n";
    std::vector<RegistryPackageSummary> out;
    std::string err;
    Assert(ParseRegistryPackageList(kListOne, out, err), "parse ok: " + err);
    Assert(out.size() == 1, "one row");
    const auto& s = out[0];
    Assert(s.name == "alpaca", "name");
    Assert(s.version == 3, "int version");
    Assert(s.semantic_version == "1.4.2", "semantic version");
    Assert(s.description.find("Alpaca brokerage API") == 0, "description");
    Assert(s.content_hash.size() == 64, "sha256-length content hash");
    Assert(!s.official, "official default false");
    Assert(s.published_at == "2026-09-14T20:49:26.678Z", "published_at passthrough");
}

void TestListMultipleAndEmpty() {
    std::cerr << "  list: multiple rows + empty list\n";
    const std::string two = R"JSON({"packages":[{"name":"a","version":1,"semantic_version":"0.1.0"},{"name":"b","version":7,"semantic_version":"2.0.0","official":true}]})JSON";
    std::vector<RegistryPackageSummary> out;
    std::string err;
    Assert(ParseRegistryPackageList(two, out, err), "parse ok");
    Assert(out.size() == 2, "two rows");
    Assert(out[0].official == false, "row0 not official");
    Assert(out[1].official == true, "row1 official");
    Assert(out[1].description.empty(), "missing description defaults empty");
    Assert(out[1].published_at.empty(), "missing published_at defaults empty");

    Assert(ParseRegistryPackageList(R"({"packages":[]})", out, err), "empty list parses");
    Assert(out.empty(), "empty list yields zero rows");
}

void TestListRejections() {
    std::cerr << "  list: malformed envelopes rejected\n";
    std::vector<RegistryPackageSummary> out;
    std::string err;
    Assert(!ParseRegistryPackageList("not json", out, err), "non-JSON rejected");
    Assert(!err.empty(), "non-JSON carries error");
    Assert(!ParseRegistryPackageList(R"({"nope":1})", out, err), "missing packages[] rejected");
    Assert(!ParseRegistryPackageList(R"({"packages":"nope"})", out, err), "non-array packages rejected");
    Assert(!ParseRegistryPackageList(R"({"packages":[{"version":1}]})", out, err),
           "row without name rejected");
    Assert(!ParseRegistryPackageList(R"({"packages":[42]})", out, err),
           "non-object row rejected");
}

void TestVersionsHappyPath() {
    std::cerr << "  versions: happy path (live envelope)\n";
    std::vector<RegistryPackageVersion> out;
    std::string name, err;
    Assert(ParseRegistryVersionList(kVersionsThree, name, out, err), "parse ok: " + err);
    Assert(name == "alpaca", "name extracted");
    Assert(out.size() == 3, "three versions");
    // Registry serves newest-first; parse layer must preserve order.
    Assert(out[0].semantic_version == "1.4.2", "newest first");
    Assert(out[0].version == 3, "int version newest");
    Assert(out[2].semantic_version == "1.4.0", "oldest last");
    Assert(out[1].published_at == "2026-09-12T15:44:42.659Z", "published_at row1");
}

void TestVersionsRejections() {
    std::cerr << "  versions: malformed envelopes rejected\n";
    std::vector<RegistryPackageVersion> out;
    std::string name, err;
    Assert(!ParseRegistryVersionList("{oops", name, out, err), "non-JSON rejected");
    Assert(!ParseRegistryVersionList(R"({"name":"x"})", name, out, err),
           "missing versions[] rejected");
    Assert(!ParseRegistryVersionList(R"({"versions":{}})", name, out, err),
           "non-array versions rejected");
    Assert(!ParseRegistryVersionList(
               R"({"name":"x","versions":[{"version":2,"published_at":"z"}]})",
               name, out, err),
           "row without semantic_version rejected");
}

}  // namespace

int main() {
    std::cerr << "RegistryClient tests:\n";
    TestListHappyPath();
    TestListMultipleAndEmpty();
    TestListRejections();
    TestVersionsHappyPath();
    TestVersionsRejections();
    if (g_failures == 0) std::cerr << "All registry client tests passed.\n";
    else std::cerr << g_failures << " failures.\n";
    return g_failures == 0 ? 0 : 1;
}
