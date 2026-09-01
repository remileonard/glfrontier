# Captures d'écran de debug (temporaires)

Ce dossier contient des captures d'écran temporaires générées lors des sessions
de debug du rendu OpenGL des planètes (voir issue #1). Elles sont committées ici
uniquement pour permettre leur consultation via l'interface GitHub, car les
images affichées dans l'outil interne de l'agent ne sont pas visibles dans le
chat.

Ce dossier et son contenu doivent être supprimés une fois les captures
consultées/validées ; il ne s'agit pas d'un artefact destiné à rester dans
l'historique du projet.

## Paires OpenGL / logiciel (même frame)

`06_planet_opengl_frame5870.png` et `07_planet_software_frame5871.png` sont
une paire de captures quasi-synchronisées : la première est prise en mode
`R_GL` au frame 5870 (juste après le rendu d'une planète avec 21 features),
puis le raccourci Ctrl+E (`Screen_ToggleRenderer`) est envoyé pour basculer
vers `R_OLD`, et la seconde capture est prise immédiatement au frame suivant
(5871). Un décalage d'une frame subsiste car le renderer ne peut être basculé
qu'entre deux frames complètes, mais la scène est identique à l'oeil.

Cette session a aussi révélé une deuxième valeur de type de feature dans les
logs de debug (`type_d7=12`, en plus de `type_d7=4` déjà connu), observée sur
2010 appels à `Nu_PutPlanetFeatureStart` contre 6638 pour le type 4 — piste à
approfondir pour le décodage des features planétaires ("cercles" éventuels).

## Scène d'intro, frame 2015 : comparaison invalidée

Une tentative précédente de comparaison `R_GL`/`R_OLD` à la frame 2015,
capturée sur deux lancements séparés du jeu (un par mode), a été retirée :
les deux images ne montraient en réalité pas la même scène (un vaisseau avec
ses réacteurs côté `R_GL`, la planète attendue côté `R_OLD`). Le numéro de
frame atteint dans les logs de debug n'est donc pas suffisant pour garantir
que deux processus distincts sont dans le même état de jeu au même instant
(la séquence d'intro ne semble pas parfaitement déterministe d'un lancement
à l'autre). Toute comparaison future devrait, comme pour la paire
`06`/`07`, basculer de renderer via Ctrl+E **au sein d'un seul et même
processus/run** plutôt que recouper deux lancements séparés par numéro de
frame.

## Halo d'atmosphère + continent vert, scène d'intro frame ~2286-2291

`08_planet_opengl_frame2287.png` (R_GL, frame 2287) et
`09_planet_software_frame2291.png` (R_OLD, frame 2291) sont une paire
capturée dans un seul run continu (bascule Ctrl+E à la volée, comme pour
la paire `06`/`07`) ; le décalage de quelques frames vient du fait que le
premier frame après la bascule reste noir le temps que le renderer logiciel
reconstruise son propre buffer. Composition de scène identique (mêmes
nuages, mêmes étoiles) aux quelques frames de décalage près.

Cette paire documente la correction de deux manques identifiés dans le
rendu OpenGL des planètes par rapport au renderer logiciel d'origine :

1. **Halo d'atmosphère** : fe2.s calcule, dans `L3da2e_AtmosphereColNShit`,
   une vraie couleur d'atmosphère par frame (rampe `L60f6_light_tint_table`
   indexée par l'angle soleil/caméra, stockée dans `204(a3)`) - déjà
   utilisée pour teinter le fond d'écran quand la planète est proche/grande,
   mais jamais exposée pour dessiner l'anneau lumineux au limbe de la
   planète elle-même. Un nouvel hcall `Nu_PutPlanetAtmosphere` capture
   cette vraie couleur ST (jamais inventée côté GL) ; `Nu_DrawPlanet`
   dessine désormais une sphère légèrement plus grande (`draw_planet_halo`,
   x1.025) de cette couleur juste avant la sphère opaque de la planète -
   comme il n'y a pas de depth-test (rendu peintre, voir `draw_3dview`),
   seul un fin anneau reste visible à la silhouette, imitant l'effet du
   renderer logiciel sans inventer de géométrie 2D nouvelle.
2. **Remplissage des continents** : fe2.s associe à chaque chaîne de
   points de contour un octet de "type" réel (lu dans `Nu_PutPlanetFeatureStart`,
   valeurs observées 4 et 12, qui ne diffèrent que par le bit 3) - c'est ce
   même octet qui sert de masque XOR pour les "colour flips" du rasteriseur
   de spans d'origine (`L3ddc0`/`l3e02e`). `draw_planet_features()` capture
   maintenant ce type réel (nouveau champ dans le znode
   `NU_PLANETFEATURESTART`) et l'utilise pour distinguer les contours de
   type "terre" (bit 3 à 0, valeur 4 dans les données observées à la frame
   2286) des contours de type "mer/fond" (bit 3 à 1, valeur 12) : les
   premiers sont remplis d'un vert plausible échantillonné sur une vraie
   capture R_OLD, les seconds restent non remplis pour laisser transparaître
   la couleur de base (océan) de la sphère, exactement comme le fait le
   renderer logiciel.

Limite connue et documentée (non résolue dans cette session) : la couleur
RGB exacte du remplissage terre/mer dans le renderer logiciel d'origine est
en réalité produite par l'interpréteur de primitives 2D historique
(`Call_FillLine` etc., piloté par `212(a3)`), qui n'est jamais exécuté en
mode GL (voir le grand commentaire au-dessus de `draw_3dview`) ; la couleur
verte utilisée ici est donc une valeur plausible échantillonnée sur une
vraie capture d'écran R_OLD plutôt qu'une valeur relue en direct depuis le
jeu à l'exécution. De même, la couleur du halo (capturée en direct via
`Nu_PutPlanetAtmosphere`) ne correspond pas forcément à la teinte exacte de
l'anneau du limbe telle que dessinée par l'interpréteur 2D d'origine (qui
utilise sa propre palette indexée), mais elle provient bien d'un calcul
réel du jeu (rampe d'atmosphère), jamais d'une valeur inventée côté GL.
