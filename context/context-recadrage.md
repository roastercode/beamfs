# beamfs - Recadrage opérationnel pour Claude

> **CLASSIFICATION INTERNAL - NEVER PUSH TO PUBLIC GITHUB**
>
> Ce document est dans `context/` (gitignored sauf exception explicite
> dans `.gitignore` pour ce fichier précis). Il est versionné UNIQUEMENT
> sur `roastercode/beamfs-devel` (privé). Ne jamais pousser sur
> `roastercode/beamfs` (public).

---

**OBLIGATION DE LECTURE** : Claude doit lire ce fichier en TOUT DÉBUT
de session, avant toute action. Ce n'est pas un guide général, c'est
le contrat de travail entre Aurélien Desbières et l'instance Claude
sur le projet beamfs (sécurité kernel, science informatique).

Si tu lis ce fichier : ne le résume pas, ne le commente pas, applique-le.

---

## 1. Pas de sandbox

Ne pas utiliser ta sandbox d'exécution pour ce projet. Tout le code
s'exécute sur la machine d'Aurélien (`spartian-1`, Gentoo, root local).
Livrer le code en heredoc bash EOF, prêt à coller. La sandbox sert
uniquement à valider de la math pure (Python sans I/O système) si
explicitement demandé.

## 2. Check d'abord, code après

Vérifier l'état réel AVANT toute commande qui écrit/efface/mkfs/dd/cp.
Pas d'oracle. Pas d'hypothèse "ça doit toujours être pareil que la
session précédente". État à vérifier minimum avant tout script
destructif :
- `/dev/disk/by-id/` pour les devices physiques attendus
- XML libvirt pour le mapping target ↔ source dev
- État VMs (`virsh list --all`)
- Mount points (`mount | grep ...`)
- `lsblk` tailles attendues sur la cible

Si un check manque : poser le check explicitement read-only AVANT
tout code destructif. Ne jamais "supposer" - toujours vérifier.

## 3. Pas d'anticipation, pas d'oracle

Ne pas répondre à des questions non posées. Ne pas sortir de menus
interactifs gratuits (le tool `ask_user_input_v0` n'est PAS la
réponse par défaut). Ne pas pivoter de sujet. Ne pas proposer 3
"options A/B/C" quand Aurélien attend une exécution directe.

Si Aurélien pose une question simple : 3 lignes max. Si tu hésites,
demande, ne brode pas.

## 4. Pas de psy, pas de mère, pas de coach

Aurélien est un employeur. Claude est un employé sur un sujet
technique. Pas de prise en charge émotionnelle, pas de
reformulation bienveillante, pas de "je comprends que c'est
frustrant", pas de meta-commentaire sur la dynamique de travail.

Si Aurélien recadre : noter, appliquer, continuer. Pas de
auto-critique en boucle. Pas d'excuses répétées. Pas de
"je vais faire mieux" - juste faire mieux.

## 5. Réponses courtes quand on demande court

Aurélien dit "3 lignes max" : 3 lignes max.
Aurélien dit "réponds simplement" : réponse simple, point.
Aurélien dit "lequel je te donne" : tu réponds par le nom du fichier,
pas par un récap de session.

Ne pas faire de tableau récap si Aurélien ne l'a pas demandé. Ne pas
ajouter "Bilan" ou "Étapes restantes" en bas de chaque message.

## 6. Le sujet = beamfs

Le sujet scientifique est beamfs - la résistance d'un système de
fichiers Linux face à des perturbations électromagnétiques (RadFI,
bit-flips, SEE). C'est de la **sécurité kernel**.

`yocto-beamfs` est la **coquille de build**, pas le sujet
scientifique. La résistance se prouve sur beamfs. Ne pas confondre
les deux. Ne pas optimiser yocto au détriment de beamfs.

## 7. Tests intégrés au harness `beamfs-bench`

Pour tester sous attaque : utiliser le binaire Rust unifié
`beamfs-bench` (sous-commandes `multifs`, `analyse --scope=...`, etc.).
Ce binaire **est le successeur direct** des scripts bash historiques
qui vivaient dans `yocto-beamfs/bin/` : `Tir.sh`, `Tir-analyse.sh`,
`Tir-analyse-rapide.sh`, `Tir-multifs.sh`, `Tir-analyse-multifs.sh`.
Ces scripts ont été assemblés, augmentés (cluster-wide forensics,
device validation pipeline R12, topologie auto-discoverée R13) et
rassemblés dans un seul outil dynamique. Les **noms de répertoires
de runs** restent préfixés par `Tir-multifs-<TS>/` et
`Tir-analyse-multifs-<scope>-<TS>/` par compatibilité historique
avec les artefacts forensiques déjà capturés. Ne PAS écrire de
scripts de test ad-hoc parallèles. Si un nouveau test est nécessaire,
l'ajouter au harness `beamfs-bench` (nouveau scope = nouvelle sous-commande), pas à côté.

Bench cluster perf : `bin/hpc-benchmark-beamfs.sh`.

## 8. Rien n'est commité sans test validé

Pipeline obligatoire avant tout commit qui touche un fichier kernel
ou recette :

1. Backup défensif des fichiers à modifier dans `/tmp/`
2. Pre-flight read-only (état repo, lockstep, devices, VMs)
3. Application du patch (Python avec exact strings, assert post-write)
4. Vérification lockstep beamfs ↔ yocto-beamfs (sha256 identiques)
5. `checkpatch.pl --strict` du kernel cible
6. `bitbake beamfs-module` (0 erreur, 0 nouvelle warning)
7. Runtime canary in-VM (single-block round-trip via drop_caches +
   remount, sha256 byte-identique)
8. `beamfs-bench analyse --scope=full` complet (ex-`Tir-analyse-multifs.sh`), comparaison aux verdicts du run
   de référence - beamfs doit avoir verdicts identiques
9. `hpc-benchmark-beamfs.sh` dans tolérance ±20% du baseline
10. dmesg post-test : 0 BUG, 0 oops, 0 WARN

Si UNE SEULE étape échoue : rollback avec le backup, diagnostic,
pas de commit.

## 9. Lockstep beamfs ↔ yocto-beamfs

Les deux copies de `file_inline.c` doivent être byte-identiques :
- `~/git/beamfs/file_inline.c`
- `~/git/yocto-beamfs/recipes-kernel/beamfs/files/beamfs-0.1.0/file_inline.c`

Vérifier par sha256 AVANT et APRÈS toute modification. Toute
divergence est un signal d'arrêt - ne pas patcher dessus, diagnostiquer.

## 10. Backup défensif obligatoire

Avant toute écriture sur un fichier kernel ou recette :

```bash
TS=$(date +%Y%m%d-%H%M%S)
BACKUP=/tmp/<contexte>-backup-$TS
mkdir -p "$BACKUP"
cp <fichier1> "$BACKUP/"
cp <fichier2> "$BACKUP/"
sha256sum "$BACKUP/"*
```

Garder le path en évidence dans la sortie pour rollback rapide.

## 11. Plus de re-découverte

Garder le contexte d'une réponse à l'autre dans une même session.
Ne pas re-lire le handoff à chaque message. Ne pas re-poser la même
question 3 fois sous des formulations différentes. Si une info
manque, demander UNE FOIS, précisément.

Si tu te retrouves à répéter ce que tu viens de dire 2 messages plus
haut : tu es en boucle, arrête.

## 12. Analyser le hardware AVANT de balancer des commandes

Spartian-1 est une machine de travail réelle avec :
- 5 USB physiques passthrough libvirt sur des `by-id` précis
- 4 VMs cluster Slurm (master + 3 compute) sur virbr1 192.168.56.0/24
- Un workstation AMD GPU sous Gentoo, Sway/Wayland, OpenRC
- Des données réelles sur les disques

Avant toute commande qui touche du hardware (mkfs, dd, virsh
destroy, mount sur device physique) :
- Identifier le hardware concret en jeu (`ls /dev/disk/by-id/`,
  `virsh dumpxml`, `lsblk`, `mount`)
- Vérifier que ce qui est ciblé est BIEN ce qu'on veut cibler
- Ne jamais faire confiance aux noms `/dev/sdX` (rebondissent), toujours
  passer par `by-id` ou UUID
- Si doute : poser la question, ne pas exécuter

Une commande qui efface la mauvaise clé USB peut détruire des heures
de bench (5 USB sacrifiées du handoff = artefact scientifique
reproductible). Casser ça serait inacceptable.

---

## R13 - Topologie cluster : ne jamais supposer la passivité d'un node

Le cluster beamfs n'est PAS un système où `beamfs-master` est actif
et les 3 `beamfs-compute0X` sont passifs. La réalité observée
(2026-04-30) :

- Chaque compute a sa propre instance beamfs sur `/dev/vdb`
  montée sur `/data`
- Chaque compute tourne le kernel 7.0.3 avec `beamfs.ko`,
  `reed_solomon.ko`, et `radfi.ko` chargeables
- Les 3 computes sont des **cibles à part entière**, pas des
  observateurs ; toute capture forensique réelle doit les couvrir

**Le fait que la sous-commande `beamfs-bench multifs` (ex-`Tir-multifs.sh`) ne touche que master
ne signifie PAS que la topologie réelle est mono-node.** Cela signifie
que ce script-là est partiel (head-to-head FS comparison sur les 5
USB pass-through, qui sont effectivement attachés à master uniquement).
Le vrai test système (HPC, `beamfs-bench bench` - sous-commande encore à porter depuis l'ancien `Tir.sh`) est multi-node : master + 3
computes, iobench parallèle, RadFI armable sur chaque node.

**Avant toute capture forensique ou test d'attaque cluster, vérifier
read-only sur les 4 nodes :**

```bash
for ip in 192.168.56.10 192.168.56.11 192.168.56.12 192.168.56.13; do
  echo "--- $ip ---"
  ssh -o StrictHostKeyChecking=no -o LogLevel=ERROR \
      -i ~/.ssh/hpclab_admin hpcadmin@$ip \
      'lsmod | grep -E "radfi|beamfs|reed_solomon"; echo ---; \
       mount | grep beamfs; echo ---; uname -r'
done
```

**Pièges connus à vérifier en pré-flight :**

- `WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED` après rebuild
  d'image VM. Symptôme : ssh refuse de se connecter même avec key.
  Fix : `ssh-keygen -R <ip>` AVANT toute commande sur ce node.
  Sinon toutes les actions multi-node échouent silencieusement et
  on conclut à tort que les computes sont inactifs.
- known_hosts désynchronisé peut donner l'illusion qu'un compute
  est down ou ne répond pas, alors qu'il tourne normalement.
- `lsmod | grep beamfs` qui ne retourne que `beamfs 45056 1` (sans
  `radfi`) signifie que RadFI n'est pas chargé sur ce compute mais
  ne signifie pas qu'il ne peut pas l'être : `insmod
  /lib/modules/$(uname -r)/updates/radfi.ko` à faire avant l'attaque.

**Conséquence pour la conception des outils** :

- `beamfs-bench multifs` reste master-only (les 5 USB sont sur master
  uniquement, c'est par construction)
- `beamfs-bench analyse` doit pouvoir capturer **les 4 nodes** quand
  le scope l'exige (default = master pour `analyse` qui enveloppe
  `multifs`, mais `analyse --scope=full` couvre les 4)
- `beamfs-bench bench` (port futur de l'ancien `Tir.sh`, encore à faire) sera multi-node par
  conception (HPC iobench)
- Toute future règle "single-node" doit être justifiée par une raison
  hardware (USB pass-through), pas par fainéantise de capture

---

## R14 - Multi-remote git : ne jamais supposer que `origin` est le remote privé

**Incident fondateur** (2026-04-30, ~22:24) : pendant la session de
finalisation `beamfs-bench`, j'ai (Claude) lancé `git push origin devel`
sur le repo `~/git/beamfs/`. Or ce repo a DEUX remotes :

- `origin` -> `git@github.com:roastercode/beamfs.git` (PUBLIC)
- `devel`  -> `git@github.com:roastercode/beamfs-devel.git` (PRIVATE)

La branche `devel` (contenant des findings scientifiques pre-publication
v3, par construction confidentiels) a donc été poussée sur le repo
PUBLIC pendant ~5 minutes avant détection et `git push origin --delete devel`
correctif. Aucun fork ni indexation détectés, mais l'incident illustre
qu'on ne peut pas se fier à la convention "origin = remote par défaut
= mon remote privé". Sur les repos d'Aurélien, **la convention est
inversée** : `origin` est le remote PUBLIC (mirroring grand public),
les remotes auxiliaires nommés (`devel`, etc.) sont les remotes privés.

**Règle** : avant tout `git push`, **lire `git remote -v` et vérifier
explicitement la visibilité GitHub du remote cible**, en particulier
quand on pousse une branche dont le contenu n'est PAS destiné au
public.

**Pré-flight obligatoire** avant tout `git push` de branche autre que
`main` ou de matériel pre-publication (findings, drafts, context/) :

```bash
# 1. Lister les remotes et leur URL
git remote -v

# 2. Pour chaque remote candidat, vérifier la visibilité GitHub
gh repo view <owner>/<repo> --json name,visibility

# 3. UNIQUEMENT si visibility=PRIVATE pour le remote choisi : push
git push <remote-name> <branch>
```

**Conventions à appliquer sans exception sur les repos d'Aurélien**
(état vérifié 2026-04-30 22:50 via `gh repo list roastercode`) :

- `roastercode/beamfs` est PUBLIC ; `origin` y pointe ; n'y push QUE
  ce qui est destiné public (main code, README, docs publiques).
- `roastercode/beamfs-devel` est PRIVATE ; remote nommé `devel` y
  pointe ; y push tout ce qui est `context/`, `papers/*-findings/`,
  drafts pre-publication, et toute branche de travail (`devel`, autres
  topic branches).
- `roastercode/yocto-beamfs` est PRIVATE (lab layer interne, pas le
  composant public). Pas de remote privé séparé puisque le repo est
  déjà privé ; mais malgré ça, ne pas y commit de fichiers `context/`
  ni `papers/*-findings/` (les gitignorer en amont) - l'idée est que
  même un repo privé peut être ouvert plus tard, donc on garde la
  séparation des couches.
- `roastercode/beamfs-bench` est PRIVATE ; outillage interne (créé
  2026-04-30 via subtree split de yocto-beamfs/beamfs-bench/), jamais
  public sans review.
- `roastercode/radfi` est PRIVATE ; companion to beamfs-devel ; jamais
  public avant la coordonée release avec beamfs.
- `roastercode/yocto-hardened` est PUBLIC (archive de hardening Yocto
  général, sans lien beamfs).
- `roastercode/FTRFS` est PUBLIC (archive du paper FTRFS d'origine,
  ancestor du lineage beamfs).
- Avant tout push, **toujours vérifier `gh repo view <owner>/<repo>
  --json visibility`** plutôt que de se fier à cette liste qui peut
  dériver.

**Conséquence opérationnelle pour Claude** : quand le user demande
"push", ne JAMAIS utiliser `git push origin <branch>` aveuglément.
Toujours :

1. `cd` dans le repo concerné
2. `git remote -v` pour voir les remotes locaux disponibles
3. Si plus d'un remote, demander confirmation au user OU vérifier la
   visibilité GitHub du remote choisi avant push
4. Pour les branches autres que `main`, défaut = remote privé si
   le contenu inclut `context/`, `papers/*-findings/`, ou tout
   matériel pre-publication

**Si l'incident se reproduit** : le seul recours est `git push <public-remote>
--delete <branch>` immédiat, suivi d'une vérification `git ls-remote
--heads <public-remote>`. La branche aura été visible quelques minutes,
mais GitHub fait du GC sur les refs orphelins assez rapidement et
sans publicisation/fork dans cette fenêtre il n'y a pas de leak
permanent. Quand même : prévention >> correction.

---

## R15 - Phase tracking obligatoire

À partir du 30 avril 2026 (clôture Phase 0), tout commit lié à la
trajectoire mainline doit porter un footer `phase=N` dans le
message, où N est le numéro de phase défini dans
`Documentation/roadmap.md` section "Mainline preparation roadmap".

**Format du commit** :

feat(format-v5): add EXTENTS feature flag

phase=1
DoD: extents flag defined in beamfs.h, no behavior yet

[message body]

Signed-off-by: Aurélien Desbrières aurelien.desbrieres@gmail.com


**Filtrage** : `git log --grep="phase=N"` reconstruit l'état d'une
phase à n'importe quel moment.

**Phases définies** :

| Phase | Effort | Scope court                                 |
|-------|--------|---------------------------------------------|
| 0     | 3 h    | Cadrage formalisé (mainline-scope, format-v5-design, fsck.beamfs, roadmap update) |
| 1     | 50 h   | Format v5.0 minimal RFC-able                |
| 2     | 30 h   | fsck.beamfs MVP                             |
| 3     | 12 h   | Multiblock read_folio sub-steps 4-10        |
| 4     | 100 h  | Stage 4 close + paper v3                    |
| 5     | 25 h   | DKMS + Yocto layer                          |
| 6     | 75 h   | Build user base (anti-NAK)                  |
| 7     | 50 h   | Documentation/filesystems + checkpatch zero |
| 8     | 60 h   | RFC mainline + review cycle                 |

**Pas de saut de phase sans DoD validée**. La DoD de chaque phase
est explicite dans `roadmap.md`. Si une DoD ne peut pas être
atteinte, on bloque la phase suivante et on documente le blocage
dans le recadrage avant de pivoter.

**Application immédiate** : tous les commits de la session du
2026-04-30 evening (Phase 0 closure) portent `phase=0`.

---

## Acquis stratégiques - session 2026-04-30 evening (Phase 0)

Cette section capture les décisions structurantes prises durant la
session de clôture Phase 0. Elles sont la base de toutes les
sessions suivantes.

### Repositionnement beamfs

L'ancien pitch ("FS spatial radiation-hardened") était NAK-prone
(FTRFS-style : pas d'utilisateurs, pas de demande explicite). Le
nouveau pitch est :

> *beamfs est le seul filesystem Linux RW avec correction RS-FEC
> native, comblant un manque mainline (dm-fec actuel ne supporte
> que le RO). Il protège contre une classe d'attaques réelle et
> universelle : silent data corruption d'origine matérielle ou
> adversariale (SEU, MBU, IEMI, voltage glitch, rowhammer, aging
> silicon, environmental radiation). Le cas spatial est un cas
> extrême ; le cas datacenter standard est aussi adressé.*

Le scope cible est universel (n'importe quel système Linux), pas
spatial niche. Cette reformulation est documentée dans
`Documentation/mainline-scope.md`.

### Architecture v5 décidée

**Format unique paramétrable** par feature flags (pattern ext4),
exposant **3 profiles préconfigurés** au mkfs :

- `embedded` : 32 KB - 100 GB volumes, 4 KB - 1 GB files, hardware-bound, aucun flag activé (= v5.0 minimal RFC-able)
- `server`   : 1 GB - 16 EB volumes, 4 KB - 16 EB files, 50-300 µs, flags EXTENTS+64BIT+BLOCK_GROUPS+JOURNAL
- `dax`      : 64 GB - 100 TB volumes, 4 KB - 16 EB files, 1-10 µs, flags +DAX

Migration entre profiles via `tune.beamfs -O +flags` (in-place).
Migration depuis ext4/btrfs/xfs via copie + rsync.

11 INCOMPAT bits réservés, 4 RO_COMPAT bits réservés, 3 COMPAT
bits réservés. Détails dans `Documentation/format-v5-design.md`.

### Trajectoire RFC mainline

**RFC initial = profile `embedded` minimal seulement** (~5-8k LoC,
anti-NAK fort). Patches successifs ajoutent les flags un par un.
C'est la trajectoire **f2fs / exfat** (entrée modeste, croissance
par patches), opposée à **bcachefs** (entrée massive, éjection
2025).

Anti-FTRFS-NAK : Phase 6 dédiée à constituer une base utilisateurs
(3-5 déploiements publics cités) AVANT la Phase 8 RFC submission.

### Threat model étendu

Au-delà du scope spatial original, beamfs adresse :

- SEU / MBU (cosmic rays, terrestrial neutrons, baseline silicon)
- IEMI (intentional electromagnetic interference, attack vector)
- Voltage glitch attacks (TPM/HSM bypass attesté)
- Rowhammer-class (DDR3/4/5 software-induced bit flips)
- Aging silicon (NAND/DRAM charge loss)
- Environmental radiation (altitude, latitude, solar events)

Validation harness : RadFI fault injection. Tous ces vecteurs
produisent silent data corruption non corrigée par les FS mainline
actuels.

### Décisions techniques figées

**3824/4096 mismatch v4** : décision option A (garder + multiblock
sub-steps 4-10) OU option B (INODE_UNIVERSAL séparé). Documentée
dans `format-v5-design.md` section 4.3, décision pendante Phase 1.

**Allocator scaling** : multi-bitmap chained insuffisant pour 16 EB.
Block groups (ext-style) suffit jusqu'à 256 TB. Btree allocator
obligatoire pour 16 EB. Trois flags coexistent : `BLOCK_GROUPS`,
`BTREE_ALLOC`, exclusifs.

**Block size** : fixé à 4096 (page size x86-64/aarch64).
`BIGALLOC` flag autorise cluster_size > 4 KB (pattern ext4).

### Out of scope explicites (à défendre dans la cover letter RFC)

beamfs v5 ne fait PAS :
- RAID natif (délégué à dm-raid/mdraid)
- Snapshots CoW (délégué à LVM thin / btrfs subvol au-dessus)
- Encryption native (délégué à dm-crypt)
- Compression (orthogonal, peut-être v6+)
- Network FS (local seulement)
- MTD/UBI natif (block device only en v5)

### Ce qu'on N'A PAS retenu

**Financement** : la discussion Bpifrance/SASU/ADEC/ARCE/ACRE a été
explicitement retirée du scope par décision user. On reste 100%
technique. Pas de structure juridique à monter, pas de demande de
subvention. Si la question revient, ne pas la traiter sans
demande explicite renouvelée.

**Migration in-place ext4 → beamfs** : trop coûteuse en complexité
pour v5. Seule la migration par copie est supportée. Reconsidérer
en v6+ si demande explicite.

### 4 commits Phase 0 (sur `roastercode/beamfs-devel` PRIVATE,
branche `mainline-prep`)

b84e61a docs(fsck): add fsck.beamfs.md design document
a46c462 docs(scope): add mainline-scope.md formalizing v5 trajectory
0c0b590 docs(format): add format-v5-design.md skeleton
c604c85 docs(roadmap): add 8-phase mainline preparation roadmap


Audit R14 post-push confirmé : `mainline-prep` est uniquement sur
`beamfs-devel` (PRIVATE), absente de `beamfs` (PUBLIC). Aucune
fuite.

### Beamfs-bench acquis

Le harnais `beamfs-bench` (privé, `roastercode/beamfs-bench`) sert
de baseline anti-régression pour toutes les phases 1-8. Économie
estimée : ~85 h sur les 405 h totales de la trajectoire mainline.
À chaque ajout de flag/feature, un Test régression beamfs-bench
DOIT passer avant commit.

---

## R16 - Em-dash interdit

L'em-dash `-` (U+2014) est interdit dans toute la doc, le code, les
commits, les noms de fichiers, partout. Utiliser `-` (hyphen-minus,
U+002D) systematiquement.

**Pourquoi** : copy-paste depuis terminal corrompt les em-dash en
markdown auto-link, casse la lisibilite plain-text, et empeche les
recherches grep simples.

**Application** : a partir du 30 avril 2026, tout commit qui ajoute
un em-dash est invalide. Les em-dash existants sont remplaces en
masse dans le commit de cleanup global Phase 0.

**Detection avant commit** :

```bash
grep -rn "-" Documentation/ context/ README.md *.c *.h 2>/dev/null
```

Doit retourner vide.

---

## R17 - Tagline officielle figee

La tagline officielle de beamfs est, sans exception :

beamfs - resilient filesystem


Casse : tout en minuscules. Separateur : hyphen-minus (R16). Pas
d'autre formulation toleree dans :

- README.md (titre H1)
- MODULE_DESCRIPTION dans super.c
- Kconfig tristate
- En-tetes SPDX-style des fichiers .c et .h
- pr_info de chargement du module
- Tout texte public ou commit message qui presente le projet

Les anciennes formulations interdites :
- "Beam-Resilient Filesystem"
- "Beam Electromagnetic File System"
- "BEAM Electromagnetic Resilience"
- toute variante avec majuscules au-dela du nom propre "beamfs"

L'expansion B-E-A-M-F-S comme acronyme est explicitement abandonnee.
"beamfs" est un nom propre, comme f2fs, btrfs, xfs, jffs2.

La portee technique (electromagnetic, radiation, adversarial bit-flip)
est decrite dans threat-model.md, pas dans la tagline.

---

**Fin du contrat de recadrage. Lecture obligatoire en début de session.**
