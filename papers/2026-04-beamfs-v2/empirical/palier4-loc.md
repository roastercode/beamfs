# Palier 4 — LOC audit

Date: 2026-04-29
Source: ~/git/beamfs/*.c *.h (head c47f130, tag v0.2.0-palier3-validated)
Method: awk filter excluding C block comments (/* ... */ mono and multi-line),
line comments (//), and blank lines. Tool-independent, no cloc dependency.

## Result

**Total kernel module LOC = 2198**
Budget (threat-model §6.5) = 5000
Utilization = 44%

## Per-file breakdown

| File           | LOC | Share |
|----------------|----:|------:|
| super.c        | 479 |  22%  |
| namei.c        | 420 |  19%  |
| alloc.c        | 274 |  12%  |
| file_inline.c  | 212 |  10%  |
| file.c         | 208 |   9%  |
| beamfs.h       | 205 |   9%  |
| edac.c         | 176 |   8%  |
| inode.c        | 117 |   5%  |
| dir.c          | 107 |   5%  |

## Reproducibility

awk 'BEGIN{ib=0;t=0} {l=$0; while(length(l)>0){if(ib){p=index(l,"*/");if(p==0){l="";}else{l=substr(l,p+2);ib=0;}continue} sub(/\/\/.*/,"",l); p=index(l,"/*"); if(p==0)break; h=substr(l,1,p-1); r=substr(l,p+2); q=index(r,"*/"); if(q==0){l=h;ib=1;}else{l=h substr(r,q+2);}} gsub(/[[:space:]]/,"",l); if(length(l)>0)t++} END{print "TOTAL LOC =", t}' ~/git/beamfs/*.c ~/git/beamfs/*.h

## Significance for certification regimes

- DO-178C / ECSS-E-ST-40C / IEC 61508 typically require LOC audit
  for safety-critical filesystems. Sub-5000 LOC is the documented
  threshold in BEAMFS threat model §6.5.
- Current 2198 LOC leaves 2802 LOC headroom for write path
  (~250 LOC estimated) and post-Zenodo additions.
