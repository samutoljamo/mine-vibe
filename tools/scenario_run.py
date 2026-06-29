#!/usr/bin/env python3
"""End-to-end gameplay scenario runner for the headless agent harness.

Spawns `./build/minecraft --agent --headless`, feeds it the command lines from a
`.scenario` file, captures the JSON event stream, and evaluates `expect`
assertions against the most recent `state` snapshot. Exits 0 if every assertion
passes, 1 (with a diagnostic) on the first failure / crash / timeout.

Usage:
    scenario_run.py <scenario-file> [binary]

The minecraft binary is resolved from, in order:
  1. the optional second CLI argument,
  2. the MINECRAFT_BIN environment variable,
  3. ./build/minecraft relative to the repo root.

------------------------------------------------------------------ FILE FORMAT
A `.scenario` file is a line-oriented text file:

  # comment            -- lines starting with '#' are ignored; blank lines too.
  > {json command}     -- send this JSON command verbatim to the harness stdin.
  expect <path> <op> <value>
                       -- assert against the LATEST `state` event JSON.

The harness emits a fresh `{"event":"state",...}` line after EVERY command, so
the runner always has an up-to-date snapshot. An `expect` evaluates against the
most recent state seen so far; if no state has been observed yet, the runner
issues an implicit `{"cmd":"get_state"}` first.

`expect` paths are dotted with optional `[index]` subscripts, e.g.:
    health
    food
    inventory[3].count
    inventory[3].item
    mobs[0].health
    target.block
    pos[1]
    container.slots[0].item
    weather

Supported ops:
    ==  !=  >  >=  <  <=        -- numeric where both sides parse as numbers,
                                   else string compare for == / !=.
    contains                    -- substring (string actual) or membership
                                   (list/dict actual); also matches any element
                                   field for a list of dicts is NOT done — use an
                                   explicit index instead.

Values are parsed as JSON when possible (numbers, true/false/null, quoted
strings); otherwise treated as a bare string.

Every scenario runs under a wall-clock timeout (default 60s, override with the
SCENARIO_TIMEOUT env var). A `> {"cmd":"quit"}` should end every scenario so the
harness exits cleanly; the runner appends one if the file omits it.
"""

import json
import os
import subprocess
import sys
import threading


def fail(msg):
    sys.stderr.write("SCENARIO FAIL: " + msg + "\n")
    sys.exit(1)


def resolve_binary(argv):
    if len(argv) >= 3:
        return argv[2]
    env = os.environ.get("MINECRAFT_BIN")
    if env:
        return env
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(here, "..", "build", "minecraft")


def parse_value(tok):
    """Parse an expect RHS token: JSON if possible, else bare string."""
    try:
        return json.loads(tok)
    except (ValueError, TypeError):
        return tok


def get_path(obj, path):
    """Resolve a dotted/indexed path like 'inventory[3].count' against obj.

    Returns (found, value). Missing keys / out-of-range indices -> (False, None).
    """
    i = 0
    cur = obj
    token = ""

    def step_key(container, key):
        if isinstance(container, dict):
            if key in container:
                return True, container[key]
            return False, None
        return False, None

    # Tokenise into a list of ('key', name) / ('idx', n).
    ops = []
    n = len(path)
    while i < n:
        c = path[i]
        if c == '.':
            if token:
                ops.append(("key", token))
                token = ""
            i += 1
        elif c == '[':
            if token:
                ops.append(("key", token))
                token = ""
            j = path.find(']', i)
            if j < 0:
                return False, None
            ops.append(("idx", int(path[i + 1:j])))
            i = j + 1
        else:
            token += c
            i += 1
    if token:
        ops.append(("key", token))

    for kind, val in ops:
        if kind == "key":
            ok, cur = step_key(cur, val)
            if not ok:
                return False, None
        else:  # idx
            if isinstance(cur, list) and 0 <= val < len(cur):
                cur = cur[val]
            else:
                return False, None
    return True, cur


def as_number(v):
    if isinstance(v, bool):
        return None
    if isinstance(v, (int, float)):
        return v
    if isinstance(v, str):
        try:
            return float(v) if ('.' in v or 'e' in v or 'E' in v) else int(v)
        except ValueError:
            return None
    return None


def evaluate(op, actual, expected):
    if op == "contains":
        if isinstance(actual, str):
            return str(expected) in actual
        if isinstance(actual, (list, tuple)):
            return expected in actual
        if isinstance(actual, dict):
            return expected in actual
        return False

    na, ne = as_number(actual), as_number(expected)
    numeric = na is not None and ne is not None

    if op == "==":
        return (na == ne) if numeric else (actual == expected)
    if op == "!=":
        return (na != ne) if numeric else (actual != expected)
    if not numeric:
        return False
    if op == ">":
        return na > ne
    if op == ">=":
        return na >= ne
    if op == "<":
        return na < ne
    if op == "<=":
        return na <= ne
    fail("unknown op %r" % op)


class Harness:
    def __init__(self, binary, timeout):
        self.proc = subprocess.Popen(
            [binary, "--agent", "--headless"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
        )
        self.timeout = timeout
        self.last_state = None
        self.state_count = 0
        self.lines = []
        self._lock = threading.Lock()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

    def _read_loop(self):
        for line in self.proc.stdout:
            line = line.strip()
            if not line:
                continue
            with self._lock:
                self.lines.append(line)
                try:
                    obj = json.loads(line)
                except ValueError:
                    continue
                if isinstance(obj, dict) and obj.get("event") == "state":
                    self.last_state = obj
                    self.state_count += 1

    def send(self, cmd_line):
        self.proc.stdin.write(cmd_line + "\n")
        self.proc.stdin.flush()

    def get_state(self):
        with self._lock:
            return self.last_state

    def get_state_count(self):
        with self._lock:
            return self.state_count

    def wait_quit(self):
        # Close stdin so the harness's blocking fgets() reader sees EOF and its
        # teardown (which joins that thread) can complete promptly after quit.
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def kill(self):
        try:
            self.proc.kill()
        except Exception:
            pass


def main(argv):
    if len(argv) < 2:
        fail("usage: scenario_run.py <scenario-file> [binary]")
    scenario_path = argv[1]
    binary = resolve_binary(argv)
    timeout = float(os.environ.get("SCENARIO_TIMEOUT", "60"))

    if not os.path.exists(binary):
        fail("minecraft binary not found at %s" % binary)

    with open(scenario_path) as f:
        raw_lines = f.readlines()

    # Parse into a step list so we know if a trailing quit is present.
    steps = []  # ('cmd', json_str) | ('expect', (path, op, value, lineno, raw))
    has_quit = False
    for lineno, raw in enumerate(raw_lines, 1):
        s = raw.strip()
        if not s or s.startswith("#"):
            continue
        if s.startswith(">"):
            body = s[1:].strip()
            steps.append(("cmd", body))
            try:
                obj = json.loads(body)
                if isinstance(obj, dict) and obj.get("cmd") == "quit":
                    has_quit = True
            except ValueError:
                fail("line %d: command is not valid JSON: %s" % (lineno, body))
        elif s.startswith("expect"):
            parts = s.split(None, 3)
            if len(parts) < 4:
                fail("line %d: malformed expect (need: expect <path> <op> <value>)"
                     % lineno)
            _, path, op, value_tok = parts
            steps.append(("expect", (path, op, parse_value(value_tok), lineno, s)))
        else:
            fail("line %d: unrecognised directive: %s" % (lineno, s))

    h = Harness(binary, timeout)

    deadline = threading.Event()

    def on_timeout():
        deadline.set()
        h.kill()

    timer = threading.Timer(timeout, on_timeout)
    timer.start()

    # The harness emits exactly one `state` event per command it processes, in
    # order. We count commands we have sent and require the state stream to catch
    # up to that count before evaluating an expect, so the snapshot reflects all
    # commands issued so far (and not a stale earlier one). This makes the runner
    # robust to the async reader racing ahead/behind the writer.
    commands_sent = 0

    try:
        for kind, payload in steps:
            if deadline.is_set() or h.proc.poll() is not None:
                # Process died or timed out mid-run.
                rc = h.proc.poll()
                fail("harness exited/timed out unexpectedly (rc=%s)"
                     % (rc if rc is not None else "timeout"))
            if kind == "cmd":
                h.send(payload)
                commands_sent += 1
            else:
                path, op, expected, lineno, rawtext = payload
                # If no command has produced a state yet, request one explicitly.
                if commands_sent == 0:
                    h.send('{"cmd":"get_state"}')
                    commands_sent += 1
                state = wait_for_state_count(h, deadline, commands_sent)
                if state is None:
                    fail("line %d: timed out waiting for state snapshot for: %s"
                         % (lineno, rawtext))
                found, actual = get_path(state, path)
                if not found:
                    fail("line %d: path %r not present in state\n  expect: %s"
                         % (lineno, path, rawtext))
                if not evaluate(op, actual, expected):
                    fail("line %d: assertion failed\n  expect: %s\n  actual: %r %s %r"
                         % (lineno, rawtext, actual, op, expected))

        if not has_quit:
            h.send('{"cmd":"quit"}')
        h.wait_quit()
    finally:
        timer.cancel()
        h.kill()

    print("SCENARIO PASS: %s (%d assertions)"
          % (os.path.basename(scenario_path),
             sum(1 for k, _ in steps if k == "expect")))
    return 0


def wait_for_state_count(h, deadline, target_count, max_wait=30.0):
    """Block until the harness has emitted at least `target_count` state events
    (one per command), then return the latest. Returns None on timeout / death."""
    import time
    waited = 0.0
    while waited < max_wait and not deadline.is_set():
        if h.get_state_count() >= target_count:
            return h.get_state()
        if h.proc.poll() is not None:
            # Process exited; surface whatever final state we have, if any.
            return h.get_state() if h.get_state_count() >= target_count else None
        time.sleep(0.01)
        waited += 0.01
    return None


if __name__ == "__main__":
    sys.exit(main(sys.argv))
