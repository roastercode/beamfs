# beamfs - TODO / RAF
*Updated: 2026-07-08 (post system update + §3.10 closure session)*

---

## CLOSED this session (2026-07-08)

### §3.10 evict_inode BUG_ON (CLOSED)
Root cause: buffer_heads attached to `inode->i_data.i_private_list`
by `mark_buffer_dirty_inode()` were not detached before `clear_inode()`.
Fix: `invalidate_inode_buffers(inode)` before `clear_inode()` in
`beamfs_evict_inode()` (commit 11c844f). Defense-in-depth: I_FREEING
guard in `beamfs_write_bitmap_block()` (f964b27), NULL owner in
eviction-path `beamfs_free_block()` calls (f146cbd). Validated R19
under RadFI injection, tainted=4096 (OOT-only) on all 4 nodes.

### System update (Gentoo spartian-1)
- World update 619 packages, kernel hôte 6.18.18 -> 7.1.3 (Alder Lake)
- CFLAGS `-march=native` -> `-march=alderlake` via resolve-march-native
- Python 3.14 as system default (python3.13 for bitbake, pending upstream fix)
- beamfs-bench ebuild sudoers doublon fix (9999 + 24 versioned ebuilds)

### Yocto HPC stack bumps
- libevent 2.1.12 -> 2.1.13 (security: integer overflow, OOB read, HTTP smuggling)
- pmix 5.0.3 -> 5.0.10 (bugfix series, slurm hang fix)
- linux-mainline 7.0.3 -> 7.0.9 (6 stable point releases)

---

## OPEN - beamfs kernel

### §3.11 RS(255,239) silent miscorrection on data blocks (TRIAGED)
Format v6 + per-block CRC32 deferred. Under high-density EM injection
(probability=100000 ppm), RS can "correct" a data block to a valid
but wrong codeword. Requires per-block CRC32 as second integrity layer.

### §3.12 emufi disabled on compute01 (TRIAGED)
emufi module present on disk but not loaded/functional on compute01.
Root cause not isolated.

### Theorem v2.2b (conjecture)
Saturation observability on data blocks - split from v2.2a (proven)
in commit 66c8eb8. Conditional on format v6 per-block CRC32.

---

## OPEN - beamfs-bench

### TUI dashboard (ratatui)
Real-time multi-panel terminal UI for `beamfs-bench full`:
- VM state panel (virsh list, tainted status per node)
- RadFI injection counters and progress per FS/probability
- dmesg tail per node (filtered: beamfs/radfi/BUG/Oops)
- bitbake build progress (task N/M, current recipe)
- SSH worker activity (which node, which phase)
Motivated by repeated blind spots during long bench runs in this
session. Reference: htop-style layout, ratatui Rust crate.

### Runtime verbosity + structured logging
beamfs-bench currently outputs minimal progress on stdout during
attack/verify phases. Needed: structured log file (JSON or NDJSON)
with per-event timestamps, per-node status, injection parameters,
and phase transitions. Enables post-mortem analysis without
re-running the full bench.

### Fix locate_repo_root() heuristic
`multifs.rs::walk_up_for_repo()` searches for `Documentation` +
`bin` + `beamfs-bench` directories walking upward from cwd - only
matches when launched from within `yocto-beamfs/`. Should search
sibling directories or accept an env var `BEAMFS_YOCTO_ROOT`.
Currently worked around by launching from `~/git/yocto-beamfs/`.

---

## OPEN - Gentoo system (spartian-1)

### depclean pending
51 obsolete packages (old gcc, llvm-21, python-3.13, rust-bin-1.95,
gentoo-sources-6.18.18). Safe to run after confirming no runtime
dependency on python-3.13 beyond bitbake.

### ocaml/coccinelle chain broken
ocaml-gettext-0.4.2.1 fails to compile against ocaml 5.4 (AST
breakage). Masked in package.mask. coccinelle removed from world
set (depends on camlp4 which requires ocaml-gettext). Revisit when
Gentoo upstream fixes ocaml-gettext.

### guestfs-tools excluded
guestfs-tools-1.54.0 hardcodes `libguestfs[ocaml]` unconditionally
in its ebuild. Excluded from world update. Not needed for beamfs
workflow.

### python3.13 as bitbake default
bitbake (poky styhead) crashes under python3.14 due to
PicklingError in multiprocessing (forkserver start method change).
python3.13 set as eselect default until upstream fix in bitbake.

### CONFIG_DEBUG_INFO_BTF absent
bpftrace/bpftool functional but emit BTF warnings at build time.
Low priority - not blocking any workflow.

### Microcode amd-uc.img in grub.cfg
Cosmetic: grub references AMD microcode but CPU is Intel i7-12700F.
No functional impact.

---

## OPEN - Kernel security / bug bounty

### RDMA/rtrs-srv integer underflow
Patch submitted to linux-rdma@vger.kernel.org (June 2026).
Acked-by from Haris Iqbal received. Jason Gunthorpe (maintainer)
involved. Status: awaiting merge into rdma-next. Google buganizer
issue closed as Infeasible (not Android scope).

## 2026-09-07 — état xfstests après la semaine de correction

### Résolus et prouvés
- **generic/522** : passe en 1133 s (fsx soak, tracé de bout en bout,
  aucune erreur). Ce n'était pas un défaut mais de la durée. Budget porté
  à 1800 s dans beamfs-xfstests (LONG_TESTS). RÉSOLU.
- **generic/473** : ne peut pas passer. La parité inline rend la carte
  fiemap non-affine (addr avance de 4096 pendant que length vaut 3824),
  donc un extent déborde toujours. Prouvé par le calcul de géométrie.
  Limite de format, documentée, à mentionner dans la lettre de couverture.

### Corrigés (generic/464, fuite de blocs), tous fondés sur trace
Quatre défauts réels, chacun commité :
1. i_alloc_mutex sur les cinq parcours de l'arbre (alloc + free_blocks_from
   + free_data_blocks + evict). Commit 6bd9ca2.
2. write_iter sous inode_lock — write vs truncate concurrents. Commit a179e6c.
3. ind_parity_verify décode une COPIE, jamais bh->b_data. decode_rs8
   corrige en place ; la parité de région périmée d'un bloc réalloué
   ramenait l'ancien contenu. Commit 28c3f8b.
4. installation d'un pointeur d'indirection (sind/dind/tind) sous
   lock_buffer + ind_parity_update sous le verrou. Commit be0fa62.

Effet mesuré : pics de 1400+ blocs perdus constants -> majorité d'essais
propres, mais la fuite N'EST PAS éliminée (4 échecs sur 8 au dernier run,
pic résiduel 1126). Sources multiples et indépendantes produisant le même
symptôme fsck (used-but-unreferenced).

### Ouverts, documentés pour reprise à froid
- **generic/464, fuite résiduelle** : les essais qui échouent sont les
  courts (<140 s), les longs passent. Chaque bloc perdu tracé se termine
  par une installation de pointeur (inst/l1inst) sans free ni truncate
  après. Méthode qui marche : trace_pipe (pas de perte) sur un run,
  sondes inst/l1inst/l1read/alloc/free avec inode et site. À faire :
  tracer PLUSIEURS runs consécutifs et corréler pour savoir si la source
  restante est un chemin unique ou variable.
- **generic/589** : comportement de montage ET de propagation
  (shared/slave/private/bind) VÉRIFIÉ IDENTIQUE à ext4 côte à côte.
  Le diff est un mpC compté une fois au lieu de deux dans une séquence
  de propagation. Ni la fuite 464 (pas d'erreur fsstress dans le .full),
  ni un défaut de montage démontrable. Cause exacte non trouvée.

### Rappels de méthode (coûteux à réapprendre)
- git stash drop efface les sondes non commitées : le build suivant
  compile le code propre et on mesure sans instrumentation sans le voir.
- Tampon ftrace en mémoire déborde sur un run 464 (>2.9M événements) :
  utiliser trace_pipe drainé vers un fichier, qui bloque le producteur
  plutôt que de perdre.
- Ancres Python EOF : le code bouge, vérifier count==1 et relire le
  texte réel (cat -A) avant chaque patch.

## 2026-09-07 (suite) — sweep uncorrectable au boot : RÉSOLU

Les 23 "sweep uncorrectable" au boot étaient des FAUX POSITIFS
transitoires, prouvé par trois vérifications indépendantes :
- fsck valide l'image entière (5 passes OK) ;
- le décodeur de référence hors ligne rend 0 uncorrectable sur les
  blocs incriminés (5111, 14127, 9805) ;
- ces blocs sont bit-pour-bit identiques entre l'image et le disque VM.

Cause : le sweep lit le bdev via sb_bread EN PARALLÈLE du writeback du
rootfs au boot, et attrape des blocs à moitié synchronisés (données à
jour, parité pas encore, ou l'inverse). À froid, trois passages complets
du sweep : 0 incident.

Correction (commit 629020d) : relecture-confirmation. Sur un premier
échec de décodage, le sweep drop le buffer caché, relit frais via
__bread, redécode, et ne journalise/compte que si le second décodage
échoue aussi. Comportement d'un scrubber matériel. Validé : 3 boots
successifs, sweep uncorrectable=0 à chaque fois.

### Chantier de fond : FAIT (95a4eb1)
Le sweep lisait le bdev en parallèle du writeback. Corrigé : ilookup()
au début de scrub_one_inode -- l'inode n'est balayé que s'il n'est pas
en cache, ou s'il est au repos (ni I_DIRTY_ALL, ni I_SYNC, ni I_NEW,
ni I_FREEING/I_WILL_FREE, ni mappé en écriture). ilookup ne lit jamais
le disque, donc pas de récursion dans le FS.

Validé : 0 uncorrectable au boot ET sous charge xfstests, 7/7 tests de
contrôle passent (001 002 006 013 076 313 676), 0 autre incident.
La relecture-confirmation (629020d) reste comme seconde barrière.

## Bilan xfstests fin de semaine
- 522 : PASS (1133s), budget outil 1800s. RÉSOLU.
- 473 : impossible (format inline non-affine). Documenté.
- sweep uncorrectable boot : RÉSOLU (629020d).
- 464 : 4 correctifs réels, fuite réduite (majorité propre), source
  résiduelle documentée. OUVERT.
- 589 : montage/propagation identique à ext4 vérifié. Cause exacte
  non trouvée. OUVERT.

## 2026-09-07 — édifice assaini avant reprise de 464

Corrections de cohérence posées cette session, toutes validées :
- 629020d : sweep confirme par relecture avant de reporter
- 379b577 : read_folio_range et write_read_folio_range lisent l'arbre
  sous i_alloc_mutex (cohérence de lecture ; ext2 tient truncate_mutex
  sur le même parcours). Partiel assumé : le mutex couvre le lookup,
  pas l'intervalle jusqu'à l'usage du bloc — le tenir sur l'I/O serait
  coûteux et risqué.
- 95a4eb1 : sweep saute les inodes occupés (racine des faux positifs)

Note : generic/076 échouait en séquence après 464 — conséquence de la
fuite 464 laissant le scratch incohérent, pas un défaut propre ni un
défaut de l'outil de lancement (montages=0 vérifié avant chaque test,
scratch propre). Il passe dès que la séquence ne contient pas 464.

Reste ouvert : la fuite résiduelle 464, et 589.
