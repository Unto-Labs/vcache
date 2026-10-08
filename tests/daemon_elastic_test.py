#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Unto Labs
# SPDX-License-Identifier: Apache-2.0
import os
from pathlib import Path
import re
import select
import shlex
import subprocess
import sys

sys.dont_write_bytecode = True
from daemon_session_test import check, poll_until


def run(binary, work, writer, selected):
    work.mkdir(parents=True, exist_ok=True)
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("VCACHE_")}
    environment.update(VCACHE_DAEMON="auto", VCACHE_DAEMON_ADMISSION="1",
                       VCACHE_DAEMON_JOBSERVER="1", VCACHE_DAEMON_JOBSERVER_JOBS="4",
                       VCACHE_DAEMON_IDLE_TIMEOUT="0", VCACHE_COMPILER_CHECK="none")
    compiler = work / "gcc"
    compiler.write_text("""#!/usr/bin/env python3
import fcntl, os, pathlib, select, subprocess, sys
if '-c' not in sys.argv or '-E' in sys.argv:
    os.execvp('gcc', ['gcc'] + sys.argv[1:])
folder = pathlib.Path(os.environ['VCACHE_TEST_FOLDER'])
index = os.environ['VCACHE_TEST_INDEX']
counter = folder / 'counter'
def change(delta):
    if not counter.exists(): return
    with counter.open('r+') as state:
        fcntl.flock(state, fcntl.LOCK_EX)
        current, peak, started = map(int, state.read().split())
        current += delta
        if delta > 0: started += 1
        state.seek(0); state.truncate()
        state.write(f'{current} {max(peak, current)} {started}\\n'); state.flush()
change(1)
(folder / (index + '.entered')).write_text(str(os.getpid()))
gate = os.open(folder / (index + '.gate'), os.O_RDONLY | os.O_NONBLOCK)
try:
    if not select.select([gate], [], [], 30)[0] or not os.read(gate, 1): sys.exit(77)
    result = subprocess.run(['gcc'] + sys.argv[1:]).returncode
finally:
    os.close(gate)
    change(-1)
sys.exit(result)
""")
    compiler.chmod(0o755)

    def command(env, *arguments):
        try:
            return subprocess.run([str(binary), *arguments], env=env, cwd=work,
                                  capture_output=True, text=True, timeout=4)
        except subprocess.TimeoutExpired:
            return subprocess.CompletedProcess(arguments, 124, "", "deadline reached")

    def row(env, name, expected):
        return any(line.split() == name.split() + [str(expected)]
                   for line in command(env, "--daemon-status").stdout.splitlines())

    def contents(path):
        return path.read_text() if path.exists() else ""

    def make_phase(env, case, label, width, processes, descriptors, admission=False, wave=False):
        folder = case / label
        folder.mkdir()
        (folder / "counter").write_text("0 0 0\n")
        gates = []
        for index in range(4):
            name = "t" + str(index)
            (folder / (name + ".c")).write_text("int " + name + "(void) { return " +
                                                str(index + 100) + "; }\n")
            os.mkfifo(folder / (name + ".gate"))
            gates.append(os.open(folder / (name + ".gate"), os.O_RDWR | os.O_NONBLOCK))
        descriptors.extend(gates)
        recipe = "VCACHE_TEST_INDEX=$@ " + shlex.quote(str(binary)) + " " + \
                 shlex.quote(str(compiler)) + " -c $@.c -o $@.o"
        (folder / "Makefile").write_text(".PHONY: all t0 t1 t2 t3\nall: t0 t1 t2 t3\n"
                                         "t0 t1 t2 t3:\n\t" + recipe + "\n")
        flags = command(env, "--jobserver-env").stdout.strip().removeprefix("MAKEFLAGS=")
        build_env = env | {"MAKEFLAGS": flags, "MAKELEVEL": "0", "MFLAGS": "",
                           "VCACHE_DAEMON_ADMISSION": "1" if admission else "0",
                           "VCACHE_RECACHE": "1", "VCACHE_TEST_FOLDER": str(folder),
                           "VCACHE_ROOTS": str(folder) + "=peers"}
        fixtures = [subprocess.run([str(writer), "--write-admission-cost-fixture",
                                   env["VCACHE_DIR"], build_env["VCACHE_ROOTS"], "compile",
                                   "2097152", str(compiler), "-c", "t" + str(index) + ".c",
                                   "-o", "t" + str(index) + ".o"], cwd=folder, env=build_env,
                                  capture_output=True, timeout=4) for index in range(4)]
        check(all(fixture.returncode == 0 for fixture in fixtures),
              label + ": production writer seeds four 2 GB make compiler estimates")
        process = subprocess.Popen(["make"], cwd=folder, env=build_env,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        processes.append(process)

        def counts():
            try:
                return tuple(map(int, (folder / "counter").read_text().split()))
            except ValueError:
                return (0, 0, 0)
        if wave:
            check(poll_until(lambda: row(env, "memory waiting", 3) and counts()[0] == 1),
                  label + ": make's three other 2 GB compilers wait behind the admitted compiler")
            released = set()
            for step in (1, 2):
                def entered():
                    return [index for index in range(4) if index not in released and
                            (folder / ("t" + str(index) + ".entered")).exists()]
                ready = poll_until(lambda: bool(entered()))
                check(ready, label + ": the next admitted make compiler reaches its barrier")
                if ready:
                    index = entered()[0]
                    released.add(index)
                    os.write(gates[index], b"x")
                check(poll_until(lambda: row(env, "memory waiting", 3 - step) and
                                 row(env, "jobserver tokens withdrawn", step)),
                      label + ": ending a make compile withdraws its newly free token")
            check(counts()[1] <= 2 and row(env, "memory waiting", 1) and
                  row(env, "jobserver tokens withdrawn", 2),
                  label + ": make compilers never exceed two while pressure holds the floor")
            (case / "meminfo").write_text("MemAvailable: 12582912 kB\n")
            check(poll_until(lambda: row(env, "memory waiting", 0) and
                             row(env, "jobserver tokens withdrawn", 0)) and
                  row(env, "jobserver tokens restored total", 2),
                  label + ": drained make pressure restores both shared tokens")
        else:
            check(poll_until(lambda: len(counts()) == 3 and counts()[0] == width),
                  label + ": make reaches the expected compiler width " + str(width))
            first = next((index for index in range(4)
                          if (folder / ("t" + str(index) + ".entered")).exists()), 0)
            os.write(gates[first], b"x")
            if width < 4:
                check(poll_until(lambda: len(counts()) == 3 and counts()[2] > width),
                      label + ": a returned token starts another compiler")
            else:
                check(counts()[2] == 4, label + ": all four compiler peers start")
        for gate in gates:
            os.write(gate, b"x")
        process.communicate(timeout=5)
        check(process.returncode == 0 and all((folder / ("t" + str(index) + ".o")).exists()
                                              for index in range(4)),
              label + ": all four make compiles produce their outputs")
        check(counts()[1] == width, label + ": peak compiler count is " + str(width))

    def scenario(label, pressure=True, stopping=False, minimum="2", idle=False, admitted=False):
        if selected and label not in selected:
            return
        case = work / label
        case.mkdir()
        meminfo = case / "meminfo"
        meminfo.write_text("MemAvailable: 3145728 kB\n")
        log = case / "decisions.log"
        env = environment | {"VCACHE_DIR": str(case / "cache"), "VCACHE_LOG": str(log),
                             "VCACHE_DAEMON_MEMINFO": str(meminfo),
                             "VCACHE_DAEMON_JOBSERVER_MIN_JOBS": minimum}
        if idle:
            env["VCACHE_DAEMON_IDLE_TIMEOUT"] = "1"
        starter = env | {"VCACHE_DAEMON_ADMISSION": "0"}
        processes, descriptors, gates = [], [], []
        try:
            check(command(starter, "--start-daemon").returncode == 0,
                  label + ": daemon starts with the production four-slot pool")
            if admitted:
                make_phase(env, case, "admitted-pressure-make", 2, processes, descriptors,
                           admission=True, wave=True)
                make_phase(env, case, "admitted-recovered-make", 4, processes, descriptors,
                           admission=True)
                check(poll_until(lambda: row(env, "memory reserved", 0) and
                                 row(env, "memory waiting", 0)) and
                      row(env, "jobserver tokens free", 4),
                      label + ": completed make builds return every reservation and token")
                check("jobserver: withdrew" in contents(log) and
                      "jobserver: restored 1 tokens" in contents(log),
                      label + ": admission and elastic decisions share the real build log")
            elif pressure:
                folder = case / "pressure"
                folder.mkdir()
                for index in range(2):
                    name = str(index)
                    source = folder / (name + ".c")
                    source.write_text("int pressure" + name + "(void) { return " + name + "; }\n")
                    os.mkfifo(folder / (name + ".gate"))
                    gates.append(os.open(folder / (name + ".gate"), os.O_RDWR | os.O_NONBLOCK))
                    descriptors.append(gates[-1])
                    build_env = env | {"VCACHE_TEST_FOLDER": str(folder), "VCACHE_TEST_INDEX": name,
                                       "VCACHE_ROOTS": str(folder) + "=pressure"}
                    arguments = [str(compiler), "-c", str(source), "-o", str(folder / (name + ".o"))]
                    fixture = subprocess.run([str(writer), "--write-admission-cost-fixture",
                                              env["VCACHE_DIR"], build_env["VCACHE_ROOTS"],
                                              "compile", "2097152", *arguments], env=build_env,
                                             cwd=folder, capture_output=True, timeout=4)
                    check(fixture.returncode == 0, label + ": production writer seeds compiler cost")
                    processes.append(subprocess.Popen([str(binary), *arguments], env=build_env,
                                                      cwd=folder, stdout=subprocess.PIPE,
                                                      stderr=subprocess.PIPE))
                    if index == 0:
                        check(poll_until(lambda: (folder / "0.entered").exists()),
                              label + ": first admitted compiler holds its FIFO barrier")
                check(poll_until(lambda: row(env, "memory waiting", 1)),
                      label + ": a second 2 GB compiler waits against 3 GB meminfo")
                if minimum == "9":
                    check(row(env, "jobserver tokens withdrawn", 0) and
                          "clamped to 4" in contents(log),
                          label + ": a floor above total is clamped and warns")
                else:
                    check(poll_until(lambda: row(env, "jobserver tokens withdrawn", 2)) and
                          row(env, "jobserver tokens free", 2),
                          label + ": pressure withdraws only two shared tokens to the floor")
                if stopping:
                    fifo = case / "cache/daemon/jobserver.fifo"
                    reader_fd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
                    descriptors.append(reader_fd)
                    first = os.read(reader_fd, 1)
                    check(first == b"+", label + ": a client takes the last free shared token")
                    check(command(env, "--stop-daemon").returncode == 0,
                          label + ": daemon shutdown completes while reservations wait")
                    ready = select.select([reader_fd], [], [], 2)[0]
                    returned = os.read(reader_fd, 16) if ready else b""
                    check(returned == b"++", label + ": shutdown returns both withdrawn tokens within two seconds")
                    check("jobserver: restored 2 tokens" in contents(log),
                          label + ": shutdown restoration appears in VCACHE_LOG")
                elif idle:
                    for gate in gates:
                        os.write(gate, b"x")
                    for process in processes:
                        process.communicate(timeout=5)
                    lifecycle = case / "cache/daemon/log"
                    check(poll_until(lambda: "idle for 1 s; exiting" in contents(lifecycle), 2.5),
                          label + ": daemon-held tokens do not extend the client idle timeout")
                else:
                    if minimum == "2":
                        make_phase(env, case, "pressure-make", 2, processes, descriptors)
                        check(row(env, "memory waiting", 1) and
                              row(env, "jobserver tokens withdrawn", 2),
                              label + ": make stayed at width two while admission still waited")
                    meminfo.write_text("MemAvailable: 12582912 kB\n")
                    check(poll_until(lambda: row(env, "memory waiting", 0)),
                          label + ": memory recovery drains the admission queue")
                    if minimum == "2":
                        check(poll_until(lambda: row(env, "jobserver tokens withdrawn", 0)) and
                              row(env, "jobserver tokens restored total", 2),
                              label + ": drained pressure restores the full pool")
                        restores = re.findall(r"\] jobserver: restored ([0-9]+) tokens", contents(log))
                        check(restores == ["1", "1"], label + ": recovery logs one restore per tick")
                        check("jobserver: withdrew 1 tokens (waiting 1, available " in contents(log),
                              label + ": withdrawal logs the queue length and available memory")
                        make_phase(env, case, "recovered-make", 4, processes, descriptors)
            else:
                make_phase(starter, case, "admission-off-make", 4, processes, descriptors)
                check(row(starter, "jobserver tokens withdrawn", 0) and
                      row(starter, "memory waiting", 0) and "jobserver: withdrew" not in contents(log),
                      label + ": admission off leaves the jobserver fixed")
            for gate in gates:
                os.write(gate, b"x")
            for process in processes:
                if process.poll() is None:
                    process.communicate(timeout=5)
            check(all(process.returncode == 0 for process in processes),
                  label + ": all compiler and build-tool exit codes are preserved")
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=1)
            for descriptor in descriptors:
                os.close(descriptor)
            command(starter, "--stop-daemon")

    scenario("elastic")
    scenario("shutdown", stopping=True)
    scenario("off", pressure=False)
    scenario("clamp", minimum="9")
    scenario("idle", idle=True)
    scenario("make-admission", admitted=True)


if __name__ == "__main__":
    run(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(),
        Path(sys.argv[3]).resolve(), sys.argv[4:])
