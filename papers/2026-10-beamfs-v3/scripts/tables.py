#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""beamfs v3 paper: the numbers and tables the text quotes, generated
from data/derived/*.csv (scripts/extract.py) so that no figure in the
paper is typed by hand.

Standard library only. Writes generated/numbers.tex (one \\newcommand
per quoted number) and generated/tab-*.tex. Exit status 1 when an
expected row is missing or an internal consistency check fails.
"""

import csv
import hashlib
import os
import re
import subprocess
import sys

W = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DER = os.path.join(W, "data", "derived")
RAW = os.path.join(W, "data", "raw")
GEN = os.path.join(W, "generated")
PROBS = (1000, 100000, 1000000)
RUNWORD = {"run2": "Two", "run3": "Three", "run4": "Four"}
FAIL = []


def check(cond, what):
    print(("  ok     " if cond else "  ECHEC  ") + what)
    if not cond:
        FAIL.append(what)


def rows(name):
    with open(os.path.join(DER, name), newline="") as f:
        return list(csv.DictReader(f))


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def thousands(n):
    """1000000 -> 1\\,000\\,000 (thin space, as SI)."""
    s = str(n)
    out = []
    while len(s) > 3:
        out.insert(0, s[-3:])
        s = s[:-3]
    out.insert(0, s)
    return "\\,".join(out)


MACROS = []


def macro(name, value):
    if not name.isalpha():
        raise SystemExit("nom de macro invalide : %s" % name)
    MACROS.append("\\newcommand{\\%s}{%s}" % (name, value))


OUTCODE = {"intact": "I", "no_flip": "N", "silent_corruption": "S",
           "read_refused": "R", "not_exercised": "n/e"}


def cell(r):
    o = r["outcome"]
    if o == "not_exercised":
        return "n/e"
    return "%s\\,%s" % (r["flip_delta"] or "0", OUTCODE[o])


def main():
    os.makedirs(GEN, exist_ok=True)

    # ---------------------------------------------------------- facts
    facts = {(r["run"], r["key"]): r for r in rows("facts.csv")}
    for run in ("run2", "run3", "run4"):
        w = RUNWORD[run]
        for key, mname in (("commit_bench", "commitBench"),
                           ("manifest_sha256", "manifestSha")):
            v = facts[(run, key)]["value"]
            macro(mname + w, v[:7] if key == "commit_bench" else v)
        macro("manifestName" + w, facts[(run, "manifest_sha256")]["source"]
              .replace("_", "\\_"))
    cb = {facts[(r, "commit_beamfs")]["value"] for r in ("run2", "run3", "run4")}
    cy = {facts[(r, "commit_yocto")]["value"] for r in ("run2", "run3", "run4")}
    ci = {facts[(r, "canonical_beamfs_sha256")]["value"] for r in ("run2", "run3", "run4")}
    check(len(cb) == 1 and len(cy) == 1 and len(ci) == 1,
          "runs 2, 3, 4 : meme commit beamfs, meme commit yocto, meme image")
    macro("commitBeamfs", sorted(cb)[0][:7])
    macro("commitBeamfsFull", sorted(cb)[0])
    macro("commitYocto", sorted(cy)[0][:7])
    macro("imageSha", sorted(ci)[0])
    macro("rcRunFour", facts[("run4", "overall_rc")]["value"])
    # Size of the measured module: every .c and .h at the root of the
    # beamfs tree at the measured commit, read from git, not from the
    # working copy.
    repo = os.path.expanduser(os.environ.get("BEAMFS_REPO", "~/git/beamfs"))
    commit = sorted(cb)[0]
    names = subprocess.run(["git", "--no-pager", "-C", repo, "ls-tree",
                            "--name-only", commit],
                           check=True, capture_output=True, text=True).stdout.split()
    src = [n for n in names if n.endswith((".c", ".h"))]
    loc = 0
    for n in src:
        body = subprocess.run(["git", "--no-pager", "-C", repo, "show",
                               "%s:%s" % (commit, n)],
                              check=True, capture_output=True, text=True).stdout
        loc += body.count("\n")
    check(len(src) > 10 and loc > 0, "module : %d fichiers .c/.h, %d lignes a %s" % (len(src), loc, commit[:7]))
    macro("moduleLines", thousands(loc))
    macro("moduleFiles", str(len(src)))
    for key in ("x86_64_full", "aarch64_full", "x86_64_476", "aarch64_476"):
        r = facts[("bx", key + "_sha256")]
        name = {"x86_64_full": "sweepXfull", "aarch64_full": "sweepAfull",
                "x86_64_476": "sweepXsolo", "aarch64_476": "sweepAsolo"}[key]
        macro(name + "Sha", r["value"])
        macro(name + "Name", r["source"])
    macro("xfsXsoloSeconds", thousands(int(facts[("bx", "x86_64_476_seconds")]["value"])))
    macro("xfsAsoloSeconds", thousands(int(facts[("bx", "aarch64_476_seconds")]["value"])))
    macro("snapshotSha", sha256(os.path.join(RAW, "SHA256SUMS")))
    with open(os.path.join(RAW, "SHA256SUMS")) as f:
        macro("snapshotFiles", str(sum(1 for ln in f if ln.strip())))
    for script in ("extract.py", "figures.jl", "tables.py"):
        p = os.path.join(W, "scripts", script)
        macro({"extract.py": "extractSha", "figures.jl": "figuresSha",
               "tables.py": "tablesSha"}[script], sha256(p))

    # ------------------------------------------------------- xfstests
    # From the output of check (extract.py), not from the harness's
    # summary: a test xfstests declined is NOTRUN, not PASS.
    xf = rows("xfstests.csv")
    check(len(xf) == 734, "xfstests : 734 tests du groupe auto (%d)" % len(xf))
    macro("xfsTotal", str(len(xf)))
    macro("xfsGenericAuto", str(sum(1 for r in xf if r["test"].startswith("generic/"))))
    macro("xfsSharedAuto", str(sum(1 for r in xf if r["test"].startswith("shared/"))))
    for arch, k in (("x86_64", "X"), ("aarch64", "A")):
        v = [r[arch + "_verdict"] for r in xf]
        npass, nfail, nnr = v.count("PASS"), v.count("FAIL"), v.count("NOTRUN")
        nother = len(v) - npass - nfail - nnr
        cats = [r[arch + "_category"] for r in xf if r[arch + "_verdict"] == "NOTRUN"]
        check(cats.count("feature") + cats.count("environment") + cats.count("scope") == nnr,
              "xfstests %s : %d non executes, tous classes" % (arch, nnr))
        # The text knows three outcomes: passed, failed, declined.
        check(nother == 0, "xfstests %s : aucun test sans verdict (%d)" % (arch, nother))
        macro("xfs%sRan" % k, str(npass + nfail))
        macro("xfs%sPass" % k, str(npass))
        macro("xfs%sFail" % k, str(nfail))
        macro("xfs%sNotrun" % k, str(nnr))
        macro("xfs%sNoverdict" % k, str(nother))
        macro("xfs%sNrFeature" % k, str(cats.count("feature")))
        macro("xfs%sNrEnv" % k, str(cats.count("environment")))
        macro("xfs%sNrScope" % k, str(cats.count("scope")))
        # Abstract and introduction: "most of them for features beamfs does
        # not implement".
        check(2 * cats.count("feature") > nnr,
              "xfstests %s : la majorite des non executes pour une fonction absente (%d sur %d)"
              % (arch, cats.count("feature"), nnr))
        fl = sorted(r["test"] for r in xf if r[arch + "_verdict"] == "FAIL")
        macro("xfs%sFailList" % k, ", ".join(fl) if fl else "none")
        print("  info   xfstests %s : %d executes (%d reussis, %d en echec %s), %d non executes, %d sans verdict"
              % (arch, npass + nfail, npass, nfail, fl, nnr, nother))
    check([r["test"] for r in xf if r["aarch64_verdict"] == "FAIL"] == ["generic/476"],
          "xfstests aarch64 : seul generic/476 en echec dans le sweep")
    # The text says every test that ran on x86-64 passed.
    check([r["test"] for r in xf if r["x86_64_verdict"] == "FAIL"] == [],
          "xfstests x86-64 : aucun echec dans le sweep")
    macro("xfsXpass", str(sum(1 for r in xf if r["x86_64_verdict"] == "PASS")))
    macro("xfsApass", str(sum(1 for r in xf if r["aarch64_verdict"] == "PASS")))
    a476 = [r for r in xf if r["test"] == "generic/476"][0]
    # The text names the aarch64 budget, 14 400 s.
    check(14000 < int(a476["aarch64_seconds"] or 0) <= 14400,
          "xfstests aarch64 : generic/476 arrete au budget de 14400 s (%s s)" % a476["aarch64_seconds"])
    macro("xfsAfullSeconds", thousands(int(a476["aarch64_seconds"])))
    macro("xfsAfullVerdict", a476["aarch64_verdict"])
    differ = [r["test"] for r in xf
              if (r["x86_64_verdict"] == "NOTRUN") == (not r["aarch64_verdict"] == "NOTRUN")]
    macro("xfsNotrunDiffer", str(len(differ)))
    macro("xfsNotrunDifferList", ", ".join(differ))
    # The text: generic/317 alone, passed on aarch64, declined on x86-64
    # for want of procfs uid_map.
    d317 = [r for r in xf if r["test"] == "generic/317"]
    check(differ == ["generic/317"] and len(d317) == 1 and d317[0]["aarch64_verdict"] == "PASS"
          and d317[0]["x86_64_verdict"] == "NOTRUN" and d317[0]["x86_64_group"] == "user namespaces",
          "xfstests : seul generic/317 differe, PASS sur aarch64, uid_map absent sur x86-64 (%s)" % differ)
    print("  info   xfstests : non executes d'un cote seulement : %s" % differ)
    groups = {}
    for r in xf:
        for arch in ("x86_64", "aarch64"):
            if r[arch + "_verdict"] == "NOTRUN":
                key = (r[arch + "_group"], r[arch + "_category"])
                groups.setdefault(key, {"x86_64": 0, "aarch64": 0})[arch] += 1
    # Groups of fewer than four tests on both sides are summed per kind,
    # so that the table fits a column; every group is in xfstests.csv.
    big, small = {}, {}
    for (g, c), n in groups.items():
        if max(n["x86_64"], n["aarch64"]) >= 4:
            big[(g, c)] = n
        else:
            t = small.setdefault(c, {"x86_64": 0, "aarch64": 0, "kinds": 0})
            t["x86_64"] += n["x86_64"]
            t["aarch64"] += n["aarch64"]
            t["kinds"] += 1
            t.setdefault("names", []).append(g)
    kind_order = {"feature": 0, "environment": 1, "scope": 2}
    order = sorted(big.items(), key=lambda kv: (kind_order[kv[0][1]],
                                                -(kv[1]["x86_64"] + kv[1]["aarch64"]), kv[0][0]))
    def vcount(arch, v):
        return sum(1 for r in xf if r[arch + "_verdict"] == v)
    lines = ["\\begin{tabular}{@{}llrr@{}}", "\\toprule",
             " & & x86-64 & aarch64 \\\\", "\\midrule",
             "passed & & %d & %d \\\\" % (vcount("x86_64", "PASS"), vcount("aarch64", "PASS")),
             "failed & & %d & %d \\\\" % (vcount("x86_64", "FAIL"), vcount("aarch64", "FAIL")),
             "declined by check & & %d & %d \\\\" % (vcount("x86_64", "NOTRUN"), vcount("aarch64", "NOTRUN")),
             "\\midrule", "\\multicolumn{4}{@{}l}{declined for} \\\\"]
    last = None
    for c in ("feature", "environment", "scope"):
        rows_c = [(g, n) for (g, cc), n in order if cc == c]
        if not rows_c and c not in small:
            continue
        if last is not None:
            lines.append("\\addlinespace[2pt]")
        last = c
        for g, n in rows_c:
            lines.append("\\quad %s & %s & %d & %d \\\\" % (g.replace("_", "\\_"), c, n["x86_64"], n["aarch64"]))
        if c in small:
            t = small[c]
            label = (t["names"][0] if t["kinds"] == 1 else
                     "%d other %s" % (t["kinds"], "features" if c == "feature" else "needs"))
            lines.append("\\quad %s & %s & %d & %d \\\\" % (
                label.replace("_", "\\_"), c, t["x86_64"], t["aarch64"]))
    lines += ["\\bottomrule", "\\end{tabular}"]
    with open(os.path.join(GEN, "tab-xfs.tex"), "w") as f:
        f.write("\n".join(lines) + "\n")
    macro("xfsGroups", str(len(groups)))
    fx = {}
    with open(os.path.join(DER, "facts.csv"), newline="") as f:
        for r in csv.DictReader(f):
            if r["run"] == "xfs_x86":
                fx[r["key"]] = r["value"]
    macro("xfsXtag", fx.get("tag", ""))
    macro("xfsXkernelSha", fx.get("start_kernel_image", " ").split()[-1])
    macro("xfsXtaintStart", fx.get("start_tainted", ""))
    macro("xfsXtaintEnd", fx.get("end_tainted", ""))
    # The text says no warning or oops occurred in the x86-64 sweep: the
    # node was not restarted (no recovery recorded, uptime grown by the
    # length of the sweep), and its taint is unchanged with the warning
    # (9) and oops (7) bits clear.
    ts, te = fx.get("start_tainted", ""), fx.get("end_tainted", "")
    check(ts.isdigit() and ts == te and int(ts) & (512 | 128) == 0,
          "xfstests x86-64 : taint %s au debut, %s a la fin, bits 7 et 9 libres" % (ts, te))
    check(fx.get("end_recoveries", "").split()[:1] == ["0"],
          "xfstests x86-64 : aucune recuperation du noeud (%s)" % fx.get("end_recoveries"))
    try:
        grown = float(fx.get("end_uptime", "0")) - float(fx.get("start_uptime", "0"))
        wall = int(fx.get("end_at", "0")) - int(fx.get("start_at", "0"))
    except ValueError:
        grown, wall = -1.0, 0
    check(wall > 0 and abs(grown - wall) < 120,
          "xfstests x86-64 : pas de redemarrage, uptime +%.0f s pour %d s de sweep" % (grown, wall))
    # generic/476 passed in the x86-64 sweep, under a budget of 14 400 s.
    x476 = [r for r in xf if r["test"] == "generic/476"][0]
    check(x476["x86_64_verdict"] == "PASS" and fx.get("start_budget") == "14400"
          and fx.get("end_budget") == "14400",
          "xfstests x86-64 : generic/476 PASS sous un budget de 14400 s (%s, %s)"
          % (x476["x86_64_verdict"], fx.get("start_budget")))
    macro("xfsXfullSeconds", thousands(int(x476["x86_64_seconds"] or 0)))
    macro("xfsXkernLogs", fx.get("kernel_logs", ""))
    macro("xfsXtreeLogs", fx.get("kernel_tree_clean", ""))
    print("  info   xfstests x86-64 : detecteur de taches bloquees : %s" % fx.get("kernel_hung"))
    # The superseded 2.5.0 sweep, stopped after generic/476 hit the
    # default budget of 1 900 s.
    sup = {}
    with open(os.path.join(DER, "facts.csv"), newline="") as f:
        for r in csv.DictReader(f):
            if r["run"] == "xfs_superseded":
                sup[r["key"]] = r["value"]
    check(0 < int(sup.get("sweep_476_seconds", "0") or 0) <= 1900,
          "xfstests x86-64 remplace : generic/476 arrete dans 1900 s (%s s)" % sup.get("sweep_476_seconds"))
    macro("xfsSupFullSeconds", thousands(int(sup.get("sweep_476_seconds", "0") or 0)))
    macro("xfsSupDone", sup.get("done", ""))
    macro("xfsSupTag", sup.get("tag", ""))
    macro("xfsSupVerdictsSha", sup.get("verdicts_sha256", ""))
    macro("xfsXmetaSha", fx.get("meta_sha256", ""))
    macro("xfsXverdictsSha", fx.get("verdicts_sha256", ""))
    xa = {}
    with open(os.path.join(DER, "facts.csv"), newline="") as f:
        for r in csv.DictReader(f):
            if r["run"] == "xfs_aarch64":
                xa[r["key"]] = r["value"]
    check(xa.get("check_outputs") == "733", "xfstests aarch64 : 733 sorties de check du sweep")
    macro("xfsAcheckOutputs", xa.get("check_outputs", ""))
    for tool, name in (("mkfs.beamfs", "Mkfs"), ("fsck.beamfs", "Fsck")):
        # tool=<name> <path> <sha256> <first line of "<name> -V">
        t = fx.get("tool_" + tool, "").split()
        ver = re.search(r"\b(\d+\.\d+\.\d+)\b", " ".join(t[3:]))
        check(len(t) > 3 and len(t[2]) == 64 and ver is not None,
              "xfstests x86-64 : %s identifie (%s)" % (tool, " ".join(t)))
        macro("xfsX%sSha" % name, t[2] if len(t) > 2 else "")
        macro("xfsX%sVersion" % name, ver.group(1) if ver else "")
    macro("featXfstests", "0x7a000")
    idt = {}
    with open(os.path.join(DER, "facts.csv"), newline="") as f:
        for r in csv.DictReader(f):
            if r["run"] == "identity":
                idt[r["key"]] = r["value"]
    check(all(k in idt for k in ("image_x86", "image_aarch64", "kernel_x86", "kernel_aarch64")),
          "identite : images et noyaux presents dans facts.csv")
    macro("imageXSha", idt.get("image_x86", ""))
    macro("kernelXSha", idt.get("kernel_x86", ""))
    macro("kernelASha", idt.get("kernel_aarch64", ""))
    macro("srcTrees", idt.get("trees", ""))
    check(idt.get("kernel_x86", "x") == fx.get("start_kernel_image", " ").split()[-1],
          "identite : noyau x86-64 du poste = noyau du releve BX")

    # -------------------------------------------------------- multifs
    mf = rows("multifs.csv")
    idx = {(r["run"], r["fs"], int(r["prob_ppm"])): r for r in mf}
    fss = ("beamfs", "ext4", "ext3", "btrfs", "squashfs")
    for run in ("run2", "run3", "run4"):
        for fs in fss:
            for p in PROBS:
                check((run, fs, p) in idx, "multifs %s %s %d present" % (run, fs, p))
    bmax = max(int(idx[(r, "beamfs", p)]["flip_delta"]) for r in RUNWORD for p in PROBS)
    macro("mfBeamfsMaxFlips", str(bmax))
    check(all(idx[(r, "beamfs", p)]["outcome"] in ("intact", "no_flip")
              for r in RUNWORD for p in PROBS),
          "multifs : beamfs intact ou sans flip dans les 9 attaques")
    check(all(idx[(r, "squashfs", p)]["outcome"] == "not_exercised"
              for r in RUNWORD for p in PROBS),
          "multifs : squashfs jamais exerce")
    for fs, word in (("ext4", "ExtFour"), ("ext3", "ExtThree"), ("btrfs", "Btrfs")):
        hi = [idx[(r, fs, 1000000)] for r in RUNWORD]
        flips = sorted(int(h["flip_delta"]) for h in hi)
        outs = {h["outcome"] for h in hi}
        check(len(outs) == 1, "multifs : %s a 1 000 000 ppm, meme issue aux trois runs (%s)" % (fs, ",".join(sorted(outs))))
        macro("mf%sFlipsMin" % word, str(flips[0]))
        macro("mf%sFlipsMax" % word, str(flips[-1]))
        macro("mf%sOutcome" % word, {"silent_corruption": "silent corruption",
                                     "read_refused": "read refused"}.get(sorted(outs)[0], sorted(outs)[0]))
        lower = [idx[(r, fs, p)]["outcome"] for r in RUNWORD for p in (1000, 100000)]
        check(all(o == "no_flip" for o in lower),
              "multifs : %s sans flip a 1 000 et 100 000 ppm" % fs)
    for run in ("run2", "run3", "run4"):
        macro("mfBeamfsHigh" + RUNWORD[run], idx[(run, "beamfs", 1000000)]["flip_delta"])
        macro("mfBeamfsMid" + RUNWORD[run], idx[(run, "beamfs", 100000)]["flip_delta"])

    lines = ["\\begin{tabular}{@{}llccc@{}}", "\\toprule",
             "FS & ppm & run 2 & run 3 & run 4 \\\\", "\\midrule"]
    for fs in fss:
        for i, p in enumerate(PROBS):
            lines.append("%s & %s & %s & %s & %s \\\\" % (
                fs if i == 0 else "", thousands(p),
                cell(idx[("run2", fs, p)]), cell(idx[("run3", fs, p)]),
                cell(idx[("run4", fs, p)])))
        if fs != fss[-1]:
            lines.append("\\addlinespace[2pt]")
    lines += ["\\bottomrule", "\\end{tabular}"]
    with open(os.path.join(GEN, "tab-multifs.tex"), "w") as f:
        f.write("\n".join(lines) + "\n")

    # -------------------------------------------------------- cluster
    cl = rows("cluster.csv")
    cidx = {(r["run"], r["node"], int(r["prob_ppm"])): r for r in cl}
    nodes = ("beamfs-master", "beamfs-compute01", "beamfs-compute02", "beamfs-compute03")
    check(all(cidx[("run2", n, p)]["outcome"] == "not_exercised" for n in nodes for p in PROBS),
          "cluster : run 2 jamais exerce")
    r4 = [cidx[("run4", n, p)] for n in nodes for p in PROBS]
    check(all(r["outcome"] in ("intact", "no_flip") for r in r4),
          "cluster : run 4, 12 cases intactes ou sans flip")
    macro("clRunFourIntact", str(sum(1 for r in r4 if r["outcome"] == "intact")))
    macro("clRunFourNoFlip", str(sum(1 for r in r4 if r["outcome"] == "no_flip")))
    refused = [(n, p) for n in nodes for p in PROBS if cidx[("run3", n, p)]["outcome"] == "read_refused"]
    check(refused == [("beamfs-master", 1000000)], "cluster : run 3, seul master a 1 000 000 ppm refuse")
    macro("clRunThreeRefusedFlips", cidx[("run3", "beamfs-master", 1000000)]["flip_delta"])
    lines = ["\\begin{tabular}{@{}llcc@{}}", "\\toprule",
             "node & ppm & run 3 & run 4 \\\\", "\\midrule"]
    for n in nodes:
        for i, p in enumerate(PROBS):
            lines.append("%s & %s & %s & %s \\\\" % (
                n.replace("beamfs-", "") if i == 0 else "", thousands(p),
                cell(cidx[("run3", n, p)]), cell(cidx[("run4", n, p)])))
        if n != nodes[-1]:
            lines.append("\\addlinespace[2pt]")
    lines += ["\\bottomrule", "\\end{tabular}"]
    with open(os.path.join(GEN, "tab-cluster.tex"), "w") as f:
        f.write("\n".join(lines) + "\n")

    # -------------------------------------------------------- RS
    ms = rows("rs_margin_summary.csv")
    check(len(ms) == 6, "rs_margin_summary : 6 attaques beamfs avec flips")
    kl = sum(int(r["kernel_lines"]) for r in ms)
    ku = sum(int(r["kernel_unmatched"]) for r in ms)
    check(ku == 0, "lignes noyau toutes expliquees (%d, %d non expliquees)" % (kl, ku))
    macro("kernLines", str(kl))
    macro("kernMaxPerDecode", str(max(int(r["kernel_max_symbols_per_decode"]) for r in ms)))
    macro("unionMax", str(max(int(r["max_symbols_per_codeword_union_resolved"]) for r in ms)))
    hi = [r for r in ms if r["prob_ppm"] == "1000000"]
    dtp = max(int(r["dt_pair_max_ns"]) for r in hi)
    dto = min(int(r["dt_other_min_ns"]) for r in hi)
    check(dtp < dto, "paires de crochets separees (%d < %d ns)" % (dtp, dto))
    macro("dtPairMaxMicro", str(round(dtp / 1000)))
    macro("dtOtherMinMilli", "%.2f" % (dto / 1e6))
    macro("ambiguousHigh", str(sum(int(r["ambiguous"]) for r in hi)))
    check(all(r["feat_ro_compat"] == "0x0000000000000000" for r in ms),
          "volumes mesures : ro_compat nul (DATA_CSUM inactif)")
    check(all(r["feat_incompat"] == "0x000000000007a100" for r in ms),
          "volumes mesures : incompat 0x7a100")
    macro("featIncompat", "0x7a100")
    lines = ["\\begin{tabular}{@{}lrrlrrrrr@{}}", "\\toprule",
             "run & ppm & flips & target & pairs & single & k-lines & dec & union \\\\",
             "\\midrule"]
    for r in ms:
        single = int(r["single_rq"]) + int(r["single_endio"]) + int(r["single_ambiguous"])
        lines.append("%s & %s & %s & %s & %s & %s & %s & %s & %s \\\\" % (
            r["run"].replace("run", ""), thousands(int(r["prob_ppm"])),
            r["flip_delta"], r["target_mode"], r["hook_pairs"], single,
            r["kernel_lines"], r["kernel_max_symbols_per_decode"],
            r["max_symbols_per_codeword_union_resolved"]))
    lines += ["\\bottomrule", "\\end{tabular}"]
    with open(os.path.join(GEN, "tab-rs.tex"), "w") as f:
        f.write("\n".join(lines) + "\n")

    # ------------------------------------------- indirect block hits
    fl = rows("flips_beamfs.csv")
    ind = [r for r in fl if r["kind"] == "indirect"]
    check(all(r["indirect_slot_used"] == "0" for r in ind),
          "multifs : aucun pointeur en usage touche dans le bloc indirect")
    macro("indHits", str(len(ind)))
    macro("indSlots", ", ".join("%s (run %s)" % (r["indirect_slot"], r["run"][-1]) for r in ind))
    macro("flipsAnalysed", str(len(fl)))
    check(all(r["bio_op"] == "0" for r in fl), "flips beamfs : tous sur des bios de lecture (bio_op 0)")
    check(all(r["remount_verdict"] == "MOUNTED" and r["remount_files_changed"] == "0" for r in mf),
          "multifs : apres chaque attaque, 12 fichiers intacts au remontage, tous FS")

    with open(os.path.join(GEN, "numbers.tex"), "w") as f:
        f.write("% Generated by scripts/tables.py from data/derived/*.csv. Do not edit.\n")
        f.write("\n".join(MACROS) + "\n")
    print("  ecrit  generated/numbers.tex (%d macros), tab-multifs, tab-cluster, tab-rs, tab-xfs" % len(MACROS))
    print("=== bilan : %d controle(s) en echec" % len(FAIL))
    for x in FAIL:
        print("  ECHEC  " + x)
    return 1 if FAIL else 0


if __name__ == "__main__":
    sys.exit(main())
