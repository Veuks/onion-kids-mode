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
- Sous une vignette de dossier, seul le nom du dossier est affiché, sans
  préfixe `.../`.
- L'image portant le nom d'un dossier s'applique aux vidéos sans image propre
  qu'il contient. Une image portant exactement le nom d'un épisode reste
  prioritaire.
- Sans image dans le dossier courant, la vignette remonte vers le dossier
  parent puis jusqu'à la catégorie racine (`Films`, `Séries`, etc.).
- L'héritage fonctionne aussi bien avec `Dossier/Imgs/Dossier.png` qu'avec
  `Parent/Imgs/Dossier.png`.
- Le nom de l'épisode ou du fichier reste affiché sous l'image héritée.
- Les titres des cartes noires du carrousel suivent la même règle.
- Avec une image de série ou d'épisode, le nom de l'épisode reste sous l'image.
- Le verrou **Games only / Videos only** masque la flèche verticale.
- A reprend et X redémarre après confirmation.
- En pause, MENU + X crée `Imgs/Nom de la vidéo.bmp`. Une seconde capture
  remplace ce fichier et le BMP apparaît dans le carrousel à l'endroit.
- MENU + X pendant la lecture ne crée aucune capture. Supprimer le BMP
  restaure l'affiche PNG/JPG ou l'image héritée du dossier.
- Une extinction pendant un jeu ou une vidéo reprend le contenu.
- Une extinction depuis le carrousel restaure seulement la sélection.
- Le timer est partagé entre les jeux et les vidéos.
- À zéro, **Time's up!** et **See you next time** sont affichés.
- SELECT + START pendant trois secondes ouvre le menu parent.
