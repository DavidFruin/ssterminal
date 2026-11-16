#!/usr/bin/env bash
# tests/test_cli.sh - Exercises the built CLI binary end to end, in both
# human and --json output modes.
#
# Normally runs against a throwaway LOCAL server (tests/bench.sh), never dev
# or prod. It needs an ssapi checkout:
#
#   SSAPI=~/dev/ssapi ./tests/test_cli.sh
#
# The bench seeds owner@, alice@, bob@ and carol@test.local (password
# Passw0rd!) and captures email in a file, so registration with an invite
# code, password reset and every new command can be tested.
#
# Against another server instead (only the basic cases run there):
#   TEST_BASE_URL=http://host TEST_EMAIL=... TEST_PASSWORD='...' ./tests/test_cli.sh
#
# Runs the binary with HOME pointed at a scratch directory for the whole
# run, so it never touches your real ~/.simple-social-cli or
# ~/.config/simple-social-cli, and cleans that scratch dir up on exit
# whether the run passed or failed.

set -u
cd "$(dirname "$0")/.."

BIN="./simple-social-cli"
BENCH=""
if [ -z "${TEST_BASE_URL:-}" ]; then
  if [ -z "${SSAPI:-}" ]; then
    echo "Set SSAPI to an ssapi checkout (to start a local server), or TEST_BASE_URL + TEST_EMAIL + TEST_PASSWORD." >&2
    exit 1
  fi
  BENCH="$(mktemp -d)"
  BASE_URL="$(PORT=${PORT:-8191} tests/bench.sh start "$BENCH")" || { echo "bench failed to start" >&2; exit 1; }
  TEST_EMAIL=alice@test.local
  TEST_PASSWORD='Passw0rd!'
else
  BASE_URL="$TEST_BASE_URL"
  if [ -z "${TEST_EMAIL:-}" ] || [ -z "${TEST_PASSWORD:-}" ]; then
    echo "Set TEST_EMAIL and TEST_PASSWORD (a real account on $BASE_URL) before running." >&2
    exit 1
  fi
fi

if [ ! -x "$BIN" ]; then
  echo "Building..." >&2
  make >&2 || { echo "build failed"; exit 1; }
fi

SCRATCH_HOME="$(mktemp -d)"
mkdir -p "$SCRATCH_HOME/.config/simple-social-cli"
echo "base_url = $BASE_URL/api.php" > "$SCRATCH_HOME/.config/simple-social-cli/config.ini"
export HOME="$SCRATCH_HOME"

CLEANUP_POST_IDS=()
cleanup() {
  for pid in "${CLEANUP_POST_IDS[@]:-}"; do
    [ -n "$pid" ] && "$BIN" --json delete "$pid" > /dev/null 2>&1
  done
  rm -rf "$SCRATCH_HOME"
  if [ -n "$BENCH" ]; then tests/bench.sh stop "$BENCH"; rm -rf "$BENCH"; fi
}
trap cleanup EXIT

PASS=0
FAIL=0
fail() { echo "FAIL: $1"; FAIL=$((FAIL + 1)); }
ok()   { PASS=$((PASS + 1)); }

# Runs a command and asserts its exit code, printing stdout only (the app
# separates streams deliberately - "Error: ..." on stderr, clean JSON on
# stdout, verified by hand before this script existed - so a caller that
# wants to validate JSON needs stdout alone, not stdout+stderr merged).
# $1 = label, $2 = expected exit code, rest = command.
run() {
  local label="$1" want_rc="$2"; shift 2
  local out err rc
  err="$(mktemp)"
  out="$("$@" 2>"$err")"; rc=$?
  if [ "$rc" != "$want_rc" ]; then
    fail "$label: exit $rc, wanted $want_rc (stdout: $out / stderr: $(cat "$err"))"
    rm -f "$err"
    return 1
  fi
  rm -f "$err"
  ok
  printf '%s' "$out"
}

json_field() {
  # $1 = json string, $2 = python expression on the parsed object
  python3 -c "import sys, json; d = json.loads(sys.argv[1]); print($2)" "$1" 2>/dev/null
}

assert_valid_json() {
  local label="$1" json="$2"
  if echo "$json" | python3 -m json.tool > /dev/null 2>&1; then ok; else
    fail "$label: not valid JSON: $json"
  fi
}

echo "== auth =="
run "login" 0 "$BIN" login "$TEST_EMAIL" "$TEST_PASSWORD" > /dev/null

out=$(run "whoami --json" 0 "$BIN" --json whoami)
assert_valid_json "whoami --json shape" "$out"
[ "$(json_field "$out" 'd["email"]')" = "$TEST_EMAIL" ] && ok || fail "whoami email mismatch: $out"

run "whoami (human)" 0 "$BIN" whoami > /dev/null

echo "== reads, --json validity =="
for cmd in "feed --limit 3" "posts --limit 3" "users" "profile" \
           "followers" "following" "notifications --limit 3" "notify-count" \
           "blocked" "invite" "devices"; do
  out=$(run "$cmd --json" 0 "$BIN" --json $cmd)
  assert_valid_json "$cmd --json" "$out"
done

echo "== reads, human mode doesn't crash =="
for cmd in "feed --limit 2" "users" "profile" "notify-count"; do
  run "$cmd (human)" 0 "$BIN" $cmd > /dev/null
done

echo "== post lifecycle =="
MARKER="cli-test-$(date +%s)-$$"
out=$(run "create" 0 "$BIN" --json create "$MARKER")
assert_valid_json "create shape" "$out"
POST_ID="$(json_field "$out" 'd["postId"]')"
if [ -z "$POST_ID" ]; then
  fail "create: no postId in response: $out"
else
  ok
  CLEANUP_POST_IDS+=("$POST_ID")

  out=$(run "post <id> --json" 0 "$BIN" --json post "$POST_ID")
  [ "$(json_field "$out" 'd["post"]["text"]')" = "$MARKER" ] && ok || fail "post text mismatch: $out"

  # The web UI hides the like button on your own posts; confirm the server
  # enforces the same rule rather than just the UI, and that the error
  # comes back as a clean, parseable envelope on both streams.
  out=$(run "like own post (must fail)" 1 "$BIN" --json like "$POST_ID")
  assert_valid_json "like-own-post error shape" "$out"
  [ "$(json_field "$out" 'd["ok"]')" = "False" ] && ok || fail "like-own-post: expected ok:false, got $out"

  # Unlike on a post you never liked is deliberately idempotent server-side
  # (postFound/wasLiked guard in api.php) - exit 0, not an error.
  run "unlike (idempotent, not liked)" 0 "$BIN" --json unlike "$POST_ID" > /dev/null

  out=$(run "comment" 0 "$BIN" --json comment "$POST_ID" "cli-test comment")
  COMMENT_ID="$(json_field "$out" 'd["commentId"]')"
  if [ -z "$COMMENT_ID" ]; then
    fail "comment: no commentId in response: $out"
  else
    ok
    out=$(run "comments --json" 0 "$BIN" --json comments "$POST_ID")
    assert_valid_json "comments shape" "$out"
    [ "$(json_field "$out" 'd["totalCount"]')" = "1" ] && ok || fail "comments totalCount: $out"
    out=$(run "reply" 0 "$BIN" --json reply "$POST_ID" "$COMMENT_ID" "cli-test reply")
    [ "$(json_field "$out" 'd["parentId"]')" = "$COMMENT_ID" ] && ok || fail "reply: $out"
    out=$(run "comments threaded --json" 0 "$BIN" --json comments "$POST_ID")
    [ "$(json_field "$out" '[c["parent_id"] for c in d["comments"]]')" = "[None, $COMMENT_ID]" ] && ok || fail "threaded: $out"
    run "delete-comment" 0 "$BIN" --json delete-comment "$COMMENT_ID" > /dev/null
  fi

  run "delete post" 0 "$BIN" --json delete "$POST_ID" > /dev/null
  CLEANUP_POST_IDS=()   # already deleted, don't double-delete in the trap

  out=$(run "post <id> after delete (must 404)" 1 "$BIN" --json post "$POST_ID")
  [ "$(json_field "$out" 'd["ok"]')" = "False" ] && ok || fail "post-after-delete shape: $out"
fi


if [ -n "$BENCH" ]; then
  MAIL="$BENCH/mail.log"
  code_from_mail() { sleep 1; grep -oE '[0-9]{6}' "$MAIL" | tail -1; }
  other() { HOME="$SCRATCH_HOME/$1" "$BIN" "${@:2}"; }
  for u in owner bob newbie; do
    mkdir -p "$SCRATCH_HOME/$u/.config/simple-social-cli"
    cp "$SCRATCH_HOME/.config/simple-social-cli/config.ini" "$SCRATCH_HOME/$u/.config/simple-social-cli/"
  done
  other owner login owner@test.local 'Passw0rd!' >/dev/null 2>&1
  other bob login bob@test.local 'Passw0rd!' >/dev/null 2>&1

  echo "== registration with an invite code (local server) =="
  CODE="$(other owner --json invite --generate | python3 -c 'import sys,json; print(json.load(sys.stdin)["code"])')"
  [ -n "$CODE" ] && ok || fail "owner could not make a code"
  out=$(run "register with a wrong code" 1 other newbie --json register 'Wr0ng!xy' newbie@test.local 'Passw0rd!' 'Passw0rd!' --otp 000000 --agree)
  run "json register without --agree is refused" 1 other newbie --json register "$CODE" newbie@test.local 'Passw0rd!' 'Passw0rd!' --otp 000000 > /dev/null
  : > "$MAIL"
  run "send-otp with the code" 0 other newbie --json send-otp newbie@test.local "$CODE" > /dev/null
  OTP="$(code_from_mail)"
  run "register" 0 other newbie --json register "$CODE" newbie@test.local 'Passw0rd!' 'Passw0rd!' --otp "$OTP" --agree > /dev/null
  out=$(run "new member is logged in" 0 other newbie --json whoami)
  [ "$(json_field "$out" 'd["email"]')" = "newbie@test.local" ] && ok || fail "new member whoami: $out"
  err="$(other newbie whoami 2>&1 >/dev/null)"
  case "$err" in *"Terms of Use have changed"*) fail "terms notice after agreeing at registration";; *) ok;; esac

  echo "== password reset (local server) =="
  : > "$MAIL"
  run "forgot-password" 0 other bob --json forgot-password bob@test.local > /dev/null
  OTP="$(code_from_mail)"
  run "reset-password" 0 other bob --json reset-password bob@test.local "$OTP" 'N3w-Passw0rd!' 'N3w-Passw0rd!' > /dev/null
  run "login with the new password" 0 other bob --json login bob@test.local 'N3w-Passw0rd!' > /dev/null

  echo "== report, fold, block (local server) =="
  BPOST="$(other bob --json create "bob's post" | python3 -c 'import sys,json; print(json.load(sys.stdin)["postId"])')"
  run "report a post" 0 "$BIN" --json report post "$BPOST" spam "test" > /dev/null
  out=$(run "the post is marked reported" 0 "$BIN" --json post "$BPOST")
  [ "$(json_field "$out" 'd["post"]["reportedByMe"]')" = "True" ] && ok || fail "reportedByMe: $out"
  case "$("$BIN" post "$BPOST")" in *"You reported this post"*) ok;; *) fail "post not folded";; esac
  case "$("$BIN" post "$BPOST" --show-reported)" in *"bob's post"*) ok;; *) fail "--show-reported";; esac
  run "report with a made-up reason is refused" 1 "$BIN" report post "$BPOST" because > /dev/null
  run "block" 0 "$BIN" --json block 3 > /dev/null
  out=$(run "blocked list" 0 "$BIN" --json blocked)
  [ "$(json_field "$out" '[u["id"] for u in d["users"]]')" = "[3]" ] && ok || fail "blocked list: $out"
  run "unblock" 0 "$BIN" --json unblock 3 > /dev/null

  echo "== notifications, profile, times (local server) =="
  out=$(run "profile has a role" 0 "$BIN" --json profile 1)
  [ "$(json_field "$out" 'd["role"]')" = "owner" ] && ok || fail "profile role: $out"
  case "$("$BIN" post "$BPOST" --show-reported)" in *"Date:   UTC "*) ok;; *) fail "times not in UTC";; esac
  out=$(run "post records the poster's GMT offset" 0 "$BIN" --json post "$BPOST")
  [ "$(json_field "$out" 'd["post"]["timestampTz"]')" = "$(date +%:z)" ] && ok || fail "timestampTz: $out"

  echo "== devices (local server) =="
  out=$(run "devices" 0 "$BIN" --json devices)
  [ "$(json_field "$out" 'sum(1 for s in d["sessions"] if s["isCurrent"])')" = "1" ] && ok || fail "devices: $out"
fi

echo "== error handling =="
run "unknown command" 1 "$BIN" this-is-not-a-command > /dev/null
run "missing required arg" 1 "$BIN" comment > /dev/null
run "--help" 0 "$BIN" --help > /dev/null

echo "== stream separation (--json goes to stdout, errors to stderr) =="
STDOUT_ONLY="$("$BIN" --json post "1.0" 2>/dev/null)"
assert_valid_json "stdout-only is clean JSON" "$STDOUT_ONLY"

echo "== session persistence across separate process runs =="
"$BIN" whoami > /dev/null 2>&1
run "second process, still logged in" 0 "$BIN" whoami > /dev/null

echo "== logout =="
run "logout" 0 "$BIN" logout > /dev/null
out=$(run "whoami after logout (must fail)" 1 "$BIN" --json whoami)
assert_valid_json "logged-out error shape" "$out"

echo
echo "=================================="
echo "  $PASS passed, $FAIL failed"
echo "=================================="
[ "$FAIL" -eq 0 ]
