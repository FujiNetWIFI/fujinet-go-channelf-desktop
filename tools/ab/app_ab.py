#!/usr/bin/env python3
"""app_ab.py -- the FujiNet apps against MAME, through a recorded BoIP conversation.

    app_ab.py [--machine M] [--only NAME] [--report] [--keep]

The cartridge's mailbox talks to fujinet-pc over BoIP. To hold MAME and our core
to the same network each scenario is RECORDED once from a private fujinet-pc
(a copy of ~/Workspace/fujinet-pc-rs232/build/dist on its own port, in its own
network namespace so a client that dials out finds nothing, and the user's FujiNet
is never touched), with our core driving the app; the recorded frames are then
REPLAYED (tools/ab/boipreplay.py) to MAME and to chf_headless in turn, and the
two runs are compared frame for frame (picture, VRAM and latches, every I/O write
with its CPU clock) with the FujiNet cartridge's 60 Hz tick checked every frame.
The replay also checks that both sides ask the same questions in the same order.

Scenarios: the firmware's test clients (fujitest: a real transaction; fujiboot:
mounts /hello.bin from the SD host, takes the pushed image and swaps it in;
fujibank: APPBANK pages and the tick), CONFIG (host list, directory, boot) booting
a plain image, and the real apps booted by CONFIG from the SD host: M.U.L.E. (a 64K
APPBANK image), 5 Card Stud, Fujitzee and Battleship.

Needs the real BIOS (CHF_BIOS_DIR), MAME, the assembler (the firmware's test ROMs are
built from $CHF_FW_DIR, default ~/Workspace/fw-chf-bake/pico/channelf), fujinet-pc
($FUJINET_PC_DIST) and `unshare` with unprivileged user namespaces; exit 77 without
them. The app images come from their repositories: $CHF_MULE (default
~/Workspace/fujinet-multiplayer-mule/clients/channelf/build/mule.bin), $CHF_5CARD,
$CHF_FUJITZEE and $CHF_BATTLESHIP; a scenario whose image is missing is skipped.
"""
import argparse
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import boipreplay  # noqa: E402
import chfab  # noqa: E402

FW_DIR = os.path.expanduser(os.environ.get("CHF_FW_DIR", "~/Workspace/fw-chf-bake/pico/channelf"))
FN_DIST = os.path.expanduser(os.environ.get("FUJINET_PC_DIST", "~/Workspace/fujinet-pc-rs232/build/dist"))
HOME = os.path.expanduser("~/Workspace")
APPS = {
    "mule": os.path.expanduser(os.environ.get("CHF_MULE", HOME + "/fujinet-multiplayer-mule/clients/channelf/build/mule.bin")),
    "5card": os.path.expanduser(os.environ.get("CHF_5CARD", "")),     # built from the branch below when unset
    "fujitzee": os.path.expanduser(os.environ.get("CHF_FUJITZEE", HOME + "/fujinet-fujitzee/channelf/build/fujitzee.bin")),
    "battleship": os.path.expanduser(os.environ.get("CHF_BATTLESHIP", HOME + "/fujinet-battleship/channelf/build/battleship.bin")),
}
BOIP_PORT, PROXY_PORT = 9972, 9973        # inside the recording namespace


def push(frame, bit=7, hold=6):
    return [(frame, 0, bit, 1), (frame + hold, 0, bit, 0)]


# CONFIG, driven like cfgdrive.lua: let the host list settle, open host 0 (the SD card), move to the
# second file (row 0 is the dummy) and boot it
def config_boot(extra=()):
    ev = push(300) + push(420, 2) + push(440)
    ev += list(extra)
    return ev


def after_boot(first, n=4, step=90):
    """Presses to give a booted app something to react to."""
    ev = []
    for i in range(n):
        ev += push(first + i * step, 7 if i % 2 == 0 else 2)
    return ev


# name: (cart or None for CONFIG, SD files {name: key}, ctl events, frames)
SCENARIOS = {
    "fujitest": ("test:fujitest", {}, [], 300),
    "fujiboot": ("test:fujiboot", {"hello.bin": "test:hello"}, [], 900),
    "fujibank": ("test:fujibank", {}, [], 500),
    "config-hello": (None, {"aaa.bin": "test:hello", "bbb.bin": "test:hello"}, config_boot(), 1100),
    "mule": (None, {"aaa.bin": "test:hello", "bbb.bin": "app:mule"}, config_boot(after_boot(1700)), 2400),
    "5card": (None, {"aaa.bin": "test:hello", "bbb.bin": "app:5card"}, config_boot(after_boot(1000)), 1800),
    "fujitzee": (None, {"aaa.bin": "test:hello", "bbb.bin": "app:fujitzee"}, config_boot(after_boot(1000)), 1800),
    "battleship": (None, {"aaa.bin": "test:hello", "bbb.bin": "app:battleship"}, config_boot(after_boot(1000)), 1800),
}


def tools_ok():
    return (chfab.bios_dir() and os.path.exists(chfab.MAME) and chfab.have_asl()
            and os.path.exists(os.path.join(FN_DIST, "fujinet")) and shutil.which("unshare")
            and subprocess.run(["unshare", "-rn", "true"], stderr=subprocess.DEVNULL).returncode == 0)


def build_testroms(work):
    d = os.path.join(work, "fw")
    if not os.path.exists(os.path.join(d, "build", "fujitest.bin")):
        os.makedirs(d, exist_ok=True)
        for item in ("build.sh", "testrom", "tools"):
            src, dst = os.path.join(FW_DIR, item), os.path.join(d, item)
            if os.path.isdir(src):
                shutil.copytree(src, dst, dirs_exist_ok=True)
            else:
                shutil.copy(src, dst)
        subprocess.run(["./build.sh", "hello", "fujitest", "fujiboot", "fujibank"], cwd=d, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       env=dict(os.environ, ASL=chfab.ASL, P2BIN=chfab.P2BIN))
    return d


def config_image(work):
    p = os.path.join(work, "config.bin")
    t = open(os.path.join(chfab.ROOT, "core/chf/fuji-generated/fujiconfigrom.h")).read()
    body = t[t.index("{", t.index("[")):]
    open(p, "wb").write(bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", body)))
    return p


def build_5card(work):
    """5 Card Stud's Channel F client, from its repository's add-channelf branch (git archive: the
    repository is not touched), against an endpoint nothing listens on."""
    repo = os.path.expanduser(os.environ.get("CHF_5CARD_REPO", "~/Workspace/fujinet-5cardstud"))
    d = os.path.join(work, "5card")
    out = os.path.join(d, "channelf", "build", "5card.bin")
    if os.path.exists(out):
        return out
    os.makedirs(d, exist_ok=True)
    try:
        a = subprocess.run(["git", "-C", repo, "archive", os.environ.get("CHF_5CARD_REF", "origin/add-channelf"),
                            "channelf"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True)
        subprocess.run(["tar", "-x", "-C", d], input=a.stdout, check=True)
        open(os.path.join(d, "channelf", "src", "endpoint.inc"), "w").write(
            '; the A/B rig: no server\nUENDPT: DB "http://127.0.0.1:1",0\n')
        subprocess.run(["./build.sh"], cwd=os.path.join(d, "channelf"), check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, env=dict(os.environ, ASL=chfab.ASL, P2BIN=chfab.P2BIN))
    except (subprocess.CalledProcessError, OSError):
        return ""
    return out


def resolve(key, work):
    kind, name = key.split(":")
    if kind == "test":
        p = os.path.join(build_testroms(work), "build", name + ".bin")
    elif name == "5card" and not APPS["5card"]:
        p = build_5card(work)
    else:
        p = APPS.get(name, "")
    return p if p and os.path.exists(p) else None


def fn_template(work):
    """A private fujinet-pc: the dist, without the real GoogleDrive token, BoIP on our port."""
    d = os.path.join(work, "fn-template")
    if not os.path.exists(d):
        shutil.copytree(FN_DIST, d, ignore=shutil.ignore_patterns("SD"))
        os.makedirs(os.path.join(d, "SD"))
        ini = os.path.join(d, "fnconfig.ini")
        t = open(ini).read()
        t = re.sub(r"\[GoogleDrive\].*?(?=\n\[)", "", t, flags=re.S)
        t = re.sub(r"(\[BOIP\]\nenabled=1\nhost=localhost\nport=)\d+", r"\g<1>%d" % BOIP_PORT, t)
        open(ini, "w").write(t)
    return d


def wait_port(port, secs=15):
    end = time.time() + secs
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def ours_cmd(machine, cart, frames, ctls, boip, extra=()):
    first = chfab.MACHINES[machine][3]
    cmd = [chfab.find_runner(), "-machine", machine, "-bios-dir", chfab.bios_dir(), "-bios", first, "-frames",
           str(frames), "-cart", cart, "-boip", boip]
    return cmd + chfab.ctl_args(ctls) + list(extra)


def record_child(machine, cart, frames, ctls_spec, sd_spec, outdir):
    """Runs inside `unshare -rn`: a private fujinet-pc, the recording proxy, our core."""
    subprocess.run(["ip", "link", "set", "lo", "up"], check=False, stderr=subprocess.DEVNULL)
    ctls = [tuple(int(x) for x in e.split(":")) for e in ctls_spec.split(",") if e]
    fn = os.path.join(outdir, "fn")
    shutil.copytree(os.path.join(os.path.dirname(outdir), "fn-template"), fn, dirs_exist_ok=True)
    for item in sd_spec.split(","):
        if item:
            name, path = item.split("=", 1)
            shutil.copy(path, os.path.join(fn, "SD", name))
    log = open(os.path.join(outdir, "fujinet.log"), "wb")
    proc = subprocess.Popen(["./fujinet", "-u", "http://127.0.0.1:8072", "-c", "fnconfig.ini"], cwd=fn,
                            stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        if not wait_port(BOIP_PORT):
            print("record: fujinet-pc never listened", file=sys.stderr)
            return 3
        rec = boipreplay.Recorder(PROXY_PORT, BOIP_PORT)
        r = subprocess.run(ours_cmd(machine, cart, frames, ctls, "127.0.0.1:%d" % PROXY_PORT), stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=900)
        time.sleep(0.3)
        rec.save(os.path.join(outdir, "trace.json"))
        rec.close()
        return r.returncode
    finally:
        os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()


def record(machine, cart, frames, ctls, sd, outdir):
    os.makedirs(outdir, exist_ok=True)
    spec = ",".join("%d:%d:%d:%d" % e for e in ctls)
    sdspec = ",".join("%s=%s" % kv for kv in sd.items())
    cmd = ["unshare", "-rn", sys.executable, "-I", os.path.abspath(__file__), "--record-child", machine, cart,
           str(frames), spec, sdspec, outdir]
    p = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=1200)
    path = os.path.join(outdir, "trace.json")
    if p.returncode != 0 or not os.path.exists(path):
        return None, p.stderr.decode(errors="replace")[-200:]
    return boipreplay.load(path), ""


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def run_scenario(name, machine, work, keep):
    cartkey, sdkeys, ctls, frames = SCENARIOS[name]
    cart = config_image(work) if cartkey is None else resolve(cartkey, work)
    sd = {}
    for sdname, key in sdkeys.items():
        p = resolve(key, work)
        if not p:
            return "SKIP %-14s %-8s image not available (%s)" % (name, machine, key), True
        sd[sdname] = p
    if not cart:
        return "SKIP %-14s %-8s image not available (%s)" % (name, machine, cartkey), True
    outdir = os.path.join(work, "rec-%s-%s" % (name, machine))
    trace, err = record(machine, cart, frames, ctls, sd, outdir)
    if trace is None:
        return "FAIL %-14s %-8s recording failed: %s" % (name, machine, err.strip()), False
    nframes = sum(len(c) for c in trace)
    # one replay server for MAME, one for ours, so each side's mismatches are its own
    pm, po = free_port(), free_port()
    rm, ro = boipreplay.Replay(trace, pm), boipreplay.Replay(trace, po)
    try:
        abdir = os.path.join(work, "ab-%s-%s" % (name, machine))
        os.makedirs(abdir, exist_ok=True)
        r = chfab.ab(machine, cart, frames, abdir, ctls=ctls, fujinet=True, iolog=True, tick=True,
                     boip="127.0.0.1:%d" % po, mame_env={"FUJINET_TCP": "127.0.0.1:%d" % pm})
    finally:
        time.sleep(0.3)
        rm.close()
        ro.close()
    line = chfab.report(name, machine, r)
    ok = r.ok and not rm.mismatches and not ro.mismatches
    line += " | %d frames recorded; replay: MAME %d mismatch(es) %d extra, ours %d mismatch(es) %d extra" % (
        nframes, len(rm.mismatches), rm.extra, len(ro.mismatches), ro.extra)
    if not ok and (rm.mismatches or ro.mismatches):
        m = (rm.mismatches or ro.mismatches)[0]
        line += " (first: conn %d frame %d)" % (m[0], m[1])
    return line, ok


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--record-child":
        return record_child(*sys.argv[2:8])
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--machine", action="append", default=[], choices=sorted(chfab.MACHINES))
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not tools_ok():
        print("SKIP: needs the real BIOS (CHF_BIOS_DIR), MAME, the assembler, fujinet-pc and `unshare -rn`")
        return 77
    work = chfab.work_dir()
    fn_template(work)
    build_testroms(work)
    chfab.vm_image(work)
    bad = 0
    for machine in args.machine or ["channelf", "sabavdpl"]:
        for name in SCENARIOS:
            if args.only and name not in args.only:
                continue
            line, ok = run_scenario(name, machine, work, args.keep)
            if not ok and not line.startswith("SKIP"):
                bad += 1
            if args.report or not ok:
                print(line)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
