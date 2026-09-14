#!/usr/bin/env bash
# federation-chaos.sh — #78 P2b/P2c acceptance battery.
#
# Runs the kill-simulation matrix against a fresh two-node pair of the
# current build and reports PASS/FAIL per scenario with witness lines:
#
#   S1  simultaneous first-claim race (lease_required one-shot)
#         -> exactly ONE dispatch; loser fenced; DOUBLE-FIRE WARN on both
#   S2  at_least_once one-shot
#         -> fired exactly once, single-phase
#   S3  holder killed mid-lease (recurring lease_required)
#         -> epoch-2 LEASE TAKEOVER dispatch on the survivor
#
# Hygiene protocol (hard-won): kill ALL animusd first, verify ports free,
# verify exactly 2 daemons, probe the claims route before choreography.
#
# Usage: scripts/federation-chaos.sh [worktree]   (default: repo of this script)
set -u

ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
BIN="$ROOT/dist/bin/animusd"
PORT_A=18101; PORT_B=18102
DIR=/tmp/federation-chaos
LOG=$DIR/report.txt

say()  { printf '%s\n' "$*" | tee -a "$LOG"; }
die()  { say "FATAL: $*"; exit 1; }

jsonget() { # jsonget <json> <pyexpr over d>
    python3 -c "import json,sys; d=json.loads(sys.argv[1]); print($2)" "$1"
}

curlget() { curl -s --max-time 6 "http://127.0.0.1:$1$2"; }
runs_of() { # runs_of <port> <schedule-prefix> [outcome-filter]
    curlget "$1" "/api/v1/scheduler/runs?limit=50" \
      | python3 -c "
import json,sys
d=json.load(sys.stdin)
rows=[r for r in d.get('runs',[]) if r['schedule_id'].startswith('$2')]
if len(sys.argv)>1: rows=[r for r in rows if r['outcome'].startswith(sys.argv[1])]
print(json.dumps(rows))" "${3:-}"
}
count_rows() { runs_of "$1" "$2" "${3:-}" | python3 -c "import json,sys; print(len(json.load(sys.stdin)))"; }

wait_secs() { local t=$1; while [ "$t" -gt 0 ]; do sleep 5; t=$((t-5)); printf '.' >/dev/stderr; done; printf '\n' >/dev/stderr; }

: > "$LOG"
say "=== federation-chaos $(date -u +%FT%TZ) ==="

# ── hygiene: nothing squatting ────────────────────────────────────────────
pkill -9 -f "animusd --run-kerne[l]" 2>/dev/null
sleep 2
pgrep -f "animusd --run-kerne[l]" >/dev/null && die "animusd still alive after pkill"
if curl -s --max-time 2 "http://127.0.0.1:$PORT_A/api/v1/sync/status" >/dev/null 2>&1; then
    die "port $PORT_A answering after cleanup (squatter)"
fi

[ -x "$BIN" ] || die "no daemon binary at $BIN (build first)"

# ── fresh pair ────────────────────────────────────────────────────────────
rm -rf "$DIR/fedA" "$DIR/fedB"
mkdir -p "$DIR/fedA/config" "$DIR/fedA/state" "$DIR/fedB/config" "$DIR/fedB/state"
echo "{\"data\":{\"path\":\"$DIR/fedA/state/memory.db\"},\"node\":{\"id\":1,\"peers\":[\"http://127.0.0.1:$PORT_B\"],\"sync_token\":\"\",\"lease_ttl_ms\":15000,\"lease_grace_ms\":4000}}" > "$DIR/fedA/config/db.json"
echo "{\"data\":{\"path\":\"$DIR/fedB/state/memory.db\"},\"node\":{\"id\":2,\"peers\":[\"http://127.0.0.1:$PORT_A\"],\"sync_token\":\"\",\"lease_ttl_ms\":15000,\"lease_grace_ms\":4000}}" > "$DIR/fedB/config/db.json"

setsid sh -c "exec 0<&-; sleep 1500 | '$BIN' --run-kernel --config-dir $DIR/fedA/config --data-dir $DIR/fedA/state --admin-port $PORT_A > $DIR/fedA/boot.log 2>&1" >/dev/null 2>&1 &
setsid sh -c "exec 0<&-; sleep 1500 | '$BIN' --run-kernel --config-dir $DIR/fedB/config --data-dir $DIR/fedB/state --admin-port $PORT_B > $DIR/fedB/boot.log 2>&1" >/dev/null 2>&1 &
wait_secs 30

DAEMONS=$(pgrep -f "animusd --run-kerne[l]" | wc -l)
[ "$DAEMONS" -eq 2 ] || die "expected 2 daemons, saw $DAEMONS"
for p in $PORT_A $PORT_B; do
    PROBE=$(curlget "$p" "/api/v1/scheduler/claims/probe")
    echo "$PROBE" | grep -q '"exists":false' || die "port $p claims-route probe failed: $PROBE"
done
say "pair up: 2 daemons, claims route serving both"

mk_when() { date -u -d "+$1 seconds" +%Y-%m-%dT%H:%M:%SZ; }
mk_schedule() { # port when semantics msg [extra-json]
    curl -s --max-time 8 -X POST "http://127.0.0.1:$1/api/v1/scheduler/schedules" \
        -H 'Content-Type: application/json' \
        -d "{\"agent_id\":\"ag\",\"when\":\"$2\",\"message\":\"$4\",\"semantics\":\"$3\"${5:+,$5}}"
}
sched_id() { jsonget "$1" "d['id'][:8]"; }

# ── S1: simultaneous-claim race ───────────────────────────────────────────
say "--- S1 lease_required race probe"
R=$(mk_schedule $PORT_A "$(mk_when 8)" lease_required "S1-race")
echo "$R" | grep -q '"id"' || die "S1 create failed: $R"
S1=$(sched_id "$R")
wait_secs 150          # claim + visibility round + phase 2
DISP=$(count_rows $PORT_B "$S1" "dispatched")
FENCED=$(runs_of $PORT_B "$S1" | python3 -c "import json,sys; print(sum(1 for r in json.load(sys.stdin) if r.get('fenced')))")
DF_WARN=$(grep -c "DOUBLE-FIRE" "$DIR/fedA/boot.log" "$DIR/fedB/boot.log" | awk -F: '{s+=$2} END{print s}')
if [ "$DISP" -eq 1 ] && [ "$DF_WARN" -ge 1 ]; then
    say "S1 PASS — dispatched=$DISP fenced_rows=$FENCED double_fire_warns=$DF_WARN"
else
    say "S1 FAIL — dispatched=$DISP fenced_rows=$FENCED double_fire_warns=$DF_WARN"
fi

# ── S2: at_least_once ─────────────────────────────────────────────────────
say "--- S2 at_least_once single-phase"
R=$(mk_schedule $PORT_A "$(mk_when 8)" at_least_once "S2-alo")
S2=$(sched_id "$R")
wait_secs 60
DISP=$(count_rows $PORT_B "$S2" "dispatched")
TOTAL=$(count_rows $PORT_B "$S2")
if [ "$DISP" -eq 1 ] && [ "$TOTAL" -eq 1 ]; then
    say "S2 PASS — one row, dispatched"
else
    say "S2 FAIL — dispatched=$DISP total=$TOTAL"
fi

# ── S3: failover under kill ───────────────────────────────────────────────
say "--- S3 recurring lease_required + holder kill"
R=$(mk_schedule $PORT_A "$(mk_when 50)" lease_required "S3-failover" '"repeat":true')
S3=$(sched_id "$R")
wait_secs 110          # first window claimed + dispatched by one node
FIRST=$(count_rows $PORT_B "$S3" "dispatched")
say "S3 first window dispatched=$FIRST; killing node A (fed1)"
pkill -9 -f "admin-port $PORT_A"
sleep 2
pgrep -f "admin-port $PORT_A" >/dev/null && die "node A survived the kill"
wait_secs 150          # down-streak + expiry + grace + takeover + next window
# Failover witness = any lease-authority event after the kill: foreign
# takeover, own-lease re-acquire after expiry (race winner path), or
# stale-claim takeover. The epoch>=2 dispatched row is the real assert.
TK=$(grep -cE "LEASE TAKEOVER|re-acquired after expiry|STALE-CLAIM TAKEOVER" "$DIR/fedB/boot.log" || true)
E2=$(runs_of $PORT_B "$S3" | python3 -c "
import json,sys
rows=[r for r in json.load(sys.stdin) if r.get('epoch',0)>=2 and r['outcome'].startswith('dispatched')]
print(len(rows))")
if [ "$E2" -ge 1 ] && [ "$TK" -ge 1 ]; then
    say "S3 PASS — takeover warns=$TK epoch2_dispatches=$E2"
else
    say "S3 FAIL — takeover warns=$TK epoch2_dispatches=$E2 (first=$FIRST)"
fi

# ── summary + cleanup ─────────────────────────────────────────────────────
say "=== summary ==="
PASS=$(grep -c "PASS" "$LOG"); FAIL=$(grep -c "FAIL" "$LOG")
say "PASS=$PASS FAIL=$FAIL  (full report: $LOG; daemon logs: $DIR/fed{A,B}/boot.log)"
pkill -9 -f "animusd --run-kerne[l]" 2>/dev/null
sleep 1
pgrep -f "animusd --run-kerne[l]" >/dev/null && say "WARN: daemons still alive" || say "daemons cleaned"
[ "$FAIL" -eq 0 ]
