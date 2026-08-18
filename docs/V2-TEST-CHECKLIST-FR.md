# Test matériel — Kids Mode V2

Ce document accompagne le premier bundle V2. Le ZIP source sert à lancer la
compilation GitHub. Sur la carte SD, installer uniquement le dossier
`KidsModeV2` provenant de l'artefact vert **KidsMode-V2-build** de GitHub.

## 1. Démarrage

- Lancer **Kids Mode V2** depuis Apps.
- Choisir un timer court pour le test.
- Vérifier que l'écran du carrousel apparaît rapidement après le timer.

## 2. Deux étages

- GAMES est affiché en bas ; appuyer sur HAUT pour ouvrir VIDEOS.
- VIDEOS est affiché en haut ; appuyer sur BAS pour revenir à GAMES.
- Seuls l'affiche et le titre doivent glisser verticalement.
- Les commandes du bas, le timer et les flèches doivent rester immobiles.
- GAUCHE/DROITE doit parcourir uniquement l'étage visible.

## 3. Jeux et vidéos

- Sur GAMES : A reprend/lance le jeu ; X demande confirmation avant de
  recommencer le jeu.
- Sur VIDEOS : A lit/reprend ; X demande confirmation avant de recommencer.
- Dans un dossier de série : A ouvre/lit et B revient au carrousel principal.
- En lecture vidéo, vérifier le son, l'orientation et les commandes de la V1.

## 4. Menu parent et verrouillage

- Maintenir SELECT+START pendant trois secondes, puis saisir le PIN.
- Activer **Lock current floor** avec l'interrupteur ON vert.
- Revenir au carrousel : HAUT/BAS ne doivent plus changer d'étage et les
  flèches verticales ne doivent plus être visibles.
- Retourner au menu parent et remettre l'option sur OFF.

## 5. Reprise réelle

Faire les essais séparément :

- éteindre sur GAMES : redémarrage sur GAMES et sur la même sélection ;
- éteindre sur VIDEOS : redémarrage sur VIDEOS et sur la même sélection ;
- éteindre pendant un jeu : reprise du jeu ;
- éteindre pendant une vidéo : reprise de la vidéo et de sa position ;
- revenir au carrousel avant d'éteindre : ne pas relancer automatiquement le
  dernier jeu ou film.

## 6. Timer

- Vérifier que le même temps restant est visible sur GAMES et VIDEOS.
- À zéro, le jeu ou la vidéo doit s'arrêter et afficher **Time's up!**.
- Ajouter du temps depuis le menu parent et vérifier que le carrousel redevient
  accessible.
