# BEAMFS — Recadrage opérationnel pour Claude

> **CLASSIFICATION INTERNAL — NEVER PUSH TO PUBLIC GITHUB**
>
> Ce document est dans `context/` (gitignored sauf exception explicite
> dans `.gitignore` pour ce fichier précis). Il est versionné UNIQUEMENT
> sur `roastercode/beamfs-devel` (privé). Ne jamais pousser sur
> `roastercode/beamfs` (public).

---

**OBLIGATION DE LECTURE** : Claude doit lire ce fichier en TOUT DÉBUT
de session, avant toute action. Ce n'est pas un guide général, c'est
le contrat de travail entre Aurélien Desbières et l'instance Claude
sur le projet BEAMFS (sécurité kernel, science informatique).

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
tout code destructif. Ne jamais "supposer" — toujours vérifier.

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
"je vais faire mieux" — juste faire mieux.

## 5. Réponses courtes quand on demande court

Aurélien dit "3 lignes max" : 3 lignes max.
Aurélien dit "réponds simplement" : réponse simple, point.
Aurélien dit "lequel je te donne" : tu réponds par le nom du fichier,
pas par un récap de session.

Ne pas faire de tableau récap si Aurélien ne l'a pas demandé. Ne pas
ajouter "Bilan" ou "Étapes restantes" en bas de chaque message.

## 6. Le sujet = beamfs

Le sujet scientifique est BEAMFS — la résistance d'un système de
fichiers Linux face à des perturbations électromagnétiques (RadFI,
bit-flips, SEE). C'est de la **sécurité kernel**.

`yocto-beamfs` est la **coquille de build**, pas le sujet
scientifique. La résistance se prouve sur BEAMFS. Ne pas confondre
les deux. Ne pas optimiser yocto au détriment de BEAMFS.

## 7. Tests intégrés au harness Tir

Pour tester sous attaque : utiliser `bin/Tir-analyse-multifs.sh` ou
les autres `Tir-*.sh` du repo `yocto-beamfs/bin/`. Ne PAS écrire de
scripts de test ad-hoc parallèles. Si un nouveau test est nécessaire,
l'ajouter au harness Tir, pas à côté.

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
8. `Tir-analyse-multifs.sh` complet, comparaison aux verdicts du run
   de référence — BEAMFS doit avoir verdicts identiques
9. `hpc-benchmark-beamfs.sh` dans tolérance ±20% du baseline
10. dmesg post-test : 0 BUG, 0 oops, 0 WARN

Si UNE SEULE étape échoue : rollback avec le backup, diagnostic,
pas de commit.

## 9. Lockstep beamfs ↔ yocto-beamfs

Les deux copies de `file_inline.c` doivent être byte-identiques :
- `~/git/beamfs/file_inline.c`
- `~/git/yocto-beamfs/recipes-kernel/beamfs/files/beamfs-0.1.0/file_inline.c`

Vérifier par sha256 AVANT et APRÈS toute modification. Toute
divergence est un signal d'arrêt — ne pas patcher dessus, diagnostiquer.

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

**Fin du contrat de recadrage. Lecture obligatoire en début de session.**
