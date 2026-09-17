#include "animus_kernel/tools/FileTool.h"
#include "animus_kernel/tools/ToolRegistry.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <json/json.h>
#include <json/reader.h>
#include <json/writer.h>

using namespace animus::kernel;

namespace {

int g_failures = 0;

void Assert(bool condition, const std::string& msg) {
    if (!condition) {
        std::cerr << "  ASSERT FAILED: " << msg << "\n";
        g_failures++;
    }
}

// ----------------------------------------------------------------------------
// Scratch workspace
// ----------------------------------------------------------------------------

struct TempDir {
    std::filesystem::path path;

    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count() % 1000000;
        path = std::filesystem::temp_directory_path()
            / ("animus_filetool_tests_" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void WriteFile(const std::filesystem::path& dir, const std::string& name,
               const std::string& content) {
    std::ofstream f(dir / name, std::ios::trunc);
    f << content;
}

std::string ReadFile(const std::filesystem::path& dir, const std::string& name) {
    std::ifstream f(dir / name);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Execute a file-tool call built from individual JSON fields (action, path, ...).
ToolResult Run(FileTool& tool, const Json::Value& args) {
    Json::Value full(args);
    Json::StreamWriterBuilder wb;
    wb.settings_["indentation"] = "";
    ToolCall call;
    call.name = "file";
    call.arguments = Json::writeString(wb, full);
    return tool.Execute(call);
}

bool HasParam(const ToolDefinition& def, const std::string& name) {
    for (const auto& p : def.parameters) {
        if (p.name == name) return true;
    }
    return false;
}

} // anonymous namespace

// ============================================================================
// Test: definition advertises the replacement contract (#98)
// ============================================================================
void TestDefinitionAdvertisesReplacementParams() {
    std::cerr << "  TestDefinitionAdvertisesReplacementParams...";

    FileTool tool;
    auto def = tool.GetDefinition();

    Assert(HasParam(def, "old_text"), "definition must advertise old_text");
    Assert(HasParam(def, "new_text"), "definition must advertise new_text");
    Assert(HasParam(def, "content"), "definition must keep content (legacy alias)");
    Assert(HasParam(def, "allow_empty_replacement"), "definition must advertise allow_empty_replacement");
    Assert(HasParam(def, "allow_empty_write"), "definition must advertise allow_empty_write");

    std::cerr << " done\n";
}

// ============================================================================
// Test: edit with new_text (no content) performs the replacement — the exact
// #98 call shape that previously deleted old_text and reported success.
// ============================================================================
void TestEditWithNewTextReplaces() {
    std::cerr << "  TestEditWithNewTextReplaces...";

    TempDir dir;
    FileTool tool(dir.path.string());
    WriteFile(dir.path, "doc.md", "alpha\nBETA\ngamma\n");

    Json::Value args;
    args["action"] = "edit";
    args["path"] = "doc.md";
    args["old_text"] = "BETA";
    args["new_text"] = "one\ntwo\nthree"; // multi-line replacement, as in the incident

    auto result = Run(tool, args);

    Assert(result.success, "edit with new_text must succeed, got: " + result.error);
    const std::string after = ReadFile(dir.path, "doc.md");
    Assert(after == "alpha\none\ntwo\nthree\ngamma\n",
           "multi-line new_text must be inserted, got: " + after);

    std::cerr << " done\n";
}

// ============================================================================
// Test: legacy 'content' alias still works (back-compat)
// ============================================================================
void TestEditContentAliasStillWorks() {
    std::cerr << "  TestEditContentAliasStillWorks...";

    TempDir dir;
    FileTool tool(dir.path.string());
    WriteFile(dir.path, "doc.md", "alpha\nBETA\ngamma\n");

    Json::Value args;
    args["action"] = "edit";
    args["path"] = "doc.md";
    args["old_text"] = "BETA";
    args["content"] = "REPLACED";

    auto result = Run(tool, args);

    Assert(result.success, "edit via legacy content alias must succeed, got: " + result.error);
    Assert(ReadFile(dir.path, "doc.md") == "alpha\nREPLACED\ngamma\n",
           "legacy content alias must replace");

    std::cerr << " done\n";
}

// ============================================================================
// Test: when both new_text and content are present, new_text wins
// ============================================================================
void TestEditPrefersNewTextWhenBothPresent() {
    std::cerr << "  TestEditPrefersNewTextWhenBothPresent...";

    TempDir dir;
    FileTool tool(dir.path.string());
    WriteFile(dir.path, "doc.md", "alpha\nBETA\ngamma\n");

    Json::Value args;
    args["action"] = "edit";
    args["path"] = "doc.md";
    args["old_text"] = "BETA";
    args["new_text"] = "FROM_NEW_TEXT";
    args["content"] = "FROM_CONTENT";

    auto result = Run(tool, args);

    Assert(result.success, "edit with both params must succeed, got: " + result.error);
    Assert(ReadFile(dir.path, "doc.md").find("FROM_NEW_TEXT") != std::string::npos,
           "new_text must take precedence over content");
    Assert(ReadFile(dir.path, "doc.md").find("FROM_CONTENT") == std::string::npos,
           "content must not win when new_text is present");

    std::cerr << " done\n";
}

// ============================================================================
// Test: the #98 incident — replacement argument absent entirely. Must refuse
// and leave the file untouched, not delete old_text.
// ============================================================================
void TestEditWithoutReplacementRefusesAndPreservesFile() {
    std::cerr << "  TestEditWithoutReplacementRefusesAndPreservesFile...";

    TempDir dir;
    FileTool tool(dir.path.string());
    const std::string original = "alpha\nBETA\ngamma\n";
    WriteFile(dir.path, "trail.md", original);

    Json::Value args;
    args["action"] = "edit";
    args["path"] = "trail.md";
    args["old_text"] = "BETA";
    // Replacement deliberately omitted (dropped/misnamed argument).

    auto result = Run(tool, args);

    Assert(!result.success, "edit without replacement must fail, not silently delete");
    Assert(result.error.find("replacement") != std::string::npos,
           "error must explain the missing replacement, got: " + result.error);
    Assert(ReadFile(dir.path, "trail.md") == original,
           "file must be byte-identical after refused edit");

    std::cerr << " done\n";
}

// ============================================================================
// Test: explicit deletion is possible, but only with the confirmation flag
// ============================================================================
void TestEditEmptyReplacementRequiresExplicitAllow() {
    std::cerr << "  TestEditEmptyReplacementRequiresExplicitAllow...";

    TempDir dir;
    FileTool tool(dir.path.string());
    WriteFile(dir.path, "doc.md", "alpha\nBETA\ngamma\n");

    // Without the flag: refused.
    Json::Value refused;
    refused["action"] = "edit";
    refused["path"] = "doc.md";
    refused["old_text"] = "BETA";
    refused["new_text"] = "";
    auto r1 = Run(tool, refused);
    Assert(!r1.success, "empty new_text without allow_empty_replacement must fail");
    Assert(ReadFile(dir.path, "doc.md") == "alpha\nBETA\ngamma\n",
           "file must be untouched by the refused empty edit");

    // With the flag: explicit deletion succeeds.
    Json::Value allowed = refused;
    allowed["allow_empty_replacement"] = true;
    auto r2 = Run(tool, allowed);
    Assert(r2.success, "empty replacement with allow_empty_replacement must succeed, got: " + r2.error);
    Assert(ReadFile(dir.path, "doc.md") == "alpha\n\ngamma\n",
           "explicit empty replacement must delete old_text");

    std::cerr << " done\n";
}

// ============================================================================
// Test: write with empty content over an existing non-empty file is refused
// unless allow_empty_write confirms the truncation.
// ============================================================================
void TestWriteEmptyOverNonEmptyRefused() {
    std::cerr << "  TestWriteEmptyOverNonEmptyRefused...";

    TempDir dir;
    FileTool tool(dir.path.string());
    const std::string ledger = "| date | note |\n|---|---|\n| Sep 16 | checkpoint |\n";
    WriteFile(dir.path, "ledger.md", ledger);

    Json::Value refused;
    refused["action"] = "write";
    refused["path"] = "ledger.md";
    refused["content"] = "";
    auto r1 = Run(tool, refused);

    Assert(!r1.success, "empty write over non-empty file must fail");
    Assert(r1.error.find("allow_empty_write") != std::string::npos,
           "error must name allow_empty_write, got: " + r1.error);
    Assert(ReadFile(dir.path, "ledger.md") == ledger,
           "ledger must survive the refused empty write byte-identical");

    Json::Value allowed = refused;
    allowed["allow_empty_write"] = true;
    auto r2 = Run(tool, allowed);
    Assert(r2.success, "empty write with allow_empty_write must succeed, got: " + r2.error);
    Assert(ReadFile(dir.path, "ledger.md").empty(),
           "confirmed empty write must truncate the file");

    std::cerr << " done\n";
}

// ============================================================================
// Test: writing empty content to a path that does not exist yet is allowed
// (creating an empty file destroys nothing).
// ============================================================================
void TestWriteEmptyToNewPathAllowed() {
    std::cerr << "  TestWriteEmptyToNewPathAllowed...";

    TempDir dir;
    FileTool tool(dir.path.string());

    Json::Value args;
    args["action"] = "write";
    args["path"] = "fresh.md";
    args["content"] = "";
    auto result = Run(tool, args);

    Assert(result.success, "empty write to a new path must succeed, got: " + result.error);
    Assert(std::filesystem::exists(dir.path / "fresh.md"), "new empty file must exist");
    Assert(std::filesystem::file_size(dir.path / "fresh.md") == 0, "new file must be empty");

    std::cerr << " done\n";
}

// ============================================================================
// Test: pre-existing edit validation still behaves (old_text not found / not
// unique, missing old_text)
// ============================================================================
void TestEditValidationUnchanged() {
    std::cerr << "  TestEditValidationUnchanged...";

    TempDir dir;
    FileTool tool(dir.path.string());
    WriteFile(dir.path, "doc.md", "alpha\nBETA\ngamma\nBETA\n");

    Json::Value missing;
    missing["action"] = "edit";
    missing["path"] = "doc.md";
    missing["new_text"] = "x";
    auto r1 = Run(tool, missing);
    Assert(!r1.success && r1.error.find("old_text") != std::string::npos,
           "missing old_text must error");

    Json::Value dup;
    dup["action"] = "edit";
    dup["path"] = "doc.md";
    dup["old_text"] = "BETA";
    dup["new_text"] = "x";
    auto r2 = Run(tool, dup);
    Assert(!r2.success && r2.error.find("unique") != std::string::npos,
           "non-unique old_text must error");

    Json::Value absent;
    absent["action"] = "edit";
    absent["path"] = "doc.md";
    absent["old_text"] = "NOT_PRESENT";
    absent["new_text"] = "x";
    auto r3 = Run(tool, absent);
    Assert(!r3.success && r3.error.find("not found") != std::string::npos,
           "absent old_text must error");

    std::cerr << " done\n";
}

// ============================================================================
// Test: edit success receipt echoes sizes (verify-after-write support)
// ============================================================================
void TestEditReceiptEchoesSizes() {
    std::cerr << "  TestEditReceiptEchoesSizes...";

    TempDir dir;
    FileTool tool(dir.path.string());
    WriteFile(dir.path, "doc.md", "alpha\nBETA\ngamma\n");

    Json::Value args;
    args["action"] = "edit";
    args["path"] = "doc.md";
    args["old_text"] = "BETA";
    args["new_text"] = "one\ntwo\nthree";
    auto result = Run(tool, args);

    Assert(result.success, "edit must succeed");
    Assert(result.output.find("bytes") != std::string::npos
           && result.output.find("replaced 1 occurrence") != std::string::npos,
           "receipt must echo byte sizes and occurrence count, got: " + result.output);

    std::cerr << " done\n";
}

// ============================================================================
// Main
// ============================================================================
int main() {
    std::cerr << "FileTool tests:\n";

    TestDefinitionAdvertisesReplacementParams();
    TestEditWithNewTextReplaces();
    TestEditContentAliasStillWorks();
    TestEditPrefersNewTextWhenBothPresent();
    TestEditWithoutReplacementRefusesAndPreservesFile();
    TestEditEmptyReplacementRequiresExplicitAllow();
    TestWriteEmptyOverNonEmptyRefused();
    TestWriteEmptyToNewPathAllowed();
    TestEditValidationUnchanged();
    TestEditReceiptEchoesSizes();

    std::cerr << "\n";
    if (g_failures > 0) {
        std::cerr << "FAILED: " << g_failures << " assertion(s)\n";
        return 1;
    }
    std::cerr << "All FileTool tests passed!\n";
    return 0;
}
