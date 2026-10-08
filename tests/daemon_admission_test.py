#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Unto Labs
# SPDX-License-Identifier: Apache-2.0
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True
from daemon_session_test import check, poll_until


def run(binary, work, writer, selected):
    work.mkdir(parents=True, exist_ok=True)
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("VCACHE_")}
    environment.update(VCACHE_DAEMON="auto", VCACHE_DAEMON_ADMISSION="1",
                       VCACHE_DAEMON_IDLE_TIMEOUT="0", VCACHE_COMPILER_CHECK="none",
                       VCACHE_RUST_DEP_INFO="manifest")
    compilers = work / "compilers"
    compilers.mkdir()
    compiler_text = """#!/usr/bin/env python3
import os, select, sys
real = os.path.basename(sys.argv[0])
actual = ('-c' in sys.argv and '-E' not in sys.argv) or '-shared' in sys.argv
if real == 'rustc':
    actual = any(a.startswith('--emit=') and 'link' in a for a in sys.argv)
if actual:
    with open(os.environ['VCACHE_TEST_ENTERED'], 'w') as entered:
        entered.write(str(os.getpid()))
    gate = os.open(os.environ['VCACHE_TEST_GATE'], os.O_RDONLY | os.O_NONBLOCK)
    if not select.select([gate], [], [], 8)[0] or not os.read(gate, 1):
        sys.exit(77)
    os.close(gate)
os.execvp(real, [real] + sys.argv[1:])
"""
    for name in ("gcc", "rustc"):
        path = compilers / name
        path.write_text(compiler_text)
        path.chmod(0o755)

    def command(env, *arguments):
        try:
            return subprocess.run([str(binary), *arguments], env=env, cwd=work,
                                  capture_output=True, text=True, timeout=5)
        except subprocess.TimeoutExpired:
            return subprocess.CompletedProcess(arguments, 124, "", "deadline reached")

    def contents(path):
        return path.read_text() if path.exists() else ""

    def row(env, name, expected):
        return any(line.split() == name.split() + [str(expected)]
                   for line in command(env, "--daemon-status").stdout.splitlines())

    def pair(label, operation="c", ending="release", settings=None, same_key=False,
             seeded=True):
        if selected and label not in selected:
            return
        case = work / label
        case.mkdir()
        meminfo = case / "meminfo"
        meminfo.write_text("MemAvailable: 3145728 kB\n")
        log = case / "decisions.log"
        env = environment | {"VCACHE_DIR": str(case / "cache"), "VCACHE_LOG": str(log),
                             "VCACHE_DAEMON_MEMINFO": str(meminfo)} | (settings or {})
        estimate_kb = 4194304 if operation == "link" and not seeded else 2097152
        started_env = env | {"VCACHE_DAEMON": "auto", "VCACHE_DAEMON_ADMISSION": "0"}
        if ending == "refused":
            started_env["VCACHE_CACHE_SIZE"] = "1G"
            env["VCACHE_CACHE_SIZE"] = "2G"
        processes, gates, markers, outputs, descriptors = [], [], [], [], []
        try:
            check(command(started_env, "--start-daemon").returncode == 0,
                  label + ": daemon starts with admission off")
            for index in range(2):
                tree = case / str(index)
                tree.mkdir()
                fifo, marker = tree / "gate", tree / "entered"
                os.mkfifo(fifo)
                gates.append(os.open(fifo, os.O_RDWR | os.O_NONBLOCK))
                markers.append(marker)
                build_env = env | {"VCACHE_ROOTS": str(tree) + "=project",
                                   "VCACHE_TEST_GATE": str(fifo),
                                   "VCACHE_TEST_ENTERED": str(marker)}
                value = 42 if same_key else 42 + index
                if operation == "rust":
                    (tree / "source.rs").write_text("pub fn value() -> u32 { " +
                                                     str(value) + " }\n")
                    arguments = [str(compilers / "rustc"), "--crate-name", "memory",
                                 "--crate-type", "lib", "--emit=link,dep-info",
                                 "--out-dir", "out", "source.rs"]
                    outputs.append(tree / "out/libmemory.rlib")
                elif operation == "link":
                    (tree / "source.c").write_text("int value(void) { return " +
                                                   str(value) + "; }\n")
                    subprocess.run(["gcc", "-fPIC", "-c", "source.c", "-o", "input.o"],
                                   cwd=tree, check=True, timeout=5)
                    arguments = [str(compilers / "gcc"), "-shared", "input.o", "-o", "out.so"]
                    build_env["VCACHE_LINK_CACHE"] = "1"
                    outputs.append(tree / "out.so")
                else:
                    (tree / "source.c").write_text("int value(void) { return " +
                                                   str(value) + "; }\n")
                    arguments = [str(compilers / "gcc"), "-c", "source.c", "-o", "out.o"]
                    outputs.append(tree / "out.o")
                if index == 0 and seeded:
                    fixture = subprocess.run(
                        [str(writer), "--write-admission-cost-fixture", env["VCACHE_DIR"],
                         build_env["VCACHE_ROOTS"],
                         {"c": "compile", "rust": "rustc", "link": "link"}[operation],
                         "2097152", *arguments], env=build_env, cwd=tree,
                        capture_output=True, timeout=5)
                    check(fixture.returncode == 0 and (case / "cache/costs").is_dir(),
                          label + ": production writer creates the 2 GB cost record")
                inherited = ()
                if operation == "link":
                    marker_fd = os.open(marker, os.O_WRONLY | os.O_CREAT, 0o600)
                    descriptors.append(marker_fd)
                    inherited = (gates[-1], marker_fd)
                    build_env.update(VCACHE_TEST_GATE="/proc/self/fd/" + str(gates[-1]),
                                     VCACHE_TEST_ENTERED="/proc/self/fd/" + str(marker_fd))
                processes.append(subprocess.Popen(
                    [str(binary), *arguments], env=build_env, cwd=tree, pass_fds=inherited,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE))
                if index == 0:
                    check(poll_until(lambda: bool(contents(marker))),
                          label + ": first compiler reaches its FIFO barrier")
            enabled = env.get("VCACHE_DAEMON_ADMISSION") != "0" and \
                env.get("VCACHE_DAEMON") != "off" and ending != "refused" and \
                env.get("VCACHE_READONLY") != "1"
            if enabled:
                if same_key:
                    check(poll_until(lambda: row(env, "leases waiting", 1)) and
                          row(env, "memory waiting", 0),
                          label + ": a lease waiter holds no memory reservation")
                else:
                    check(poll_until(lambda: row(env, "memory waiting", 1)) and
                          row(env, "memory reserved", estimate_kb) and
                          row(env, "reserve waits", 1),
                          label + ": status shows one estimated reservation and one waiter")
                    check("reserve: waiting (" in contents(log) and not contents(markers[1]),
                          label + ": waiting is logged before the second compiler spawns")
                if ending == "shutdown":
                    check(command(env, "--stop-daemon").returncode == 0,
                          label + ": shutdown completes with a queued reservation")
                    check(poll_until(lambda: bool(contents(markers[1]))),
                          label + ": queued client runs unreserved after daemon shutdown")
                else:
                    os.write(gates[0], b"x")
                    processes[0].communicate(timeout=5)
                    if same_key:
                        processes[1].communicate(timeout=5)
                        check(not contents(markers[1]),
                              label + ": deduplicated client never spawns a compiler")
                    else:
                        check(poll_until(lambda: bool(contents(markers[1]))),
                              label + ": ending the first reservation starts the second")
            else:
                check(poll_until(lambda: bool(contents(markers[1]))),
                      label + ": both compilers proceed without admission")
                check(row(started_env, "memory reserved", 0) and
                      row(started_env, "memory waiting", 0),
                      label + ": daemon status has no memory scheduling state")
            for gate in gates:
                os.write(gate, b"x")
            for process in processes:
                if process.poll() is None:
                    process.communicate(timeout=5)
            check(all(process.returncode == 0 for process in processes) and
                  all(output.exists() for output in outputs),
                  label + ": both builds finish with their production outputs")
            if ending == "shutdown":
                check(contents(log).count("reserve: daemon unavailable, running unreserved") >= 2,
                      label + ": both clients log unreserved fallback")
                command(env, "--start-daemon")
            elif ending == "refused":
                check("reserve: daemon unavailable, running unreserved" in contents(log),
                      label + ": refused clients log unreserved fallback")
            elif enabled:
                granted = re.findall(r"reserve: granted ([0-9]+) kB after ([0-9]+) ms "
                                     r"\(available ([0-9]+), unrealised ([0-9]+)\)", contents(log))
                check(len(granted) == (1 if same_key else 2) and
                      all(int(fields[0]) == estimate_kb for fields in granted),
                      label + ": grant logs include estimate, wait and memory arithmetic")
                check(" rss " in contents(log), label + ": compiler RSS sampling is logged")
            else:
                check("reserve: " not in contents(log),
                      label + ": feature off or read-only emits no reserve requests")
            check(poll_until(lambda: row(started_env, "memory reserved", 0) and
                             row(started_env, "memory waiting", 0) and
                             row(started_env, "compile sessions", 0)),
                  label + ": final status has no sessions, reservations or waiters")
        finally:
            for gate in gates:
                os.close(gate)
            for descriptor in descriptors:
                os.close(descriptor)
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=1)
            command(started_env, "--stop-daemon")

    pair("c-queued")
    if shutil.which("rustc"):
        pair("rust-queued", "rust")
    pair("link-queued", "link")
    pair("shutdown", ending="shutdown")
    pair("refused", ending="refused")
    pair("off", settings={"VCACHE_DAEMON_ADMISSION": "0", "VCACHE_DAEMON_SINGLE_FLIGHT": "1"})
    pair("readonly", settings={"VCACHE_READONLY": "1"})
    pair("lease-first", same_key=True, settings={"VCACHE_DAEMON_SINGLE_FLIGHT": "1"})
    pair("default-compile", seeded=False)
    pair("default-link", "link", seeded=False)
    pair("daemon-off", settings={"VCACHE_DAEMON": "off"})


if __name__ == "__main__":
    run(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(),
        Path(sys.argv[3]).resolve(), sys.argv[4:])
