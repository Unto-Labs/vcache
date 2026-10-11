#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Unto Labs
# SPDX-License-Identifier: Apache-2.0
#
# Concurrent-worktree benchmark for the daemon's scheduler features.
#
# Run by hand, not by `make test`: it builds a whole repository in two or three
# git worktrees at once, which on a real tree takes minutes and most of the
# machine. It measures what single-flight, memory admission and the jobserver
# pool change when several checkouts of one tree build together.
#
#   tests/concurrent_worktrees_bench.sh --repo PATH --build CMD [options]
#   tests/concurrent_worktrees_bench.sh --matrix --repo PATH --build CMD [options]
#
#   --repo PATH          the git repository; worktrees are made at its HEAD
#   --build CMD          build command, run with `bash -c` in each worktree
#   --clean CMD          run in each worktree before every build, untimed (default: none)
#   --setup CMD          run once in each worktree when it is first created
#   --worktrees N        2 or 3; with --matrix a comma list (default 2; matrix 2,3)
#   --features SET       off | single_flight | admission | jobserver_fixed |
#                        jobserver_elastic (default off)
#   --temperature T      cold | warm (default cold)
#   --jobs N             jobserver pool size (default: nproc)
#   --min-jobs N         elastic floor, daemon.jobserver_min_jobs (default: the daemon's)
#   --masquerade NAMES   compiler names linked to vcache ahead of PATH, e.g. "gcc g++ cc c++"
#   --scratch DIR        worktrees, runs and stores (default /build/$USER/claude-tmp/bench-vcache)
#   --vcache PATH        the binary under test (default ../bin/vcache)
#   --cache-dir DIR      VCACHE_DIR to use (default: a fresh store in each run directory)
#   --tsv FILE           append each summary row here (matrix default <scratch>/results.tsv)
#   --keep-store         do not --clear the stores this script created
#   --matrix             every feature set x each worktree count x cold and warm
#
# Feature sets are cumulative: off runs without a daemon; single_flight starts
# one and turns on single-flight; admission adds memory admission;
# jobserver_fixed adds the jobserver pool with the elastic floor at the pool
# size, so nothing is ever withdrawn; jobserver_elastic lowers the floor again.
# In the jobserver sets the builds get MAKEFLAGS from `vcache --jobserver-env`,
# so the build command must not pass -j to make, ninja or cargo.
#
# Each build sees VCACHE_DIR, its own VCACHE_LOG, VCACHE_ROOTS mapping its
# worktree and its CARGO_TARGET_DIR (<scratch>/targets/<n>) to fixed names,
# RUSTC_WRAPPER=vcache, and FBG_VCACHE=0 / SUNBIRD_VCACHE=0 so a repository's
# own vcache-env.sh leaves those alone instead of pointing at the user's store.
#
# Cold runs start from an empty store. Warm runs first build worktree 1 alone
# with the daemon off, clean, and then measure. Every run leaves its logs, a
# 1 Hz samples.csv and summary.tsv under <scratch>/runs/<stamp>-<set>-<n>wt-<temp>/.
# See docs/daemon.md, "Measuring".

set -uo pipefail

TOP="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

REPO=""
BUILD=""
CLEAN=""
SETUP=""
WORKTREES=""
FEATURES="off"
TEMPERATURE="cold"
JOBS=""
MIN_JOBS=""
MASQUERADE=""
SCRATCH="/build/${USER:-$(id -un)}/claude-tmp/bench-vcache"
VCACHE="$TOP/bin/vcache"
CACHE_DIR=""
TSV=""
KEEP_STORE=0
MATRIX=0

while (( $# )); do
  case "$1" in
    --repo)        REPO="$2"; shift 2 ;;
    --build)       BUILD="$2"; shift 2 ;;
    --clean)       CLEAN="$2"; shift 2 ;;
    --setup)       SETUP="$2"; shift 2 ;;
    --worktrees)   WORKTREES="$2"; shift 2 ;;
    --features)    FEATURES="$2"; shift 2 ;;
    --temperature) TEMPERATURE="$2"; shift 2 ;;
    --jobs)        JOBS="$2"; shift 2 ;;
    --min-jobs)    MIN_JOBS="$2"; shift 2 ;;
    --masquerade)  MASQUERADE="$2"; shift 2 ;;
    --scratch)     SCRATCH="$2"; shift 2 ;;
    --vcache)      VCACHE="$2"; shift 2 ;;
    --cache-dir)   CACHE_DIR="$2"; shift 2 ;;
    --tsv)         TSV="$2"; shift 2 ;;
    --keep-store)  KEEP_STORE=1; shift ;;
    --matrix)      MATRIX=1; shift ;;
    -h|--help)     awk 'NR>=5 && /^#/ { sub(/^# ?/, ""); print; next }
                        NR>=5 { exit }' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) printf 'unknown option: %s\n' "$1" >&2; exit 2 ;;
  esac
done

section() { printf '\n\033[1m%s\033[0m\n' "$1"; }
note()    { printf '       %s\n' "$1"; }
warn()    { printf '  \033[33mWARN\033[0m %s\n' "$1"; }
die()     { printf '\033[31m%s\033[0m\n' "$1" >&2; exit 2; }

FEATURE_SETS=(off single_flight admission jobserver_fixed jobserver_elastic)
SUMMARY_HEADER="features	worktrees	temperature	max_wall_s	walls_s	exits	min_mem_available_mb	oom_kills	oomd_kills	hits	misses	compiles_deduplicated	leases_expired	reserve_waits	longest_reserve_wait_ms	peak_tokens_withdrawn	tokens_restored_total	lease_waiting_lines	reserve_waiting_lines	reserve_wait_ms	run_dir"

# ---------------------------------------------------------------------------
section "0. prerequisites"

[[ -n "$REPO" && -n "$BUILD" ]] || die "--repo and --build are required (see --help)"
REPO="$(cd "$REPO" 2>/dev/null && git rev-parse --show-toplevel 2>/dev/null)" ||
  die "--repo is not a git checkout"
[[ -x "$VCACHE" ]] || die "no vcache binary at $VCACHE; run make first, or pass --vcache"
VCACHE="$(realpath "$VCACHE")"
SCRATCH="$(realpath -m "$SCRATCH")"
case "$SCRATCH" in
  /tmp|/tmp/*) die "--scratch must not be under /tmp: worktrees and stores outgrow it" ;;
esac
[[ -z "$CACHE_DIR" ]] || CACHE_DIR="$(realpath -m "$CACHE_DIR")"
[[ -z "$JOBS" ]] && JOBS="$(nproc 2>/dev/null || echo 4)"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "--jobs must be a positive integer"
[[ -z "$MIN_JOBS" || "$MIN_JOBS" =~ ^[1-9][0-9]*$ ]] || die "--min-jobs must be a positive integer"

if (( MATRIX )); then
  [[ -n "$WORKTREES" ]] || WORKTREES="2,3"
  [[ -n "$TSV" ]] || TSV="$SCRATCH/results.tsv"
  [[ -z "$CACHE_DIR" ]] || die "--matrix makes a fresh store per run; drop --cache-dir"
else
  [[ -n "$WORKTREES" ]] || WORKTREES=2
  [[ "$WORKTREES" == 2 || "$WORKTREES" == 3 ]] || die "--worktrees must be 2 or 3"
  printf '%s\n' "${FEATURE_SETS[@]}" | grep -qxF -- "$FEATURES" ||
    die "--features must be one of: ${FEATURE_SETS[*]}"
  [[ "$TEMPERATURE" == cold || "$TEMPERATURE" == warm ]] || die "--temperature must be cold or warm"
fi
MAX_WORKTREES=0
for n in ${WORKTREES//,/ }; do
  [[ "$n" == 2 || "$n" == 3 ]] || die "--worktrees takes 2 or 3, got $n"
  (( n > MAX_WORKTREES )) && MAX_WORKTREES=$n
done

note "$("$VCACHE" --version | head -1) at $VCACHE"
note "repo $REPO at $(git -C "$REPO" rev-parse --short HEAD)"
note "scratch $SCRATCH"
if grep -qE '(^|[[:space:]])-j' <<<"$BUILD"; then
  warn "the build command passes -j; the jobserver sets need the build tool to read MAKEFLAGS"
fi
[[ -n "$CLEAN" ]] || warn "no --clean: a second run on a worktree measures an up-to-date build"

mkdir -p "$SCRATCH/worktrees" "$SCRATCH/targets" "$SCRATCH/runs" "$SCRATCH/masq" ||
  die "cannot create $SCRATCH"

# ---------------------------------------------------------------------------
section "1. worktrees"

REPO_NAME="$(basename "$REPO")"
HEAD_SHA="$(git -C "$REPO" rev-parse HEAD)"
WT=()
for (( i = 1; i <= MAX_WORKTREES; i++ )); do
  wt="$SCRATCH/worktrees/$REPO_NAME-$i"
  WT[i]="$wt"
  mkdir -p "$SCRATCH/targets/$i"
  if [[ -e "$wt/.git" ]]; then
    [[ -z "$(git -C "$wt" status --porcelain --untracked-files=no)" ]] ||
      die "$wt has local changes; commit or move them before benchmarking"
    git -C "$wt" checkout -q --detach "$HEAD_SHA" || die "cannot move $wt to $HEAD_SHA"
    note "reused $wt"
  else
    git -C "$REPO" worktree add -q --detach "$wt" "$HEAD_SHA" || die "cannot create $wt"
    if [[ -n "$SETUP" ]]; then
      ( cd "$wt" && bash -c "$SETUP" ) > "$wt.setup.log" 2>&1 ||
        die "--setup failed in $wt; see $wt.setup.log"
    fi
    note "created $wt"
  fi
done

if [[ -n "$MASQUERADE" ]]; then
  for c in $MASQUERADE; do ln -sfn "$VCACHE" "$SCRATCH/masq/$c"; done
  # A ccache masquerade directory ahead of ours would take the compile first.
  BUILD_PATH="$SCRATCH/masq:$(tr ':' '\n' <<<"$PATH" | grep -v ccache | paste -sd:)"
  note "masquerading as: $MASQUERADE"
else
  BUILD_PATH="$PATH"
fi

# ---------------------------------------------------------------------------

# status_value <file> <row name>: the value of one `--daemon-status` row.
status_value() {
  awk -v name="$2" 'index($0, name " ") == 1 {
    v = substr($0, length(name) + 1); sub(/^ +/, "", v); print v; exit }' "$1"
}

stats_value() {
  awk -v name="$2" 'index($0, name " ") == 1 { print $NF; exit }' "$1"
}

# sampler <samples.csv> <stop file> <with daemon>: MemAvailable and the scheduler
# rows once a second until the stop file appears.
sampler() {
  local csv=$1 stop=$2 with_daemon=$3 status mem
  local row col value
  local -a cols=("compile sessions" "leases held" "leases waiting" "memory reserved"
                 "memory waiting" "jobserver tokens free" "jobserver tokens withdrawn")
  printf 'epoch_s,mem_available_kb,compile_sessions,leases_held,leases_waiting,memory_reserved_kb,memory_waiting,tokens_free,tokens_withdrawn\n' > "$csv"
  status="$(dirname "$csv")/.status-sample"
  while [[ ! -e "$stop" ]]; do
    mem=$(awk '/^MemAvailable:/ { print $2; exit }' /proc/meminfo)
    row="$(date +%s),$mem"
    if (( with_daemon )) && "$VCACHE" --daemon-status > "$status" 2>/dev/null; then
      for col in "${cols[@]}"; do
        value=$(status_value "$status" "$col")
        row+=",${value:-}"
      done
    else
      row+=",,,,,,,"
    fi
    printf '%s\n' "$row" >> "$csv"
    sleep 1
  done
}

# oom_events <since epoch> <kernel|oomd>: kills logged since the run started,
# or n/a when the log is not readable.
oom_events() {
  local since=$1 source=$2
  if [[ "$source" == kernel ]]; then
    if dmesg -T >/dev/null 2>&1; then
      dmesg --since "@$since" 2>/dev/null | grep -ciE 'out of memory|oom-kill|killed process'
    elif journalctl -k -n 0 -q >/dev/null 2>&1; then
      journalctl -k --since "@$since" --no-pager -q 2>/dev/null |
        grep -ciE 'out of memory|oom-kill|killed process'
    else
      echo n/a
    fi
  else
    if journalctl -u systemd-oomd -n 0 -q >/dev/null 2>&1; then
      journalctl -u systemd-oomd --since "@$since" --no-pager -q 2>/dev/null | grep -c 'Killed '
    else
      echo n/a
    fi
  fi
}

# run_one <features> <worktrees> <temperature>: one measured run, in a subshell
# so its exports, daemon and traps end with it.
run_one() (
  local features=$1 count=$2 temperature=$3
  local run created_store=0 store i
  run="$SCRATCH/runs/$(date +%Y%m%d-%H%M%S)-$features-${count}wt-$temperature"
  mkdir -p "$run" || die "cannot create $run"
  section "$features, $count worktrees, $temperature -> $run"

  if [[ -n "$CACHE_DIR" ]]; then
    store="$CACHE_DIR"
    if [[ "$temperature" == cold && -n "$(ls -A "$store" 2>/dev/null)" ]]; then
      die "a cold run needs an empty store; $store is not"
    fi
  else
    store="$run/store"
    created_store=1
  fi
  mkdir -p "$store"

  export VCACHE_DIR="$store"
  export VCACHE_CACHE_SIZE="${VCACHE_CACHE_SIZE:-100G}"
  export VCACHE_RUST_PATH_ENV_VARS="${VCACHE_RUST_PATH_ENV_VARS:-OUT_DIR}"
  export RUSTC_WRAPPER="$VCACHE"
  export FBG_VCACHE=0 SUNBIRD_VCACHE=0
  export PATH="$BUILD_PATH"
  unset VCACHE_LOG VCACHE_DAEMON_SOCKET VCACHE_DAEMON_SINGLE_FLIGHT VCACHE_DAEMON_ADMISSION \
        VCACHE_DAEMON_JOBSERVER VCACHE_DAEMON_JOBSERVER_JOBS VCACHE_DAEMON_JOBSERVER_MIN_JOBS

  if VCACHE_DAEMON=on "$VCACHE" --daemon-status >/dev/null 2>&1; then
    die "a daemon already serves $store; this script only measures daemons it starts"
  fi

  # build_env <index>: the per-worktree part of a build's environment.
  build_env() {
    local i=$1
    export VCACHE_ROOTS="${WT[i]}=/src/bench-tree:$SCRATCH/targets/$i=/src/bench-target"
    export CARGO_TARGET_DIR="$SCRATCH/targets/$i"
  }
  clean_tree() {
    [[ -n "$CLEAN" ]] || return 0
    ( build_env "$1"; cd "${WT[$1]}" && VCACHE_DAEMON=off bash -c "$CLEAN" ) \
      >> "$run/clean.log" 2>&1 || die "--clean failed in ${WT[$1]}; see $run/clean.log"
  }

  if [[ "$temperature" == warm ]]; then
    note "warming the store from worktree 1, daemon off"
    clean_tree 1
    ( build_env 1; cd "${WT[1]}" && VCACHE_DAEMON=off VCACHE_LOG="$run/warmup.vcache.log" \
        bash -c "$BUILD" ) > "$run/warmup.build.log" 2>&1 ||
      die "warm-up build failed; see $run/warmup.build.log"
  fi
  for (( i = 1; i <= count; i++ )); do clean_tree "$i"; done
  "$VCACHE" --zero-stats >/dev/null

  local with_daemon=1
  case "$features" in
    off) with_daemon=0; export VCACHE_DAEMON=off ;;
    single_flight)
      export VCACHE_DAEMON=on VCACHE_DAEMON_SINGLE_FLIGHT=1 VCACHE_DAEMON_ADMISSION=0
      export VCACHE_DAEMON_JOBSERVER=0 ;;
    admission)
      export VCACHE_DAEMON=on VCACHE_DAEMON_SINGLE_FLIGHT=1 VCACHE_DAEMON_ADMISSION=1
      export VCACHE_DAEMON_JOBSERVER=0 ;;
    jobserver_fixed|jobserver_elastic)
      export VCACHE_DAEMON=on VCACHE_DAEMON_SINGLE_FLIGHT=1 VCACHE_DAEMON_ADMISSION=1
      export VCACHE_DAEMON_JOBSERVER=1 VCACHE_DAEMON_JOBSERVER_JOBS="$JOBS"
      if [[ "$features" == jobserver_fixed ]]; then
        export VCACHE_DAEMON_JOBSERVER_MIN_JOBS="$JOBS"
      elif [[ -n "$MIN_JOBS" ]]; then
        export VCACHE_DAEMON_JOBSERVER_MIN_JOBS="$MIN_JOBS"
      else
        unset VCACHE_DAEMON_JOBSERVER_MIN_JOBS
      fi ;;
  esac

  local -a pids=()
  local stop="$run/.sampler-stop" sampler_pid="" daemon_started=0
  # shellcheck disable=SC2329  # invoked by the trap
  finish() {
    local pid
    for pid in "${pids[@]}"; do kill -TERM -- "-$pid" 2>/dev/null; done
    : > "$stop"
    [[ -n "$sampler_pid" ]] && wait "$sampler_pid" 2>/dev/null
    if (( daemon_started )); then
      "$VCACHE" --stop-daemon > "$run/stop-daemon.txt" 2>&1
      daemon_started=0
    fi
  }
  trap finish EXIT
  trap 'exit 130' INT TERM

  if (( with_daemon )); then
    VCACHE_DAEMON_IDLE_TIMEOUT=0 VCACHE_LOG="$run/daemon.vcache.log" \
      "$VCACHE" --start-daemon > "$run/start-daemon.txt" 2>&1 ||
      die "--start-daemon failed; see $run/start-daemon.txt"
    daemon_started=1
  fi
  if [[ "$features" == jobserver_* ]]; then
    local line
    line="$("$VCACHE" --jobserver-env)" || die "the daemon has no jobserver pool"
    export MAKEFLAGS="${line#MAKEFLAGS=}"
    unset MFLAGS GNUMAKEFLAGS CARGO_BUILD_JOBS
    note "MAKEFLAGS=$MAKEFLAGS"
  fi

  sampler "$run/samples.csv" "$stop" "$with_daemon" &
  sampler_pid=$!

  local started_epoch started_at
  started_epoch=$(date +%s)
  started_at=$EPOCHREALTIME
  declare -A index_of_pid=()
  for (( i = 1; i <= count; i++ )); do
    # setsid makes each build its own process group, so an interrupt can stop
    # the whole build rather than only its shell.
    ( build_env "$i"; cd "${WT[i]}" && VCACHE_LOG="$run/wt$i.vcache.log" \
        exec setsid bash -c "$BUILD" ) > "$run/wt$i.build.log" 2>&1 &
    pids+=($!)
    index_of_pid[$!]=$i
  done

  local -a walls=() exits=()
  local done_pid rc pid
  local -a running
  while (( ${#pids[@]} > 0 )); do
    done_pid=""
    wait -n -p done_pid "${pids[@]}"
    rc=$?
    [[ -n "$done_pid" ]] || break
    i=${index_of_pid[$done_pid]}
    walls[i]=$(awk -v a="$started_at" -v b="$EPOCHREALTIME" 'BEGIN { printf "%.1f", b - a }')
    exits[i]=$rc
    note "worktree $i: exit $rc after ${walls[i]} s"
    running=()
    for pid in "${pids[@]}"; do [[ "$pid" == "$done_pid" ]] || running+=("$pid"); done
    pids=("${running[@]}")
  done

  : > "$stop"
  wait "$sampler_pid" 2>/dev/null
  sampler_pid=""

  local status="$run/daemon-status.txt"
  if (( with_daemon )); then
    "$VCACHE" --daemon-status > "$status" 2>&1
    finish
    cp "$store/daemon/log" "$run/daemon.log" 2>/dev/null
  fi
  "$VCACHE" --show-stats > "$run/stats.txt" 2>&1

  local counter
  local -a counters=()
  for counter in "compiles deduplicated" "leases expired" "reserve waits" "longest wait ms"; do
    if (( with_daemon )); then counters+=("$(status_value "$status" "$counter")")
    else counters+=("-"); fi
  done
  local peak_withdrawn="-" restored="-" reserve_waiting_lines="-"
  if [[ "$features" == jobserver_* ]]; then
    peak_withdrawn=$(awk -F, 'NR > 1 && $9 != "" && $9 + 0 > m { m = $9 + 0 } END { print m + 0 }' \
                     "$run/samples.csv")
    restored=$(status_value "$status" "jobserver tokens restored total")
  fi
  (( with_daemon )) && reserve_waiting_lines=$(grep -c 'reserve: waiting' "$run/daemon.vcache.log" 2>/dev/null)

  local -a lease_lines=() reserve_ms=()
  for (( i = 1; i <= count; i++ )); do
    lease_lines+=("$(grep -c 'lease: waiting' "$run/wt$i.vcache.log" 2>/dev/null || true)")
    reserve_ms+=("$(awk '/reserve: request waited [0-9]+ ms/ {
                         for (f = 1; f < NF; f++) if ($f == "waited") s += $(f + 1) }
                       END { print s + 0 }' "$run/wt$i.vcache.log" 2>/dev/null)")
  done

  local max_wall min_mem
  max_wall=$(printf '%s\n' "${walls[@]}" | sort -g | tail -1)
  min_mem=$(awk -F, 'NR > 1 && (m == "" || $2 + 0 < m) { m = $2 + 0 }
                     END { if (m == "") print "n/a"; else printf "%d", m / 1024 }' "$run/samples.csv")
  join() { local IFS=,; printf '%s' "$*"; }

  local row
  row="$features	$count	$temperature	$max_wall	$(join "${walls[@]}")	$(join "${exits[@]}")	$min_mem"
  row+="	$(oom_events "$started_epoch" kernel)	$(oom_events "$started_epoch" oomd)"
  row+="	$(stats_value "$run/stats.txt" "cache hit (disk)")	$(stats_value "$run/stats.txt" "cache miss")"
  row+="	$(join "${counters[@]}" | tr , '\t')	$peak_withdrawn	$restored"
  row+="	$(join "${lease_lines[@]}")	$reserve_waiting_lines	$(join "${reserve_ms[@]}")	$run"
  printf '%s\n%s\n' "$SUMMARY_HEADER" "$row" > "$run/summary.tsv"
  if [[ -n "$TSV" ]]; then
    [[ -s "$TSV" ]] || printf '%s\n' "$SUMMARY_HEADER" > "$TSV"
    printf '%s\n' "$row" >> "$TSV"
  fi
  printf '%s\n%s\n' "$SUMMARY_HEADER" "$row" | column -t -s $'\t'

  if (( created_store && ! KEEP_STORE )); then
    "$VCACHE" --clear >/dev/null 2>&1 || warn "could not --clear $store"
  fi
  local failed=0
  for rc in "${exits[@]}"; do (( rc == 0 )) || failed=1; done
  (( failed == 0 )) || warn "a build failed; see $run/wt*.build.log"
  return "$failed"
)

# ---------------------------------------------------------------------------
trap 'exit 130' INT TERM
STATUS=0
if (( MATRIX )); then
  for count in ${WORKTREES//,/ }; do
    for temperature in cold warm; do
      for features in "${FEATURE_SETS[@]}"; do
        run_one "$features" "$count" "$temperature" || STATUS=1
      done
    done
  done
  section "results: $TSV"
  column -t -s $'\t' "$TSV"
else
  run_one "$FEATURES" "$WORKTREES" "$TEMPERATURE" || STATUS=1
fi
exit "$STATUS"
