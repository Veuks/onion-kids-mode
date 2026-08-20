# Test matériel — Kids Mode

Installer le dossier compilé `KidsMode` provenant de l'artefact vert
**Kids-Mode-build** dans le dossier `App` de la carte SD.

Pour une installation neuve, sauvegarder puis supprimer l'ancien
`Saves/kidmode`. Conserver `Saves/KidsProfile` et les profils Onion. Déplacer
manuellement les vidéos et dossiers dans `Media/KidsMode`.

## Vérifications

- L'application apparaît sous le nom **Kids Mode**.
- GAUCHE/DROITE parcourt la section affichée.
- HAUT passe des jeux aux vidéos et BAS revient aux jeux.
- La flèche du bas reste au-dessus des commandes PLAY et RESTART.
- Les dossiers `Films`, `Séries` et `Chansons` apparaissent s'ils contiennent
  une vidéo directement ou dans un sous-dossier.
- A ouvre plusieurs niveaux de dossiers et B remonte exactement d'un niveau.
- Chaque niveau retrouve sa dernière sélection après un aller-retour et après
  un redémarrage.
- La flèche vers les jeux reste masquée jusqu'au retour à `Media/KidsMode`.
- Chaque dossier utilise son propre `Imgs` : par exemple `Films/Imgs` et
  `Séries/Ulysse 31/Imgs`.
- Un dossier vidéo affiche `...` sans image, ou `.../Nom du dossier` avec une
  image portant son nom dans son propre `Imgs`.
- L'image portant le nom d'un dossier ne s'applique pas aux vidéos qu'il
  contient. Un épisode sans image portant exactement son nom conserve sa carte
  noire et son titre.
- Dans une série sans image, son nom est sous la carte noire et le nom de
  l'épisode est centré dedans, sur six lignes au maximum avant réduction de la
  police.
- Les titres des cartes noires du carrousel suivent la même règle.
- Avec une image de série ou d'épisode, le nom de l'épisode reste sous l'image.
- Le verrou **Games only / Videos only** masque la flèche verticale.
- A reprend et X redémarre après confirmation.
- Une extinction pendant un jeu ou une vidéo reprend le contenu.
- Une extinction depuis le carrousel restaure seulement la sélection.
- Le timer est partagé entre les jeux et les vidéos.
- À zéro, **Time's up!** et **See you next time** sont affichés.
- SELECT + START pendant trois secondes ouvre le menu parent.
