#!/usr/bin/env python3
"""chfab.py -- shared helpers for the whole-machine A/B tools (MAME is the
reference).

    import chfab

MAME is run with `-video none -sound none` and killed hard if it outlives its
budget (it ignores SIGTERM in that mode). Frames are what MAME's screen holds
at each frame notifier: 192x262 32-bit little-endian ARGB.
"""
import os
import signal
import subprocess
import tempfile

MAME_DIR = os.path.expanduser(os.environ.get("MAME_DIR", "~/Workspace/mame"))
MAME = os.path.join(MAME_DIR, "mame")
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
W, H = 192, 262
FRAME = W * H * 4

# matrix bit (host.h: P1 0-11 as 1 2 3 4 5 6 7 8 9 CLEAR 0 ENTER, P2 12-23,
# 24 START, 25 OPTION, 26 SELECT) -> MAME's ioport and bit mask
_COL = {0: ("col1", 0x08), 1: ("col2", 0x08), 2: ("col3", 0x08),
        3: ("col1", 0x04), 4: ("col2", 0x04), 5: ("col3", 0x04),
        6: ("col1", 0x02), 7: ("col2", 0x02), 8: ("col3", 0x02),
        9: ("col1", 0x01), 10: ("col2", 0x01), 11: ("col3", 0x01)}


def key_port(bit):
    """-> (MAME ioport tag, mask) for matrix bit `bit`."""
    if bit >= 24:
        return "panel", 1 << (bit - 24)
    p, k = divmod(bit, 12)
    col, mask = _COL[k]
    return "controller%d_%s" % (p + 1, col), mask


def stick_port(stick, ourbit):
    """-> (tag, mask) of MAME's 'joysticks' port for our stick bit
    (0 UP 1 RIGHT 2 DOWN 3 LEFT)."""
    mame = {0: 0x08, 1: 0x02, 2: 0x04, 3: 0x01}[ourbit]
    return "joysticks", mame << (4 * stick)


def run_mame(cart, lua, env, seconds, extra=(), timeout=None, log=None, cartslot=None):
    """Run MAME's `channelf` headless for `seconds` of emulated time,
    hard-killed after `timeout` wall seconds. Returns the exit status."""
    cmd = [MAME, "channelf", "-cart", cart, "-video", "none", "-sound", "none",
           "-seconds_to_run", str(seconds), "-nothrottle", "-skip_gameinfo",
           "-autoboot_script", lua]
    if cartslot:
        cmd[2:2] = ["-cartslot", cartslot]
    cmd += list(extra)
    e = dict(os.environ)
    e.update({k: str(v) for k, v in env.items()})
    logf = open(log, "wb") if log else subprocess.DEVNULL
    p = subprocess.Popen(cmd, env=e, stdout=logf, stderr=subprocess.STDOUT,
                         start_new_session=True)
    try:
        p.wait(timeout=timeout or (20 + seconds * 3))
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
    if log:
        logf.close()
    return p.returncode


def find_runner():
    env = os.environ.get("CHF_HEADLESS")
    if env:
        return env
    for cand in ("build/core/tests/chf_headless", "build-core/core/tests/chf_headless"):
        c = os.path.join(ROOT, cand)
        if os.path.exists(c):
            return c
    raise SystemExit("set CHF_HEADLESS to the chf_headless binary")


def fnv_frame(px):
    """FNV-1a-style 64-bit hash of a frame's 0x00RRGGBB words, one multiply per
    pixel -- what chf_headless -hash and the config_frames test compute."""
    h = 14695981039346656037
    for i in range(0, len(px), 4):
        h ^= int.from_bytes(px[i:i + 3], "little")
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def work_dir():
    w = os.environ.get("ARCAB_WORK") or tempfile.mkdtemp(prefix="chfab-")
    os.makedirs(w, exist_ok=True)
    return w
