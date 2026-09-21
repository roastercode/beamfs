# beamfs - état courant

> Ordre de lecture : `context/00-MINDMAP.md` pour la navigation, puis
> `context/context-recadrage.md` (R0-R39) pour le contrat opérationnel,
> puis ce fichier pour l'état réel.

État au 2026-09-21 fin de journée. L'état précédent, arrêté au
2026-05-02, est conservé sous
`context/archive/STATUS-2026-05-02-avant-reprise-xfstests.md`, qui
existe sur spartian mais n'est pas versionné : `.gitignore` exclut
`context/*` sauf les cinq fichiers nommés. Un clone frais ne l'aura
pas.

Stockage canonique : `context/STATUS.md`. Jamais publié.

---

## Avertissement méthodologique, à lire avant toute mesure

Jusqu'au 2026-09-21 10h58, les volumes de test du nœud x86-01 vivaient
sur le btrfs racine en copy-on-write. Ils portaient 179996 et 142283
extents pour un gigaoctet, soit un extent pour un bloc et demi. Chaque
lecture de 4 Ko par l'invité devenait une recherche dans l'arbre
d'extents de l'hôte, et la fragmentation s'aggravait à chaque campagne
puisque les images n'étaient jamais recréées.

Conséquence : **toute durée, tout budget de test et tout blocage
mesurés avant cette date portent cette couche**. Les défauts de beamfs
eux-mêmes restent réels, mais aucune comparaison temporelle avec les
campagnes antérieures n'est valide.

Les images sont désormais sous
`/var/lib/libvirt/images/x86-nocow/`, répertoire marqué `chattr +C`.
Elles tiennent à 3 extents après trente formatages. Le XML libvirt
pointe dessus ; l'ancien est sauvegardé dans
`~/disk-backup/libvirt-xml-20260921-105833/`.

---

## Ce qui est établi par mesure

### Le scrubber est dans le circuit des pertes de blocs

Scrubber garé (`XFSTESTS_SCRUB_MS=0`) : zéro bloc perdu sur les essais
valides. Scrubber au défaut de 100 ms : 353 et 376 blocs perdus sur
deux séries de dix essais.

La variable qui sépare les essais en échec des essais réussis est
`host.blk.vdh.rd.bytes`, avec une séparation de 195 à 1535 écarts-types
sur trois séries : 1,14 Go lus sur un volume de 1 Go quand l'essai
échoue, 69 Mo quand il passe. C'est le scrubber qui balaye.

Réserve : quatre essais valides seulement avant blocage du nœud. Le
signal est fort, le nombre est faible.

Le commentaire de `matrix.rs` affirmant *with scrub suspended, leaked
at loop 10, scrub is not it* est réfuté par cette mesure.

### known-limitations 3.14 n'est pas résolu

Le blocage a été reproduit deux fois le 2026-09-21, avec images
fragmentées puis avec images propres. Le RSS de l'invité passe de 0,8
à 8,4 Go sur trente essais et n'y redescend pas. Au blocage, les vCPU
ont brûlé 467 secondes contre 21 sur un essai sain, et
`host.blk.vdh.fl.reqs` tombe de 507 à 3 : les flush ne partent plus.

§3.14 est marqué RESOLVED sur la foi de dix essais, avec la réserve
écrite *ten trials do not prove a wedge absent*. Trente essais l'ont
prouvé présent.

## Ce qui est réfuté par mesure

### L'ordonnancement des métadonnées ne ferme pas les pointeurs perdus

`CONFIG_BEAMFS_ORDERED_META=y`, trois séries de dix essais de
generic/083 : `LOST POINTER` toujours présent, taux de réussite
inchangé dans les vingt points de dispersion du test.

La question que posent le Kconfig et `Documentation/journal-design.md`
reçoit donc un non. Un journal de métadonnées reste justifié pour la
sûreté au redémarrage ; il ne fermera pas ce symptôme.

### La cause n'est pas un désordre d'écriture

`beamfs_tc_store` compare ce que le registre a enregistré à ce que
l'appelant lit **en mémoire**, avant toute écriture. Un slot relu à
zéro n'est donc pas un pointeur parti dans le mauvais ordre vers le
support : le bloc a été remis à zéro entre deux stores.

`beamfs_tc_zeroed` le nomme : *the allocator gave out a block that is
in service, and the memset is about to erase a subtree*.

Le registre treecheck est fiable : `alloc.c:784-785` appelle
`beamfs_tc_forget_child` et `beamfs_tc_forget_parent` depuis
`beamfs_free_block`. Rien ne s'y accumule au free, donc les messages
ne sont pas des artefacts.

`beamfs_alloc_block` est correct : le bit est cherché et effacé sous
`s_lock`, donc deux allocations concurrentes ne peuvent pas rendre le
même bloc. Le bloc revient par un autre chemin, qui reste à trouver.

---

## Piste active : leak-1, double attribution d'un bloc indirect

Ce qui a été lu et écarté :

- `beamfs_alloc_block` (alloc.c) : correct sous `s_lock`.
- `beamfs_free_block` (alloc.c) : purge le registre treecheck.
- `treecheck.c` en entier : `tc_forget_child`, `tc_forget_parent`,
  `tc_clear` couvrent le free et le truncate.
- `beamfs_ind_parity_verify` (indparity.c) : décode en scratch, ne
  modifie plus le bloc vérifié. Les treize commentaires d'appelants
  de `file_inline.c` qui disent *corrected in place* sont périmés.
- L'ordonnancement, réfuté ci-dessus.

Ce qui reste à lire : `beamfs_inline_lookup_or_alloc_phys_new` dans
`file_inline.c`, la fonction que toutes les piles de `LOST POINTER`
et de `ZEROED IN SERVICE` désignent. Les trois sites de `memset` sur
un bloc indirect sont aux lignes 947, 1232 et 1371, chacun précédé de
son `beamfs_tc_zeroed`. La question : par quel chemin un bloc déjà en
service revient à cette fonction comme s'il était neuf.

---

## Données disponibles pour une analyse croisée

### Les quatre campagnes du 2026-09-21

`Documentation/runs/2026-09-21-baselines/` contient les quatre
campagnes de generic/083 du jour, avec un README qui donne les
conditions exactes de chacune et ce que chaque paire établit. Les
paires 02-03 et 03-04 ne diffèrent que par une variable.

Le fichier 03 est une recopie depuis la sortie terminal : `baseline`
écrit toujours le même nom de journal, et la campagne de 14h56 a été
écrasée par celle de 16h36. À corriger dans BX.

### L'historique

105 fichiers de résultats sous `~/.local/share/beamfs-xfstests/`, dont
trois sweeps complets de 737 tests. 772 répertoires d'évidence, 2,3 Go,
sous `evidence/`.

`beamfs-xfstests trend` agrège les pertes par test. Vue figée dans
`Documentation/runs/2026-09-21-baselines/05-trend-au-2026-09-21.txt` :
generic/269 a perdu jusqu'à 103239 blocs sur 29 runs, generic/464
24813 sur 21, generic/075 10307 sur 17. generic/083, avec 196 runs,
plafonne à 397.

`beamfs-xfstests analyse <capture>` donne, pour chaque bloc perdu,
l'inode propriétaire, le parent qui porte le pointeur, et si ce
pointeur a été posé avant ou après la dernière écriture de l'inode.
C'est l'outil qui sépare les mécanismes, et il n'a pas été employé
sur les captures du jour.

### Ce qu'une analyse croisée doit prendre en compte

- Le mode de parité. `mkfs.beamfs` pose `-I rs` par défaut. Les sweeps
  de référence du 18 septembre tournaient en `-I none`. Un taux cité
  sans son mode ne veut rien dire, et §3.13 établit que `rs` détruit
  là où `none` laisse inerte.
- L'état du scrubber, qui revient à 100 ms à chaque redémarrage du
  nœud et que rien ne fixe sauf `XFSTESTS_SCRUB_MS` posé
  explicitement.
- La dispersion. generic/083 a vingt points d'écart sur code inchangé.
  Un écart inférieur ne signifie rien, et l'outil le dit lui-même.
- Seize des vingt-quatre tests du dernier sweep travaillent sur
  TEST_DEV, qui reste monté. Le fsck de capture le lit monté, donc
  leur verdict de cohérence ne vaut rien.

---

## Consigne pour la suite

Aucune nouvelle campagne sans un plan écrit qui nomme la correction
mesurée, la variable unique qui change, et le nombre d'essais que la
dispersion exige. Les campagnes du jour ont coûté quatre heures pour
un résultat négatif utile et deux comparaisons non concluantes ; ce
n'est pas reproductible à l'échelle.

L'ordre de travail, tel que la matrice de priorités le porte
(`Documentation/TODO.md` section 14) :

1. **leak-1**, phase 1 : la double attribution. Lecture d'abord,
   correction ensuite, mesure en dernier.
2. **wedge-1**, phase 1 : §3.14, le blocage mémoire, indépendant du
   stockage et du scrubber.
3. **fsck-mounted**, phase 2 : le fsck de capture sur un TEST_DEV
   monté.

---

## État de l'outillage au 2026-09-21

- `beamfs` : `e4e9b91` sur `beamfs-devel` branche `devel`.
- `yocto-beamfs` : `31d86a3`.
- `beamfs-xfstests` : 2.3.4. Nouveautés du jour : sous-commande
  `nodes status` qui lit un nœud sans rien démarrer, cinq troncatures
  silencieuses supprimées, `deploy` qui dérive le chemin du rootfs de
  `virsh domblklist` au lieu de le coder en dur.
- Nœud x86-01 : noyau 7.3.0-rc3 sans `ORDERED_META`, images sans
  copy-on-write, rootfs identique à l'image fraîche.
- `BEAMFS_ORDERED` est à "0" dans local.conf. Changer cette variable
  n'invalide pas `do_configure` : `bitbake -f -c configure
  linux-mainline` est nécessaire pour que le `.config` suive.
