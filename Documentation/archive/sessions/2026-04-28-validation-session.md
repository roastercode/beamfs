# Session de validation beamfs v1 - 2026-04-28

## Acquis empirique

### Phase Yocto + VM
- Build Yocto `hpc-arm64-research-beamfs-qemuarm64.squashfs` réussi
  (scarthgap+styhead, slurm.conf path absolu corrigé)
- 4 VMs libvirt `beamfs-{master,compute01,compute02,compute03}` déployées
  sur `192.168.56.10/11/12/13` via MAC réservées dans hpcnet
- SSH par clé `~/.ssh/hpclab_admin` (user hpcadmin)
- vdb dédié par VM : `/var/lib/libvirt/images/hpc-arm64/beamfs-*.img` (64M)

### Module beamfs
- `beamfs.ko` charge avec dépendance `reed_solomon.ko` chargé manuellement
  (auto-load TODO via modules.dep)
- Magic on-disk confirmé : `4D 41 45 42` = "BEAM" little-endian
- Format v1 INODE_UNIVERSAL scheme=5
- `mkfs.beamfs` opérationnel : 16384 blocks, bitmap block 17 RS FEC protected
- Mount type=beamfs sur `/dev/vdb`, R/W/stat fonctionnels

### Slurm cluster
- munge + slurmctld + slurmd opérationnels sur 3 compute nodes
- `sinfo` montre les nœuds en idle
- S4 (beamfs write from Slurm) = PASS

### Bench M1-M5 (parité FTRFS ↔ beamfs)

| Métrique | FTRFS baseline (28-04 20:58) | beamfs (28-04 22:33) |
|---|---|---|
| M1 write+fsync (MB/s) | 4.348 / 5.000 / 5.263 | 2.326 / 5.000 / 5.263 |
| M2 read cold (MB/s) | 12.5 / 20 / 25 | 14.286 / 20 / 25 |
| M4 stat bulk (s) | 0.140 / 0.150 / 0.160 | 0.140 / 0.150 / 0.160 |
| M5 small write (ms) | 22 / 23.5 / 27 | 21 / 22 / 31 |

**Médianes M1/M2/M4 identiques. M5 médiane légèrement meilleure.**
Migration FTRFS→beamfs = fonctionnellement neutre. Lecture A
(rebadge mécanique) confirmée empiriquement.

## Theorem IV.1 - différé

`inject_raf <dev> <block_no> <err_bits>` est un outil de simulation
post-recovery hérité de FTRFS : il écrit un `beamfs_rs_event` (40 bytes)
dans le journal RAF du superblock, **sans toucher au block_no en argument**
et **sans recalculer le CRC32 global du superblock**.

Conséquence : le mount post-injection détecte la mismatch CRC32
(`got 0x8b0a035c, expected 0x01e1b88e`), tente la recovery RS
(`attempting RS recovery`), échoue (`RS block uncorrectable, len=211`).

**Ceci ne valide ni n'infirme Theorem IV.1.** L'outil ne fait pas
ce que le théorème exige (corruption physique d'un bit dans un block
RS-protégé hors superblock).

La validation empirique de Theorem IV.1 nécessite **RadFI** :
injection algébrique modélisant correctement les SEU/MBU
électromagnétiques. Voir `~/git/radfi/`.

## Scaffold RadFI

`~/git/radfi/` créé avec :
- README.md (positionnement scientifique, roadmap 5 phases)
- papers/v1/paper.tex (préambule LuaLaTeX/IEEEtran/amsthm complet)
- papers/v1/sections/00-12 (stubs avec TODO)
- papers/v1/refs.bib (FTRFS + beamfs pré-cités)
- papers/v1/Makefile (latexmk -lualatex)
- Documentation/design-notes.md

Pas de versioning git encore.
Pas de code kernel encore.

## Snapshot

`~/backup-beamfs/beamfs-m1-m5-baseline-ok--20260428-223654.tar.gz` (476K, 71 files)

## Pending fixes (avant push kernel.org)

1. `install_hostname_init()` dans `hpc-arm64-research-beamfs.bb` :
   regex `ftrfs\.hostname=` → `beamfs\.hostname=`
2. `setup_hosts_static` : générer `beamfs-*` dans /etc/hosts au build
3. `slurm.conf` source : `arm64-*` → `beamfs-*` (ou fork dans yocto-beamfs)
4. SUMMARY/DESCRIPTION du module : "Fault-Tolerant Radiation-Robust" →
   "Beam-Resilient Filesystem"
5. Auto-load reed_solomon avant beamfs (via modules.dep ou modprobe.d)

## Position scientifique honnête pour publication

Le paper beamfs v1 doit être publié comme :
- Theorem IV.1 énoncé
- Format implémenté (mkfs/mount/RW + Slurm + M1-M5 PASS)
- **Validation empirique de IV.1 différée au paper RadFI compagnon**

Pair publication : RadFI = problème (EM induces corruption),
beamfs = solution (RS FEC survives same injection).
Pattern Spectre+KAISER, Meltdown+KPTI.
