#!/bin/sh
#
# ArcadeOS boot smoke test.
#
# Boots arcadeos.img headless in QEMU and asserts the full happy path:
#   1. kernel init completes and the launcher reaches Ring 3
#   2. the REST API answers on port 8080 (status/games/scores)
#   3. pressing Enter through the QEMU monitor launches a game
#   4. the serial log contains no crash/panic/unknown-syscall lines
#
# Usage: sh tools/smoke_test.sh   (image must already be built)
# Exits 0 on success, 1 with a diagnostic on failure.

set -u

IMG=arcadeos.img
SERIAL=serial-smoke.log
MON=qemu-smoke.sock
API=http://localhost:8080
QEMU_PID=""

fail() {
    echo "SMOKE FAIL: $1" >&2
    echo "--- serial tail ---" >&2
    tail -20 "$SERIAL" 2>/dev/null >&2
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null
    exit 1
}

cleanup() {
    [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null
    rm -f "$MON"
}
trap cleanup EXIT

# Poll for a pattern in the serial log. $1=pattern $2=timeout_s $3=min_count
wait_serial() {
    _i=0
    _want="${3:-1}"
    while [ $_i -lt "$2" ]; do
        _n=$(grep -c "$1" "$SERIAL" 2>/dev/null || true)
        [ "${_n:-0}" -ge "$_want" ] && return 0
        sleep 1
        _i=$((_i + 1))
    done
    return 1
}

[ -f "$IMG" ] || fail "$IMG not built (run make first)"
rm -f "$SERIAL" "$MON"

qemu-system-x86_64 -m 128 \
    -audiodev none,id=snd0 \
    -device AC97,audiodev=snd0 \
    -machine pcspk-audiodev=snd0 \
    -netdev user,id=n0,hostfwd=tcp::8080-10.0.2.86:80,hostfwd=udp::8007-10.0.2.86:7 \
    -device rtl8139,netdev=n0 \
    -drive file=$IMG,format=raw,if=none,id=gamedisk \
    -device ahci,id=ahci0 \
    -device ide-hd,drive=gamedisk,bus=ahci0.0 \
    -boot c -usb \
    -serial file:$SERIAL \
    -display none \
    -monitor unix:$MON,server,nowait \
    -no-reboot &
QEMU_PID=$!

# 1. Boot markers (generous timeouts: CI runners run TCG, not KVM/HVF)
wait_serial "Kernel init complete"    90 || fail "kernel never finished init"
wait_serial "NX enabled"              10 || fail "W^X not active"
wait_serial "Volume mounted"          10 || fail "FAT32 volume missing"
wait_serial "Jumping to Ring 3"       30 || fail "launcher never reached Ring 3"

# QEMU still alive? (-no-reboot: a triple fault exits silently)
kill -0 "$QEMU_PID" 2>/dev/null || fail "QEMU exited (triple fault?)"

# 2. REST API
sleep 2
curl -sf --max-time 5 "$API/api/status" | grep -q '"os":"ArcadeOS"' \
    || fail "/api/status bad or unreachable"
curl -sf --max-time 5 "$API/api/games"  | grep -q 'PONG.ELF' \
    || fail "/api/games missing PONG.ELF"
curl -sf --max-time 5 "$API/api/scores" >/dev/null \
    || fail "/api/scores unreachable"

# 2b. UDP echo service (proves the netplay datagram path both ways).
# python3 instead of nc -u: BSD nc exits before reading the reply.
python3 - <<'PYEOF' || fail "UDP echo on port 8007 did not answer"
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(5)
s.sendto(b"arcade", ("127.0.0.1", 8007))
try:
    data, _ = s.recvfrom(64)
    sys.exit(0 if data == b"arcade" else 1)
except socket.timeout:
    sys.exit(1)
PYEOF

# 3. Launch a game: Enter on the launcher (retry — the launcher needs a
#    few frames before it polls input)
_try=0
while [ $_try -lt 5 ]; do
    printf "sendkey ret\n" | nc -U "$MON" -w 1 >/dev/null 2>&1
    wait_serial "\[EXEC\] Loading ELF" 5 2 && break
    _try=$((_try + 1))
done
wait_serial "\[EXEC\] Loading ELF" 1 2 || fail "Enter did not launch a game"

# 4. Nothing crashed along the way
if grep -qE "CRASH|PANIC|Unknown syscall" "$SERIAL"; then
    fail "crash/panic/unknown-syscall in serial log"
fi

echo "SMOKE PASS"
exit 0
