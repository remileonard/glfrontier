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
