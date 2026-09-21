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

## Reserve sur le fichier 03

La campagne de 14h56 a ete ecrasee dans
`~/.local/share/beamfs-xfstests/baseline-generic-083.log` par celle de
16h36 : `baseline` ecrit toujours le meme nom de fichier. Les chiffres
du fichier 03 sont recopies depuis la sortie terminal de la session,
pas depuis le fichier original. Les autres sont des copies de fichiers.

Consequence a corriger : `baseline` devrait nommer son journal par
commit et par horodatage, sans quoi deux campagnes du meme jour se
detruisent.
