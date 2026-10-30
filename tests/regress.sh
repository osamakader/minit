#!/bin/bash
# Behaviour tests: run built ./minit and ./minitctl (not as PID 1) against small configs.
# Usage: tests/regress.sh   (from the repo root, after make)
M=${MINIT:-./minit}
CTL=${MINITCTL:-./minitctl}
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export MINIT_SOCKET=$T/minit.sock MINIT_LOG_DIR=$T/log
SLEEP="sleep 7391"  # distinctive, so the leftover check only sees our processes
failed=0

conf() { printf "$2" > "$T/$1.conf"; echo "$T/$1.conf"; }
# run CONFIG SECONDS: output of minit after running that long and then getting SIGTERM
run() {
	$M "$1" > "$T/out" 2>&1 & local p=$!
	sleep "$2"; kill $p 2>/dev/null; wait $p
	cat "$T/out"
}
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; [ -n "$2" ] && echo "$2" | sed 's/^/     /'; failed=1; }
has() { if grep -q -- "$2" <<<"$3"; then ok "$1"; else bad "$1" "$3"; fi; }
lacks() { if grep -q -- "$2" <<<"$3"; then bad "$1" "$3"; else ok "$1"; fi; }

o=$(run "$(conf quote '[service q]\nexec = printf [%%s] a "b c" '"'"'d e'"'"'\nrestart = never\n')" 0.5)
has "exec quoting" "\[a\]\[b c\]\[d e\]" "$o"

o=$(run "$(conf order '[service setup]\ntype = oneshot\nexec = sh -c "sleep 0.3; echo setup"\n[service app]\nexec = echo app\nrestart = never\ndepends = setup\n')" 1)
has "oneshot runs before its dependents" "setup app" "$(echo $o)"

o=$(run "$(conf rfd '[service db]\nready_fd = 3\nexec = sh -c "sleep 0.3; echo db; echo >&3; exec '"$SLEEP"'"\n[service web]\nexec = echo web\nrestart = never\ndepends = db\n')" 1)
has "ready_fd delays dependents" "db web" "$(echo $o)"

o=$(run "$(conf rto '[service h]\nready_fd = 3\nready_timeout = 1\nrestart = never\nexec = '"$SLEEP"'\n')" 1.5)
has "ready_timeout kills a service that never gets ready" "not ready after 1s" "$o"

o=$(run "$(conf give '[service c]\nexec = /bin/false\nrespawn_max = 2\n')" 4)
has "respawn_max gives up" "gave up after 2" "$o"

o=$(run "$(conf miss '[service m]\nexec = no-such-binary\nrestart = never\n')" 0.5)
has "exec failures are reported" "cannot exec no-such-binary: No such file" "$o"

o=$($M "$(conf bad '[service a]\nexec = /bin/true\ncolour = x\n')" 2>&1)
has "config errors name file and line" "bad.conf:3: unknown key" "$o"

o=$($M "$(conf cyc '[service a]\nexec=/bin/true\ndepends=b\n[service b]\nexec=/bin/true\ndepends=a\n')" 2>&1)
has "dependency cycles are rejected" "cycle among: a b" "$o"

run "$(conf log '[service lg]\nlog = file\nexec = sh -c "echo to-file; exec '"$SLEEP"'"\n')" 0.5 >/dev/null
has "log = file" "to-file" "$(cat "$T/log/lg.log" 2>&1)"

o=$(run "$(conf null '[service n]\nlog = null\nrestart = never\nexec = echo hidden\n')" 0.5)
lacks "log = null" "hidden" "$o"

$M "$(conf ctl '[service app]\nexec = '"$SLEEP"'\n')" >/dev/null 2>&1 & p=$!
sleep 0.3
has "minitctl status" "app .*running" "$($CTL status)"
$CTL stop app >/dev/null; sleep 0.3
has "minitctl stop" "app .*stopped" "$($CTL status)"
$CTL start app >/dev/null; sleep 0.3
has "minitctl start" "app .*running" "$($CTL status)"
printf '[service app]\nexec = %s\n[service extra]\nexec = %s\n' "$SLEEP" "$SLEEP" > "$T/ctl.conf"
kill -HUP $p; sleep 0.3
has "SIGHUP reload starts new services" "extra .*running" "$($CTL status)"
kill $p; wait $p

start=$(date +%s%N)
run "$(conf term '[service s]\nexec = '"$SLEEP"'\n')" 0.2 >/dev/null
ms=$(( ($(date +%s%N) - start) / 1000000 ))
if [ $ms -lt 1500 ]; then ok "SIGTERM stops minit quickly (${ms}ms)"; else bad "SIGTERM took ${ms}ms"; fi

sleep 0.2
left=$(pgrep -af "^$SLEEP\$")
if [ -z "$left" ]; then ok "no processes left behind"; else bad "processes left behind" "$left"; pkill -f "^$SLEEP\$"; fi

[ $failed = 0 ] && echo "all tests passed"
exit $failed
