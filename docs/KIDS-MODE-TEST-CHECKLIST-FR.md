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
- Les dossiers `Movies`, `Music`, `Cartoons`, `Series` et `Stories` apparaissent s'ils
  contiennent un média directement ou dans un sous-dossier.
- A ouvre plusieurs niveaux de dossiers et B remonte exactement d'un niveau.
- Chaque niveau retrouve sa dernière sélection après un aller-retour et après
  un redémarrage.
- La flèche vers les jeux reste masquée jusqu'au retour à `Media/KidsMode`.
- Chaque dossier utilise son propre `Imgs` : par exemple `Movies/Imgs` et
  `Series/Ulysses 31/Imgs`.
- Sous une vignette de dossier, seul le nom du dossier est affiché, sans
  préfixe `.../`.
- L'image portant le nom d'un dossier s'applique aux vidéos sans image propre
  qu'il contient. Une image portant exactement le nom d'un épisode reste
  prioritaire.
- Sans image dans le dossier courant, la vignette remonte vers le dossier
  parent puis jusqu'à la catégorie racine (`Movies`, `Series`, etc.).
- L'héritage fonctionne aussi bien avec `Dossier/Imgs/Dossier.png` qu'avec
  `Parent/Imgs/Dossier.png`.
- Le nom de l'épisode ou du fichier reste affiché sous l'image héritée.
- Les titres des cartes noires du carrousel suivent la même règle.
- Avec une image de série ou d'épisode, le nom de l'épisode reste sous l'image.
- Le verrou **Games only / Videos only** masque la flèche verticale.
- A reprend et X redémarre après confirmation.
- En pause, MENU + X + Y crée `Imgs/Nom de la vidéo.bmp`. Une seconde capture
  remplace ce fichier et le BMP apparaît dans le carrousel à l'endroit.
- MENU + X + Y pendant la lecture ne crée aucune capture. Supprimer le BMP
  restaure l'affiche PNG/JPG ou l'image héritée du dossier.
- Les fichiers MP3, M4A, AAC, FLAC, OGG, OPUS, WAV et WMA apparaissent et se
  lancent comme les vidéos.
- Pendant un fichier audio, la pochette correcte est affichée avec une ligne
  de progression, le temps écoulé à gauche et le temps restant à droite.
- Après 30 secondes sans action, la luminosité passe au minimum. Quinze secondes
  plus tard, le rétroéclairage s'éteint sans interrompre le son ni le timer.
- Un premier appui sur A, B ou MENU rallume uniquement l'écran, sans reprendre,
  mettre en pause ni quitter le lecteur.
- Sur une vidéo, la progression apparaît en pause ou pendant une recherche,
  puis disparaît à la reprise.
- Une extinction pendant un jeu ou une vidéo reprend le contenu.
- Une extinction depuis le carrousel restaure seulement la sélection.
- Le timer est partagé entre les jeux et les vidéos.
- Pendant les cinq dernières minutes, le temps restant est affiché en rouge
  en haut à droite sur le carrousel et pendant la lecture audio/vidéo. Dans
  un jeu, vérifier l'indication native compacte de RetroArch (`5 min`,
  `4 min`...) et son changement à chaque minute.
- Depuis le menu parent du profil Main, **Switch to Guest profile** charge les
  favoris, médias, positions et sauvegardes Kids du profil Guest sans quitter
  Kids Mode. La bascule inverse restaure exactement l'environnement Main.
- Répéter Main → Guest → Main après avoir sauvegardé un jeu dans chaque profil :
  les deux sauvegardes doivent rester distinctes et le timer ne doit pas repartir.
- À zéro, **Time's up!** reste blanc et **See you next time** est affiché en
  vert.
- Malgré des appuis ou l'ouverture du PIN/menu parent, la console s'éteint
  cinq minutes après **Time's up!**. Passer le timer à `OFF` ou ajouter du
  temps avant l'échéance annule cette extinction automatique.
- SELECT + START pendant trois secondes ouvre le menu parent.
