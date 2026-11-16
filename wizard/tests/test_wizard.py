#!/usr/bin/env python3
"""test_wizard.py - Drives the interactive wizard's REPL over a real pipe and
asserts on what it prints, the same way a person typing into it would
experience it.

No default credentials, same reason as the web Playwright suite and the
plain CLI's tests/test_cli.sh: a hardcoded fallback account that silently
stops existing makes every test "pass" by failing to log in.

    SSAPI=~/dev/ssapi python3 wizard/tests/test_wizard.py

starts a throwaway local server (tests/bench.sh) and logs in as its seeded
alice@test.local. It never points at dev or prod. Against another server:

    TEST_BASE_URL=http://host TEST_EMAIL=... TEST_PASSWORD='...' python3 wizard/tests/test_wizard.py
 The binary is run with HOME pointed at a scratch
directory for the whole run, so it never touches a real
~/.simple-social-cli or ~/.config/simple-social-cli.
"""
import fcntl
import os
import re
import subprocess
import sys
import tempfile
import time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BIN = os.path.join(REPO_ROOT, "simple-social-cli-interactive")
BASE_URL = os.environ.get("TEST_BASE_URL")
EMAIL = os.environ.get("TEST_EMAIL")
PASSWORD = os.environ.get("TEST_PASSWORD")
BENCH = None
if not BASE_URL:
    if not os.environ.get("SSAPI"):
        print("Set SSAPI to an ssapi checkout (to start a local server), or TEST_BASE_URL + TEST_EMAIL + TEST_PASSWORD.", file=sys.stderr)
        sys.exit(1)
    BENCH = tempfile.mkdtemp()
    env = dict(os.environ, PORT=os.environ.get("PORT", "8192"))
    BASE_URL = subprocess.run([os.path.join(REPO_ROOT, "tests", "bench.sh"), "start", BENCH],
                              env=env, capture_output=True, text=True, check=True).stdout.strip().splitlines()[-1]
    EMAIL, PASSWORD = "alice@test.local", "Passw0rd!"

    import atexit
    atexit.register(lambda: subprocess.run([os.path.join(REPO_ROOT, "tests", "bench.sh"), "stop", BENCH]))

if not EMAIL or not PASSWORD:
    print("Set TEST_EMAIL and TEST_PASSWORD (a real account on "
          f"{BASE_URL}) before running.", file=sys.stderr)
    sys.exit(1)

if not os.path.exists(BIN):
    print("Building...", file=sys.stderr)
    if subprocess.run(["make"], cwd=REPO_ROOT).returncode != 0:
        print("build failed", file=sys.stderr)
        sys.exit(1)

passed = 0
failed = 0
failures = []


def fail(label, detail):
    global failed
    failed += 1
    failures.append(f"{label}: {detail}")
    print(f"FAIL: {label}: {detail}")


def ok():
    global passed
    passed += 1


class Wizard:
    """Feeds lines to the REPL's stdin and reads whatever's accumulated on
    stdout since the last read, with a timeout per read so a wedged prompt
    fails one assertion instead of hanging the whole run."""

    def __init__(self, home_dir):
        env = dict(os.environ)
        env["HOME"] = home_dir
        self.proc = subprocess.Popen(
            [BIN], cwd=REPO_ROOT, env=env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=0,   # raw bytes, unbuffered
        )
        # select() reports readiness at the OS pipe level; a buffered
        # TextIOWrapper's first read can pull a whole chunk into its own
        # internal buffer, after which select() correctly says "nothing new"
        # even though plenty of already-received data is sitting right there
        # unread. Sidestepping that entirely: put the raw fd itself in
        # non-blocking mode and read directly from it, so every read returns
        # whatever the OS actually has, immediately, with no buffering layer
        # in between to get out of sync with select().
        fd = self.proc.stdout.fileno()
        flags = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
        self.buf = ""

    def send(self, line):
        self.proc.stdin.write((line + "\n").encode())
        self.proc.stdin.flush()

    def read_until(self, pattern, timeout=8):
        """Reads until `pattern` (a substring) appears in the accumulated
        output since the last read_until call, or times out. Returns
        (found, chunk) - chunk is everything read this call."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if pattern in self.buf:
                found, self.buf = self.buf, ""
                return True, found
            try:
                data = os.read(self.proc.stdout.fileno(), 4096)
            except BlockingIOError:
                data = b""
            if data:
                self.buf += data.decode(errors="replace")
            else:
                time.sleep(0.05)
        found = pattern in self.buf
        out = self.buf
        self.buf = ""
        return found, out

    def close(self):
        try:
            self.send("exit")
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


def expect(w, label, pattern, timeout=8):
    found, out = w.read_until(pattern, timeout)
    if found:
        ok()
    else:
        fail(label, f"never saw {pattern!r} within {timeout}s. Got: {out!r}")
    return out


with tempfile.TemporaryDirectory() as home:
    os.makedirs(os.path.join(home, ".config", "simple-social-cli"))
    with open(os.path.join(home, ".config", "simple-social-cli", "config.ini"), "w") as f:
        f.write(f"base_url = {BASE_URL}/api.php\n")

    w = Wizard(home)

    print("== boot ==")
    expect(w, "banner", "Simple Social CLI (interactive)")

    print("== auth-required command before login ==")
    w.send("feed")
    expect(w, "feed requires auth", "Not logged in")

    print("== the just-fixed trailing-argument bug ==")
    w.send("login me@x.com")
    expect(w, "trailing arg on login gives the specific hint, not 'Unknown command'",
           "doesn't take arguments on this line")

    print("== genuinely unknown command ==")
    w.send("frobnicate")
    expect(w, "unknown command still reported", "Unknown command: frobnicate")

    print("== login flow ==")
    w.send("login")
    expect(w, "email prompt", "Email:")
    w.send(EMAIL)
    expect(w, "password prompt", "Password:")
    w.send(PASSWORD)
    out = expect(w, "logged in", "Logged in as")
    if EMAIL not in out:
        fail("login response contains the right email", out)
    else:
        ok()

    print("== prompt reflects logged-in state ==")
    w.send("help")
    expect(w, "prompt shows email after login", f"({EMAIL})")

    print("== feed with default answers ==")
    w.send("feed")
    expect(w, "limit prompt", "Limit")
    w.send("")   # accept default
    expect(w, "offset prompt", "Offset")
    w.send("")   # accept default
    expect(w, "feed printed without crashing", "simple-social")

    print("== post create/comment/like/delete lifecycle ==")
    marker = f"wizard-test-{int(time.time())}"
    w.send("create")
    expect(w, "text prompt", "Text:")
    w.send(marker)
    expect(w, "media prompt", "Media file path")
    w.send("")  # no media
    out = expect(w, "post created", "Created post")
    m = re.search(r"Created post (\S+)", out)
    if not m:
        fail("extract post id from create output", out)
        post_id = None
    else:
        ok()
        post_id = m.group(1)

    if post_id:
        w.send("like")
        expect(w, "like post-id prompt", "Post ID:")
        w.send(post_id)
        expect(w, "self-like is rejected, not silently accepted or crashed",
               "Cannot like your own post")

        w.send("comment")
        expect(w, "comment post-id prompt", "Post ID:")
        w.send(post_id)
        expect(w, "comment text prompt", "Text:")
        w.send("wizard test comment")
        out = expect(w, "comment added", "added to")
        cm = re.search(r"Comment (\d+) added", out)

        if cm:
            print("== reply and threads ==")
            w.send("reply")
            expect(w, "reply post-id prompt", "Post ID:")
            w.send(post_id)
            expect(w, "reply comment-id prompt", "Comment ID:")
            w.send(cm.group(1))
            expect(w, "reply text prompt", "Text:")
            w.send("wizard test reply")
            expect(w, "reply added", "added under comment")
            w.send("comments")
            expect(w, "comments post-id prompt", "Post ID:")
            w.send(post_id)
            expect(w, "comments offset prompt", "Offset")
            w.send("")
            expect(w, "the reply is shown under its comment", "wizard test reply")

        print("== times are UTC ==")
        w.send("post")
        expect(w, "post id prompt", "Post ID:")
        w.send(post_id)
        expect(w, "post date in UTC", "Date:   UTC ")

        w.send("delete")
        expect(w, "delete post-id prompt", "Post ID:")
        w.send(post_id)
        expect(w, "post deleted", "Deleted post")

    print("== safety, invites, devices ==")
    w.send("blocked")
    expect(w, "blocked list", "haven't blocked anyone")
    w.send("invite")
    expect(w, "invite note", "choose wisely")
    w.send("n")
    w.send("devices")
    expect(w, "devices list", "(this device)")
    w.send("")
    w.send("whoami")
    expect(w, "whoami shows the role", "Role:")

    print("== logout ==")
    w.send("logout")
    expect(w, "logged out", "Logged out")
    w.send("help")
    expect(w, "prompt drops the email after logout", "simple-social>")

    print("== exit ==")
    w.send("exit")
    expect(w, "goodbye", "Goodbye")

    w.close()

print()
print("==================================")
print(f"  {passed} passed, {failed} failed")
print("==================================")
sys.exit(1 if failed else 0)
