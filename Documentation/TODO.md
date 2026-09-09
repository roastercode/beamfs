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

## 2026-09-08 — 464 : ce qui est établi, et ce qui reste

### Harnais de capture (beamfs-xfstests, tools/beamfs-trace.sh)
Déployé en /usr/bin sur le nœud. Résout les échecs de mécanique qui
avaient coûté plusieurs journées : setsid pour survivre à la session
ssh, écriture sur le disque réel (pas le tmpfs de 982 Mo), détection
du trace_printk à newline mal échappée, corrélation faite sur le nœud.
  usage : sudo beamfs-trace.sh 464 4
  sortie : /var/beamfs-trace/{run,lost,sum}N.txt

### Établi par la trace, définitivement
- Allocateur SAIN : 0 double allocation sur 1,5M d'événements.
- Pointeurs CORRECTS en mémoire : install puis relectures voient la
  bonne valeur, même buffer_head, même blocknr, dirty=1.
- Chemin d'écriture CORRECT en isolation : un fichier de 40 blocs
  (12 directs + indirect + 28) écrit ses 41 blocs, fsck propre.
- Les blocs perdus sont des blocs de données neufs (memset à zéro,
  visibles en zéros sur disque) dont le bloc d'indirection parent
  n'a jamais été écrit (motif cdcd = jamais touché depuis mkfs).
- Un même slot d'un ibh est installé deux fois à quelques secondes
  d'écart : l'écrivain pose le pointeur, le flusher relit val=0 et
  réalloue. Entre les deux, aucune troncature.

### Correctifs posés cette session (tous validés, tous commités)
- 629020d sweep : confirme par relecture
- 379b577 lecteurs d'arbre sous i_alloc_mutex
- 95a4eb1 sweep : saute les inodes occupés (racine des faux positifs)
- bbac7a8 installation de pointeur : mmb_mark_buffer_dirty + mark_inode_dirty
- 13d6c8b iget : décodage RS de l'inode dans une copie
Plus, sessions précédentes : i_alloc_mutex sur les 5 parcours,
inode_lock sur write_iter, verify non destructif, lock_buffer sur les
3 installations.

### Ce qui reste
La fuite ne se manifeste que sous la concurrence de 464 (16 processus,
write + append + writeback + truncate sur 200 fichiers). 13 à 40 blocs
par run fautif, ~50% des runs. Piste non épuisée : pourquoi un ibh
marqué dirty, rattaché à i_metadata_bhs, survit à writeback_inodes_sb
+ sync_inodes_sb + deux sync_blockdev sans être écrit. Prochaine sonde :
block_rq_issue croisé avec les numéros d'ibh, sous charge 464, pour
voir si le bloc est soumis puis perdu, ou jamais soumis.

### Non publiable en l'état
Un volume qui perd des blocs sous charge concurrente ordinaire, sans
injection de fautes, ne peut pas servir de référence pour mesurer
l'effet d'un faisceau : on ne distinguerait pas les deux causes.

## Règle de diagnostic (2026-09-08)

Face à une erreur, épuiser les moyens disponibles jusqu'à la cause
exacte, AVANT toute correction et AVANT toute annonce.

- Capturer tout ce qui est capturable en un seul run. Un filtre choisi
  d'avance sur l'hypothèse du moment ne peut que confirmer ou infirmer
  une idée à la fois, et fait perdre un cycle par question.
- Répéter jusqu'à reproduction certaine. Un run non concluant n'est pas
  un résultat et ne doit jamais être présenté comme tel.
- Croiser les niveaux : filesystem, page cache, writeback, périphérique
  bloc, contenu disque bit à bit.
- Ne dire "c'est trouvé" que quand la chaîne causale est complète et
  vérifiée. Sinon : "voici un fait, voici ce qu'il reste à établir".
- Un fait qui contredit l'hypothèse a priorité sur l'hypothèse.
- Si le diagnostic bute sur l'outillage plutôt que sur le défaut,
  réparer l'outillage d'abord.

### Vérifier avant de conclure (2026-09-09)

Un test ne prouve rien si l'artefact testé n'est pas celui qu'on croit.
Avant toute conclusion tirée d'une exécution :

- **Vérifier que le binaire ou le module contient la modification.**
  `cargo build` qui affiche "Finished in 0.00s" n'a rien recompilé ;
  `strings binaire | grep <chaîne ajoutée>` et la comparaison des dates
  source/binaire coûtent deux secondes. Deux fois aujourd'hui une
  demi-heure a été perdue sur un artefact périmé : un module beamfs sans
  ses sondes après un `git stash drop`, un binaire xfstests sans le
  correctif après un cargo qui n'a pas recompilé.
- **Vérifier la cible.** Quel nœud, quel volume, quel montage. Une
  campagne lancée sur le nœud par défaut alors qu'on travaille sur un
  autre depuis la veille ne mesure rien.
- **Projeter la durée.** Une commande qui tourne deux heures ne se lance
  pas en avant-plan dans le terminal de quelqu'un. Détaché, journal dans
  /tmp, et le terminal reste libre.
- **Nettoyer l'état laissé par l'essai précédent.** Montage occupé,
  processus résiduel, fichier sentinelle : chacun a coûté un cycle.

Faire les choses complètement du premier coup, pas par tâtonnement :
lire le code avant de patcher, vérifier l'ancre avant de l'appliquer,
contrôler l'artefact avant de le tester.
