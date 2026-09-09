# Cycle de vie d'un bloc, et l'invariant que fsck impose

Écrit le 2026-09-09, après deux semaines passées à chercher un verrou
manquant alors que la question était structurelle. À lire avant de
toucher à l'allocation, à la troncature ou au writeback.

## L'invariant

`fsck.beamfs` pass 4 lit **le disque seul**. Pour chaque inode de la
table, il marque `i_direct[]`, puis descend `i_indirect`, `i_dindirect`
et `i_tindirect` par `walk_indirect_tree`, qui marque le bloc
d'indirection lui-même **et** ses enfants. Il compare l'ensemble obtenu
au bitmap sur disque.

    pour tout bloc B marqué occupé dans le bitmap sur disque,
    il existe un chemin depuis un inode sur disque, à travers des
    blocs d'indirection sur disque, jusqu'à B.

Trois écritures doivent donc atteindre le disque de façon cohérente :
le bitmap, l'inode, et chaque bloc d'indirection du chemin.

## L'asymétrie de publication

    beamfs_alloc_block
      clear_bit(bitmap mémoire)
      beamfs_write_bitmap_block   -> encode + mark_buffer_dirty
      sync_fs / put_super         -> sync_dirty_buffer explicite
                                     => « occupé » TOUJOURS sur disque

    lookup_or_alloc_phys
      ptrs[slot] = bloc           -> tampon mémoire, sous lock_buffer
      ind_parity_update           -> parité, hors du verrou du bloc
      mmb_mark_buffer_dirty       -> mark_buffer_dirty + liste inode
      write_inode / mmb_sync      -> disque, SI le writeback passe
                                     => « référencé » NON GARANTI

Le côté « occupé » est publié de force et immédiatement. Le côté
« référencé » dépend du writeback. `fsck` teste l'égalité des deux.
Tout écart de timing produit `used-but-unreferenced` par construction.

ext2 vit avec la même asymétrie mais ne fuit pas après un démontage
propre : ses blocs d'indirection sont dans le cache du périphérique, que
`sync_blockdev` vide en fin de `sync_filesystem`. beamfs les y a aussi.
L'asymétrie seule n'explique donc pas une fuite après démontage propre.

## Ce que la mesure dit, et qu'aucune hypothèse de writeback n'explique

Capture du 2026-09-09, generic/464, deux blocs perdus :

    2492.823012  464-5526  parent=3324 slot=26  0->21961  bh=ffff8c185c2a8a28
    2493.204954  464-5521  parent=3324 slot=26  0->37496  bh=ffff8c185c2a8a28

Même tampon, même emplacement, 380 ms d'écart, et la seconde écriture
lit l'emplacement à **zéro** là où la première a posé 21961. Aucun
`block_free`, aucune troncature, aucun `memset` entre les deux. Le
slot 9 du même bloc suit le même chemin. Les deux blocs ainsi effacés
sont exactement les deux que `fsck` déclare perdus.

Capture antérieure, même forme à l'échelle : 512 pointeurs installés
dans le bloc 1485, la troncature en relit 446 à zéro, `fsck` compte 446
blocs perdus. La coupure est temporelle et sans chevauchement — tout ce
qui est écrit avant 295,166 survit, tout ce qui l'est après disparaît.

La perte est donc **en mémoire**, avant tout démontage. Ce n'est pas un
problème de persistance.

## Ce qui a été vérifié et écarté

- `mmb_mark_buffer_dirty` appelle bien `mark_buffer_dirty` : le tampon
  ne peut pas être évincé sans écriture.
- `mmb_sync` détache puis rattache correctement ; `b_mmb` revient à NULL
  et le marquage suivant re-rattache.
- `beamfs_ind_parity_verify` décode dans un tampon de travail et ne
  touche jamais le bloc vivant.
- `iget_locked` garantit un seul `struct inode` par numéro : les deux
  écrivains partagent bien `i_alloc_mutex`.
- Le sweep est hors de cause (mesuré, curseurs figés, fuite quand même).
- Le journal est forensique, pas transactionnel : il n'enregistre ni
  allocation ni installation de pointeur.

## Ce qui reste ouvert

Deux écrivains du même inode, sérialisés par `i_alloc_mutex`, voient
tous deux le même emplacement à zéro. Soit la lecture de l'emplacement
se fait hors du mutex sur un chemin, soit le contenu du tampon est
modifié par un tiers non identifié.

`beamfs_ind_parity_update` lit `bh->b_data` **hors du verrou du bloc**
— il ne verrouille que le tampon de parité. Deux écrivains concurrents
peuvent donc lui faire calculer une parité qui ne correspond à aucun
état réel. Cela corrompt la parité, pas le bloc, mais c'est la seule
lecture non protégée identifiée à ce jour.

## Ce qui ne marche pas comme approche

Ajouter des verrous site par site. Le 2026-09-09, verrouiller les trois
sites de splice a fait passer `464` de 8/10 à 4/10 — une régression
nette. Un verrou de plus élargit les fenêtres au lieu de les fermer
quand le modèle de synchronisation n'est pas établi.
