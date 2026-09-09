# beamfs — arbre algorithmique du cycle de vie d'un bloc

État au commit 22da463. Écrit après deux semaines passées à corriger des
sites isolés sans modèle d'ensemble, et deux correctifs consécutifs qui
ont dégradé `generic/464` au lieu de l'améliorer. Le but de ce document
est qu'aucun correctif ne soit plus proposé sans être confronté à cet
arbre.

Rien ici n'est supposé : chaque affirmation renvoie à une ligne de code
ou à une mesure. Ce qui n'est pas établi est marqué comme tel.

---

## 0. Deux implémentations parallèles

Le code contient **deux** chemins d'allocation complets, de structure
identique (direct, indirect, double, triple) :

| Fichier | `a_ops` | Actif quand |
|---|---|---|
| `file.c` | `beamfs_aops` | `s_scheme != UNIVERSAL_INLINE` |
| `file_inline.c` | `beamfs_inline_aops` | `s_scheme == UNIVERSAL_INLINE` |

Le choix est fait en deux endroits : `inode.c:262` (à `iget`) et
`namei.c:665` (à la création). Les volumes de test sont formatés en
`scheme=2 UNIVERSAL_INLINE`, donc **`file_inline.c` est le chemin actif**
et `file.c` est mort pour `464`.

*Conséquence pratique* : un correctif posé dans `file.c` n'a aucun effet
sur `464`, et une lecture de `file.c` ne dit rien du comportement
observé. Vérifier le chemin avant de patcher.

---

## 1. Les états d'un bloc

    LIBRE
      │  beamfs_alloc_block               alloc.c:542
      ▼
    ALLOUE  (bitmap sur disque dit « occupé », rien ne le référence)
      │  ptrs[slot] = blk                 file_inline.c, 6 sites
      ▼
    POINTE-EN-MEMOIRE  (le tampon parent porte le pointeur, marqué sale)
      │  write_inode → mmb_sync           namei.c:1191
      │  ou sync_blockdev au démontage
      ▼
    POINTE-SUR-DISQUE  (fsck le trouve)
      │  troncature / unlink
      ▼
    LIBRE

L'état **ALLOUE** est la fenêtre dangereuse : le bitmap le dit occupé sur
le disque, et rien ne le référence encore. Toute interruption dans cette
fenêtre produit `used-but-unreferenced`.

---

## 2. L'invariant que fsck impose

`fsck.beamfs` pass 4 (`tools/fsck.beamfs/fsck.beamfs.c:704`) lit **le
disque seul**. Pour chaque inode de la table : `i_direct[]`, puis
`walk_indirect_tree` sur `i_indirect`, `i_dindirect`, `i_tindirect`. Ce
parcours marque le bloc d'indirection **et** ses enfants. Le résultat est
comparé au bitmap sur disque.

    pour tout bloc B marqué occupé dans le bitmap sur disque,
    il existe un chemin depuis un inode sur disque, à travers des blocs
    d'indirection sur disque, jusqu'à B.

Trois écritures doivent atteindre le disque de façon cohérente : le
bitmap, l'inode, chaque bloc d'indirection du chemin.

---

## 3. L'asymétrie de publication

`beamfs_alloc_block` appelle `beamfs_write_bitmap_block` **hors du
verrou**, immédiatement après avoir pris le bit (alloc.c:66). Le bitmap
est en outre synchronisé explicitement dans `sync_fs` et `put_super`.

    « occupé »    → publié de force, immédiatement, toujours
    « référencé » → dépend du writeback

`fsck` teste l'égalité des deux ensembles. **L'ordre de publication est
donc inversé par rapport à l'invariant.**

ext2 vit avec la même asymétrie sans fuir après un démontage propre :
ses blocs d'indirection sont dans le cache du périphérique, que
`sync_blockdev` vide en fin de `sync_filesystem`. beamfs les y a aussi.
L'asymétrie seule n'explique donc pas une fuite après démontage propre —
elle explique une fuite après plantage.

---

## 4. Le tampon de bitmap sur une liste par inode

`beamfs_write_bitmap_block` (alloc.c:75) :

    mmb_mark_buffer_dirty(bh, &BEAMFS_I(owner)->i_metadata_bhs);

Le bitmap est une structure **globale**, partagée par tous les inodes du
volume. Elle est ici rattachée à la liste de métadonnées de **l'inode qui
a déclenché l'allocation**.

`mmb_mark_buffer_dirty` ne rattache que si `!bh->b_mmb`, donc le tampon
reste sur la liste du premier inode qui l'a touché. Quand cet inode est
évincé, `beamfs_evict_inode` (super.c:443) appelle `mmb_invalidate`, qui
détache tout — y compris ce tampon de bitmap partagé.

*Ce qui est établi* : le détachement a lieu. `mmb_invalidate` fait
`__remove_assoc_queue` sur chaque tampon, ce qui met `b_mmb` à NULL et
retire de la liste, **sans effacer le drapeau sale**.

*Ce qui n'est pas établi* : que cela cause une perte. Le bitmap est aussi
tenu par `sbi->s_bitmap_blkhs[]` et synchronisé explicitement à
`put_super`, donc il atteint le disque par un autre chemin. À vérifier
avant toute conclusion.

---

## 5. Le cycle de vie du tampon d'un bloc libéré

`beamfs_free_block` (alloc.c:672) remet le bit, met à jour le compteur,
écrit le bloc de bitmap. **Il ne touche pas au tampon du bloc libéré.**

Le `buffer_head` reste donc dans le cache du périphérique, possiblement
sale, et — s'il s'agissait d'un bloc d'indirection — toujours rattaché à
`i_metadata_bhs` de son ancien propriétaire.

Réattribué, `sb_getblk` rend **le même `buffer_head`**. Le nouveau
propriétaire le met à zéro et y installe ses pointeurs.

ext2 fait `bforget(bh)` avant `ext2_free_blocks` (ext2/inode.c:543 et
1165). `__bforget` fait trois choses : `clear_buffer_dirty`,
`remove_assoc_queue`, `__brelse`.

*Mesure* : ajouter `bforget` aux deux sites de libération de beamfs
(commit 726fa22) a fait passer `464` de 8/10 à **3/10**. Le correctif a
été annulé. Pourquoi le comportement actuel — tampon relâché sans être
oublié — donne un meilleur taux **n'est pas expliqué**.

---

## 6. Les six sites qui écrivent un pointeur

Dans `file_inline.c`, tous sous `lock_buffer` au commit 22da463 :

| Ligne | Écrit | Dans |
|---|---|---|
| 814 | pointeur de donnée | indirect simple |
| 984 | splice L1 | indirect |
| 1103 | pointeur de donnée | L2 sous dindirect |
| 1272 | splice L1 | dindirect |
| 1363 | splice L2 | L1 |
| 1482 | pointeur de donnée | L3 sous tindirect |

Chaque écriture est suivie de `beamfs_ind_parity_update`, appelé
**hors du verrou du bloc** — il ne verrouille que le tampon de parité et
lit `bh->b_data` sans protection (indparity.c:4, 23).

*Ce qui est établi* : cette lecture n'est pas protégée.
*Ce qui n'est pas établi* : qu'elle cause une perte de pointeur. Elle
peut produire une parité incohérente, pas effacer le bloc.

*Mesure* : verrouiller les trois sites de splice (commit 8f4c03d) a fait
passer `464` de 8/10 à 4/10. Annulé.

---

## 7. Les trois appelants de l'allocateur

`beamfs_inline_lookup_or_alloc_phys` est appelé depuis :

| Appelant | Ligne | Verrou |
|---|---|---|
| `iomap_begin` | 2134 | `i_alloc_mutex` autour de l'appel |
| `writeback_range` | 2605 | idem |
| `zero_tail_block` | 3444 | idem |

Les trois prennent et relâchent `i_alloc_mutex` autour de l'appel
complet, qui contient la lecture de l'emplacement **et** l'installation
du pointeur. La sérialisation par inode est donc correcte.

`iget_locked` (inode.c:58) garantit un seul `struct inode` par numéro :
les trois chemins partagent bien le même mutex.

---

## 8. Ce que la mesure montre et qu'aucun point ci-dessus n'explique

Capture `464`, deux blocs perdus :

    2492.823012  464-5526  parent=3324 slot=26  0->21961  bh=ffff8c185c2a8a28
    2493.204954  464-5521  parent=3324 slot=26  0->37496  bh=ffff8c185c2a8a28

Même tampon, même emplacement, 380 ms, et la seconde écriture lit
l'emplacement à zéro là où la première a posé 21961. Aucun `block_free`,
aucune troncature, aucun `memset` tracé entre les deux. Le slot 9 du même
bloc suit le même chemin. Les deux blocs ainsi effacés sont exactement
les deux que `fsck` déclare perdus.

Capture antérieure, même forme à l'échelle : 512 pointeurs dans le bloc
1485, la troncature en relit 446 à zéro, `fsck` compte 446 perdus. La
coupure est temporelle et sans chevauchement — écrit avant 295,166 :
conservé ; après 295,169 : perdu.

**Deux écrivains du même inode, sérialisés par le même mutex, voient tous
deux le même emplacement à zéro.** Aucune explication testée n'en rend
compte.

---

## 9. Mécanismes vérifiés et écartés

| Mécanisme | Vérification | Verdict |
|---|---|---|
| Tampon sale évincé sans écriture | `mmb_mark_buffer_dirty` appelle `mark_buffer_dirty` (buffer.c:719) | écarté |
| Rattachement perdu après vidage | `mmb_sync` remet `b_mmb` si encore sale (buffer.c:59-61) | écarté |
| `verify` corrige le bloc vivant | décode dans `beamfs_scratch_get`, ne touche jamais `b_data` | écarté |
| Deux inodes pour un numéro | `iget_locked` | écarté |
| Sweep concurrent | mesuré, curseurs figés, fuite quand même | écarté |
| Journal transactionnel absent | le journal est forensique (64 entrées RS), pas transactionnel | confirmé, mais ext2 n'en a pas non plus |
| Second superbloc beamfs | mesuré : un second **ext2** suffit aussi | ce n'est pas beamfs-spécifique |

---

## 10. Conditions mesurées de la fuite

| Condition | Résultat |
|---|---|
| scratch seul, 51 boucles | aucune fuite |
| scratch + second beamfs | fuite boucle 5 |
| idem, sweep suspendu | fuite boucle 10 |
| scratch + second ext2 | fuite boucle 2 |
| `464` complet | 8 succès / 10 |

La fuite exige **un second système de fichiers monté**, de n'importe quel
type. Ce que cela change : pression mémoire, éviction d'inodes, un
flusher à servir deux superblocs.

---

## 11. Taux mesurés par commit

| Commit | Contenu | `464` |
|---|---|---|
| 0995c00 | référence | 8/10 |
| f7fdad4 | + 9 sites rattachés à `i_metadata_bhs` | **jamais mesuré seul** |
| 8f4c03d | + verrou sur les splices | 4/10 |
| 726fa22 | + `bforget` | 3/10 |
| 22da463 | reverts de 8f4c03d et 726fa22 | à mesurer |

**`f7fdad4` reste dans l'arbre et n'a jamais été mesuré isolément.** La
chute de 8/10 à 4/10 peut lui être due autant qu'à `8f4c03d`. C'est la
première chose à établir.

---

## 12. Ce qui reste ouvert, par ordre

1. Mesurer `22da463` (= 0995c00 + f7fdad4). Si le taux est à 4/10, la
   régression vient de `f7fdad4` et non du verrou de splice.
2. Expliquer pourquoi deux correctifs qui rendent le code plus correct au
   regard d'ext2 dégradent le taux. Tant que ce n'est pas compris,
   aucun correctif de cette famille n'est justifiable.
3. Instrumenter `mmb_invalidate` et l'éviction : ce sont les seuls
   chemins qui touchent un tampon sans être tracés.
4. Décider de l'ordre de publication bitmap/référence. C'est un choix de
   conception, pas un correctif ponctuel.

---

## 13. Règles de travail sur cet arbre

- Vérifier le chemin actif (`file_inline.c`, pas `file.c`) avant tout
  patch.
- Mesurer avant et après, dix essais minimum, avec `beamfs-xfstests
  bench` qui compare automatiquement.
- Un écart de un ou deux sur dix est dans le bruit ; dix essais séparent
  30 % de 80 %, pas 70 % de 80 %.
- Un essai avorté (« aborting ») n'est pas un échec du système de
  fichiers.
- Ne rien affirmer sans avoir vérifié.
