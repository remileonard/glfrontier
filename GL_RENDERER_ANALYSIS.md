# Analyse du renderer OpenGL (src/gl.c)

Ce document liste les bugs et les fonctionnalités incomplètes trouvés en
analysant [src/gl.c](src/gl.c) (le renderer `WITH_GL`), en le comparant au
renderer logiciel [src/soft.c](src/soft.c) et à son intégration dans
[src/hostcall.c](src/hostcall.c) / [src/screen.h](src/screen.h).

## Contexte / architecture

- Le jeu 68k émulé appelle des "hostcalls" `Nu_Put*` / `Nu_Complex*` pour
  chaque primitive 3D de la frame courante. En mode GL, ces primitives ne
  sont **pas** dessinées immédiatement : elles sont sérialisées dans un
  buffer (`obj_data_area`) et indexées dans un arbre binaire trié par
  profondeur (`struct ZNode`, `znode_insert`), pour être dessinées plus
  tard dans l'ordre "peintre" (`draw_3dview`), du plus loin au plus proche.
- Une fois la frame 68k terminée, `Nu_DrawScreen()` parcourt l'arbre,
  dessine chaque primitive avec OpenGL fixed-function (`glBegin`/`glEnd`,
  éclairage `GL_LIGHT0`/`GL_LIGHT1`), puis compose par-dessus le bitmap 2D
  du panneau de contrôle (`draw_control_panel`, rendu comme texture).
- Trois modes : `R_OLD` (le renderer logiciel 68k d'origine, aucune des
  primitives `Nu_Put*` GL n'est utilisée), `R_GL` (solide/éclairé),
  `R_GLWIRE` (fil de fer). Basculé avec la touche `E`
  ([src/shortcut.c](src/shortcut.c#L75)).

## Bugs identifiés

### 1. Lecture hors-limites dans `draw_control_panel()` (sévérité : haute)

[src/gl.c](src/gl.c#L456-L471)

```c
unsigned int line[320];
...
for (y=0; y<200; y++) {
    ...
    for (x=0; x<320; x++) { line[x] = ...; }
    glTexSubImage2D (GL_TEXTURE_2D, 0, 0, y, 320, 2, GL_RGBA, GL_UNSIGNED_BYTE, line);
}
```

`line` ne contient que 320 pixels (une seule ligne), mais l'appel demande
une sous-image de hauteur **2** (`320, 2`). OpenGL va donc lire 320 pixels
supplémentaires juste après la fin du tableau `line` sur la pile — un
dépassement de tampon de 1280 octets, exécuté 200 fois par frame. C'est un
comportement indéfini (UB) : selon la plateforme/le compilateur, cela peut
rester "silencieux" (les lignes fantômes sont réécrites par l'itération
suivante), planter, ou faire fuiter des données de la pile (`pal`, `scr`,
`x`, `y`) dans la texture. Sur macOS/arm64 ou avec un binaire durci
(stack-protector, ASan), c'est un candidat direct à des artefacts visuels
aléatoires voire des crashs.

**Correctif** : remplacer `2` par `1`.

### 2. Fuite mémoire dans `combineCallback()` (sévérité : basse, mais fuite continue)

[src/gl.c](src/gl.c#L860)

```c
void CALLBACK combineCallback(...)
{
   GLdouble *vertex = (GLdouble *) malloc(3 * sizeof(GLdouble));
   ...
   *dataOut = vertex;
}
```

GLU appelle ce callback à chaque intersection d'arêtes pendant la
tessellation (`gluTessBeginPolygon`/`gluTessEndPolygon` dans
`Nu_DrawComplexStart`/`Nu_DrawComplexEnd`). Le pointeur alloué n'est
jamais libéré. Comme la tessellation est utilisée pour les formes
complexes (stations spatiales, etc.), sur une session longue cela peut
accumuler des allocations non libérées.

**Correctif** : conserver la liste des pointeurs alloués pendant
`gluTessBeginPolygon`/`EndPolygon` (ex. dans un buffer scratch réinitialisé
à chaque appel) et les `free()` juste après `gluTessEndPolygon` dans
`Nu_DrawComplexEnd`.

### 3. Pas de "billboard" pour les cercles/points scintillants (sévérité : moyenne)

`Nu_DrawTwinklyCircle`, `Nu_DrawCircle`, `Nu_DrawBlob`
([src/gl.c](src/gl.c#L1000), [src/gl.c](src/gl.c#L1514), [src/gl.c](src/gl.c#L1706))
dessinent un disque avec `gluDisk`, placé avec un simple `glTranslatef`
mais **sans annuler la rotation de la caméra** (pas de billboard face à la
caméra). Dans le jeu d'origine ces éléments représentent des points/halos
lumineux (étoiles scintillantes, feux de balise, etc.) censés toujours
faire face au joueur. En 3D réelle, vus presque de profil, ces disques se
réduisent à une ligne quasi invisible — un défaut visuel classique de
"rendu incomplet" (objets qui disparaissent selon l'angle de caméra).

**Correctif** : avant de dessiner le disque, annuler la rotation de la
modelview courante (billboard sphérique classique : extraire uniquement la
translation de la matrice modelview courante, ou dessiner un quad texturé
toujours face-caméra plutôt qu'un `gluDisk`).

### 4. Aspect ratio 3D figé, indépendant de la résolution réelle (sévérité : moyenne)

[src/gl.c](src/gl.c#L121)

```c
gluPerspective (36.5f, 1.9f, 1.0f, 10000000000.0f);
```

L'aspect ratio de la projection est une constante (`1.9`), correspondant à
320×168. Mais `set_main_viewport()` utilise le vrai `screen_w`/`screen_h`
(configurable via `--size`, voir [src/main.c](src/main.c#L204-L210)). Si
la fenêtre n'a pas exactement ce ratio, la scène 3D sera étirée/écrasée
horizontalement ou verticalement par rapport au panneau de contrôle 2D
(qui lui reste toujours en 320×200 logique).

**Correctif** : recalculer l'aspect ratio à partir de
`screen_w`/`(screen_h - ctrl_h)` à chaque appel de `change_vidmode()` (et
idéalement à chaque redimensionnement, si supporté un jour).

### 5. Depth test jamais réactivé (sévérité : basse à moyenne, dépend des scènes)

[src/gl.c](src/gl.c#L133-L138) désactive `GL_DEPTH_TEST` à l'initialisation
et ce n'est réactivé nulle part ailleurs dans le fichier. Le tri de
profondeur repose donc entièrement sur l'arbre binaire `ZNode` construit à
partir de la valeur `D4` fournie par le 68k (ordre peintre). Cela
fonctionne pour des primitives simples bien ordonnées, mais ne gère pas
les cas d'auto-intersection à l'intérieur d'un même noeud (ex. la sphère
de planète en `Nu_DrawPlanet`, qui est un objet plein dessiné en un seul
noeud znode: ses faces arrière/avant ne sont pas z-testées entre elles, on
compte uniquement sur `glCullFace(GL_BACK)`/`GL_CULL_FACE` pour ça).

**Remarque** : ce n'est pas forcément un bug à corriger aveuglément (activer
le depth test partout casserait le compositing 2D/texture actuel), mais
c'est une limite d'architecture à garder en tête si des artefacts de
superposition apparaissent (ex. vaisseaux/anneaux qui se chevauchent mal).

## Fonctionnalités explicitement incomplètes (annotées par l'auteur d'origine)

### 6. `Nu_PutPlanet` / `Nu_DrawPlanet` — "not finished by a long shot"

[src/gl.c](src/gl.c#L1427)

Les planètes sont approximées par une icosphere subdivisée
(`nuSphere`/`nuSubdivide`, profondeur fixe `NUSPHERE_SUBDIVS=4`), avec un
éclairage `GL_LIGHT1` mais **aucune texture de surface** (pas de bandes
nuageuses, pas de relief, pas de texture planète du tout). Le facteur
`size*1.0080` ("why the fucking fudge factor??") suggère aussi un ajustement
empirique jamais vraiment résolu.

### 7. `Nu_PutOval` / `Nu_DrawOval` — "this primitive is WRONG"

[src/gl.c](src/gl.c#L1611-L1642)

Le code lit bien 3 angles (`d`, `e`, `f`) depuis le 68k mais les 3 lignes
`glRotatef` correspondantes sont commentées :

```c
//glRotatef (RAD_2_DEG*M_PI*(d/32768.0f), 0.0f, 1.0f, 0.0f);
//glRotatef (RAD_2_DEG*M_PI*(e/32768.0f), 1.0f, 0.0f, 0.0f);
//glRotatef (-RAD_2_DEG*M_PI*(f/65536.0f), 0.0f, 1.0f, 0.0f);
```

Résultat : l'ovale est toujours dessiné comme un disque plat non orienté
(orientation d'origine ignorée). Utilisé probablement pour les anneaux
planétaires ou des effets similaires — visuellement, ils apparaîtront à
plat quelle que soit l'inclinaison prévue par le jeu.

**Piste de correctif** : réactiver les `glRotatef`, mais il faudra vérifier
le bon ordre des rotations et le facteur d'échelle des angles (le
commentaire "WRONG" suggère que la tentative précédente donnait un
résultat visuellement incorrect, pas juste "non implémenté").

### 8. `Nu_PutTeardrop` — "a bit crap ... as you will see by panning around"

[src/gl.c](src/gl.c#L1095)

Utilisé pour les flammes/réacteurs (moteurs, cheminées d'industrie). La
forme est un ruban de Bézier construit dans le plan perpendiculaire à la
direction du jet (`ppd`), mais sans billboard face-caméra : en tournant
autour de l'objet, l'épaisseur du "ruban" varie de façon irréaliste
(visible de plein fouet sous un angle, quasi invisible sous un autre) — le
même problème de fond que le point 3 (pas de billboard), mais assumé
explicitement comme non résolu par l'auteur d'origine.

## Petits points de nettoyage (sans impact visuel confirmé)

- `Nu_DrawCircle` et `Nu_DrawTwinklyCircle` calculent `isize` (un
  swap 16 bits de `dreg2`) mais ne l'utilisent jamais — code mort qui
  laisse penser qu'un calcul de taille/échelle est resté inachevé.
- L'allocation initiale de `screen_tex` utilise `GL_INT` comme type de
  pixel ([src/gl.c](src/gl.c#L127)) alors que toutes les mises à jour
  suivantes (`glTexSubImage2D`) utilisent `GL_UNSIGNED_BYTE`. Sans
  conséquence pratique ici (les données initiales sont `NULL`), mais
  incohérent et à corriger par souci de clarté.

## Recommandations, par priorité

1. **Corriger le dépassement de tampon dans `draw_control_panel()`**
   (hauteur `2` → `1`) — c'est un bug mémoire réel, pas juste cosmétique.
2. **Ajouter le billboard face-caméra** pour les disques/points scintillants
   et le teardrop — impact visuel direct et régulier en jeu (le plus
   probable candidat pour "le rendu est incomplet").
3. **Corriger l'aspect ratio 3D** pour qu'il suive la résolution réelle de
   la fenêtre.
4. **Réactiver/corriger la rotation de `Nu_DrawOval`**.
5. Fuite mémoire de `combineCallback` — à corriger si des sessions de jeu
   longues sont un cas d'usage visé.
6. Ajout de texture(s) planète pour `Nu_DrawPlanet` — gros morceau,
   optionnel/esthétique.

Dis-moi lesquels tu veux que je corrige en premier (je recommande de
commencer par le point 1, qui est un vrai bug mémoire, puis le point 2 qui
devrait avoir l'impact visuel le plus visible).
