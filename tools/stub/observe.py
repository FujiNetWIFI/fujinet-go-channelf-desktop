#!/usr/bin/env python3
"""observe.py -- black-box observation of the Channel F BIOS entry points
(RESET $0000, clear screen $00D0, push K $0107, pop K $011E) in MAME, or of
a stand-in BIOS built from SPEC.md.

    observe.py                       observe both real first-half BIOS versions
                                     (~1 minute: ~2600 cases x 2 versions, 40 MAME jobs)
    observe.py --bios sl31253        one version
    observe.py --check --rompath DIR check a stand-in: DIR/channelf/sl31253.rom
                                     and sl31254.rom (1 KiB each; MAME warns about
                                     checksums and runs it anyway).  Exit status 1 on
                                     any tier-1 mismatch; tiers: see biosmodel.py/SPEC.md.
                                     Uses the real-BIOS table obs-data/clr_hidden.json
                                     (--like sl90025 to match the other version's clear).
    observe.py --header PATH         also write the C contract (bios_contract.h)
    observe.py --quick               ~1/5 of the cases (smoke test)

Clean-room: the BIOS is a black box.  This tool never reads or dumps BIOS
bytes; MAME loads the ROMs itself and obs.lua records machine state before
and after each call (see obs.lua).  Everything is seeded: the same --seed on
the same BIOS gives the same cases and the same results.

Needs MAME (env MAME_DIR, default ~/Workspace/mame: run from there so its
rompath finds roms/channelf.zip), asl and p2bin (env ASL, P2BIN), `timeout`.

Outputs (under --out, default tools/stub/obs-data):
    <bios>.jsonl.gz      every case: pre-state, post-state, port-write summary
    clr_hidden.json      per-version/mode/r3 table of the hidden columns (0, 122-127)
    report-<bios>.txt    invariants, model checks (tier 1/2/3 mismatches), sensitivity
"""
import sys
sys.dont_write_bytecode = True      # the repo should not collect __pycache__ from a tool run

import argparse
import collections
import gzip
import json
import os
import random
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import biosmodel as bm                      # noqa: E402
from obslib import (apply_launch, build_probe, probe_variant, random_case,  # noqa: E402
                    read_jsonl, run_cases, run_mame, vram_expand)

REAL_BIOS = ("sl31253", "sl90025")
TARGET = {"clr": 0x00D0, "push": 0x0107, "pop": 0x011E}
ROUTINE_OF_GROUP = {}


# ---------------------------------------------------------------------------
# the suite
# ---------------------------------------------------------------------------
def build_suite(pads, seed, quick=False):
    rng = random.Random(seed)
    n = (lambda full, small: small if quick else full)
    groups = collections.OrderedDict()
    nid = [0]

    def add(group, c, routine):
        c.id = nid[0]; nid[0] += 1
        groups.setdefault(group, []).append(c)
        ROUTINE_OF_GROUP[group] = routine
        return c

    def launch_pad(routine, i):
        return ("clr" if routine == "clr" else routine if routine in ("push", "pop") else "jmp") \
            if i % 5 < 3 else ("pk", "lr")[i % 5 - 3]

    def mk(routine, i, **kw):
        pad = launch_pad(routine, i)
        c = random_case(rng, pad, pads, target=TARGET.get(routine))
        for k, v in kw.items():
            setattr(c, k, v)
        apply_launch(c, pads, TARGET.get(routine))
        return c

    # --- RESET -----------------------------------------------------------
    for i in range(n(200, 40)):
        add("reset_rand", random_case(rng, "jmp", pads), "reset")

    # --- clear screen --------------------------------------------------------
    def valid_r59():
        return rng.randrange(bm.CLR_STACK_MIN, bm.CLR_STACK_MAX + 1) | (rng.randrange(4) << 6)

    for i in range(n(200, 40)):                      # r3 = 0, any state
        c = mk("clr", i)
        c.r[3] = 0; c.r[59] = valid_r59()
        add("clr_r3zero", c, "clr")
    base = random_case(rng, "clr", pads, target=TARGET["clr"])
    base.r[59] = 0x28
    for mode, low in (("A", 0), ("B", 1)):           # r3 sweep on one fixed base, r31 & 3 = 0 / 1
        for r3 in range(256):
            c = base.copy(); c.r[3] = r3; c.r[31] = (c.r[31] & ~3) | low
            add("clr_r3sweep" + mode, c, "clr")
    for i in range(n(100, 20)):                      # random r3, random state
        c = mk("clr", i)
        c.r[3] = rng.randrange(256); c.r[59] = valid_r59()
        add("clr_r3rand", c, "clr")
    for i in range(n(90, 18)):                       # colour-only r3 values
        c = mk("clr", i)
        c.r[3] = (1 + i % 3) << 6; c.r[59] = valid_r59()
        if i % 3 != 2:
            c.r[31] &= ~3                            # most with r31 & 3 = 0 (the state after RESET)
        add("clr_colour", c, "clr")
    for p in range(256):                             # every r59, r3 = 0
        c = random_case(rng, "clr", pads, target=TARGET["clr"])
        c.r[3] = 0; c.r[59] = p
        add("clr_r59sweep", c, "clr")
    for vs in ("z", "c1", "c2", "c3"):               # known pre-VRAM
        c = random_case(rng, "clr", pads, target=TARGET["clr"], vspec=vs)
        c.r[3] = 0; c.r[59] = 0x10
        add("clr_vram", c, "clr")
    fields = ["w", "is_", "dc0", "dc1", "p0", "p1", "p4", "p5", "vram"] + ["r%d" % i for i in range(64)]
    for b in range(n(2, 1)):                         # one-field-at-a-time sensitivity
        sb = random_case(rng, "clr", pads, target=TARGET["clr"])
        sb.r[3] = 0; sb.r[59] = 0x28
        sb.tag = "base%d" % b
        add("clr_sens", sb, "clr")
        for f in fields:
            c = sb.copy()
            if f.startswith("r"):
                i = int(f[1:])
                if i == 59:
                    continue
                c.r[i] = (c.r[i] + 1 + rng.randrange(255)) & 0xFF
            elif f == "vram":
                c.vspec = "r%x" % rng.randrange(1 << 32)
            elif f == "w":
                c.w = (c.w ^ (1 + rng.randrange(31))) & 31
            elif f == "is_":
                c.is_ = (c.is_ ^ (1 + rng.randrange(63))) & 63
            elif f in ("dc0", "dc1"):
                setattr(c, f, rng.randrange(65536))
            else:
                setattr(c, f, (getattr(c, f) + 1 + rng.randrange(255)) & 0xFF)
            c.tag = "%s:%s" % (sb.tag, f)
            add("clr_sens", c, "clr")

    # --- the contract set (core/tests/bios_contract.h) -----------------------
    def vseed():
        return "r%x" % rng.randrange(1, 1 << 32)

    for i in range(12):
        add("contract_reset", random_case(rng, "jmp", pads, vspec=vseed()), "reset")
    cl = []
    for p in (14, 16, 40, 48, 14 | 0x40, 48 | 0x80):
        cl.append(dict(r3=0, r59=p))
    for i in range(18):
        cl.append(dict(r3=0, r59=valid_r59()))
    for r3 in (0x40, 0x80, 0xC0):
        for k in range(2):
            p = rng.choice([x for x in range(bm.CLR_STACK_MIN, bm.CLR_STACK_MAX + 1) if not 28 <= x <= 31])
            cl.append(dict(r3=r3, r59=p | (rng.randrange(4) << 6), r31a=True))
    for vs in ("z", "c1", "c2", "c3"):
        cl.append(dict(r3=0, r59=16, vspec=vs))
    for d in cl:
        c = random_case(rng, "clr", pads, target=TARGET["clr"], vspec=d.get("vspec", vseed()))
        c.r[3] = d["r3"]; c.r[59] = d["r59"]
        if d.get("r31a"):
            c.r[31] &= ~3
        add("contract_clr", c, "clr")
    for rt in ("push", "pop"):
        for p in list(range(64)) + [64, 65, 127, 128, 191, 192, 255]:
            c = random_case(rng, rt, pads, target=TARGET[rt]); c.r[59] = p
            add("contract_" + rt, c, rt)
        for i in range(16):
            add("contract_" + rt, random_case(rng, rt, pads, target=TARGET[rt]), rt)

    # --- push K / pop K ----------------------------------------------------
    for rt in ("push", "pop"):
        for i in range(n(200, 40)):
            add("%s_rand" % rt, mk(rt, i), rt)
        for p in range(256):
            c = mk(rt, p)
            c.r[59] = p
            add("%s_r59sweep" % rt, c, rt)
    return groups


# ---------------------------------------------------------------------------
# state conversions
# ---------------------------------------------------------------------------
def pre_state(c, want_vram=False):
    s = dict(a=c.a, w=c.w, is_=c.is_, dc0=c.dc0, dc1=c.dc1, pc1=c.pc1, r=list(c.r),
             p0=c.p0, p1=c.p1, p4=c.p4, p5=c.p5, vram=None)
    if want_vram:
        s["vram"] = vram_expand(c.vspec)
    return s


def rle_expand(runs):
    out = []
    for v, n in runs:
        out.extend([v] * n)
    return out


def post_state(c, r, want_vram=False):
    last = r["io"]["last"]
    s = dict(a=r["a"], w=r["w"], is_=r["is"], dc0=r["dc0"], dc1=r["dc1"], pc1=r["pc1"],
             r=list(r["r"]),
             p0=last.get("0", c.p0), p1=last.get("1", c.p1), p4=last.get("4", c.p4),
             p5=last.get("5", c.p5), vram=None)
    if want_vram:
        s["vram"] = rle_expand(r["vram"])
    return s


def fnv(cells):
    h = 0x811C9DC5
    for x in cells:
        h = ((h ^ (x & 0xFF)) * 0x01000193) & 0xFFFFFFFF
    return h


# ---------------------------------------------------------------------------
# tiers: what the models promise (1 = FujiNet may rely on it, 2 = real-BIOS
# leftover: reproduce if cheap, 3 = corner case / no promise)
# ---------------------------------------------------------------------------
def compare(routine, pre, obs, model):
    """-> list of (field, tier, observed, expected) mismatches between the
    observed post-state and the model's (tiers: biosmodel.field_tiers)."""
    out = []
    tiers = bm.field_tiers(routine, pre)

    def chk(field, o, e):
        if o != e:
            out.append((field, tiers[field], o, e))

    for k in range(64):
        chk("r%d" % k, obs["r"][k], model["r"][k])
    chk("w.I", obs["w"] & 0x10, model["w"] & 0x10)
    chk("w.flags", obs["w"] & 0x0F, model["w"] & 0x0F)
    for f in ("a", "is_", "dc0", "dc1", "pc1", "p0", "p1", "p4"):
        chk(f, obs[f], model[f])
    chk("p5.sound", obs["p5"] & 0xC0, model["p5"] & 0xC0)
    chk("p5.row", obs["p5"] & 0x3F, model["p5"] & 0x3F)
    if pre.get("vram") is not None:
        chk("vram", fnv(obs["vram"]), fnv(model["vram"]))
    return out


def model_of(routine, pre, hidden, ver):
    if routine == "reset":
        return bm.model_reset(pre)
    if routine == "clr":
        return bm.model_clr(pre, hidden, ver)
    if routine == "push":
        return bm.model_push(pre)
    return bm.model_pop(pre)


# ---------------------------------------------------------------------------
# running
# ---------------------------------------------------------------------------
def run_boot_variants(bios, cart, pads, work, rompath, jobs, quick):
    """cold boot + cartridges whose first bytes differ; returns a dict."""
    os.makedirs(work, exist_ok=True)
    out = {}
    of = os.path.join(work, "cold.json")
    run_mame(bios, cart, {"OBS_MODE": "cold", "OBS_OUT": of, "OBS_FRAMES": "60"}, 20,
             rompath=rompath, timeout=300)
    out["cold"] = read_jsonl(of)[0]

    def one(arg):
        b0, b1 = arg
        cp = probe_variant(cart, b0, b1, os.path.join(work, "v_%02x_%02x.bin" % (b0, b1)))
        o = os.path.join(work, "v_%02x_%02x.out" % (b0, b1))
        run_mame(bios, cp, {"OBS_MODE": "bad", "OBS_OUT": o, "OBS_FRAMES": "12"}, 8,
                 rompath=rompath, timeout=300)
        return arg, bool(read_jsonl(o)[0]["entered"])
    b0s = [(b, 0) for b in (range(0, 256, 17) if quick else range(256))]
    b1s = [(0x55, b) for b in (0x00, 0x01, 0x2B, 0x55, 0xAA, 0xFF)]
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        res = dict(ex.map(one, b0s + b1s))
    out["byte0_enters"] = sorted(b0 for (b0, b1) in b0s if res[(b0, b1)])
    out["byte0_tested"] = len(b0s)
    out["byte1_all_enter"] = all(res[a] for a in b1s)
    return out


def run_group_set(bios, cart, pads, groups, work, rompath, jobs):
    allc = [c for g in groups.values() for c in g]
    maxf = 130
    res = run_cases(bios, cart, pads, allc, work, "suite-" + bios, rompath=rompath, jobs=jobs,
                    maxframes=maxf, batch=max(1, len(allc) // (jobs * 2)))
    return res


# ---------------------------------------------------------------------------
# analysis
# ---------------------------------------------------------------------------
def must_return(gname, c):
    """Cases a correct stand-in has to bring back (the others are r59 / r3
    values the real routine itself does not survive)."""
    if gname in ("clr_r3sweepA", "clr_r3sweepB", "clr_r3rand"):
        return (c.r[3] & 0x3F) == 0
    if gname == "clr_r59sweep":
        return bm.CLR_STACK_MIN <= (c.r[59] & 63) <= bm.CLR_STACK_MAX
    return True

def ranges(vals):
    out = []; s = p = None
    for x in sorted(vals):
        if s is None:
            s = p = x
        elif x == p + 1:
            p = x
        else:
            out.append((s, p)); s = p = x
    if s is not None:
        out.append((s, p))
    return ", ".join(("%d" % a) if a == b else "%d-%d" % (a, b) for a, b in out)


def build_hidden_mode(groups, res, gname, ver):
    """Hidden-column table from the r3 sweep: base (r3 & 0x2F) -> 64 strings of
    7 symbols (columns 0, 122..127) ('0' written 0, 'c' written the colour r3 >> 6, 'x' not written).
    Also checks the claims the table rests on."""
    anomalies = []
    V = bm.VERSIONS[ver]
    cases = {c.r[3]: c for c in groups[gname]}
    pre_v = vram_expand(groups[gname][0].vspec)
    view = {}
    for r3, c in cases.items():
        r = res[c.id]
        if not r["ok"]:
            anomalies.append("r3 sweep case %02x did not return" % r3)
            view[r3] = None
            continue
        post = rle_expand(r["vram"]); wc = rle_expand(r["io"]["wcells"])
        colour = r3 >> 6
        for row in range(64):
            for col in range(128):
                if col in V["hid"]:
                    continue
                i = row * 128 + col
                written = wc[i] != 4
                want = col in V["vis"]
                if written != want:
                    anomalies.append("write set differs at row %d col %d r3=%02x" % (row, col, r3))
                if written and post[i] != 0:
                    anomalies.append("non-zero write outside hidden columns r3=%02x" % r3)
                if not written and post[i] != pre_v[i]:
                    anomalies.append("unwritten cell changed r3=%02x" % r3)
        rows = []
        for row in range(64):
            line = ""
            for col in V["hid"]:
                i = row * 128 + col
                if wc[i] == 4:
                    line += "x"
                else:
                    line += "0" if post[i] == 0 else ("c" if post[i] == colour else "?")
            rows.append(line)
        view[r3] = rows
    ok_colour = all(view[(c << 6) | low] == view[0x40 | low]
                    for c in (2, 3) for low in range(64) if view[(c << 6) | low] and view[0x40 | low])
    ok_zero = all(view[low] == [row.replace("c", "0") for row in view[0x40 | low]]
                  for low in range(64) if view[low] and view[0x40 | low])
    ok_bit4 = all(view[r3] == view[r3 ^ 0x10] for r3 in range(256) if view[r3] and view[r3 ^ 0x10])
    hidden = {base: view[0x40 | base] for base in range(64) if not base & 0x10 and view[0x40 | base]}
    return hidden, dict(colour_independent=ok_colour, colour0_is_zero_pattern=ok_zero,
                        bit4_independent=ok_bit4, anomalies=anomalies[:10])


def build_hidden(groups, res, ver):
    """-> ({'A': table, 'B': table}, claims) from the two r3 sweeps."""
    ha, ca = build_hidden_mode(groups, res, "clr_r3sweepA", ver)
    hb, cb = build_hidden_mode(groups, res, "clr_r3sweepB", ver)
    claims = {k: ca[k] and cb[k] for k in ("colour_independent", "colour0_is_zero_pattern", "bit4_independent")}
    claims["anomalies"] = ca["anomalies"] + cb["anomalies"]
    return {"A": ha, "B": hb}, claims


def analyze(bios, groups, res, hidden, report, ver):
    """Check every observed case against the models; returns summary stats."""
    P = report.append
    stats = {}
    # per-group pass/fail
    P("=" * 78)
    P("BIOS %s  (%d cases)" % (bios, sum(len(g) for g in groups.values())))
    P("=" * 78)
    for gname, cases in groups.items():
        routine = ROUTINE_OF_GROUP[gname]
        ok_run = sum(1 for c in cases if res[c.id]["ok"])
        must = [c for c in cases if must_return(gname, c)]
        noret = [c for c in must if not res[c.id]["ok"]]
        tiers = collections.Counter()
        fails = collections.Counter()
        need_vram = routine in ("clr", "reset")
        n_checked = 0
        for c in cases:
            r = res[c.id]
            if not r["ok"]:
                continue
            if gname in ("clr_r59sweep",) and not (bm.CLR_STACK_MIN <= (c.r[59] & 63) <= bm.CLR_STACK_MAX):
                continue
            if gname == "clr_sens" and c.tag.endswith(":vram") is False and False:
                continue
            pre = pre_state(c, want_vram=need_vram)
            obs = post_state(c, r, want_vram=need_vram)
            mod = model_of(routine, pre, hidden, ver)
            for f, t, o, e in compare(routine, pre, obs, mod):
                tiers[t] += 1
                fails[(f, t)] += 1
            n_checked += 1
        ticks = sorted(res[c.id]["ticks"] for c in cases if res[c.id]["ok"])
        tk = "ticks min/med/max %d/%d/%d" % (ticks[0], ticks[len(ticks) // 2], ticks[-1]) if ticks else ""
        P("%-14s ran-to-return %d/%d, model-checked %d: tier1 mismatches %d, tier2 %d, tier3 %d; %s" % (
            gname, ok_run, len(cases), n_checked, tiers[1], tiers[2], tiers[3], tk))
        for (f, t), n in sorted(fails.items(), key=lambda x: (x[0][1], -x[1]))[:12]:
            P("      mismatch %-9s tier %d  x%d" % (f, t, n))
        if noret:
            P("      %d case(s) that must return did not (first: id %d)" % (len(noret), noret[0].id))
        stats[gname] = dict(ran=ok_run, n=len(cases), checked=n_checked, t1=tiers[1] + len(noret),
                            t2=tiers[2], t3=tiers[3])

    # r59 domains
    for gname in ("clr_r59sweep",):
        cs = groups[gname]
        okp = sorted({c.r[59] & 63 for c in cs if res[c.id]["ok"]})
        badp = sorted({c.r[59] & 63 for c in cs if not res[c.id]["ok"]})
        both = sorted(set(okp) & set(badp))
        P("clr r59 sweep: pointer values (r59 & 63) that always returned: %s" % ranges([p for p in okp if p not in badp]))
        P("                values that never returned:                  %s" % ranges([p for p in badp if p not in okp]))
        P("                values that depend on the other state:       %s" % ranges(both))
    # hidden-table claims
    hid, claims = build_hidden(groups, res, ver)
    P("clr hidden table: pattern independent of colour bits %s, colour 0 = same pattern written as 0 %s, "
      "r3 bit 4 ignored %s, anomalies %s" % (claims["colour_independent"], claims["colour0_is_zero_pattern"],
                                            claims["bit4_independent"], claims["anomalies"] or "none"))
    # which inputs influence which outputs (clr sensitivity)
    P("")
    P("clr: sensitivity (input -> outputs that changed beyond the same-named field)")
    sens = groups["clr_sens"]
    base = {c.tag: c for c in sens if ":" not in c.tag}
    dep = collections.defaultdict(set)
    for c in sens:
        if ":" not in c.tag:
            continue
        b, f = c.tag.split(":")
        rb, rc = res[base[b].id], res[c.id]
        if not (rb["ok"] and rc["ok"]):
            P("   (sens case %s did not return)" % c.tag)
            continue
        for k in range(64):
            if rb["r"][k] != rc["r"][k] and f != "r%d" % k:
                dep[f].add("r%d" % k)
        for k in ("a", "w", "is", "pc1", "dc0", "dc1"):
            if rb[k] != rc[k] and not (f.rstrip("_") == k):
                dep[f].add(k)
        if rb["vram"] != rc["vram"]:
            dep[f].add("vram")
        if rb["io"]["last"] != rc["io"]["last"] or rb["io"]["first"] != rc["io"]["first"]:
            dep[f].add("port-writes")
        if rb["io"]["wcells"] != rc["io"]["wcells"]:
            dep[f].add("written-cells")
    for f, outs in sorted(dep.items()):
        P("   %-5s -> %s" % (f, ", ".join(sorted(outs))))
    P("   (every other input: no effect beyond being preserved/clobbered in place; the cycle count varies by a few "
      "hundred ticks of 1.14M with the state)")
    noret = sorted({hex(c.r[3]) for c in groups["clr_r3rand"] if not res[c.id]["ok"]})
    P("clr: random-r3 cases that did not return: r3 = %s" % (noret or "none"))
    return stats, hid


def write_data(path, groups, res):
    with gzip.open(path, "wt") as f:
        for gname, cases in groups.items():
            for c in cases:
                r = res[c.id]
                io = r["io"]
                keep = {k: io[k] for k in ("n", "cnt", "last", "strobes", "first", "cartreads", "inr")}
                seen = collections.defaultdict(set)
                for s in io["seen"]:
                    seen[s >> 8].add(s & 255)
                keep["vals"] = {str(p): (sorted(v) if p != 4 else [min(v), max(v), len(v)])
                                for p, v in seen.items()}
                rec = dict(g=gname, id=c.id, pad=c.pad, tag=c.tag,
                           pre=dict(a=c.a, w=c.w, is_=c.is_, dc0=c.dc0, dc1=c.dc1, pc1=c.pc1,
                                    p=[c.p0, c.p1, c.p4, c.p5], vspec=c.vspec,
                                    r="".join("%02x" % x for x in c.r)),
                           post=dict(ok=r["ok"], frames=r["frames"], ticks=r["ticks"], a=r["a"], w=r["w"],
                                     is_=r["is"], dc0=r["dc0"], dc1=r["dc1"], pc1=r["pc1"],
                                     r="".join("%02x" % x for x in r["r"]),
                                     vram_fnv=fnv(rle_expand(r["vram"])),
                                     vram_rle=r["vram"] if len(r["vram"]) < 300 else None,
                                     wcells_rle=io["wcells"] if (len(io["wcells"]) < 300 or gname.startswith("clr_r3sweep")) else None,
                                     io=keep))
                f.write(json.dumps(rec, separators=(",", ":")) + "\n")


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bios", default=",".join(REAL_BIOS), help="comma list (default: both real versions)")
    ap.add_argument("--rompath", help="private rompath holding channelf/<bios>.rom + sl31254.rom (stand-in)")
    ap.add_argument("--seed", type=int, default=1976)
    ap.add_argument("--out", default=os.path.join(HERE, "obs-data"))
    ap.add_argument("--work", default=None, help="scratch dir (default: a temp dir)")
    ap.add_argument("--jobs", type=int, default=min(40, os.cpu_count() or 8))
    ap.add_argument("--quick", action="store_true", help="fewer cases (smoke test)")
    ap.add_argument("--check", action="store_true",
                    help="stand-in mode: do not write data files, exit 1 on any tier-1 mismatch; "
                         "needs the stored clr_hidden.json")
    ap.add_argument("--like", default=None, choices=sorted(bm.VERSIONS),
                    help="which real version's clear screen the BIOS under test should match "
                         "(default: the one named by --bios)")
    ap.add_argument("--header", help="write the C contract here (from the first --bios)")
    args = ap.parse_args()

    import atexit
    import shutil
    import tempfile
    work = os.path.abspath(args.work or tempfile.mkdtemp(prefix="chf-observe-"))
    if not args.work:
        atexit.register(shutil.rmtree, work, True)
    args.out = os.path.abspath(args.out)
    if args.rompath:
        args.rompath = os.path.abspath(args.rompath)
    os.makedirs(work, exist_ok=True)
    os.makedirs(args.out, exist_ok=True)
    cart, pads = build_probe(os.path.join(work, "probe"))
    groups = build_suite(pads, args.seed, args.quick)
    print("suite: %d cases in %d groups (seed %d)" % (sum(len(g) for g in groups.values()), len(groups), args.seed))

    hidden_file = os.path.join(args.out, "clr_hidden.json")
    stored_hidden = None
    if args.check and not os.path.exists(hidden_file):
        sys.exit("--check needs the real-BIOS table %s (run observe.py on the real BIOS first)" % hidden_file)
    if os.path.exists(hidden_file):
        stored_hidden = {ver: {m: {int(k): v for k, v in t.items()} for m, t in tabs.items()}
                         for ver, tabs in json.load(open(hidden_file))["table"].items()}

    results = {}
    allrep = {}
    fail = False
    for bios in args.bios.split(","):
        t0 = time.time()
        try:
            boot = run_boot_variants(bios, cart, pads, os.path.join(work, bios), args.rompath, args.jobs, args.quick)
            if not boot["cold"].get("entered"):
                raise RuntimeError("cold boot never reaches the cartridge at $0802 (cartridge with [$0800] == $55)")
            res = run_group_set(bios, cart, pads, groups, os.path.join(work, bios), args.rompath, args.jobs)
        except RuntimeError as e:
            print("BIOS %s: %s" % (bios, e))
            if args.check:
                print("CHECK: FAIL")
                sys.exit(1)
            raise
        report = []
        ver = args.like or (bios if bios in bm.VERSIONS else bm.DEFAULT_VERSION)
        hid, claims = build_hidden(groups, res, ver)
        use_hidden = stored_hidden[ver] if args.check and stored_hidden else hid
        stats, _ = analyze(bios, groups, res, use_hidden, report, ver)
        cold = boot["cold"]
        report.append("")
        if cold.get("entered"):
            report.append("cold boot: entered=%s after %s ticks; A=%02x W=%02x IS=%02x DC0=%04x DC1=%04x PC1=%04x; regs set: %s; "
                          "port writes %d; cart reads %s; IN reads %s" % (
                              cold.get("entered"), cold.get("entry_ticks"), cold["a"], cold["w"], cold["is"],
                              cold["dc0"], cold["dc1"], cold["pc1"],
                              {i: x for i, x in enumerate(cold["r"]) if x}, cold["io"]["n"],
                              [hex(a) for a in cold["io"]["cartreads"]], cold["io"]["inr"]))
        else:
            report.append("cold boot: the console never entered the probe cartridge")
        report.append("cart signature: byte at $0800 values that enter the cartridge: %s (of %d tested); "
                      "byte at $0801 irrelevant: %s" % (
                          [hex(b) for b in boot["byte0_enters"]], boot["byte0_tested"], boot["byte1_all_enter"]))
        report.append("elapsed %.0f s" % (time.time() - t0))
        txt = "\n".join(report)
        print(txt)
        results[bios] = (res, hid, stats, boot)
        allrep[bios] = txt
        if args.check and any(s["t1"] for s in stats.values()):
            fail = True
        if args.check and (boot["byte0_enters"] != [0x55] and not args.quick):
            print("CHECK: cartridge signature test failed")
            fail = True
        if not args.check:
            write_data(os.path.join(args.out, "%s.jsonl.gz" % bios), groups, res)
            open(os.path.join(args.out, "report-%s.txt" % bios), "w").write(txt + "\n")

    # compare versions
    bl = list(results)
    if len(bl) > 1:
        a, b = bl[0], bl[1]
        diffs = collections.Counter()
        for gname, cases in groups.items():
            for c in cases:
                ra, rb = results[a][0][c.id], results[b][0][c.id]
                for k in ("ok", "a", "w", "is", "pc1", "dc0", "dc1", "r", "vram", "ticks"):
                    if ra[k] != rb[k]:
                        diffs[(gname, k)] += 1
                if ra["io"]["last"] != rb["io"]["last"] or ra["io"]["wcells"] != rb["io"]["wcells"]:
                    diffs[(gname, "io")] += 1
        print("\nBIOS %s vs %s: %s" % (a, b, "IDENTICAL on every case and field" if not diffs else dict(diffs)))
    if not args.check and bl:
        tables = {}
        for bios in bl:
            if bios in bm.VERSIONS:
                tables[bios] = {m: {str(k): v for k, v in t.items()} for m, t in results[bios][1].items()}
        json.dump(dict(note="clr hidden columns per BIOS version, mode (A: r31&3==0, B: r31&3!=0) and r3 & 0x2F: "
                            "64 rows x 7 symbols (columns bm.VERSIONS[ver]['hid']); 0 = written 0, "
                            "c = written colour (r3 >> 6), x = not written",
                       table=tables), open(hidden_file, "w"), separators=(",", ":"))
    if args.header and bl:
        import genheader
        ref = "sl31253" if "sl31253" in results else bl[0]
        alt = "sl90025" if "sl90025" in results and ref != "sl90025" else None
        genheader.write(args.header, groups, results, args.seed, ref, alt)
        print("wrote", args.header)
    if args.check:
        print("CHECK:", "FAIL (tier-1 mismatches above)" if fail else "PASS")
        sys.exit(1 if fail else 0)


if __name__ == "__main__":
    main()
