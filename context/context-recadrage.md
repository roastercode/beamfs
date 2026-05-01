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

**Lecture ligne-à-ligne obligatoire** : Claude lit ce document de la
première à la dernière ligne, sans sauter, sans skim. Si la session
redémarre, la lecture redémarre. Pas de "je m'en souviens" - la mémoire
de session est volatile, le contrat est sur disque. R0 jusqu'à R28 sont
tous lus, intégrés, appliqués avant tout autre fichier projet.

---

## 0. Mapping de versioning (cristallisé 2026-05-01)

Le projet beamfs vit sur **trois** versions, sur **deux** repos GitHub.
Cette structure est non-négociable et doit être respectée à chaque push :

```
  beamfs v2 (PUBLIC)         roastercode/beamfs PUBLIC, branch main
                             = vitrine cristallisée à la dernière publication
                             = paper Zenodo v2 + état du code à ce moment
                             = NE BOUGE PAS entre deux publications
                             = headers actuels: tags v1.x, papier v2 publié
                                                                                
  beamfs-devel v3 (PRIVATE)  roastercode/beamfs-devel PRIVATE, branch mainline-prep
                             = la trajectoire de travail courante
                             = sub-step 4 → 10 INLINE-MULTIBLOCK
                             = paper v3 in progress (papers/2026-04-beamfs-v3-findings/)
                             = context/, manifests, drafts, findings
                             = TOUTES les sessions de dev poussent ici
                                                                                
  beamfs v3 (PUBLIC, futur)  roastercode/beamfs PUBLIC, branch main
                             = sera produit à la double-publication suivante :
                               1. Zenodo paper v3 publié (DOI assigné)
                               2. Mail RFC kernel.org prêt à partir
                             = à ce moment, on push beamfs-devel → beamfs PUBLIC
                               (filtré pour exclure context/, papers v3 findings,
                                manifests audit-grade internes)
                             = tag annoté GPG : v0.5.0-... ou similaire
```

**Implication immédiate** : aucun push sur `beamfs` PUBLIC entre les
publications. Pendant tout le développement v3 (sub-step 4 → RFC), seul
`beamfs-devel` PRIVATE reçoit les commits. Le PUBLIC est gelé sur la
dernière release publiée.

**Workflow opérationnel** :

- Tout commit code/docs/papers va sur `beamfs-devel` PRIVATE branche
  `mainline-prep` (ou autre branche topic, jamais `main`).
- Tout commit yocto va sur `yocto-beamfs` PRIVATE branche `main` (lockstep
  miroir des sources kernel + manifests pipeline).
- Tout commit harness va sur `beamfs-bench` PRIVATE branche `main`.
- Tout commit overlay Gentoo va sur `beamfs-overlay` PRIVATE branche `main`.
- **Aucun** commit ne va sur `beamfs` PUBLIC tant que paper v3 + RFC ne
  sont pas tous deux prêts à publication coordonnée.

**Vérification avant tout push** (R14) : `gh repo view <owner>/<repo>
--json visibility` doit retourner `PRIVATE` pour le remote ciblé. Si
un push vers PUBLIC est tenté pendant la phase de dev v3, c'est un
bug de la procédure. Ne pas forcer.

**Quand publier v3 PUBLIC ?** Conditions cumulatives obligatoires :

1. Sub-step 4 → 10 INLINE-MULTIBLOCK closed avec pipeline `beamfs-bench
   full` overall_rc=0 (R19) sur le commit final.
2. xfstests subset generic/{001,002,010,098,257} passing.
3. checkpatch.pl --strict sur tous les fichiers .c/.h : 0 errors,
   0 warnings.
4. Paper v3 finalisé, soumis sur Zenodo, DOI assigné.
5. Cover letter RFC kernel.org rédigée, patches cleans (rebased,
   atomiques), `git send-email` prêt à partir.

À ce moment-là, et pas avant, on prépare le push PUBLIC v3 :

- Branche temporaire `public-v3-staging` créée depuis `mainline-prep`.
- Filter : exclure `context/`, `papers/2026-04-beamfs-v3-findings/`
  (sauf publication finale Zenodo), `Documentation/runs/manifest-*.json{,.asc}`
  (rester sur PRIVATE comme audit interne).
- Cherry-pick ou rebase pour produire un historique linéaire propre.
- Tag annoté GPG : `v0.x.0-beamfs-v3-rfc`.
- Push `public-v3-staging:main` sur `roastercode/beamfs` PUBLIC.
- `git send-email` patches sur linux-fsdevel.
- Annoncer Zenodo DOI dans la cover letter.

Cette opération est **manuelle, intentionnelle, finale**. Elle est
préparée avec rigueur dans une session dédiée, jamais comme un effet
de bord d'une autre session.

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
`beamfs-bench` (sys-fs/beamfs-bench, installé via overlay Gentoo).

**Sous-commande canonique pre-push : `beamfs-bench full`.** Cette
commande est autonome et fait tout en un seul appel :

  1. VM lifecycle (destroy aveugle + start + wait_ssh parallèle)
  2. Cluster /data bootstrap (insmod + mkfs.beamfs + mount sur 4 nodes)
  3. Device validation USB sticks (R12 prompt anti-NAK)
  4. Multifs head-to-head 5 FS x 3 probs sur USB
  5. Cluster attack 4 nodes x 3 probs sur /dev/vdb
  6. Forensics complet (dmesg, ftrace, perf, RadFI counters, RS journal)
  7. Tarball archive

`beamfs-bench full` rend obsolètes tous les anciens scripts bash :
`Tir.sh`, `Tir-analyse.sh`, `Tir-analyse-rapide.sh`, `Tir-multifs.sh`,
`Tir-analyse-multifs.sh`, `bin/hpc-benchmark.sh`, `bin/hpc-benchmark-beamfs.sh`.
Ces scripts ne doivent plus être invoqués. Le naming `Tir-*` n'existe
plus dans le vocabulaire opérationnel.

Sous-commandes restantes :
- `beamfs-bench multifs` : multifs seul (USB sticks, sans cluster)
- `beamfs-bench analyse --scope=quick|standard|full` : forensics modulaire
- `beamfs-bench full` : pipeline complet autonome (canonique pre-push)
- `beamfs-bench bitrot|metadata|crash|fsck` : DONE 2026-05-01 (4 nouveaux tests scopes)

Ne PAS écrire de scripts de test ad-hoc parallèles. Si un nouveau test
est nécessaire, l'ajouter au harness `beamfs-bench` comme nouvelle
sous-commande, pas à côté.

Les **noms de répertoires de runs** historiques (`Tir-multifs-<TS>/`,
`Tir-analyse-multifs-<scope>-<TS>/`) restent en place sous
`Documentation/runs/` comme artefacts archivistiques. Les nouveaux runs
gardent ce naming jusqu'à nettoyage explicite (TODO future tooling).

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
8. `beamfs-bench full` exit code 0 sur cluster live 4 nodes
   (R19 : critères détaillés plus bas). Ce step remplace les anciennes
   étapes 8 et 9 (analyse + hpc-benchmark séparés, retirés).
9. (vide - fusionné dans step 8)
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

**Le fait que la sous-commande `beamfs-bench multifs` ne touche que master
ne signifie PAS que la topologie réelle est mono-node.** Cela signifie
que cette sous-commande est partielle (head-to-head FS comparison sur les
5 USB pass-through, qui sont effectivement attachés à master uniquement).
Le vrai test système multi-node passe par `beamfs-bench full` (qui appelle
`analyse --scope=full` en interne, lequel orchestre le cluster_setup/
attack/verify sur master + 3 computes en plus du multifs USB).

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
- `beamfs-bench analyse` capture les 4 nodes au scope `full` (master
  uniquement aux scopes `quick` et `standard`)
- `beamfs-bench full` est multi-node par construction (orchestre
  lifecycle + bootstrap + analyse scope=full sur les 4 nodes)
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

## R18 - Pas de glob shell ouvert, pas de checkpatch --file sur header

**Incident fondateur** (2026-05-01, ~08:50) : pendant le bloc 3 de
validation Phase 1.1, j'ai (Claude) genere un script combinant deux
anti-patterns critiques :

1. Glob non borne dans une boucle for :
   `for p in /usr/src/linux-*/scripts/checkpatch.pl ; do ... done`
   Sur Gentoo, `/usr/src/linux-*/` peut matcher de nombreux
   repertoires (sources kernel multiples). L'expansion shell injecte
   tous les paths dans la boucle, sans borne.

2. `checkpatch.pl --file beamfs.h 2>&1 | tail -30` sur un header de
   28 KB. checkpatch en mode `--file` audite TOUT le fichier (pas le
   diff), produit des centaines a milliers de warnings sur du code
   legacy, et le flux stdout/stderr massif via pipe peut tuer le
   terminal sous Wayland (compositor backpressure foot/alacritty/kitty).

Resultat : terminal tue, perte de la session shell.

**Regle** : ne JAMAIS combiner glob ouvert + invocation checkpatch
verbeuse + pipe direct vers un terminal interactif.

**Anti-patterns interdits** :

```bash
# Glob expansion non bornee
for p in /usr/src/linux-*/scripts/checkpatch.pl ; do ... done
for f in /var/log/**/*.log ; do ... done

# checkpatch --file sur header existant, pipe vers tail
checkpatch.pl --file beamfs.h 2>&1 | tail -30
checkpatch.pl --strict --file include/linux/fs.h | head -100

# Tout pipe verbeux non borne vers terminal interactif
find / -name '*.h' 2>&1 | head
dmesg -w | grep BUG
```

**Patterns corrects** :

```bash
# Path explicite, echec rapide si absent
CHECKPATCH=/usr/src/linux/scripts/checkpatch.pl
[ -x "$CHECKPATCH" ] || { echo "checkpatch absent"; exit 0; }

# checkpatch sur PATCH uniquement, sortie redirigee vers fichier
git diff beamfs.h > /tmp/phase.patch
"$CHECKPATCH" --strict /tmp/phase.patch > /tmp/checkpatch.out 2>&1
wc -l /tmp/checkpatch.out
head -50 /tmp/checkpatch.out  # consultation bornee a posteriori

# Si volume incertain : toujours rediriger d'abord
commande_verbeuse > /tmp/out 2>&1
wc -l /tmp/out
head -100 /tmp/out
```

**Conclusion** : Claude ne genere JAMAIS, sans exception :
- de glob expansion non bornee dans une boucle for ou un argument de
  commande qui itere
- de `checkpatch.pl --file` sur un fichier kernel existant
- de pipe direct d'une sortie potentiellement volumineuse vers un
  terminal interactif (utiliser redirection vers /tmp/ + consultation
  bornee via `head`/`wc -l`/`grep` ensuite)

Si le volume de sortie est incertain : redirection fichier obligatoire,
consultation bornee ensuite. Le terminal d'Aurelien sous River/Wayland
n'est pas une cible jetable, sa perte = perte de la session de travail.

---

## R19 - Validation pre-push : `beamfs-bench full` exit 0 obligatoire

Avant tout `git commit` qui touche du code kernel, du code Rust
`beamfs-bench`, ou des recettes Yocto associées, ET avant tout `git
push` (origin OU devel), la validation suivante est obligatoire :

```bash
beamfs-bench full --auto-confirm 2>&1 | tee /tmp/beamfs-bench-full-$(date +%s).log
echo "Exit: $?"
```

**Critère de succès (cumulatif, tous obligatoires)** :

- Exit code = 0
- Phase 1 lifecycle : 4 VMs running, 4 nodes SSH ready
- Phase 2 bootstrap : 4 nodes /data monté beamfs (BOOTSTRAP=OK x4)
- Phase 5 multifs beamfs : RECOVERED 3/3 (probs 1k, 100k, 1M)
  Note : ext4/btrfs FS_PANIC à prob=1M est attendu et non bloquant.
  Seul beamfs doit RECOVERED 3/3.
- Phase 6 cluster : 12/12 RECOVERED DIFFS=0 (4 nodes x 3 probs)
- Phase 7 forensics : pas de nouveau BUG/Oops/WARN dmesg vs baseline

**Si UN SEUL critère échoue** : pas de commit, pas de push. Diagnostic
d'abord, fix, re-run `beamfs-bench full`, puis seulement commit/push.

**Exception** : commits docs-only (pas de code touché, juste
`Documentation/`, `context/`, `README.md`). Ces commits sont exempts
de R19 mais doivent être clairement marqués `docs(...)` ou similaire
dans le subject, et ne JAMAIS toucher de fichier `.c`/`.h`/`.rs`/
`.bb`/`.bbappend`. Si un commit mélange code et docs, R19 s'applique.

**Reproductibilité** : `beamfs-bench full` est installé via
`sys-fs/beamfs-bench` (overlay `beamfs-overlay` sous `/var/db/repos/`).
Le binaire est `/usr/bin/beamfs-bench` après `emerge sys-fs/beamfs-bench`.
L'utilisateur doit être dans le groupe `libvirt` (sudoers NOPASSWD
virsh généré par l'ebuild en `/etc/sudoers.d/beamfs-bench`).

**Anti-pattern interdit** : invoquer `bin/Tir-*.sh`, `bin/Tir.sh`,
`bin/hpc-benchmark.sh`, `bin/hpc-benchmark-beamfs.sh`, ou tout script
shell de bench legacy. Ces scripts existent encore sur disque pour
raison archivistique (artefacts forensiques cités dans les papers)
mais NE DOIVENT PLUS être invoqués. Toujours `beamfs-bench full`.

---

## R20 - Format de sortie console pour Aurélien

Quand Claude livre un bloc bash/Python à exécuter dans cette interface,
la sortie capturée par Aurélien doit être facile à localiser dans le
flot du terminal. Donc : tout bloc encadré par un marker visuel clair
**généré par des `echo ""` exécutables**, pas par du markdown plain-text
(qui voit ses lignes blanches collapsées par le rendu chat).

**Format obligatoire** : tout bloc shell livré dans cette interface est
un bloc bash fenced (```bash ... ```) qui contient les `echo ""`
nécessaires pour produire les lignes blanches réelles dans le terminal.

Exemple canonique :

```bash
echo ""
echo ""
echo ""
echo "#######"
echo "#début#"
echo "#######"
echo ""
echo ""
echo ""
<commande réelle à exécuter>
echo ""
echo ""
echo ""
```

**Composition obligatoire** :
- 3 `echo ""` AVANT le marker (sépare du prompt précédent)
- marker exactement `echo "#######"` / `echo "#début#"` / `echo "#######"`
  (7 hash, espace zéro, hyphens zéro, accent é dans `début`)
- 3 `echo ""` APRÈS le marker (sépare du contenu de la commande)
- la commande réelle
- 3 `echo ""` APRÈS la commande (sépare la sortie du prompt suivant)

**Pourquoi `echo ""` et pas des lignes blanches markdown** : les lignes
blanches markdown plain-text sont collapsées par le rendu chat (1 ou 2
max conservées). Seuls des `echo ""` exécutés par bash produisent
des lignes blanches réelles dans la sortie terminal d'Aurélien.

**Anti-patterns interdits** :
- bloc bash sans `echo ""` autour du marker (lignes blanches markdown
  qui collapsent au rendu)
- marker en dehors d'un bloc bash fenced (commande non copiable d'un
  coup, marker rendu comme texte)
- variantes du marker : `##### DEBUT #####`, `=== START ===`, bannière
  ASCII art, ou tout autre format. Le format est figé.
- bloc qui mélange markdown narratif et `echo ""` : le bloc bash doit
  être auto-suffisant, copiable d'un seul coup dans le terminal.

---

---

**Fin du contrat de recadrage. Lecture obligatoire en début de session.**


## R21 - R-isolation : architecture FS-test isolee, master orchestrateur seul

Contrat architectural enforce par `beamfs-bench` Phase 0 pre-flight via
`virsh dumpxml`. Le cluster doit respecter ce layout libvirt persistent :

| VM                | Disques attendus                            | Role                                       |
|-------------------|---------------------------------------------|--------------------------------------------|
| `beamfs-master`   | `vda`, `vdb`                                | orchestrateur, jamais cible RadFI          |
| `beamfs-compute01`| `vda`, `vdb`, `vdc`, `vdd`, `vde`, `vdf`, `vdg` | victime FS-test (5 USB ext4/ext3/btrfs/squashfs/beamfs) |
| `beamfs-compute02`| `vda`, `vdb`                                | cluster compute (BEAMFS sur /dev/vdb)      |
| `beamfs-compute03`| `vda`, `vdb`                                | cluster compute (BEAMFS sur /dev/vdb)      |

Justification : si le master tient les 5 USB et est aussi orchestrateur
(perf record, dmesg capture, SSH driver), une attaque RadFI sur une USB
victime peut contaminer transversalement le kernel state du master
(block layer, page cache, scheduler) et invalider la rigueur du bench.
Master doit rester un observateur non-victime.

Enforcement : `lifecycle.rs::assert_isolation_architecture()` est appele
en Phase 0 de `bring_cluster_up()`. Sur divergence, le bench abort avec
un message explicite citant R-isolation. Le bench refuse de tourner sur
un cluster non-conforme.

Validation : commit beamfs-bench `b23ab2a` (2026-05-01), test inline
verifie que `virsh attach-disk beamfs-master ... vdh` fait correctement
echouer `beamfs-bench full` en Phase 0 avec le message attendu.

Implication pour publication v3 : la rigueur architecturale du banc
d'essai est explicitement documentee dans la prochaine publication
beamfs (cf. `papers/2026-04-beamfs-v3-findings/`).

---

## R22 - R-os-stack : pile OS workstation Aurelien

Workstation `spartian-1` :

- Distribution : Gentoo Linux (source-based, OpenRC init)
- Compositor : Wayland + Sway
- Terminal : foot
- Editeur : Emacs
- Shell : bash

Contraintes operationnelles qui en decoulent :

- Pas de systemd : tout service-related utilise OpenRC (`rc-service`,
  `rc-update`)
- Pas de X11 par defaut : commandes graphiques doivent supporter
  Wayland natif ou XWayland fallback
- Pas de `gnome-terminal` / `xterm` : invocations terminal via `foot`
- Restart Sway sous Wayland : `killall -HUP sway` pas `rc-service sway
  restart` (kill la session graphique)


## R23 - R-image-canonique : recipe Yocto unique et autoritaire

Le recipe Yocto canonique de l'image cluster BEAMFS est :

  ~/git/yocto-beamfs/recipes-core/images/hpc-arm64-research-beamfs.bb

C'est le coeur des travaux. C'est le seul recipe d'image actif dans
BBLAYERS et le seul qui produit les rootfs ext2 deployes sur les 4 VMs
du cluster (master + compute01..03).

Tout ce qui concerne :

  - les outils de tracing (strace, blktrace, bpftrace, ftrace, perf)
  - les outils de bench (fio, iperf3, sysstat)
  - les modules kernel embarques (beamfs-module, mkfs-beamfs,
    beamfsd, radfi-module)
  - les paquets userspace (slurm, munge, hwloc, btrfs-tools,
    squashfs-tools, e2fsprogs)

doit etre ajoute via IMAGE_INSTALL dans CE fichier, pas ailleurs.

Ne JAMAIS chercher ce recipe en aveugle. Ne JAMAIS supposer qu'il
existe sous un autre nom (hpc-arm64-research.bb sans suffixe est
le legacy non-BEAMFS dans yocto-hardened, hors scope BEAMFS).

Recipes connexes dans yocto-beamfs :

  recipes-kernel/linux/linux-mainline_7.0.3.bb : kernel 7.0.3
  recipes-kernel/linux/BEAMFS-arm64.cfg        : kernel config arm64
  recipes-kernel/linux/files/multifs.cfg       : kernel config multifs
  recipes-kernel/beamfs/beamfs-module_0.1.0.bb : module BEAMFS
  recipes-kernel/beamfs/mkfs-beamfs_0.1.0.bb   : mkfs.beamfs userspace
  recipes-kernel/radfi/radfi-module_0.1.2.bb   : module RadFI
  recipes-beamfs/beamfsd/beamfsd_0.1.0.bb      : daemon beamfsd
  recipes-hpc/slurm/slurm_25.11.4.bb           : slurm
  recipes-hpc/munge/munge_0.5.18.bb            : munge
  recipes-hpc/pmix/pmix_5.0.3.bb               : pmix

Build :

  bitbake hpc-arm64-research-beamfs

Image produite :

  ~/yocto/poky/build-qemu-arm64/tmp/deploy/images/qemuarm64/
    hpc-arm64-research-beamfs-qemuarm64.ext2

Deployee aux 4 VMs via copie binaire vers
/var/lib/libvirt/images/hpc-arm64/beamfs-{master,compute01,compute02,compute03}.img

## R24 - R-CWD : invocation bench depuis yocto-beamfs

Le binaire `beamfs-bench full` cherche le repo root yocto-beamfs depuis
le CWD courant via `analyse.rs::find_yocto_root()`. Si CWD ne resout
pas vers `~/git/yocto-beamfs/`, l'invocation echoue avec :

  `could not locate yocto-beamfs repo root from <cwd> or exe path`

Donc l'invocation canonique R19 est :

```bash
cd ~/git/yocto-beamfs
beamfs-bench full --auto-confirm
```

Piege observe : `script -q -c "beamfs-bench full" /tmp/run.log` change
le CWD effectif et casse la resolution de root. Ne pas utiliser script(1)
pour capturer R19 ; rediriger stdout/stderr classiquement :

```bash
cd ~/git/yocto-beamfs
beamfs-bench full --auto-confirm 2>&1 | tee /tmp/r19.log
```

---

## R25 - R-bg-detach : SSH background commands need full FD detach

Quand le worker.sh lance un process background via SSH (ex : crash
test dd loop), le bash backgrounded herite des stdin/stdout/stderr du
shell parent SSH. Tant que les FDs sont ouverts, la connection SSH
parent attend EOF. Sans detach, `ssh.exec_lenient` bloque.

Pattern correct (referencer pour modules futurs) :

```bash
sudo nohup bash -c "
    (process_in_loop)
" </dev/null >/dev/null 2>&1 &
PID=$!
disown $PID 2>/dev/null || true
echo $PID > /tmp/worker-bg-$TS.pid
```

Trois elements indispensables :
- `nohup`            : detach SIGHUP au logout SSH
- `</dev/null`       : ferme stdin
- `>/dev/null 2>&1`  : ferme stdout + stderr
- `&` puis `disown`  : detache du job control bash

Sans ces 4 elements, le worker SSH bloque indefiniment.

Pattern documente dans worker.sh fonction `crash_start_writer`
(2026-05-01 commit `df76a4d`).

---

## R26 - R-tracing-rigueur : outils professionnels, pas bricolage

Pour tout diagnostic de comportement kernel/FS, utiliser les outils
fsdevel professionnels disponibles dans l'image canonique R23 :

| Outil      | Use case                                      |
|------------|-----------------------------------------------|
| strace     | syscalls userspace + errno                    |
| blktrace   | I/O block layer events                         |
| ftrace     | kernel function tracing (debugfs)              |
| perf       | sampling profiling + counters                 |
| trace-cmd  | wrapper ftrace                                 |
| dmesg      | kernel ring buffer (lecture **structuree**)    |

**Anti-patterns interdits** (bricolage) :
- `bash -x` + `set -x` comme outil de diagnostic principal
- `dmesg | grep <keyword>` comme analyse complete
- `cat /proc/...` ad-hoc sans tarball forensic
- diagnostic empirique base sur pattern matching shell sans capture

**Pattern correct** :

1. Capture forensic via tarball : `tar czf /tmp/diag-$TS.tar.gz <files>`
2. Outils tracing en parallele du test (strace + blktrace concurrent)
3. dmesg lecture **structuree** (parser par categories : EIO,
   journal_replay, panic, fsck_needed, etc.)
4. Decision basee sur **donnees empiriques convergentes** depuis 3
   sources, pas une seule.

---

## R27 - R-anchor-exact : Python patches via byte-for-byte count==1

Pour tout patch Python sur fichier code/doc, utiliser exclusivement
le pattern :

```python
old = bytes(...)  # anchor exact byte-for-byte
new = bytes(...)
assert content.count(old) == 1, "anchor not unique"
content = content.replace(old, new, 1)
```

**Anti-patterns interdits** :
- `re.search(...)` ou `re.sub(...)` multilignes pour suppression de code
- `re.compile(r'...', re.DOTALL)` pour matcher un bloc fonction
- regex gourmandes type `[^}]*\}` pour delimiter

Cause : les regex multilignes peuvent matcher au-dela de l'intention
(greedy by default), supprimant du code adjacent. Incident observe
2026-05-01 : tentative de suppression de `cmd_not_yet_implemented`
via regex a corrompu 14 lignes au-dela de la fonction, build casse.

Anchor byte-for-byte avec `count == 1` garantit que :
- Le patch ne s'applique que si l'anchor est unique
- En cas d'ambiguite, l'assertion echoue avant modification
- Pas de surprise greedy regex

---

## R28 - R-empirical-state : lire le disque avant patch, pas la memoire de session

Avant tout patch, **toujours** lire l'etat empirique actuel sur le
disque, jamais speculer depuis la memoire de session.

Symptome de violation : "je sais que ce fichier contient X parce que
je l'ai genere il y a 5 messages". Cette assertion est non-fiable :
- L'utilisateur a peut-etre edit le fichier manuellement
- Un patch precedent a peut-etre echoue partiellement
- La session a peut-etre ete compactee

**Pattern correct avant tout patch** :

```bash
sha256sum <fichier>
grep -nE '<anchor pattern>' <fichier>
# verifier que l'etat reel correspond a l'attendu
```

Ce pattern est aussi valide pour les VMs distantes : avant tout patch
SSH, lire l'etat reel via `ssh ... 'cat <fichier>'` ou `scp ... <local>`
puis diff.

Specifiquement pour le worker.sh : verifier les SHA local + deploye.
Si divergence, redeployer avant patch.

---

## R29 - R-gpg-signing : preauth interactif obligatoire avant batch loopback

`gpg --batch --pinentry-mode loopback` ne peuple PAS le cache gpg-agent
sans passphrase fournie via stdin. Sans cache prealable, l'invocation
fail silencieusement avec :

  `gpg: Sorry, we are in batchmode - can't get input`

Symptome observe (2026-05-01) : sequence de plusieurs commits GPG-signed
dans la meme session. Les premiers commits passent (cache populated par
un signing interactif anterieur), puis le cache expire (default-cache-ttl
3600s) ou est vide en debut de session, et `git commit -S` ou
`emit_manifest` fail.

**Le seul vrai preauth qui populate le cache gpg-agent** :

```bash
# Preauth interactif au debut d'une session (UNE fois par cycle 3600s)
git commit -S ...                  # via pinentry-curses au TTY
# OU
gpg --sign /tmp/dummy              # pinentry-curses au TTY
rm /tmp/dummy.gpg
```

L'invocation interactive (sans `--batch`) declenche `pinentry-curses`,
qui demande la passphrase au TTY, et populate le cache gpg-agent pour
le TTL configure (3600s default, 7200s max).

**Anti-pattern interdit** : utiliser `gpg --batch --pinentry-mode
loopback` comme "preauth" en debut de bloc shell. Ca ne marche que si
le cache est deja populated, donc c'est un no-op qui passe ou un fail
silencieux selon l'etat du cache. Ce n'est PAS un mecanisme de preauth.

**Pattern correct dans `pipeline::emit_manifest`** : `gpg --batch
--pinentry-mode loopback --detach-sign --armor` est valide UNIQUEMENT
si le cache gpg-agent a ete populated par un signing interactif
anterieur dans la fenetre TTL. Pour mega run, le preauth se fait au
debut de la session avant invocation `beamfs-bench mega`.

**Pour Claude generant des blocs shell multi-commit** : ne pas inserer
de pseudo-preauth `gpg --batch --pinentry-mode loopback` en tete de
bloc. Si plusieurs commits sont attendus, le premier `git commit -S`
populate le cache pour les suivants, sans ceremonie supplementaire.

---

**Fin R0-R29. Lecture obligatoire de R0-R29 en debut de session.**

