#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Unto Labs
# SPDX-License-Identifier: Apache-2.0
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import threading
from http.server import ThreadingHTTPServer

sys.dont_write_bytecode = True
from daemon_session_test import check, poll_until
import mock_s3


def run(binary, work, selected=None):
    work.mkdir(parents=True, exist_ok=True)
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("VCACHE_")}
    environment.update(VCACHE_DAEMON="auto", VCACHE_DAEMON_SINGLE_FLIGHT="1",
                       VCACHE_DAEMON_IDLE_TIMEOUT="0", VCACHE_COMPILER_CHECK="none",
                       VCACHE_RUST_DEP_INFO="manifest")
    compiler_text = """#!/usr/bin/env python3
import fcntl, os, select, sys
real = os.path.basename(sys.argv[0])
actual = ('-c' in sys.argv and '-E' not in sys.argv) or '-shared' in sys.argv
if real == 'rustc':
    actual = any(a.startswith('--emit=') and 'link' in a for a in sys.argv)
if actual and os.environ.get('VCACHE_TEST_ENTERED'):
    with open(os.environ['VCACHE_TEST_COUNT'], 'a') as count:
        fcntl.flock(count, fcntl.LOCK_EX)
        count.write('compile\\n')
        count.flush()
    with open(os.environ['VCACHE_TEST_ENTERED'], 'w') as entered:
        entered.write(str(os.getpid()))
    gate = os.open(os.environ['VCACHE_TEST_GATE'], os.O_RDONLY | os.O_NONBLOCK)
    if not select.select([gate], [], [], 8)[0] or not os.read(gate, 1):
        sys.exit(77)
    os.close(gate)
    if os.environ.get('VCACHE_TEST_FAIL'):
        sys.exit(1)
os.execvp(real, [real] + sys.argv[1:])
"""
    compilers = work / "compilers"
    compilers.mkdir()
    for name in ("gcc", "rustc"):
        compiler = compilers / name
        compiler.write_text(compiler_text)
        compiler.chmod(0o755)

    def command(env, *arguments):
        try:
            return subprocess.run([str(binary), *arguments], env=env, cwd=work,
                                  capture_output=True, text=True, timeout=5)
        except subprocess.TimeoutExpired:
            return subprocess.CompletedProcess(arguments, 124, "", "deadline reached")

    def row(env, name, expected, operation="--daemon-status"):
        return any(line.split() == name.split() + [str(expected)]
                   for line in command(env, operation).stdout.splitlines())

    def contents(path):
        return path.read_text() if path.exists() else ""

    def pair(label, operation="c", ending="stored", settings=None, recorded_cost=False):
        if selected and label not in selected:
            return
        case = work / label
        case.mkdir()
        env = environment | {"VCACHE_DIR": str(case / "cache")} | (settings or {})
        processes, gates, logs, outputs, entered, control_fds = [], [], [], [], [], []
        count = case / "count"
        try:
            for index in range(2):
                tree = case / str(index)
                tree.mkdir()
                log = tree / "compile.log"
                log_path = tree / "entered"
                fifo = tree / "gate"
                os.mkfifo(fifo)
                gates.append(os.open(fifo, os.O_RDWR | os.O_NONBLOCK))
                logs.append(log)
                entered.append(log_path)
                build_env = env | {"VCACHE_ROOTS": str(tree) + "=project",
                                   "VCACHE_LOG": str(log), "VCACHE_TEST_ENTERED": str(log_path),
                                   "VCACHE_TEST_GATE": str(fifo), "VCACHE_TEST_COUNT": str(count)}
                if operation == "rust":
                    (tree / "source.rs").write_text("pub fn value() -> u32 { 42 }\n")
                    arguments = [str(compilers / "rustc"), "--crate-name", "lease",
                                 "--crate-type", "lib", "--emit=link,dep-info", "--out-dir",
                                 "out", "source.rs"]
                    outputs.append(tree / "out/liblease.rlib")
                elif operation == "link":
                    (tree / "source.c").write_text("int value(void) { return 42; }\n")
                    subprocess.run(["gcc", "-fPIC", "-c", "source.c", "-o", "input.o"],
                                   cwd=tree, check=True, timeout=5)
                    arguments = [str(compilers / "gcc"), "-shared", "input.o", "-o", "out.so"]
                    build_env["VCACHE_LINK_CACHE"] = "1"
                    outputs.append(tree / "out.so")
                else:
                    (tree / "source.c").write_text("int value(void) { return 42; }\n")
                    arguments = [str(compilers / "gcc"), "-c", "source.c", "-o", "out.o"]
                    outputs.append(tree / "out.o")
                if index == 0 and recorded_cost:
                    recorded_operation = {"c": "compile", "rust": "rustc", "link": "link"}[operation]
                    fixtures = [subprocess.run(
                        [str(binary.with_name("vcache_test")), "--write-lease-cost-fixture",
                         env["VCACHE_DIR"], build_env["VCACHE_ROOTS"], recorded_operation,
                         str(wall_ms), *arguments], env=build_env, cwd=tree,
                        capture_output=True, timeout=5) for wall_ms in (20000, 5000)]
                    check(all(fixture.returncode == 0 for fixture in fixtures),
                          label + ": production writer primes two cost observations")
                if index == 0 and ending == "failed":
                    build_env["VCACHE_TEST_FAIL"] = "1"
                inherited_fds = ()
                if operation == "link":
                    marker_fd = os.open(log_path, os.O_WRONLY | os.O_CREAT, 0o600)
                    count_fd = os.open(count, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
                    control_fds.extend((marker_fd, count_fd))
                    inherited_fds = (gates[-1], marker_fd, count_fd)
                    # The tracer ignores process descriptors; a named fixture
                    # FIFO would otherwise become a link input to hash.
                    build_env.update(VCACHE_TEST_GATE="/proc/self/fd/" + str(gates[-1]),
                                     VCACHE_TEST_ENTERED="/proc/self/fd/" + str(marker_fd),
                                     VCACHE_TEST_COUNT="/proc/self/fd/" + str(count_fd))
                process = subprocess.Popen([str(binary), *arguments], cwd=tree, env=build_env,
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                           pass_fds=inherited_fds)
                processes.append(process)
                if index == 0:
                    check(poll_until(lambda: bool(contents(log_path))),
                          label + ": holder reaches compiler barrier")
            enabled = env.get("VCACHE_DAEMON_SINGLE_FLIGHT") != "0" and not any(
                env.get(name) == "1" for name in ("VCACHE_READONLY", "VCACHE_RECACHE"))
            if enabled:
                check(poll_until(lambda: row(env, "leases waiting", 1)),
                      label + ": status reports one lease waiter")
                check(row(env, "leases held", 1) and row(env, "compile sessions", 2),
                      label + ": status reports one holder and two compile sessions")
                check(not contents(entered[1]), label + ": waiter has not spawned a compiler")
            else:
                check(poll_until(lambda: bool(contents(entered[1]))),
                      label + ": both compilers reach barriers")
                check(row(env, "leases held", 0) and row(env, "leases waiting", 0),
                      label + ": status reports no leases")
            if ending == "shutdown":
                check(command(env, "--stop-daemon").returncode == 0,
                      label + ": shutdown completes during the lease wait")
            if ending == "gone":
                processes[0].kill()
            for gate in gates:
                os.write(gate, b"x")
            for process in processes:
                process.communicate(timeout=8)
            expected = [1, 0] if ending == "failed" else [-9, 0] if ending == "gone" else [0, 0]
            check([process.returncode for process in processes] == expected,
                  label + ": preserves both compiler results")
            text = "".join(contents(log) for log in logs)
            deduplicated = enabled and ending == "stored"
            check(contents(count).count("compile\n") == (1 if deduplicated else 2),
                  label + ": one compiler run when stored, two when compiling independently")
            if deduplicated:
                check(text.count("lease: acquired ") == 1 and
                      text.count("lease: holder stored, serving hit after ") == 1,
                      label + ": logs one acquisition and one stored wake")
                if operation != "link":
                    marker = "rust compile: " if operation == "rust" else "] compile: "
                    check(text.count(marker) == 1,
                          label + ": VCACHE_LOG records exactly one real compile")
                check(all(output.exists() for output in outputs) and
                      outputs[0].read_bytes() == outputs[1].read_bytes(),
                      label + ": restored outputs are byte-identical")
                check(row(env, "compiles deduplicated", 1), label + ": status deduplicated is 1")
                check(row(env, "cache miss", 1, "--show-stats") and
                      row(env, "cache hit (disk)", 1, "--show-stats"),
                      label + ": wrapper stats count one miss and one restored hit")
                check("lease: waiting on holder pid " in contents(logs[1]) and
                      ("up to 40000 ms" if recorded_cost else "up to 300000 ms") in
                      contents(logs[1]),
                      label + ": waiter logs holder pid and bounded wait duration")
            elif ending in ("failed", "gone", "shutdown"):
                reason = "failed" if ending == "failed" else "gone"
                check("lease: holder " + reason + ", compiling" in contents(logs[1]),
                      label + ": waiter logs holder " + reason)
            else:
                check("lease: " not in text, label + ": emits no lease request logs")
            if operation == "rust":
                entry = re.search(r"rust key ([0-9a-f]+)", contents(logs[0]))
                manifest = re.search(r"rust manifest key ([0-9a-f]+)", contents(logs[0]))
                acquired = re.findall(r"lease: acquired ([0-9a-f]+)", text)
                check(entry is not None and manifest is not None and
                      acquired == [entry[1][:16]] and manifest[1][:16] not in acquired,
                      label + ": leases the Rust entry key and never the manifest key")
            if operation == "link":
                pre_key = re.search(r"link pre-key ([0-9a-f]+)", contents(logs[0]))
                acquired = re.findall(r"lease: acquired ([0-9a-f]+)", text)
                check(pre_key is not None and acquired == [pre_key[1][:16]],
                      label + ": leases the link pre-key")
            if ending == "shutdown":
                check(command(env, "--start-daemon").returncode == 0,
                      label + ": daemon restarts after local compiles finish")
            check(poll_until(lambda: row(env, "compile sessions", 0) and
                             row(env, "leases held", 0) and row(env, "leases waiting", 0)),
                  label + ": status returns zero sessions, holders and waiters")
        finally:
            for gate in gates:
                os.close(gate)
            for descriptor in control_fds:
                os.close(descriptor)
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=1)
            command(env, "--stop-daemon")

    pair("c-stored")
    if shutil.which("rustc"):
        pair("rust-stored", "rust")
    pair("link-stored", "link")
    pair("cost-c", recorded_cost=True)
    if shutil.which("rustc"):
        pair("cost-rust", "rust", recorded_cost=True)
    pair("cost-link", "link", recorded_cost=True)
    pair("holder-failed", ending="failed")
    pair("holder-gone", ending="gone")
    pair("daemon-shutdown", ending="shutdown")
    pair("switch-off", settings={"VCACHE_DAEMON_SINGLE_FLIGHT": "0",
                                 "VCACHE_DAEMON_ADMISSION": "1"})
    pair("readonly", settings={"VCACHE_READONLY": "1"})
    pair("recache", settings={"VCACHE_RECACHE": "1"})

    if selected and "fetch" not in selected:
        return
    fetch_case = work / "fetch"
    fetch_case.mkdir()
    seed_tree = fetch_case / "seed"
    seed_tree.mkdir()
    (seed_tree / "source.c").write_text("int value(void) { return 42; }\n")
    seed_log = seed_tree / "compile.log"
    seed_env = environment | {"VCACHE_DIR": str(seed_tree / "cache"), "VCACHE_DAEMON": "off",
                              "VCACHE_LOG": str(seed_log),
                              "VCACHE_ROOTS": str(seed_tree) + "=project"}
    seeded = subprocess.run([str(binary), "gcc", "-c", "source.c", "-o", "out.o"],
                            env=seed_env, cwd=seed_tree, capture_output=True, timeout=5)
    check(seeded.returncode == 0, "fetch: production compiler creates the blob fixture")
    key = re.search(r"\] key ([0-9a-f]+) for", contents(seed_log))[1]
    objects = fetch_case / "objects"
    objects.mkdir()
    (objects / (key[:2] + "__" + key[2:])).write_bytes(
        (seed_tree / "cache" / key[:2] / key[2:]).read_bytes())
    mock_s3.STORAGE = str(objects)
    fetch_started, fetch_finish = threading.Event(), threading.Event()
    fetch_count = []
    count_lock = threading.Lock()

    class GatedFetch(mock_s3.Handler):
        def do_GET(self):
            with count_lock:
                fetch_count.append(self.path)
            fetch_started.set()
            if not fetch_finish.wait(8):
                self.send_error(504, "fixture deadline reached")
                return
            super().do_GET()

    service = ThreadingHTTPServer(("127.0.0.1", 0), GatedFetch)
    service.daemon_threads = True
    service_thread = threading.Thread(target=service.serve_forever, daemon=True)
    service_thread.start()
    fetch_log = fetch_case / "daemon.log"
    fetch_env = environment | {"VCACHE_DIR": str(fetch_case / "cache"),
                               "VCACHE_LOG": str(fetch_log), "VCACHE_S3_BUCKET": "testbucket",
                               "VCACHE_S3_ENDPOINT": "http://127.0.0.1:" +
                                                     str(service.server_port),
                               "VCACHE_S3_PATH_STYLE": "1", "VCACHE_S3_REGION": "us-east-1",
                               "AWS_ACCESS_KEY_ID": "testkey", "AWS_SECRET_ACCESS_KEY": "secret",
                               "AWS_SESSION_TOKEN": ""}
    fetch_processes, fetch_outputs = [], []
    try:
        check(command(fetch_env, "--start-daemon").returncode == 0, "fetch: daemon starts")
        for index in range(2):
            tree = fetch_case / str(index)
            tree.mkdir()
            (tree / "source.c").write_text("int value(void) { return 42; }\n")
            build_env = fetch_env | {"VCACHE_ROOTS": str(tree) + "=project"}
            process = subprocess.Popen([str(binary), "gcc", "-c", "source.c", "-o", "out.o"],
                                       env=build_env, cwd=tree, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE)
            fetch_processes.append(process)
            fetch_outputs.append(tree / "out.o")
            if index == 0:
                check(fetch_started.wait(3), "fetch: first S3 GET reaches its condition barrier")
        check(poll_until(lambda: "lease: fetch waiting " in contents(fetch_log)),
              "fetch: second lookup logs a bounded coalesced wait")
        check(row(fetch_env, "lookups", 2) and row(fetch_env, "leases held", 0) and
              row(fetch_env, "compile sessions", 0),
              "fetch: status counts both lookups without compiler leases or sessions")
        check(len(fetch_count) == 1, "fetch: concurrent misses issue exactly one S3 GET")
        fetch_finish.set()
        for process in fetch_processes:
            process.communicate(timeout=5)
        check(all(process.returncode == 0 for process in fetch_processes) and
              all(output.exists() for output in fetch_outputs) and
              fetch_outputs[0].read_bytes() == fetch_outputs[1].read_bytes(),
              "fetch: coalesced GETs restore identical production artifacts")
        check("lease: fetch waited " in contents(fetch_log) and
              "up to 300000 ms" in contents(fetch_log),
              "fetch: logs the wait cap and actual elapsed duration")
        check(contents(fetch_log).count("] hit on s3\n") == 2 and
              "] compile: " not in contents(fetch_log) and row(fetch_env, "hit (s3)", 2),
              "fetch: both Gets serve S3 hits without compiling")
        check(row(fetch_env, "compiles deduplicated", 0) and row(fetch_env, "leases held", 0),
              "fetch: lookup coalescing leaves compile deduplication counters unchanged")
    finally:
        fetch_finish.set()
        for process in fetch_processes:
            if process.poll() is None:
                process.kill()
                process.communicate(timeout=1)
        command(fetch_env, "--stop-daemon")
        service.shutdown()
        service.server_close()
        service_thread.join(timeout=2)


if __name__ == "__main__":
    run(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), set(sys.argv[3:]))
