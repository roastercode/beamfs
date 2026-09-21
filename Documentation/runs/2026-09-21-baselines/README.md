# generic/083, quatre campagnes du 2026-09-21

Format de chaque ligne : `<commit> <test> <pass> <fail> <hang> <blocs perdus>`.

Une campagne est trois series de dix essais, meme graine, meme charge.
La dispersion de ce test sur code inchange est de vingt points de taux
de reussite : un ecart inferieur ne signifie rien. Le nombre de blocs
perdus n'a pas de dispersion caracterisee.

| Fichier | Commit | Images | Scrubber | Code |
|---------|--------|--------|----------|------|
| 01 | 80e9826 | btrfs COW, 180000 extents | gare (0 ms) | sans patch |
| 02 | 80e9826 | NoCOW, 3 extents | 100 ms | sans patch |
| 03 | 80e9826 | NoCOW, 3 extents | 100 ms | patch scrub, non commite |
| 04 | 1f6cf7a | NoCOW, 3 extents | 100 ms | patch scrub + ORDERED_META |

## Ce que chaque paire etablit

**01 contre 02** : deux variables changent, le stockage et le
scrubber. Non comparable. 01 est la seule campagne a scrubber gare et
la seule sans aucun bloc perdu.

**02 contre 03** : une seule variable, le verrou de
`beamfs_scrub_walk_level`. Taux 60/40/80 contre 60/80/80, dans la
dispersion. Pertes maximales 260 et 172 contre 33 et 397. `LOST
POINTER` present des deux cotes. Aucun effet etabli.

**03 contre 04** : une seule variable,
`CONFIG_BEAMFS_ORDERED_META=y`. Taux 60/80/80 contre 90/70/80, dans
la dispersion. `LOST POINTER` present des deux cotes. L'ordonnancement
des metadonnees ne ferme pas les pointeurs perdus.

## Ce que valent ces quatre fichiers

Etabli le soir meme en relisant `bench.log`, que `beamfs-xfstests`
remplit a chaque serie et qui garde le brut : le chemin `baseline` de
l'outil, jusqu'en 2.3.4, reconstruisait sa ligne depuis les compteurs
du run et perdait au passage toute perte nulle, l'ordre des essais, la
colonne des blocages (un `0` litteral) et le commit propre a chaque
serie. Les fichiers 01, 02 et 04, copies de `baseline-generic-083.log`,
portent cette degradation. Le fichier 03, recopie depuis le terminal
qui affiche le brut, est le seul fidele. La reserve initiale de ce
README etait donc a l'envers.

Le brut des quatre campagnes est dans `bench.log`, lignes 22 a 34 au
2026-09-21. Deux fichiers `*-ancien-formateur.log` conservent ce que
l'ancien code ecrivait, pour montrer l'ecart.

`bench.log` porte aussi, entre 02 et 03, une cinquieme serie sous
`80e9826` qui n'est archivee nulle part : `4 5 0 0,0,0,0,0`, neuf
essais, cinq echecs, zero bloc perdu partout. Sans colonne de
condition, elle n'est attribuable a aucune configuration.

Corrige dans `beamfs-xfstests` 2.3.5 (commit 500bac6) : la ligne est
ecrite depuis le run lui-meme, identique a celle de `bench.log`.
Verifie sur trois series de generic/083 a `0eb0e8f`.
