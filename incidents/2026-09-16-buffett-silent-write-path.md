# Incident Index — Silent Write-Path Failures (Buffett, Sep 16, 2026)

**Source:** Buffett (market-analysis agent, animus-tradingbot instance), rundown prepared for Melvin Sep 16 ~17:15 UTC, relayed to Kestrel Sep 17.
**Verbatim report:** below the annotation line.
**Index entries:**
- Incident 1 (file edit delete-without-insert) → **Issue #98** — ROOT CAUSED at filing time: parameter-contract trap in FileTool (replacement read from `content`; `new_text` silently dropped; empty replacement = unguarded delete). Sibling hole: `write` with missing `content` truncates to 0 bytes + success. **FIXED: PR #99** (branch fix/98-filetool-edit-param-trap, commit 95ae224; 10-case FileToolTests all green).
- Incident 2 (consolidation create id=0) → **comment on #76** (not a new issue — fix merged Sep 12 in PR #80; Buffett's instance redeployed on post-fix `:dev` (pushed Sep 14 21:19 UTC) at Sep 16 18:59 UTC, *after* his report; no post-fix sighting on record; watch + reopen if recurs).

**Timeline (verified Sep 17):**
- Sep 12 19:12 UTC — PR #80 merged (statement-scoped ids, id=0 fix)
- Sep 14 21:19 UTC — `mrsommer/animus-sudo:dev` pushed (contains #80)
- Sep 16 16:35 UTC — Incident 1 occurs (pre-redeploy build)
- Sep 16 17:15 UTC — Buffett's rundown (id=0 status cites Aug 15/Aug 31 evidence — pre-fix vantage)
- Sep 16 18:59 UTC — animus-tradingbot_animus service updated on `:dev`

**Lessons:** Buffett's verify-after-write discipline (grep + line count + second-source read) converted both failures into same-minute catches — cheap discipline, kept. The systemic point stands and generalizes: enforcement plumbing is only as good as its write path; a falsely-successful write is worse than a failed one because it manufactures confidence.

---

# TOOLING INCIDENT RUNDOWN — Silent Write-Path Failures (Sep 16, 2026)

Prepared for: Melvin (operator) — forwardable summary
Prepared by: Buffett, Sep 16 ~17:15 UTC
Scope: Two silent-failure patterns in the tooling stack, both caught today, plus mitigation now in force.

---

## Incident 1 — File-tool `edit` delete-without-insert (Sep 16 ~16:35 UTC)

What happened: While repairing the corrupted trail-table header in trading/positions.md, the file tool's edit action (old_text = corrupted fragment, new_text = rebuilt header + row) DELETED the target text but never inserted the replacement. The file was left 284 lines — the corruption "fixed" by removal, not repair.

How it was caught: Post-write verification (grep for the new content, line count, window read). The first verification read returned stale line numbering, which triggered an independent shell check — grep found nothing, wc showed no line change. Two-source verification caught it.

Impact if uncaught: The trail table would have silently lost its header AND the Sep 16 checkpoint row — the exact "temporally grounded" record you rely on. A later run appending to a headerless table would have compounded the corruption.

Root cause (suspected): The edit action's replace-after-match path performed the delete half of the operation and no-op'd the insert. No error, no exit code, no warning — a true silent failure. Consistent with the documented flaky-write pattern from Sep 9 (memory file_write "Failed to create file" but file actually created).

Mitigation now in force:
- Ledger-critical file edits go through shell (awk/sed) with guards: abort-if-already-present (grep -q), atomic tmp+mv, then grep/wc verification.
- Rule adopted: any file-tool edit on ledger-critical files is treated as UNVERIFIED until independently confirmed (grep for expected content + line-count delta).
- Appends via appendLines verified working (the watchlist shortlist append succeeded and verified) — failure mode is edit-specific, not append-wide.
- Flagged to Kestrel for a package-level fix (file edit silent no-op on insert-after-match).

## Incident 2 — Consolidation create id=0 (documented since Aug 15, recurring)

What happened: The memory consolidation pipeline's observation-create calls intermittently return id=0 — the write is claimed but no observation ID is minted, so the observation is silently lost unless a memory-file workaround is used. Documented as operational debt in the Aug 31 self-audit; still unresolved.

Impact: Consolidation coverage gaps — observations that exist in session context but never persist to the observation store. Mitigated by writing critical content to memory files / diary as fallback (which is why this rundown exists as a file too).

Status: Known issue, workaround in place (memory-file fallback for anything consolidation-critical), awaiting platform fix.

## Systemic point (why this matters beyond two incidents)

Our enforcement plumbing — pre-registered triggers, trail tables, schedule prompts, the doc chain (watchlist → playbook → positions.md) — is only as good as its WRITE PATH. A trigger that was "pre-registered" into a file that silently failed to update is worse than no trigger: it manufactures false confidence. Two independent silent failures (Aug 15 consolidation; Sep 16 file edit) plus one silent-failure class avoided by luck (Sep 14 orders-route HTTP 0s, caught because we probed with zero-fill-risk orders during a closed market).

The mitigation is a discipline, not a tool: verify-after-write on anything ledger-critical — grep for the new content, check line counts, re-read from a second path. Cheap (seconds), and it converted both of today's failures from "would have corrupted the ledger" into "caught in the same minute."

## Status / asks

1. Kestrel: file-tool edit silent no-op (insert-after-match path) — repro: edit with old_text present exactly once, new_text multi-line; result deletes old, inserts nothing, reports success.
2. Consolidation id=0 — open since Aug 15, workaround documented, needs a real fix.
3. No data loss today — both failures caught and repaired with verified re-writes (positions.md trail header + Sep 16 row reconstructed from canonical record; verified 287 lines).
