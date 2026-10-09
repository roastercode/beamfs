#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
#
# beamfs v3 paper: build and control. Run from the workspace root.
#
#   scripts/check-paper.sh            build and report
#   scripts/check-paper.sh --release  same, and refuse while a \verify{}
#                                     mark or an overfull box remains
#
# Exit status 0 only when every control passes. Network is used for the
# DOI and Crossref controls only.

RELEASE=0
[ "${1:-}" = "--release" ] && RELEASE=1
W=$(pwd)
FAILS=0
fail() { echo "  ECHEC  $*"; FAILS=$((FAILS + 1)); }
ok() { echo "  ok     $*"; }
info() { echo "  info   $*"; }

echo "=== 1. nombres et tables (scripts/tables.py)"
if python3 -I scripts/tables.py; then ok "tables.py rc=0"; else fail "tables.py"; fi

echo "=== 2. compilation (latexmk, lualatex, biber)"
rm -f paper.pdf
latexmk -lualatex -interaction=nonstopmode -halt-on-error paper.tex >/tmp/pub-v3-latexmk.log 2>&1
RC=$?
if [ "$RC" = 0 ] && [ -f paper.pdf ]; then ok "latexmk rc=0"; else fail "latexmk rc=$RC"; cat /tmp/pub-v3-latexmk.log; fi

echo "=== 3. journal de compilation"
if [ -f paper.log ]; then
  for pat in 'Reference .* undefined' 'Citation .* undefined' 'There were undefined' \
             'Label(s) may have changed' 'Missing character'; do
    n=$(grep -c -E "$pat" paper.log)
    if [ "$n" = 0 ]; then ok "paper.log : 0 x '$pat'"; else fail "paper.log : $n x '$pat'"; grep -E "$pat" paper.log; fi
  done
  # IEEEtran asks for Times (ptm) before fontspec is loaded; the text is
  # set in Latin Modern, so those four warnings are expected.
  # The closing "Some font shapes were not available" line repeats the
  # substitutions above; every specific one is counted.
  nf=$(grep 'Font Warning' paper.log | grep -v 'TU/ptm/' | grep -v -c 'Some font shapes were not available')
  if [ "$nf" = 0 ]; then ok "paper.log : 0 avertissement de police (hors ptm de IEEEtran)"
  else fail "paper.log : $nf avertissement(s) de police"; grep -A1 'Font Warning' paper.log | grep -v 'TU/ptm/'; fi
  nb=$(grep -c 'Overfull \\hbox' paper.log)
  if [ "$nb" = 0 ]; then ok "0 overfull hbox"
  elif [ "$RELEASE" = 1 ]; then fail "$nb overfull hbox"; grep -A1 'Overfull \\hbox' paper.log
  else info "$nb overfull hbox"; grep -A1 'Overfull \\hbox' paper.log; fi
fi
if [ -f paper.blg ]; then
  n=$(grep -c -E 'WARN|ERROR' paper.blg)
  if [ "$n" = 0 ]; then ok "biber : 0 avertissement"; else fail "biber : $n avertissement(s)"; grep -E 'WARN|ERROR' paper.blg; fi
fi

echo "=== 4. typographie et vocabulaire des sources"
python3 -I - "$RELEASE" <<'PYEOF'
import glob, re, sys
release = sys.argv[1] == "1"
files = ["paper.tex"] + sorted(glob.glob("sections/*.tex"))
bad = 0
uni = {chr(0x2014): "tiret cadratin", chr(0x2013): "tiret demi-cadratin",
       chr(0x2018): "apostrophe courbe", chr(0x2019): "apostrophe courbe",
       chr(0x201C): "guillemet courbe", chr(0x201D): "guillemet courbe",
       chr(0x2026): "points de suspension"}
terms = [(r"(?i)radiation[- ]tolerant", "radiation-tolerant"),
         (r"(?i)radiation[- ]hardened", "radiation-hardened"),
         (r"(?i)beam\s+electro-?magnetic", "Beam Electromagnetic"),
         (r"BEAMFS(?!\\?_)", "BEAMFS en capitales hors identifiant")]
verify = 0
for f in files + ["refs.bib"]:
    for n, line in enumerate(open(f, encoding="utf-8"), 1):
        for ch, what in uni.items():
            if ch in line:
                print("  ECHEC  %s:%d %s" % (f, n, what)); bad += 1
        if f.endswith(".tex"):
            code = line.split("%", 1)[0] if not line.lstrip().startswith("%") else ""
            # TikZ paths use " -- " between coordinates; prose dashes touch words.
            if "---" in code or re.search(r"\w--+\w", code):
                print("  ECHEC  %s:%d tiret double ou triple : %s" % (f, n, line.strip())); bad += 1
            for rx, what in terms:
                if re.search(rx, code):
                    print("  ECHEC  %s:%d %s : %s" % (f, n, what, line.strip())); bad += 1
            if "\\verify{" in code and not f.endswith("paper.tex"):
                verify += 1
                print("  info   %s:%d a confirmer : %s" % (f, n, line.strip()))
print(("  ok     " if bad == 0 else "  ECHEC  ") + "typographie et vocabulaire : %d probleme(s)" % bad)
if verify:
    print(("  ECHEC  " if release else "  info   ") + "%d marque(s) \\verify{} restante(s)" % verify)
sys.exit(1 if bad or (release and verify) else 0)
PYEOF
[ $? = 0 ] || FAILS=$((FAILS + 1))

echo "=== 5. faits bruts cites dans le texte"
D=data/raw/runs/beamfs-bench-analyse-full-20261008-075227/forensics-beamfs-master/dmesg.log
n=$(grep -c 'corrupted indirect pointer ino=11 iblock=45 slot=33 phys=18014398509500044 (out of \[17502, 262144))' "$D")
[ "$n" = 4 ] && ok "run 3 master : ligne du pointeur presente 4 fois" || fail "run 3 master : ligne du pointeur presente $n fois (4 attendues)"
MIR=$HOME/yocto/poky/build-qemu-arm64/downloads/git2/git.kernel.org.pub.scm.linux.kernel.git.torvalds.linux.git
K=72d3fcf802c45d00b300f25b848a93c3a2bd7c7e
if [ -d "$MIR" ]; then
  body=$(git --no-pager --git-dir="$MIR" show "$K:include/linux/bio.h" | awk '/^static inline void bio_advance\(/,/^}/')
  if printf '%s\n' "$body" | grep -q 'bio->bi_iter.bi_size = 0;' && ! printf '%s\n' "$body" | grep -q 'bi_sector'; then
    ok "7.3-rc5 bio_advance : taille a zero, secteur non touche"
  else
    fail "7.3-rc5 bio_advance : corps inattendu"; printf '%s\n' "$body"
  fi
else
  fail "miroir du noyau absent : $MIR"
fi

echo "=== 6. DOI (doi.org)"
for d in $(grep -o -E 'doi += *\{[^}]+\}' refs.bib | sed -E 's/doi += *\{([^}]+)\}/\1/'); do
  code=$(curl -s -o /dev/null -w '%{http_code}' --max-time 20 "https://doi.org/$d")
  case "$code" in
    301|302|303|307|308|200) ok "doi $d -> $code" ;;
    *) fail "doi $d -> $code" ;;
  esac
done
echo "--- Crossref : article IEEE TR de Memon et al. (DOI a reporter dans refs.bib)"
curl -s --max-time 30 "https://api.crossref.org/works?query.author=Memon&query.bibliographic=Linux+radiation+proton&filter=issn:0018-9529&rows=10" \
  | python3 -I -c 'import json,sys
d=json.load(sys.stdin)
for it in d.get("message",{}).get("items",[]):
    print("  info   %s | %s | %s | vol %s no %s p %s" % (it.get("DOI"), (it.get("title") or [""])[0], (it.get("container-title") or [""])[0], it.get("volume",""), it.get("issue",""), it.get("page","")))'

echo "=== 6b. URL de refs.bib (404 ou 410 bloquant, refus d'automate signale)"
# Repositories made public at publication: reported, not failed, until then.
PUBLIC_AT_RELEASE="https://github.com/roastercode/beamfs-xfstests"
for u in $(grep -o -E 'url += *\{[^}]+\}' refs.bib | sed -E 's/url += *\{([^}]+)\}/\1/'); do
  code=$(curl -s -L -o /dev/null -w '%{http_code}' --max-time 30 -A 'Mozilla/5.0 (X11; Linux x86_64)' "$u")
  case "$code" in
    2??) ok "url $u -> $code" ;;
    404|410)
      case " $PUBLIC_AT_RELEASE " in
        *" $u "*) info "url $u -> $code : rendu public a la publication, a reverifier ensuite" ;;
        *) fail "url $u -> $code" ;;
      esac ;;
    *) info "url $u -> $code (a ouvrir a la main)" ;;
  esac
done

echo "=== 7. PDF"
if [ -f paper.pdf ]; then
  pdfinfo paper.pdf | grep -E '^(Pages|Page size|Title|Author):'
  ne=$(pdffonts paper.pdf | awk 'NR > 2 && $(NF-4) != "yes"' | wc -l)
  [ "$ne" = 0 ] && ok "polices toutes incorporees" || { fail "$ne police(s) non incorporee(s)"; pdffonts paper.pdf; }
fi
if command -v chktex >/dev/null 2>&1; then
  chktex -q -n1 -n8 -n36 -n44 paper.tex 2>/dev/null > /tmp/pub-v3-chktex.log
  n=$(grep -c '^Warning' /tmp/pub-v3-chktex.log)
  info "chktex : $n avertissement(s) (non bloquant), detail :"
  cat /tmp/pub-v3-chktex.log
fi

echo "=== bilan : $FAILS controle(s) en echec"
[ "$FAILS" = 0 ]
