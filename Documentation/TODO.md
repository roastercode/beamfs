# beamfs - TODO / RAF
*Updated: 2026-07-08 (post system update + §3.10 closure session)*

---

## 2026-09-23 mkfs and fsck against e2fsprogs (3.22, 3.23; mkfs 0.1.3, fsck 0.1.4)

- [x] mkfs O_EXCL + /proc/mounts, -F -F; fsck repair O_EXCL
- [x] UUID/label, fsync, -V -q -L -U, usage, duplicate condition; fsck -a -C -T -t -r, device size check, %u, leak
- [ ] BX 2.3.28: no umount -l, restart the domain after a kill
- [ ] kernel: superblock state flags (mounted, errors) + mkfs/fsck reading them (with the rc4 build)
- [ ] kernel: parity of an indirect block once per flush, not per pointer; unwritten blocks instead of a zero image
- [ ] kernel 7.3-rc4 in yocto-beamfs
- [ ] fsck pass 3: tell a fast symlink's target bytes from out-of-range pointers (476 image: 444 "out of range")

## 2026-09-23 tree checker index and parity verified bit (3.21, module 0.1.9)

- [x] s_tc_child index; forget_parent and zeroed probe 512 keys
- [x] BH_BeamfsVerified; scrub verifies the medium through _medium
- [ ] measure: sweep generic/074 under cpuwho, then iowho, then the six
- [ ] the zero image of a fresh allocation is written by the bdev flusher before the data (350 k bios of 4 KiB, a quarter of the bytes on 0.1.8)

## 2026-09-23 reservation windows after ext2 (3.20, module 0.1.8)

- [x] per-inode goal and window, s_rsv_windows under s_lock, discarded at evict
- [ ] measure: sweep generic/074 under iowho (bio sizes, budget), then the six

## 2026-09-23 writeback after ext2/ext4 (3.19, module 0.1.7)

- [x] bounce pages, bios of 32, boundary block deferred, folio finished once
- [x] out_free_sbi leak of s_scratch_pool and s_sb_rs_staging
- [ ] measure: sweep generic/074 under iowho, then the six against ext2's 3 minutes
- [ ] BX: STALLED must read the disk counters it already has; a `full` ending on a header with allocations running is a buffered pass, not a stall
- [ ] read path: read_folio_range still reads one block at a time, synchronously (bdev_rw_virt); same shape, same fix
- [ ] 109: 6 blocks used but unreferenced in 6 s; 650: bitmap subblock uncorrectable taken as truth at mount (alloc.c:170-210)

## 2026-09-22 cross-analysis of d14ad29 (cppcheck + per-function scan)

- [x] file_inline.c:174 uninitialised `phys` in the direct-pointer allocation check (3.17, module 0.1.6)
- [x] INODE_UNIVERSAL path without parity maintenance: refused at mkfs and mount (3.18)
- [x] dead test on NULL `ibh` in truncate, dead `ret = 0` after scrub_init, `%u` given an int in alert.c, redundant `mode` read in scrub.c
- [ ] yocto-beamfs: rename files/beamfs-0.1.5 -> beamfs-0.1.6 and the recipe before the next bitbake (sync-layer.sh already points at 0.1.6)
- [ ] retire file.c (legacy iomap path) once 3.18 has held through a full sweep

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

## 2026-09-07 -- état xfstests après la semaine de correction

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
2. write_iter sous inode_lock -- write vs truncate concurrents. Commit a179e6c.
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

## 2026-09-07 (suite) -- sweep uncorrectable au boot : RÉSOLU

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

## 2026-09-07 -- édifice assaini avant reprise de 464

Corrections de cohérence posées cette session, toutes validées :
- 629020d : sweep confirme par relecture avant de reporter
- 379b577 : read_folio_range et write_read_folio_range lisent l'arbre
  sous i_alloc_mutex (cohérence de lecture ; ext2 tient truncate_mutex
  sur le même parcours). Partiel assumé : le mutex couvre le lookup,
  pas l'intervalle jusqu'à l'usage du bloc -- le tenir sur l'I/O serait
  coûteux et risqué.
- 95a4eb1 : sweep saute les inodes occupés (racine des faux positifs)

Note : generic/076 échouait en séquence après 464 -- conséquence de la
fuite 464 laissant le scratch incohérent, pas un défaut propre ni un
défaut de l'outil de lancement (montages=0 vérifié avant chaque test,
scratch propre). Il passe dès que la séquence ne contient pas 464.

Reste ouvert : la fuite résiduelle 464, et 589.

## 2026-09-08 -- 464 : ce qui est établi, et ce qui reste

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

### Retour visuel obligatoire (2026-09-09)

Tout ce qui tourne plus de quelques secondes affiche sa progression au
fur et à mesure, sur le même terminal, sans attendre la fin.

- Une boucle affiche son numéro AVANT de commencer, son résultat après.
- Une erreur affiche ce qui a échoué et depuis combien de temps, pas un
  code de retour nu.
- Une commande distante qui échoue rapporte stderr, pas seulement rc.
- Un programme muet pendant plusieurs minutes est indistinguable d'un
  programme bloqué : la personne qui regarde n'a aucun moyen de le
  savoir, et finit par tuer un travail qui allait aboutir.

Cela vaut pour les scripts comme pour les binaires : println! puis
flush, ou echo, mais jamais rien.

## Petit défaut cosmétique (2026-09-09)

Le canary de mkfs.beamfs (inode 2, fixture INLINE, format-v4.md sec 11)
porte un horodatage à l'époque zéro : `ls` affiche "Jan 1 1970" sur
un volume qui vient d'être formaté. La racine, elle, a bien l'heure
courante. Sans conséquence sur la cohérence, mais visible partout et
de nature à faire douter d'un volume neuf lors d'une relecture.
Source : yocto-beamfs/recipes-kernel/beamfs/files/beamfs-0.1.3/mkfs.beamfs.c

## 2026-09-21 -- ce que la journée a établi, et ce qu'elle a réfuté

### Établi par mesure

Le stockage hôte dominait toutes les mesures. Les volumes de test
portaient 179996 et 142283 extents par copy-on-write btrfs, soit un
extent pour un bloc et demi. Les images vivent désormais sous
`/var/lib/libvirt/images/x86-nocow/`, répertoire marqué `chattr +C`,
et tiennent à 3 extents après trente formatages. La dérive monotone de
38 à 60 secondes par essai a disparu avec elles.

Le scrubber est dans le circuit des pertes de blocs. Garé
(`XFSTESTS_SCRUB_MS=0`) : zéro bloc perdu. Actif au défaut de 100 ms :
353 et 376 blocs sur deux séries. La variable qui sépare les essais en
échec des essais réussis est `host.blk.vdh.rd.bytes`, 1,14 Go lus sur
un volume de 1 Go contre 69 Mo, sur trois séries. Réserve : quatre
essais valides seulement avant blocage du nœud.

### Réfuté par mesure

L'ordonnancement des métadonnées ne ferme pas les pointeurs perdus.
`CONFIG_BEAMFS_ORDERED_META=y`, trois séries de dix essais de
generic/083 : `LOST POINTER` toujours présent, taux inchangé dans les
vingt points de dispersion du test. La question posée par le Kconfig
et par journal-design.md reçoit un non. Un journal de métadonnées
reste justifié pour la sûreté au redémarrage ; il ne fermera pas ce
symptôme.

`treecheck` compare des valeurs en mémoire, avant toute écriture. Un
slot relu à zéro n'est donc pas un désordre d'écriture vers le
support : le bloc a été remis à zéro entre deux stores, ce que
`ZEROED IN SERVICE` nomme. `beamfs_alloc_block` est correct sous
`s_lock`, donc le bloc revient par un autre chemin.

Le commentaire de `matrix.rs` affirmant "scrub is not it" est faux.

### État du lab modifié ce jour

- Images du nœud x86-01 déplacées vers `x86-nocow`, sans
  copy-on-write. Le XML libvirt pointe dessus ; sauvegarde de
  l'ancien dans `~/disk-backup/libvirt-xml-20260921-105833/`.
- `beamfs-xfstests` 2.2.0 vers 2.3.4 : sous-commande `nodes status`,
  cinq troncatures silencieuses supprimées, `deploy` dérive le chemin
  du rootfs de `virsh domblklist` au lieu de le coder en dur.
- `BEAMFS_ORDERED` remis à "0" dans local.conf après l'expérience.
  Changer cette variable n'invalide pas `do_configure` : il faut
  `bitbake -f -c configure linux-mainline` pour que le `.config`
  suive.

## 2026-09-21, soir : l'instrument avant le noyau

### Etabli par lecture du code et des donnees

`beamfs-xfstests` ne compte qu'une des trois classes de defauts que
fsck.beamfs rapporte. `lost` est le nombre qui precede
`used-but-unreferenced` dans le `.full` (bench.rs, one_trial).
`referenced-but-free` (fsck.beamfs.c:1369, meme ligne) et
`out-of-range pointer(s)` (ligne 875) ne sont lus nulle part. Sur le
sweep 1789974744 : generic/464 et 476 ont au moins 400
`referenced-but-free` chacun et sont enregistres "0 bloc perdu" ;
generic/269 a 6215 pointeurs hors plage, invisibles dans toute
statistique. Trois defauts de sens different, et l'outil en voit un.
A corriger en 2.3.6 avant tout correctif du noyau sur ces tests.

Le fichier baseline etait degrade par l'outil, pas par la copie : voir
runs/2026-09-21-baselines/README.md. Corrige en 2.3.5.

Un essai en erreur de transport n'etait compte nulle part et faisait
neuf essais sur dix en silence ; corrige en 2.3.5, compte comme
blocage.

### Non etabli

L'etat de CONFIG_BEAMFS_ORDERED_META dans l'image deployee au moment
de la campagne du soir. `checkpoint` verifie que l'image est
posterieure au dernier commit, pas ce que porte son .config. Les
chiffres de cette campagne (80/50/70) ne se comparent ni a 03 ni a 04
tant que ce point n'est pas lu dans l'image.

## 2026-09-22 : ce que la journée a établi

### Instrument

- La sonde invité de `beamfs-xfstests` n'a jamais rendu une valeur
  avant 2.3.7 : quatre `grep` portaient des `\"` sous `sh -c '...'`,
  le shell sortait en erreur, `run()` jetait la sortie. 204 essais de
  083 dans `state-generic-083.log` avec des clés `host.*` seules. Tout
  rapport de séparation antérieur comparait des compteurs libvirt.
  Corrigé, dit quand ça échoue, tenu par un test.
- `mem.orphan_file` mesuré par BX à chaque capture ; 1 822 168 kio au
  premier relevé. Voir known-limitations 3.15.
- Cause de 3.15 trouvée le 22 par heldfolio 2.3.14 puis bhbalance
  2.3.15 (BX) : buffer_heads du writeback jamais rendus. Corrigé, voir
  known-limitations 3.15. `stop --hard` ne redémarrait pas un nœud
  muet (BX 2.3.13) ; `deploy` ne synchronise pas la couche, c'est
  `tools/sync-layer.sh` qui le fait, à appeler avant bitbake.
- fsck.beamfs 0.1.2 (22) : « never described » distinguait mal deux
  cas ; la parité d'un bloc nul est nulle, donc un indirect vide sous
  une case nulle est correct. Sur l'image de 476-001, 1 189 « jamais
  écrits » = 1 096 vides + 93 à pointeurs sans parité, ces derniers
  parcourus désormais. Le défaut réel : la case de parité d'environ un
  indirect à pointeurs sur douze n'atteint pas le médium (93 et 94 sur
  deux images de 476). Chemin à trouver ; le noyau devrait aussi taire
  « has no parity written yet » quand le bloc est lui-même nul (à faire
  avec le prochain bitbake).
- `Trial` porte `referenced_free` et `out_of_range`, lus dans le
  `.full` comme `lost`. Cinq nombres sur chaque ligne `FAIL`.
- Ouvert, 2.3.8 : `speak` compte des formes repliées et l'écrit comme
  des blocs. « 1 blocks marked used that nothing references » pour un
  fsck qui en liste 6803 (essai 2 de 476). Le nombre à imprimer est
  celui de la ligne de résumé de fsck, pas celui de `Case::collapse`.

### generic/476, essai 2, `.full` de xfstests, scratch démonté

- pass 4 : 6803 blocs utilisés non référencés, « the first 64 span
  18521..24176 in 63 run(s), longest 2 », le premier « 0 of 4096
  bytes set -- allocated and never written ».
- pass 6 : 5084 blocs indirects « never written -- allocated, named
  by an inode, and no parity was ever filed », sous-arbres
  inaccessibles ; 792 entrées de répertoire nommant un inode libre.
- dmesg du même essai : `volume full, first refusal at data`, `at
  dindirect data`, `at L1 indirect`.
- Lecture cohérente, non établie : les branches `-ENOSPC` de
  l'installation d'un bloc indirect, après que le parent a reçu le
  pointeur, laissent le bloc nommé et jamais écrit, et les dirents
  vers un inode libre sont l'autre moitié du même chemin d'erreur
  (§3.2). À vérifier dans file_inline.c avant tout patch. 476 échoue
  9 fois sur 12 : compatible avec un chemin d'erreur, pas seulement
  avec une course.
- Les trois essais ont duré 480 à 577 s contre un budget de 570 ;
  `trend` a treize runs de 476 terminés. Le nœud portait 1,74 Gio
  d'orphelins.

### Sweep de nuit 1790043166

- 734 tests, 3 échecs : 074 par budget, 075 (12611, pire connu),
  476 (36). Non lancés : 102, 103, 650, 651. `compare` contre le
  sweep de 23 : 3 corrigés, 0 régressé.
- 075 est bimodal sur 18 runs : zéro ou plus de cinq mille, jamais
  entre les deux.

## 14. Priority matrix

### Tier 1

| Item | Phase | Effort | Note |
|------|-------|--------|------|
| leak-1 | 1 | 2-5 j | Double attribution d'un bloc indirect : ZEROED IN SERVICE et LOST POINTER survivent à ORDERED_META |
| wedge-1 | 1 | fait le 2026-09-22 | known-limitations 3.15 : `get_bh` avant `bh_submit` dans `beamfs_inline_writeback_range`, rendu par personne sur les blocs non derniers d'une plage (`bh_end_write` ne rend rien en 7.3) ; 50 000 buffer_heads par essai de 083, 196 Mio, jusqu'au blocage. Corrigé par `beamfs_inline_wb_end_block`, vérifié par heldfolio : 540 contre 50 752, +4 236 kio contre +411 940 kio. Résidu de 1 % ouvert, candidat : `sb_getblk` passé à `beamfs_ind_parity_update` dans `lookup_or_alloc_phys_new`, déficit de 489 vu par bhbalance |
| fsck-mounted | 2 | fait le 2026-09-22 (BX 2.3.17) | Le fsck de capture refuse un volume monté et le dit ; plafond de 5 000 lignes retiré. Les fsck de tests tués pris avant cette version ne sont pas des preuves |
| stall-074 | 1 | 2-5 j | known-limitations 3.16 : writeback à l'arrêt sous generic/074 (Dirty 190 Mio, Writeback 0, 0 E/S), 2 sur 13 runs, dmesg muet. Aucune pile prise ; BX 2.3.18 les prend (`stall.txt`) et redémarre le nœud avant le test suivant. 075 dans les sweeps était la conséquence (mkfs par le sweep sur un volume tenu par des tâches en D) : retiré des défauts de beamfs |
| courses | 1 | | generic/109 (133, 910 sur 9 runs), generic/650 (95 sur 1 run), generic/083 (50 %, leak-1 en direct le 22) : même famille à confirmer sur leurs dossiers ; generic/476 chronique (34 à 6 803), generic/464 fuit des blocs indirects sans parité |
| BX | 2 | | `speak` n'imprime pas les cartes à crochets ni les comptes de fsck ; `compare` ne nomme ni réparés ni nouveaux ; `state` capture après le `dd` du volume (rd_sectors de 2 Mio de secteurs = l'instrument) ; `deploy` ne synchronise pas la couche (`tools/sync-layer.sh`) et la garde juge l'heure des commits ; `stop --hard` garde 1 octet de console ; `nodes restart` absent |

### Tier 2

| Item | Effort | Note |
|------|--------|------|
| doc-refutations | 2 h | Porter les réfutations du 2026-09-21 dans known-limitations.md, sections 3.13 et 3.14 |
| sonde-volume | 2-3 j | Module optionnel pour vérifier un volume monté : gel, copie, vérification hors ligne |
| scrub-lock-measure | 1 j | Mesurer l'effet du verrou de scrub_walk_level en mode rs, seul mode où la parité agit |

### Tier 3

| Item | Effort | Note |
|------|--------|------|
| parity-pr-debug | 10 min | `indirect block N has no parity written yet` est un `pr_warn_ratelimited`, pas un `pr_debug` ; 1456 callbacks suppressed observes sur un seul essai le 2026-09-21 ; a passer en pr_debug ou en compteur par montage rapporte au demontage, comme treecheck.c:79 |
| stale-comments | 30 min | Les quatorze sites d'appel de ind_parity_verify (13 dans file_inline.c, 1 dans scrub.c) et les seize occurrences de "corrected in place" (13 file_inline.c, 2 edac.c, 1 indparity.c) décrivent une fonction qui décode en scratch depuis longtemps |
| man-stop-dup | fait | beamfs-xfstests 2.3.6, commit 53cbab2 |
