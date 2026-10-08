#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Unto Labs
# SPDX-License-Identifier: Apache-2.0
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import sys
import time


def poll_until(condition, timeout=3):
    deadline = time.monotonic() + timeout
    with selectors.DefaultSelector() as selector:
        while time.monotonic() < deadline:
            if condition():
                return True
            selector.select(min(0.01, max(0, deadline - time.monotonic())))
    return False


def check(condition, message):
    print(("PASS|" if condition else "FAIL|") + message, flush=True)


def run(binary, work):
    work.mkdir(parents=True, exist_ok=True)
    source = work / "session.c"
    source.write_text("int session_value(void) { return 42; }\n")
    compiler = work / "gcc"
    compiler.write_text("""#!/usr/bin/env python3
import os, select, sys
if '-c' in sys.argv and '-E' not in sys.argv:
    marker = os.environ.get('VCACHE_TEST_ENTERED')
    if marker:
        with open(marker, 'w') as entered:
            entered.write(str(os.getpid()))
        gate = os.open(os.environ['VCACHE_TEST_GATE'], os.O_RDONLY | os.O_NONBLOCK)
        if not select.select([gate], [], [], 5)[0] or not os.read(gate, 1):
            sys.exit(77)
        os.close(gate)
os.execvp('gcc', ['gcc'] + sys.argv[1:])
""")
    compiler.chmod(0o755)
    environment = os.environ.copy()
    for name in list(environment):
        if name.startswith("VCACHE_"):
            del environment[name]
    environment.update(VCACHE_DIR=str(work / "cache"), VCACHE_DAEMON="on",
                       VCACHE_DAEMON_IDLE_TIMEOUT="0", VCACHE_COMPILER_CHECK="none",
                       VCACHE_DAEMON_SINGLE_FLIGHT="1", VCACHE_DAEMON_ADMISSION="1")

    def command(*arguments, env=None):
        try:
            return subprocess.run([str(binary), *arguments], env=env or environment,
                                  cwd=work, capture_output=True, text=True, timeout=4)
        except subprocess.TimeoutExpired:
            return subprocess.CompletedProcess(arguments, 124, "", "deadline reached")

    def sessions(expected):
        result = command("--daemon-status")
        return any(line.split() == ["compile", "sessions", str(expected)]
                   for line in result.stdout.splitlines())

    def contents(path):
        return path.read_text() if path.exists() else ""

    def compile_once(label, settings, stop=False):
        source.write_text("int session_" + label.replace("-", "_") +
                          "(void) { return 42; }\n")
        log = work / (label + ".log")
        entered = work / (label + ".entered")
        gate_path = work / (label + ".fifo")
        os.mkfifo(gate_path)
        gate = os.open(gate_path, os.O_RDWR | os.O_NONBLOCK)
        compile_env = environment | settings | {
            "VCACHE_LOG": str(log),
            "VCACHE_TEST_ENTERED": str(entered), "VCACHE_TEST_GATE": str(gate_path)}
        process = subprocess.Popen([str(binary), str(compiler), "-c", str(source),
                                    "-o", str(work / (label + ".o"))], env=compile_env,
                                   cwd=work, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            check(poll_until(entered.exists), label + ": compiler reaches its barrier")
            if stop:
                check(sessions(1), "mid-compile status reports compile sessions 1")
                stopped = command("--stop-daemon")
                check(stopped.returncode == 0, "daemon stops while the compiler is held")
            os.write(gate, b"x")
            process.communicate(timeout=4)
            check(process.returncode == 0 and (work / (label + ".o")).exists(),
                  label + ": compile finishes with an object")
            return contents(log)
        finally:
            os.close(gate)
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=1)

    try:
        check(command("--start-daemon").returncode == 0, "session test daemon starts")
        for switch in ("SINGLE_FLIGHT", "ADMISSION"):
            label = switch.lower()
            log = compile_once(label, {"VCACHE_DAEMON_" + switch: "1"})
            check("session: opened" in log and "session: closed after " in log,
                  label + ": VCACHE_LOG records session open and close duration")
            check(poll_until(lambda: sessions(0)), label + ": status returns compile sessions 0")
        toml = work / "sessions.toml"
        toml.write_text("[daemon]\nsingle_flight = true\n")
        log = compile_once("toml", {"VCACHE_CONFIG": str(toml)})
        check("session: opened" in log and "session: closed after " in log,
              "TOML switch opens and closes a compile session")
        check(poll_until(lambda: sessions(0)), "TOML compile leaves compile sessions 0")

        for operation in ("failed", "link", "rust"):
            if operation == "rust" and not shutil.which("rustc"):
                continue
            log_path = work / (operation + ".log")
            operation_env = environment | {"VCACHE_DAEMON_SINGLE_FLIGHT": "1",
                                           "VCACHE_LOG": str(log_path)}
            if operation == "failed":
                broken = work / "broken.c"
                broken.write_text("int broken( {\n")
                result = command("gcc", "-c", str(broken), "-o", "broken.o", env=operation_env)
                succeeded = result.returncode == 1
            elif operation == "link":
                operation_env["VCACHE_LINK_CACHE"] = "1"
                result = command("gcc", "-shared", "toml.o", "-o", "session.so", env=operation_env)
                succeeded = result.returncode == 0 and (work / "session.so").exists()
            else:
                rust_source = work / "session.rs"
                rust_source.write_text("pub fn session_value() -> u32 { 42 }\n")
                result = command("rustc", "--crate-name", "session", "--crate-type", "lib",
                                 "--emit=link,dep-info", "--out-dir", "rust-out",
                                 str(rust_source), env=operation_env)
                succeeded = result.returncode == 0
            log = contents(log_path)
            check(succeeded, operation + ": preserves compiler result")
            check("session: opened" in log and "session: closed after " in log,
                  operation + ": logs compile session open and close")
            check(poll_until(lambda: sessions(0)), operation + ": leaves compile sessions 0")
        log = compile_once("switches-off", {"VCACHE_DAEMON_SINGLE_FLIGHT": "0",
                                           "VCACHE_DAEMON_ADMISSION": "0"})
        check("session: " not in log, "both switches off emit no session log")
        check(sessions(0), "both switches off leave compile sessions 0")
        log = compile_once("daemon-off", {"VCACHE_DAEMON": "off",
                                         "VCACHE_DAEMON_SINGLE_FLIGHT": "1"})
        check("session: " not in log, "daemon off emits no session log")
        check(sessions(0), "daemon off leaves compile sessions 0")
        log = compile_once("readonly", {"VCACHE_READONLY": "1",
                                       "VCACHE_DAEMON_SINGLE_FLIGHT": "1"})
        check("session: " not in log, "read-only compile holds no session")
        check(sessions(0), "read-only compile leaves compile sessions 0")
        log = compile_once("refused", {"VCACHE_CACHE_SIZE": "1G",
                                      "VCACHE_DAEMON_SINGLE_FLIGHT": "1"})
        check("session: daemon unavailable (daemon refused this client:" in log,
              "refused daemon logs session fallback")
        check(sessions(0), "refused compile leaves compile sessions 0")
        log = compile_once("shutdown", {"VCACHE_DAEMON_SINGLE_FLIGHT": "1"}, stop=True)
        check("session: daemon unavailable (daemon shutting down), continuing" in log,
              "mid-compile daemon shutdown logs unavailable fallback")
        log = compile_once("absent", {"VCACHE_DAEMON_SINGLE_FLIGHT": "1"})
        check(log.count("session: daemon unavailable (") == 1,
              "absent daemon logs session fallback once per compile")
    finally:
        command("--stop-daemon")


if __name__ == "__main__":
    run(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
