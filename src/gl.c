/*
  Hatari - screen.c

  This file is distributed under the GNU Public License, version 2 or at your
  option any later version. Read the file gpl.txt for details.
*/

#include <SDL.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#include <OpenGL/glu.h>
/* macOS's glu.h doesn't provide the "_GLUfuncptr" callback typedef that
 * Linux/Mesa's GL/glu.h does; declare a compatible one here. */
typedef void (*_GLUfuncptr)();
#else
#include <GL/gl.h>
#include <GL/glu.h>
#endif

#include "main.h"
#include "../m68000.h"
#include "screen.h"

unsigned long VideoBase;                        /* Base address in ST Ram for screen(read on each VBL) */
unsigned char *VideoRaster;                      /* Pointer to Video raster, after VideoBase in PC address space. Use to copy data on HBL */

int len_main_palette;
unsigned short MainPalette[256];
unsigned short CtrlPalette[16];
int fe2_bgcol;
int in_atmosphere;

unsigned int MainRGBPalette[256];
unsigned int CtrlRGBPalette[16];

unsigned long logscreen, logscreen2, physcreen, physcreen2;

/* ---- TEMPORARY DEBUG INSTRUMENTATION --------------------------------
 * Continuation of the investigation in src/hostcall.c (SKY_DEBUG): the
 * hostcall.c logs proved that Nu_Put* hostcalls do still fire in GL mode,
 * so the question now is what colour data quads/triangles (candidates
 * for drawing ground/terrain) actually carry when flying in atmosphere.
 * Prints one summary line every ~30 frames. Safe to remove: delete this
 * block plus the PRIM_DEBUG_HIT() call sites in Nu_DrawQuad/Nu_DrawTriangle
 * and the prim_debug_frame_end() call in Nu_DrawScreen. */
#define PRIM_DEBUG 1

#if PRIM_DEBUG
struct prim_debug_stats {
	int quad_count, tri_count;
	int quad_r_min, quad_r_max, quad_g_min, quad_g_max, quad_b_min, quad_b_max;
	int tri_r_min, tri_r_max, tri_g_min, tri_g_max, tri_b_min, tri_b_max;
};
static struct prim_debug_stats prim_stats = {
	0, 0, 999, -1, 999, -1, 999, -1, 999, -1, 999, -1, 999, -1
};

#define PRIM_DEBUG_HIT(kind, r, g, b) do { \
	prim_stats.kind##_count++; \
	if ((r) < prim_stats.kind##_r_min) prim_stats.kind##_r_min = (r); \
	if ((r) > prim_stats.kind##_r_max) prim_stats.kind##_r_max = (r); \
	if ((g) < prim_stats.kind##_g_min) prim_stats.kind##_g_min = (g); \
	if ((g) > prim_stats.kind##_g_max) prim_stats.kind##_g_max = (g); \
	if ((b) < prim_stats.kind##_b_min) prim_stats.kind##_b_min = (b); \
	if ((b) > prim_stats.kind##_b_max) prim_stats.kind##_b_max = (b); \
} while (0)

static void prim_debug_frame_end (void)
{
	static int frame;

	frame++;
	if ((frame % 30) == 0) {
		fprintf (stderr,
			"[PRIMDBG] quad=%d rgb=[%d-%d,%d-%d,%d-%d] | tri=%d rgb=[%d-%d,%d-%d,%d-%d]\n",
			prim_stats.quad_count,
			prim_stats.quad_r_min, prim_stats.quad_r_max,
			prim_stats.quad_g_min, prim_stats.quad_g_max,
			prim_stats.quad_b_min, prim_stats.quad_b_max,
			prim_stats.tri_count,
			prim_stats.tri_r_min, prim_stats.tri_r_max,
			prim_stats.tri_g_min, prim_stats.tri_g_max,
			prim_stats.tri_b_min, prim_stats.tri_b_max);
		prim_stats.quad_count = prim_stats.tri_count = 0;
		prim_stats.quad_r_min = prim_stats.quad_g_min = prim_stats.quad_b_min = 999;
		prim_stats.quad_r_max = prim_stats.quad_g_max = prim_stats.quad_b_max = -1;
		prim_stats.tri_r_min = prim_stats.tri_g_min = prim_stats.tri_b_min = 999;
		prim_stats.tri_r_max = prim_stats.tri_g_max = prim_stats.tri_b_max = -1;
	}
}
#endif /* PRIM_DEBUG */
/* ---- END TEMPORARY DEBUG INSTRUMENTATION ---------------------------- */


static SDL_Surface *sdlscrn;                             /* The SDL screen surface */
BOOL bGrabMouse = FALSE;                          /* Grab the mouse cursor in the window */
BOOL bInFullScreen = FALSE;

/* new stuff */
enum RENDERERS use_renderer = R_GL;
/* mouse shown this frame? */
int mouse_shown = 0;
/* fe2 UI blits are done to old screen memory and copied to this texture. */
static unsigned int screen_tex;

static GLUquadricObj *qobj;
static GLUtesselator *tobj;

float hack;

#define SCR_TEX_W	512
#define SCR_TEX_H	256

#define RAD_2_DEG	57.295779513082323f

/*-----------------------------------------------------------------------*/
/*
  Set window size
*/
int screen_w = 640;
int screen_h = 480;
#define GLERR { printf ("GL: %s\n", gluErrorString (glGetError ()));}

#ifndef CALLBACK
# ifdef WIN32
#  define CALLBACK __attribute__ ((__stdcall__))
# else
#  define CALLBACK
# endif
#endif /* CALLBACK */

void CALLBACK beginCallback(GLenum which);
void CALLBACK errorCallback(GLenum errorCode);
void CALLBACK endCallback(void);
void CALLBACK vertexCallback(GLvoid *vertex, GLvoid *poly_data);
void CALLBACK combineCallback(GLdouble coords[3], 
                     GLdouble *vertex_data[4],
                     GLfloat weight[4], GLdouble **dataOut );

static void set_main_viewport ()
{
	int ctrl_h = 32*screen_h/200;
	glViewport (0, ctrl_h, screen_w, screen_h - ctrl_h);
}

static void set_ctrl_viewport ()
{
	glViewport (0, 0, screen_w, screen_h);
}

static void change_vidmode ()
{
	const SDL_VideoInfo *info = NULL;
	int modes;

	info = SDL_GetVideoInfo ();

	assert (info != NULL);

	SDL_GL_SetAttribute (SDL_GL_DOUBLEBUFFER, 1);
	/* Needed for real depth-tested 3D rendering (see the GL_DEPTH_TEST
	 * enable below): without requesting a depth buffer here, enabling
	 * GL_DEPTH_TEST later has no effect since there is nothing to test
	 * against. */
	SDL_GL_SetAttribute (SDL_GL_DEPTH_SIZE, 16);
	
	modes = SDL_OPENGL | SDL_ANYFORMAT | (bInFullScreen ? SDL_FULLSCREEN : 0);
	
	if ((sdlscrn = SDL_SetVideoMode (screen_w, screen_h,
				info->vfmt->BitsPerPixel, modes)) == 0) {
		fprintf (stderr, "Video mode set failed: %s\n", SDL_GetError ());
		SDL_Quit ();
		exit (-1);
	}

	glDisable (GL_CULL_FACE);
	glShadeModel (GL_FLAT);
	glDisable (GL_DEPTH_TEST);
	glClearColor (0, 0, 0, 0);

	glMatrixMode (GL_PROJECTION);
	glLoadIdentity ();
	/* Aspect ratio of frontier's 3d view is 320/168 = 1.90 at the
	 * original ST resolution, but the 3d viewport (set_main_viewport)
	 * excludes the bottom control-panel strip and follows the actual
	 * screen_w/screen_h - so the projection must follow it too, or the
	 * 3D scene stretches/squashes relative to the 2D control panel at
	 * any resolution other than the original 4:3-ish one. */
	{
		int ctrl_h = 32*screen_h/200;
		float aspect = (float) screen_w / (float) (screen_h - ctrl_h);
		gluPerspective (36.5f, aspect, 1.0f, 10000000000.0f);
	}

	glEnable (GL_TEXTURE_2D);
	glGenTextures (1, &screen_tex);
	glBindTexture (GL_TEXTURE_2D, screen_tex);
	glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, SCR_TEX_W, SCR_TEX_H, 0, GL_RGBA, GL_INT, 0);
	glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameterf (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	
	glBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable (GL_TEXTURE_2D);
	
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glMatrixMode (GL_MODELVIEW);
	glLoadIdentity ();
	/* Opaque 3D primitives (terrain quads/triangles, ships, planets...)
	 * are z-sorted only approximately by the znode tree (painter's
	 * algorithm), which breaks down for intersecting/interleaved
	 * geometry (e.g. ground quads vs terrain triangles at similar
	 * depths). A real depth test fixes that. It's disabled again by
	 * push_ortho()/re-enabled by pop_ortho() for the flat 2D overlay
	 * (control panel, text, translucent 2D lines). */
	glEnable (GL_DEPTH_TEST);
	glDepthFunc (GL_LEQUAL);
}

void Screen_Init(void)
{
	change_vidmode ();
	
	qobj = gluNewQuadric ();

	tobj = gluNewTess ();
		
	gluTessCallback(tobj, GLU_TESS_VERTEX_DATA, (_GLUfuncptr) vertexCallback);
	gluTessCallback(tobj, GLU_TESS_BEGIN, (_GLUfuncptr) beginCallback);
	gluTessCallback(tobj, GLU_TESS_END, (_GLUfuncptr) endCallback);
	gluTessCallback(tobj, GLU_TESS_ERROR, (_GLUfuncptr) errorCallback);
	gluTessCallback(tobj, GLU_TESS_COMBINE, (_GLUfuncptr) combineCallback);
	
	/* Configure some SDL stuff: */
	SDL_WM_SetCaption(PROG_NAME, "Frontier");
	SDL_EventState(SDL_MOUSEMOTION, SDL_ENABLE);
	SDL_EventState(SDL_MOUSEBUTTONDOWN, SDL_ENABLE);
	SDL_EventState(SDL_MOUSEBUTTONUP, SDL_ENABLE);
	SDL_ShowCursor(SDL_ENABLE);
}

void Screen_UnInit(void)
{
}

void Screen_ToggleFullScreen ()
{
	bInFullScreen = !bInFullScreen;
	change_vidmode ();
	//SDL_WM_ToggleFullScreen (sdlscrn);
}

static const unsigned char font_bmp[] = {
	0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x2,0x80,0x80,0x80,0x80,0x80,0x0,
	0x80,0x0,0x0,0x2,0xa0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x4,0x0,0x50,
	0xf8,0x50,0x50,0xf8,0x50,0x0,0x0,0x6,0x20,0xf0,0xa0,0xa0,0xa0,0xa0,0xf0,0x20,
	0x0,0x5,0x0,0xc8,0xd8,0x30,0x60,0xd8,0x98,0x0,0x0,0x6,0xa0,0x0,0xe0,0xa0,
	0xa0,0xa0,0xe0,0x0,0x0,0x4,0x80,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x2,
	0xc0,0x80,0x80,0x80,0x80,0x80,0x80,0xc0,0x0,0x3,0xc0,0x40,0x40,0x40,0x40,0x40,
	0x40,0xc0,0x0,0x3,0x0,0x0,0x20,0xf8,0x50,0xf8,0x20,0x0,0x0,0x6,0x0,0x0,
	0x40,0xe0,0x40,0x0,0x0,0x0,0x0,0x4,0x0,0x0,0x0,0x0,0x0,0x0,0x80,0x80,
	0x0,0x2,0x0,0x0,0x0,0xc0,0x0,0x0,0x0,0x0,0x0,0x3,0x0,0x0,0x0,0x0,
	0x0,0x0,0x80,0x0,0x0,0x2,0x0,0x8,0x18,0x30,0x60,0xc0,0x80,0x0,0x0,0x6,
	0xe0,0xa0,0xa0,0xa0,0xa0,0xa0,0xe0,0x0,0x0,0x4,0x40,0xc0,0x40,0x40,0x40,0x40,
	0xe0,0x0,0x0,0x4,0xe0,0x20,0x20,0xe0,0x80,0x80,0xe0,0x0,0x0,0x4,0xe0,0x20,
	0x20,0xe0,0x20,0x20,0xe0,0x0,0x0,0x4,0x80,0x80,0xa0,0xa0,0xe0,0x20,0x20,0x0,
	0x0,0x4,0xe0,0x80,0x80,0xe0,0x20,0x20,0xe0,0x0,0x0,0x4,0xe0,0x80,0x80,0xe0,
	0xa0,0xa0,0xe0,0x0,0x0,0x4,0xe0,0x20,0x20,0x20,0x20,0x20,0x20,0x0,0x0,0x4,
	0xe0,0xa0,0xa0,0xe0,0xa0,0xa0,0xe0,0x0,0x0,0x4,0xe0,0xa0,0xa0,0xe0,0x20,0x20,
	0xe0,0x0,0x0,0x4,0x0,0x0,0x0,0x80,0x0,0x80,0x0,0x0,0x0,0x2,0x0,0x0,
	0x0,0x80,0x0,0x0,0x80,0x80,0x0,0x2,0xe0,0x0,0xe0,0xa0,0xa0,0xa0,0xa0,0x0,
	0x0,0x4,0x0,0x0,0xe0,0x0,0xe0,0x0,0x0,0x0,0x0,0x4,0xc0,0x0,0xe0,0xa0,
	0xe0,0x80,0xe0,0x0,0x0,0x4,0xe0,0x20,0x20,0xe0,0x80,0x0,0x80,0x0,0x0,0x4,
	0xfe,0x82,0xba,0xa2,0xba,0x82,0xfe,0x0,0x0,0x8,0xf0,0x90,0x90,0x90,0xf0,0x90,
	0x90,0x0,0x0,0x5,0xf0,0x90,0x90,0xf8,0x88,0x88,0xf8,0x0,0x0,0x6,0xe0,0x80,
	0x80,0x80,0x80,0x80,0xe0,0x0,0x0,0x4,0xf8,0x48,0x48,0x48,0x48,0x48,0xf8,0x0,
	0x0,0x6,0xf0,0x80,0x80,0xe0,0x80,0x80,0xf0,0x0,0x0,0x5,0xf0,0x80,0x80,0xe0,
	0x80,0x80,0x80,0x0,0x0,0x4,0xf0,0x80,0x80,0x80,0xb0,0x90,0xf0,0x0,0x0,0x5,
	0x90,0x90,0x90,0xf0,0x90,0x90,0x90,0x0,0x0,0x5,0xe0,0x40,0x40,0x40,0x40,0x40,
	0xe0,0x0,0x0,0x4,0xf0,0x20,0x20,0x20,0x20,0x20,0xe0,0x0,0x0,0x4,0x90,0xb0,
	0xe0,0xc0,0xe0,0xb0,0x90,0x0,0x0,0x5,0x80,0x80,0x80,0x80,0x80,0x80,0xe0,0x0,
	0x0,0x4,0x88,0xd8,0xf8,0xa8,0x88,0x88,0x88,0x0,0x0,0x6,0x90,0xd0,0xf0,0xb0,
	0x90,0x90,0x90,0x0,0x0,0x5,0xf0,0x90,0x90,0x90,0x90,0x90,0xf0,0x0,0x0,0x5,
	0xf0,0x90,0x90,0xf0,0x80,0x80,0x80,0x0,0x0,0x5,0xf0,0x90,0x90,0x90,0x90,0xb0,
	0xf0,0x18,0x0,0x5,0xf0,0x90,0x90,0xf0,0xe0,0xb0,0x90,0x0,0x0,0x5,0xf0,0x80,
	0x80,0xf0,0x10,0x10,0xf0,0x0,0x0,0x5,0xe0,0x40,0x40,0x40,0x40,0x40,0x40,0x0,
	0x0,0x3,0x90,0x90,0x90,0x90,0x90,0x90,0xf0,0x0,0x0,0x5,0x90,0x90,0x90,0xb0,
	0xe0,0xc0,0x80,0x0,0x0,0x5,0x88,0x88,0x88,0xa8,0xf8,0xd8,0x88,0x0,0x0,0x6,
	0x88,0xd8,0x70,0x20,0x70,0xd8,0x88,0x0,0x0,0x6,0x90,0x90,0x90,0xf0,0x20,0x20,
	0x20,0x0,0x0,0x5,0xf0,0x10,0x30,0x60,0xc0,0x80,0xf0,0x0,0x0,0x5,0xa0,0x0,
	0xa0,0xa0,0xa0,0xa0,0xe0,0x0,0x0,0x4,0x0,0x80,0xc0,0x60,0x30,0x18,0x8,0x0,
	0x0,0x6,0xe0,0xa0,0xa0,0xe0,0xa0,0xa0,0xe0,0x80,0x80,0x4,0xe0,0xa0,0xe0,0x0,
	0x0,0x0,0x0,0x0,0x0,0x4,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0xf8,0x0,0x6,
	0xa0,0x0,0xe0,0x20,0xe0,0xa0,0xe0,0x0,0x0,0x4,0x0,0x0,0xe0,0x20,0xe0,0xa0,
	0xe0,0x0,0x0,0x4,0x80,0x80,0xe0,0xa0,0xa0,0xa0,0xe0,0x0,0x0,0x4,0x0,0x0,
	0xc0,0x80,0x80,0x80,0xc0,0x0,0x0,0x3,0x20,0x20,0xe0,0xa0,0xa0,0xa0,0xe0,0x0,
	0x0,0x4,0x0,0x0,0xe0,0xa0,0xe0,0x80,0xe0,0x0,0x0,0x4,0xc0,0x80,0x80,0xc0,
	0x80,0x80,0x80,0x0,0x0,0x3,0x0,0x0,0xe0,0xa0,0xa0,0xa0,0xe0,0x20,0xe0,0x4,
	0x80,0x80,0xe0,0xa0,0xa0,0xa0,0xa0,0x0,0x0,0x4,0x80,0x0,0x80,0x80,0x80,0x80,
	0x80,0x0,0x0,0x2,0x40,0x0,0x40,0x40,0x40,0x40,0x40,0xc0,0x0,0x3,0x80,0x80,
	0xb0,0xe0,0xe0,0xb0,0x90,0x0,0x0,0x5,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x0,
	0x0,0x2,0x0,0x0,0xf8,0xa8,0xa8,0xa8,0xa8,0x0,0x0,0x6,0x0,0x0,0xe0,0xa0,
	0xa0,0xa0,0xa0,0x0,0x0,0x4,0x0,0x0,0xe0,0xa0,0xa0,0xa0,0xe0,0x0,0x0,0x4,
	0x0,0x0,0xe0,0xa0,0xa0,0xa0,0xe0,0x80,0x80,0x4,0x0,0x0,0xe0,0xa0,0xa0,0xa0,
	0xe0,0x20,0x30,0x4,0x0,0x0,0xc0,0x80,0x80,0x80,0x80,0x0,0x0,0x3,0x0,0x0,
	0xc0,0x80,0xc0,0x40,0xc0,0x0,0x0,0x3,0x80,0x80,0xc0,0x80,0x80,0x80,0xc0,0x0,
	0x0,0x3,0x0,0x0,0xa0,0xa0,0xa0,0xa0,0xe0,0x0,0x0,0x4,0x0,0x0,0xa0,0xa0,
	0xe0,0xc0,0x80,0x0,0x0,0x4,0x0,0x0,0x88,0xa8,0xf8,0xd8,0x88,0x0,0x0,0x6,
	0x0,0x0,0xa0,0xe0,0x40,0xe0,0xa0,0x0,0x0,0x4,0x0,0x0,0xa0,0xa0,0xa0,0xa0,
	0xe0,0x20,0xe0,0x4,0x0,0x0,0xf0,0x30,0x60,0xc0,0xf0,0x0,0x0,0x5,0x81,0x8d,
	0xe1,0xa0,0xa0,0xa0,0xa0,0x0,0x0,0x9,0x2,0x1a,0xc2,0x80,0xc0,0x40,0xc0,0x0,
	0x0,0x8,0xfe,0xfc,0xf8,0xfc,0xfe,0xdf,0x8e,0x4,0x0,0x7,0x7f,0x3f,0x1f,0x3f,
	0x7f,0xfb,0x71,0x20,0x0,0x8,0x4,0x8e,0xdf,0xfe,0xfc,0xf8,0xfc,0xfe,0x0,0x8,
	0x20,0x71,0xfb,0x7f,0x3f,0x1f,0x3f,0x7f,0x0,0x7,0xff,0x81,0x81,0x81,0x81,0x81,
	0x81,0xff,0x0,0x9,0x0,0x0,0xe0,0x80,0x80,0x80,0xe0,0x40,0xc0,0x4,0x60,0x0,
	0xe0,0xa0,0xe0,0x80,0xe0,0x0,0x0,0x4,0xc0,0x0,0xa0,0xa0,0xa0,0xa0,0xe0,0x0,
	0x0,0x4,0x40,0xa0,0x40,0x40,0x40,0x40,0x40,0x0,0x0,0x4,0x40,0xa0,0xe0,0x20,
	0xe0,0xa0,0xe0,0x0,0x0,0x4,0x40,0xa0,0xe0,0xa0,0xa0,0xa0,0xe0,0x0,0x0,0x4,
	0x40,0xa0,0xe0,0xa0,0xe0,0x80,0xe0,0x0,0x0,0x4,0xe0,0x0,0xa0,0xa0,0xa0,0xa0,
	0xe0,0x0,0x0,0x4,0xc0,0x0,0xe0,0x20,0xe0,0xa0,0xe0,0x0,0x0,0x4,0xe0,0xa0,
	0xa0,0xa0,0xe0,0xa0,0xa0,0x0,0x0,0x4,0xc0,0xa0,0xa0,0xc0,0xa0,0xa0,0xc0,0x0,
	0x0,0x4,0xe0,0x80,0x80,0x80,0x80,0x80,0xe0,0x0,0x0,0x4,0xc0,0xa0,0xa0,0xa0,
	0xa0,0xa0,0xc0,0x0,0x0,0x4,0xe0,0x80,0x80,0xe0,0x80,0x80,0xe0,0x0,0x0,0x4,
	0xe0,0x80,0x80,0xe0,0x80,0x80,0x80,0x0,0x0,0x4
};

static int DrawChar (int col, int xoffset, char *scrline, int chr)
{
	const char *font_pos;
	char *pix;
	int i;
	
	font_pos = font_bmp;
	font_pos += (chr&0xff)*10;
	scrline += xoffset;
	
	if (xoffset < 0) {
		font_pos += 9;
		return xoffset + *font_pos;
	}
	
	for (i=0; i<8; i++, font_pos++, scrline += SCREENBYTES_LINE) {
		pix = scrline;
		if (xoffset > 319) continue;
		if (*font_pos & 0x80) *pix = col;
		pix++;
		if (xoffset+1 > 319) continue;
		if (*font_pos & 0x40) *pix = col;
		pix++;
		if (xoffset+2 > 319) continue;
		if (*font_pos & 0x20) *pix = col;
		pix++;
		if (xoffset+3 > 319) continue;
		if (*font_pos & 0x10) *pix = col;
		pix++;
		if (xoffset+4 > 319) continue;
		if (*font_pos & 0x8) *pix = col;
		pix++;
		if (xoffset+5 > 319) continue;
		if (*font_pos & 0x4) *pix = col;
		pix++;
		if (xoffset+6 > 319) continue;
		if (*font_pos & 0x2) *pix = col;
		pix++;
		if (xoffset+7 > 319) continue;
		if (*font_pos & 0x1) *pix = col;
	}
	/* width of character */
	font_pos++;
	i = *font_pos;
	return xoffset + i;
}

#define MAX_QUEUED_STRINGS	200
struct QueuedString {
	int x, y, col;
	unsigned char str[64];
} queued_strings[MAX_QUEUED_STRINGS];
int queued_string_pos;

void Nu_QueueDrawStr ()
{
	assert (queued_string_pos < MAX_QUEUED_STRINGS);
	strncpy (queued_strings[queued_string_pos].str, GetReg (REG_A0) + STRam, 64);
	queued_strings[queued_string_pos].x = GetReg (REG_D1);
	queued_strings[queued_string_pos].y = GetReg (REG_D2);
	queued_strings[queued_string_pos++].col = GetReg (REG_D0);
}

int DrawStr (int xpos, int ypos, int col, unsigned char *str, bool shadowed)
{
	int x, y, chr;
	char *screen;

	x = xpos;
	y = ypos;
	
	if ((y > 192) || (y<0)) return x;
set_line:
	screen = LOGSCREEN2;
	screen += SCREENBYTES_LINE * y;

	while (*str) {
		chr = *(str++);
		
		if (chr < 0x1e) {
			if (chr == '\r') {
				y += 10;
				x = xpos;
				goto set_line;
			}
			else if (chr == 1) col = *(str++);
			continue;
		} else if (chr == 0x1e) {
			/* read new xpos */
			x = *(str++);
			x *= 2;
			continue;
		} else if (chr < 0x20) {
			/* Read new position */
			x = *(str++);
			x *= 2;
			y = *(str++);
			goto set_line;
		}
		
		//if (x > 316) continue;

		if (shadowed) {
			DrawChar (0, x+1, screen+SCREENBYTES_LINE, chr-0x20);
		}
		x = DrawChar (col, x, screen, chr-0x20);
	}

	return x;
}

static void push_ortho ()
{
	glDisable (GL_DEPTH_TEST);
	glMatrixMode (GL_PROJECTION);
	glPushMatrix ();
	glLoadIdentity ();
	glOrtho (0, 320, 0, 200, -1, 1);

	glMatrixMode (GL_MODELVIEW);
	glPushMatrix ();
	glLoadIdentity ();
}

static void pop_ortho ()
{
	glMatrixMode (GL_PROJECTION);
	glPopMatrix ();
	glMatrixMode (GL_MODELVIEW);
	glPopMatrix ();
	/* Restore real depth testing for the 3D scene (see change_vidmode). */
	glEnable (GL_DEPTH_TEST);
}

void Screen_ToggleRenderer ()
{
	use_renderer++;
	if (use_renderer >= R_MAX) use_renderer = 0;
}

static void draw_control_panel ()
{
	int x, y;
	unsigned char *scr;
	unsigned int line[320];
	unsigned int *pal;

	set_ctrl_viewport ();
	
	/* this is a big fucking hack to make starsystem names
	 * in the starmap show up. they are the only bitmap text
	 * things drawn within the fe2 3d renderer, which makes
	 * them fucking annoying. */
	y = logscreen2;
	logscreen2 = physcreen;
	for (x=0; x<queued_string_pos; x++) {
		DrawStr (	queued_strings[x].x,
				queued_strings[x].y,
				queued_strings[x].col,
				queued_strings[x].str,
				FALSE);
	}
	logscreen2 = y;
	/****************************************************/
	
	scr = VideoRaster;
	
	/* intro likes black at the bottom */
	/* hack hack hack what a pile of shite this is */
	push_ortho ();
	glColor3f (0.0f, 0.0f, 0.0f);
	glBegin (GL_TRIANGLE_STRIP);
		glVertex3f (0, 32, 0);
		glVertex3f (319, 32, 0);
		glVertex3f (0, 0, 0);
		glVertex3f (319, 0, 0);
	glEnd ();
	
	glEnable (GL_TEXTURE_2D);
	
	glBindTexture (GL_TEXTURE_2D, screen_tex);

	pal = MainRGBPalette;
	
	/* copy whole 320x200 screen to texture */
	for (y=0; y<200; y++) {
		/* the control panel at the bottom has its own palette */
		if (y >= 168) pal = CtrlRGBPalette;
		
		for (x=0; x<320; x++) {
			/* in gl mode the ui texture has transparent crap where no shit is */
			if ((*(scr)) == 255) {
				scr++;
				line[x] = 0;
			} else {
				line[x] = pal[*(scr++)];
			}
		}
		glTexSubImage2D (GL_TEXTURE_2D, 0, 0, y, 320, 2, GL_RGBA, GL_UNSIGNED_BYTE, line);
	}
	glTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	
	if (use_renderer == R_OLD) {
		glBegin (GL_TRIANGLE_STRIP);
			glTexCoord2f (0.0f, 200.0f/SCR_TEX_H);
			glVertex2i (0, 0);
			glTexCoord2f (320.0f/SCR_TEX_W, 200.0f/SCR_TEX_H);
			glVertex2i (320, 0);
			glTexCoord2f (0.0f, 0.0f);
			glVertex2i (0, 200);
			glTexCoord2f (320.0f/SCR_TEX_W, 0.0f);
			glVertex2i (320, 200);
		glEnd ();
	} else {
		glEnable (GL_BLEND);
		glBegin (GL_TRIANGLE_STRIP);
			glTexCoord2f (0.0f, 200.0f/SCR_TEX_H);
			glVertex2i (0, 0);
			glTexCoord2f (320.0f/SCR_TEX_W, 200.0f/SCR_TEX_H);
			glVertex2i (320, 0);
			glTexCoord2f (0.0f, 0.0f);
			glVertex2i (0, 200);
			glTexCoord2f (320.0f/SCR_TEX_W, 0.0f);
			glVertex2i (320, 200);
		glEnd ();
		glDisable (GL_BLEND);
	}
	
	glDisable (GL_TEXTURE_2D);
	
	pop_ortho ();
}

static void _BuildRGBPalette (unsigned int *rgb, unsigned short *st, int len)
{
	int i;
	int st_col, r, g, b;

	for (i=0; i<len; i++, st++) {
		st_col = *st;
		b = (st_col & 0xf)<<4;
		g = (st_col & 0xf0);
		r = (st_col & 0xf00)>>4;
		rgb[i] = 0xff000000 | (b<<16) | (g<<8) | (r);
	}
}

static inline void split_rgb444b (int rgb, int *r, int *g, int *b)
{
	*r = (rgb & 0xf00) >> 4;
	*g = (rgb & 0xf0);
	*b = (rgb & 0xf) << 4;
}

static inline void split_rgb444i (unsigned int rgb, unsigned int *r, unsigned int *g, unsigned int *b)
{
	*r = (rgb & 0xf00) << 20;
	*g = (rgb & 0xf0) << 24;
	*b = (rgb & 0xf) << 28;
}

static inline void read_m68k_vertex (int st_vptr, int output[3])
{
	output[0] = STMemory_ReadLong (st_vptr);
	output[1] = STMemory_ReadLong (st_vptr+4);
	output[2] = -STMemory_ReadLong (st_vptr+8);
}

struct ZNode {
	unsigned int z;
	struct ZNode *less, *more;
	void *data;
};

#define MAX_OBJ_DATA	(2<<17)
static unsigned char obj_data_area[MAX_OBJ_DATA];
static int obj_data_pos;
#define MAX_ZNODES	1000
static struct ZNode znode_buf[MAX_ZNODES];
static int znode_buf_pos;
static struct ZNode *znode_start;
static struct ZNode *znode_cur;

static inline void znode_databegin ()
{
	znode_cur->data = &obj_data_area[obj_data_pos];
}

static inline void znode_wrlong (int val)
{
	*((int*)(obj_data_area+obj_data_pos)) = val;
	obj_data_pos+=4;
}
static inline void znode_wrword (short val)
{
	*((short*)(obj_data_area+obj_data_pos)) = val;
	obj_data_pos+=2;
}
static inline void znode_wrbyte (char val)
{
	*((char*)(obj_data_area+obj_data_pos)) = val;
	obj_data_pos++;
}

static inline void znode_wrnormal (p68K loc)
{
	znode_wrword (STMemory_ReadWord (loc));
	znode_wrword (STMemory_ReadWord (loc+2));
	znode_wrword (STMemory_ReadWord (loc+4));
}

static void znode_wrmatrix (p68K loc)
{
	znode_wrword (STMemory_ReadWord (loc));
	znode_wrword (STMemory_ReadWord (loc+2));
	znode_wrword (STMemory_ReadWord (loc+4));
	znode_wrword (STMemory_ReadWord (loc+6));
	znode_wrword (STMemory_ReadWord (loc+8));
	znode_wrword (STMemory_ReadWord (loc+10));
	znode_wrword (STMemory_ReadWord (loc+12));
	znode_wrword (STMemory_ReadWord (loc+14));
	znode_wrword (STMemory_ReadWord (loc+16));
}

static inline void znode_wrvertex (p68K loc)
{
	znode_wrlong (STMemory_ReadLong (loc));
	znode_wrlong (STMemory_ReadLong (loc+4));
	znode_wrlong (-STMemory_ReadLong (loc+8));
}

static inline void znode_wrlightsource (p68K loc)
{
	znode_wrlong (-STMemory_ReadWord (loc));
	znode_wrlong (-STMemory_ReadWord (loc+2));
	znode_wrlong (STMemory_ReadWord (loc+4));
}

static inline void znode_wrcolor (int rgb444col)
{
	int r,g,b;
	split_rgb444b (rgb444col, &r, &g, &b);
	znode_wrbyte (r);
	znode_wrbyte (g);
	znode_wrbyte (b);
	znode_wrbyte (0);
}

static inline int znode_rdlong (void **data)
{
	int val = *((int*)(*data));
	(*data) += 4;
	return val;
}
static inline short znode_rdword (void **data)
{
	short val = *((short*)(*data));
	(*data) += 2;
	return val;
}
static inline char znode_rdbyte (void **data)
{
	char val = *((char*)(*data));
	(*data)++;
	return val;
}

static void znode_rdmatrix (void **data, GLfloat m[16])
{
	short val;

#define rdmatrixval(idx)	\
	{	\
		val = znode_rdword (data);	\
		m[idx] = ((float)val)/-32768.0;	\
	}
	
	rdmatrixval (0);
	rdmatrixval (1);
	rdmatrixval (2);
	m[3] = 0.0f;
	rdmatrixval (4);
	rdmatrixval (5);
	rdmatrixval (6);
	m[7] = 0.0f;
	rdmatrixval (8);
	rdmatrixval (9);
	rdmatrixval (10);
	m[11] = 0.0f;
	m[12] = 0.0f;
	m[13] = 0.0f;
	m[14] = 0.0f;
	m[15] = 1.0f;

	//m[0] = -m[0];
	//m[5] = -m[5];
	//m[10] = -m[10];
}

static inline void znode_rdnormal (void **data, short normal[3])
{
	normal[0] = znode_rdword (data);
	normal[1] = znode_rdword (data);
	normal[2] = znode_rdword (data);
}

static inline void znode_rdvertex (void **data, int vertex[3])
{
	vertex[0] = znode_rdlong (data);
	vertex[1] = znode_rdlong (data);
	vertex[2] = znode_rdlong (data);
}

static inline void znode_rdvertexf (void **data, float vertex[3])
{
	vertex[0] = znode_rdlong (data);
	vertex[1] = znode_rdlong (data);
	vertex[2] = znode_rdlong (data);
}

static inline void znode_rdvertexd (void **data, GLdouble vertex[3])
{
	vertex[0] = znode_rdlong (data);
	vertex[1] = znode_rdlong (data);
	vertex[2] = znode_rdlong (data);
}

static inline void znode_rdcolorv (void **data, int *rgb)
{
	rgb[0] = (unsigned char) znode_rdbyte (data);
	rgb[1] = (unsigned char) znode_rdbyte (data);
	rgb[2] = (unsigned char) znode_rdbyte (data);
	(*data)++;
}

static inline void znode_rdcolor (void **data, int *r, int *g, int *b)
{
	*r = znode_rdbyte (data);
	*g = znode_rdbyte (data);
	*b = znode_rdbyte (data);
	(*data)++;
}

enum NuPrimitive {
	NU_END,
	NU_TRIANGLE,
	NU_QUAD,
	NU_LINE,
	NU_BEZIER_LINE,
	NU_TEARDROP,
	NU_COMPLEX_SNEXT,
	NU_COMPLEX_START,
	NU_COMPLEX_END,
	NU_COMPLEX_INNER,
	NU_COMPLEX_BEZIER,
	NU_TWINKLYCIRCLE,
	NU_PLANET,
	NU_CIRCLE,
	NU_CYLINDER,
	NU_BLOB,
	NU_OVALTHINGY,
	NU_POINT,
	NU_2DLINE,
	NU_MAX
};

static inline void end_node ()
{
	znode_wrlong (0);
}

static void add_node (struct ZNode **node, unsigned int zval)
{
	assert (znode_buf_pos < MAX_ZNODES);
	/* end previous znode display list!!!!!!! */
	if (znode_cur) end_node ();
	
	*node = znode_cur = &znode_buf[znode_buf_pos++];
	znode_cur->z = zval;
	znode_cur->less = NULL;
	znode_cur->more = NULL;
	znode_databegin ();
}

static void znode_insert (struct ZNode *node, unsigned int zval)
{
	if (zval > node->z) {
		if (node->more) {
			znode_insert (node->more, zval);
		} else {
			add_node (&node->more, zval);
		}
	} else {
		if (node->less) {
			znode_insert (node->less, zval);
		} else {
			add_node (&node->less, zval);
		}
	}
}

static bool no_znodes_kthx;

void Nu_InsertZNode ()
{
	unsigned int zval = GetReg (4);
	if (use_renderer == R_OLD) return;
	if (no_znodes_kthx) return;
	if (znode_start == NULL) {
		add_node (&znode_start, zval);
	} else {
		znode_insert (znode_start, zval);
	}
}

void Nu_3DViewInit ()
{
	queued_string_pos = 0;
	//printf ("3dviewinit()\n");
	znode_buf_pos = 0;
	//printf ("%d bytes object data\n", obj_data_pos);
	obj_data_pos = 0;

	//add_node (&znode_start, 0);
	znode_start = NULL;
	znode_cur = NULL;
	no_znodes_kthx = FALSE;
}

static void lighting_on (float light_vec[4], int rgb444_light_col, int rgb444_extra_col, int rgb444_obj_col)
{
	bool do_not_light;
	unsigned int extra_col[4], obj_col[4], light_col[4];

	do_not_light = rgb444_obj_col & (1<<8);
	
	/* object color bit 0x8 set means DO NOT LIGHT */
	if (do_not_light) {
		rgb444_obj_col ^= (1<<8);
	} else {
		split_rgb444i (rgb444_light_col, &light_col[0], &light_col[1], &light_col[2]);
		light_col[3] = 0;
		light_vec[3] = 0.0f;
	}

	if (rgb444_obj_col & (1<<4)) {
		rgb444_obj_col ^= (1<<4);
		split_rgb444i (rgb444_obj_col, &obj_col[0], &obj_col[1], &obj_col[2]);
		split_rgb444i (rgb444_extra_col, &extra_col[0], &extra_col[1], &extra_col[2]);
		obj_col[0] += extra_col[0];
		obj_col[1] += extra_col[1];
		obj_col[2] += extra_col[2];
	} else {
		split_rgb444i (rgb444_obj_col, &obj_col[0], &obj_col[1], &obj_col[2]);
	}
	obj_col[3] = 0;

	if (do_not_light) {
		glDisable (GL_LIGHTING);
		glDisable (GL_LIGHT0);
		glColor3ui (obj_col[0], obj_col[1], obj_col[2]);
	} else {
		glLightfv (GL_LIGHT0, GL_POSITION, light_vec);
		glLightiv (GL_LIGHT0, GL_DIFFUSE, light_col);
		glLightiv (GL_LIGHT0, GL_AMBIENT, obj_col);
		glEnable (GL_LIGHTING);
		glEnable (GL_LIGHT0);
	}
}

static void lighting_off ()
{
	glDisable (GL_LIGHTING);
	glDisable (GL_LIGHT0);
}

/*
 * Billboard helpers: several 2D "sprite-like" primitives (twinkly
 * circles/stars, distant planet dots, glow blobs...) are just a
 * gluDisk() translated to a world position, with no attempt to keep
 * them facing the camera. Viewed close to edge-on they degenerate to a
 * near-invisible line, which is one of the main things making the GL
 * renderer look incomplete compared to the ST software renderer (where
 * these were always flat, screen-aligned sprites).
 *
 * billboard_begin() pushes a modelview matrix translated to (x,y,z)
 * with the camera's rotation cancelled out, so that anything drawn in
 * the local XY plane (e.g. gluDisk, which faces local +Z) ends up
 * facing the viewer regardless of the current camera orientation.
 */
static void billboard_begin (float x, float y, float z)
{
	GLfloat m[16];

	glPushMatrix ();
	glTranslatef (x, y, z);
	glGetFloatv (GL_MODELVIEW_MATRIX, m);

	/* Cancel the rotation (and any scale) part of the matrix, keep the
	 * translation: this leaves the local axes aligned with the camera's
	 * eye-space axes, i.e. always facing the viewer. */
	m[0] = 1.0f; m[1] = 0.0f; m[2] = 0.0f;
	m[4] = 0.0f; m[5] = 1.0f; m[6] = 0.0f;
	m[8] = 0.0f; m[9] = 0.0f; m[10] = 1.0f;

	glLoadMatrixf (m);
}

static void billboard_end ()
{
	glPopMatrix ();
}

/* Object-space direction that corresponds to the camera's eye-space Z
 * axis (roughly "towards the viewer"). Cheap approximation of a full
 * look-at billboard, good enough to keep a thin axis-aligned ribbon
 * (teardrop engine flares) from turning edge-on to the camera. */
static void get_view_axis (float axis[3])
{
	GLfloat m[16];

	glGetFloatv (GL_MODELVIEW_MATRIX, m);
	axis[0] = m[2];
	axis[1] = m[6];
	axis[2] = m[10];
}

static void cross3 (const float a[3], const float b[3], float out[3])
{
	out[0] = a[1]*b[2] - a[2]*b[1];
	out[1] = a[2]*b[0] - a[0]*b[2];
	out[2] = a[0]*b[1] - a[1]*b[0];
}

void CALLBACK beginCallback(GLenum which)
{
   glBegin(which);
}

void CALLBACK errorCallback(GLenum errorCode)
{
   const GLubyte *estring;

   estring = gluErrorString(errorCode);
   fprintf(stderr, "Tessellation Error: %s\n", estring);
}

void CALLBACK endCallback(void)
{
   glEnd();
}

static int complex_col[3];
void CALLBACK vertexCallback(GLvoid *vertex, GLvoid *poly_data)
{
   const GLdouble *pointer;

   pointer = (GLdouble *) vertex;
   glColor3ub (complex_col[0], complex_col[1], complex_col[2]);
   glVertex3dv(pointer);
}
/*  combineCallback is used to create a new vertex when edges
 *  intersect.  coordinate location is trivial to calculate,
 *  but weight[4] may be used to average color, normal, or texture
 *  coordinate data.  In this program, color is weighted.
 */
void CALLBACK combineCallback(GLdouble coords[3], 
                     GLdouble *vertex_data[4],
                     GLfloat weight[4], GLdouble **dataOut )
{
   GLdouble *vertex;

   vertex = (GLdouble *) malloc(3 * sizeof(GLdouble));

   vertex[0] = coords[0];
   vertex[1] = coords[1];
   vertex[2] = coords[2];
   *dataOut = vertex;
}
#define MAX_TESS_VERTICES	400
static GLdouble tess_vertices[MAX_TESS_VERTICES][3];
static int tess_vpos;

static GLdouble tessModelMatrix[16];
static GLdouble tessProjMatrix[16];
static GLint tessViewport[4];

static bool do_start_complex;
static int complex_col_rgb444;

static void put_complex_start_4real ()
{
	znode_wrlong (NU_COMPLEX_START);
	znode_wrcolor (complex_col_rgb444);
	no_znodes_kthx = TRUE;
}

/* well it works */
static inline void push_tess_vertex (GLdouble v[3])
{
	static double prev[3];

	if ((v[0]==prev[0]) && (v[1]==prev[1]) && (v[2]==prev[2])) return;
	prev[0] = v[0];
	prev[1] = v[1];
	prev[2] = v[2];

	if (!gluProject (v[0],v[1],v[2], tessModelMatrix, tessProjMatrix, tessViewport,
			&v[0], &v[1], &v[2])) {
		//printf ("fuck %f,%f,%f\n", prev[0], prev[1], prev[2]);
		tess_vpos--;
	} else {
		/* bad return.. */
		if (prev[2] >= 0.0f) {
			/*printf ("(%.2f,%.2f,%.2f) -> (%.2f,%.2f,%.2f)\n",
					prev[0], prev[1], prev[2],
					v[0], v[1], v[2]);
			*/return;
		}
		gluTessVertex (tobj, v, v);
	}
}

void Nu_ComplexSNext ()
{
	if (use_renderer == R_OLD) return;
	if (do_start_complex) { put_complex_start_4real (); do_start_complex = FALSE; }
	znode_wrlong (NU_COMPLEX_SNEXT);
	znode_wrvertex (GetReg (REG_A0)+4);
}
void Nu_DrawComplexSNext (void **data)
{
	if (use_renderer == R_GLWIRE) {
		znode_rdvertexd (data, tess_vertices[tess_vpos]);
		glVertex3dv (tess_vertices[tess_vpos++]);
	} else {
		assert (tess_vpos < MAX_TESS_VERTICES);
		znode_rdvertexd (data, tess_vertices[tess_vpos]);
		push_tess_vertex (tess_vertices[tess_vpos]);
		tess_vpos++;
	}
}
void Nu_ComplexSBegin ()
{
	Nu_ComplexSNext ();
}

void Nu_ComplexStart ()
{
	if (use_renderer == R_OLD) return;
	do_start_complex = TRUE;
	complex_col_rgb444 = GetReg (REG_D6);
}
void Nu_DrawComplexStart (void **data)
{
	tess_vpos = 0;
	
	if (use_renderer == R_GL) {
		glGetDoublev (GL_MODELVIEW_MATRIX, tessModelMatrix);
		glGetDoublev (GL_PROJECTION_MATRIX, tessProjMatrix);
		glGetIntegerv (GL_VIEWPORT, tessViewport);
		
		glMatrixMode (GL_PROJECTION);
		glPushMatrix ();
		glLoadIdentity ();
		glOrtho (tessViewport[0], tessViewport[0]+tessViewport[2], tessViewport[1], tessViewport[1]+tessViewport[3], -1, 1);

		glMatrixMode (GL_MODELVIEW);
		glPushMatrix ();
		glLoadIdentity ();
		
		gluTessNormal (tobj, 0, 0, 1);
		gluTessProperty(tobj, GLU_TESS_WINDING_RULE, GLU_TESS_WINDING_ODD);
		gluTessBeginPolygon (tobj, NULL);
		gluTessBeginContour (tobj);
	} else {
		glBegin (GL_LINE_STRIP);
	}
	znode_rdcolor (data, &complex_col[0], &complex_col[1], &complex_col[2]);
	glColor3ub (complex_col[0], complex_col[1], complex_col[2]);
}


void Nu_ComplexEnd ()
{
	if (use_renderer == R_OLD) return;
	if (do_start_complex) { put_complex_start_4real (); do_start_complex = FALSE; }
	znode_wrlong (NU_COMPLEX_END);
	do_start_complex = FALSE;
	no_znodes_kthx = FALSE;
}
void Nu_DrawComplexEnd (void **data)
{
	if (use_renderer == R_GL) {
		gluTessEndContour (tobj);
		gluTessEndPolygon (tobj);
		
		glMatrixMode (GL_PROJECTION);
		glPopMatrix ();
		glMatrixMode (GL_MODELVIEW);
		glPopMatrix ();
	} else if (use_renderer == R_GLWIRE) {
		glVertex3dv (tess_vertices[0]);
		glEnd ();
	}
}

void Nu_ComplexStartInner ()
{
	if (use_renderer == R_OLD) return;
	if (do_start_complex) { put_complex_start_4real (); do_start_complex = FALSE; }
	znode_wrlong (NU_COMPLEX_INNER);
}
void Nu_DrawComplexStartInner (void **data)
{
	if (use_renderer == R_GL) {
		gluTessEndContour (tobj);
		gluTessBeginContour (tobj);
	} else if (use_renderer == R_GLWIRE) {
		glEnd ();
		glBegin (GL_LINE_STRIP);
		tess_vpos = 0;
	}
}

#define BEZIER_STEPS	10
static void eval_bezier (GLdouble *out, float _t, float ctrlpoints[4][3])
{
	float a,b,c,d,t2;
	t2 = _t*_t;
	c = 1.0f-_t;
	d = t2*_t;
	b = c*c;
	a = b*c;
	b = b*_t*3.0f;
	c = c*3.0f*t2;
	/* x */	
	out[0] =
	    ctrlpoints[0][0] * a +
	    ctrlpoints[1][0] * b +
	    ctrlpoints[2][0] * c +
	    ctrlpoints[3][0] * d;
	/* y */	
	out[1] =
	    ctrlpoints[0][1] * a +
	    ctrlpoints[1][1] * b +
	    ctrlpoints[2][1] * c +
	    ctrlpoints[3][1] * d;
	/* y */	
	out[2] =
	    ctrlpoints[0][2] * a +
	    ctrlpoints[1][2] * b +
	    ctrlpoints[2][2] * c +
	    ctrlpoints[3][2] * d;
}

void Nu_ComplexBezier ()
{
	if (use_renderer == R_OLD) return;
	if (do_start_complex) { put_complex_start_4real (); do_start_complex = FALSE; }
	znode_wrlong (NU_COMPLEX_BEZIER);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrvertex (GetReg (REG_A1)+4);
	znode_wrvertex (GetReg (REG_A2)+4);
	znode_wrvertex (GetReg (REG_A3)+4);
}
void Nu_DrawComplexBezier (void **data)
{
	int i, bezier_steps;
	float delta;
	double v[3];
	GLfloat ctrlpoints[4][3];
	
	znode_rdvertexf (data, ctrlpoints[0]);
	znode_rdvertexf (data, ctrlpoints[1]);
	znode_rdvertexf (data, ctrlpoints[2]);
	znode_rdvertexf (data, ctrlpoints[3]);
	
	/*float poo = MAX (abs (ctrlpoints[0][0]-ctrlpoints[3][0]),
			 abs (ctrlpoints[0][1]-ctrlpoints[3][1]));
	poo /= MIN (ctrlpoints[0][2], ctrlpoints[3][2]);
	bezier_steps = MIN (6 - 20*poo, 16);*/
	//printf ("%d ", bezier_steps);
	bezier_steps = 10;
	
	assert (tess_vpos + bezier_steps < MAX_TESS_VERTICES);
	delta = 1.0f/bezier_steps;

	if (use_renderer == R_GLWIRE) {
		tess_vertices[tess_vpos][0] = ctrlpoints[0][0];
		tess_vertices[tess_vpos][1] = ctrlpoints[0][1];
		tess_vertices[tess_vpos++][2] = ctrlpoints[0][2];
		for (i=0; i<=bezier_steps; i++) {
			eval_bezier (v, i*delta, ctrlpoints);
			glVertex3dv (v);
		}
		return;
	}
	for (i=0; i<=bezier_steps; i++) {
		eval_bezier (&tess_vertices[tess_vpos][0], i*delta, ctrlpoints);
		push_tess_vertex (tess_vertices[tess_vpos]);
		tess_vpos++;
	}
}


/* For engines and industry chimney flares.
 * This is a bit crap, as you will see by panning around the effect. */
void Nu_PutTeardrop ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_TEARDROP);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrvertex (GetReg (REG_A1)+4);
	znode_wrcolor (GetReg (REG_D6));
}
void Nu_DrawTeardrop (void **data)
{
	int i;
	float delta;
	GLfloat ctrlpoints[4][3];
	GLfloat dir[3], ppd[3];
	GLdouble out[3];
	int r, g, b;

#define TD_STRETCH	1.3333333333
#define TD_BROADEN	0.33
#define TD_BEZIER_STEPS	40
	
	if (use_renderer == R_OLD) return;
	znode_rdvertexf (data, dir);
	znode_rdvertexf (data, ctrlpoints[0]);
	znode_rdcolor (data, &r, &g, &b);
	
	dir[0] -= ctrlpoints[0][0];
	dir[1] -= ctrlpoints[0][1];
	dir[2] -= ctrlpoints[0][2];
	
	/* Broaden the flare perpendicular to both its own axis (dir) and the
	 * camera's viewing direction, so it stays roughly face-on to the
	 * viewer instead of being pancake-thin from some angles (the "bit of
	 * crap" the original author warned about). Falls back to the old
	 * fixed in-plane perpendicular if degenerate (dir parallel to the
	 * view axis). */
	{
		float view_axis[3], len_dir, len_ppd;

		get_view_axis (view_axis);
		cross3 (dir, view_axis, ppd);
		len_ppd = sqrt (ppd[0]*ppd[0] + ppd[1]*ppd[1] + ppd[2]*ppd[2]);
		len_dir = sqrt (dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
		if (len_ppd > 0.0001f * (len_dir + 1.0f)) {
			float scale = len_dir / len_ppd;
			ppd[0] *= scale;
			ppd[1] *= scale;
			ppd[2] *= scale;
		} else {
			ppd[0] = -dir[1];
			ppd[1] = dir[0];
			ppd[2] = dir[2];
		}
	}

	//h = sqrt (dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
	
	ctrlpoints[1][0] = ctrlpoints[0][0] + TD_STRETCH*dir[0] + TD_BROADEN*ppd[0];
	ctrlpoints[1][1] = ctrlpoints[0][1] + TD_STRETCH*dir[1] + TD_BROADEN*ppd[1];
	ctrlpoints[1][2] = ctrlpoints[0][2] + dir[2];
	
	ctrlpoints[2][0] = ctrlpoints[0][0] + TD_STRETCH*dir[0] - TD_BROADEN*ppd[0];
	ctrlpoints[2][1] = ctrlpoints[0][1] + TD_STRETCH*dir[1] - TD_BROADEN*ppd[1];
	ctrlpoints[2][2] = ctrlpoints[0][2] + dir[2];
	
	ctrlpoints[3][0] = ctrlpoints[0][0];
	ctrlpoints[3][1] = ctrlpoints[0][1];
	ctrlpoints[3][2] = ctrlpoints[0][2];

	delta = 1.0f/TD_BEZIER_STEPS;
	glColor3ub (r, g, b);
	glBegin (GL_TRIANGLE_FAN);
	/* the tessellator prefers it :-) */
	for (i=0; i<=TD_BEZIER_STEPS; i++) {
		eval_bezier (out, i*delta, ctrlpoints);
		glVertex3dv (out);
	}
	glEnd ();
}


void Nu_PutBezierLine ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_BEZIER_LINE);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrvertex (GetReg (REG_A1)+4);
	znode_wrvertex (GetReg (REG_A2)+4);
	znode_wrvertex (GetReg (REG_A3)+4);
	znode_wrcolor (GetReg (REG_D6));
}
void Nu_DrawBezierLine (void **data)
{
	int i, r, g, b;
	GLfloat ctrlpoints[4][3];
	GLfloat delta;
	GLdouble out[3];

	znode_rdvertexf (data, ctrlpoints[0]);
	znode_rdvertexf (data, ctrlpoints[1]);
	znode_rdvertexf (data, ctrlpoints[2]);
	znode_rdvertexf (data, ctrlpoints[3]);
	znode_rdcolor (data, &r, &g, &b);
	
	delta = 1.0f/20;
	glColor3ub (r, g, b);
	glBegin (GL_LINE_STRIP);
	for (i=0; i<=20; i++) {
		eval_bezier (out, i*delta, ctrlpoints);
		glVertex3dv (out);
	}
	glEnd ();
}

void Nu_PutTriangle ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_TRIANGLE);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrvertex (GetReg (REG_A1)+4);
	znode_wrvertex (GetReg (REG_A2)+4);
	znode_wrcolor (GetReg (REG_D6));
}
void Nu_DrawTriangle (void **data)
{
	float v1[3], v2[3], v3[3];
	int rgb[3];
	
	znode_rdvertexf (data, v1);
	znode_rdvertexf (data, v2);
	znode_rdvertexf (data, v3);
	znode_rdcolorv (data, rgb);
	glColor3ub (rgb[0], rgb[1], rgb[2]);
#if PRIM_DEBUG
	PRIM_DEBUG_HIT (tri, rgb[0], rgb[1], rgb[2]);
#endif
	if (use_renderer == R_GLWIRE) {
		glBegin (GL_LINE_STRIP);
			glVertex3fv (v1);
			glVertex3fv (v2);
			glVertex3fv (v3);
			glVertex3fv (v1);
		glEnd ();
	} else {
		glBegin (GL_TRIANGLES);
			glVertex3fv (v1);
			glVertex3fv (v2);
			glVertex3fv (v3);
		glEnd ();
	}
}

void Nu_PutQuad ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_QUAD);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrvertex (GetReg (REG_A1)+4);
	znode_wrvertex (GetReg (REG_A2)+4);
	znode_wrvertex (GetReg (REG_A3)+4);
	znode_wrcolor (GetReg (REG_D6));
}
void Nu_DrawQuad (void **data)
{
	int v1[3], v2[3], v3[3], v4[3];
	int r, g, b;
	
	znode_rdvertex (data, v1);
	znode_rdvertex (data, v2);
	znode_rdvertex (data, v3);
	znode_rdvertex (data, v4);
	znode_rdcolor (data, &r, &g, &b);
	
	glColor3ub (r, g, b);
#if PRIM_DEBUG
	PRIM_DEBUG_HIT (quad, r, g, b);
#endif
	if (use_renderer == R_GLWIRE) {
		glBegin (GL_LINE_STRIP);
			glVertex3iv (v1);
			glVertex3iv (v2);
			glVertex3iv (v3);
			glVertex3iv (v4);
			glVertex3iv (v1);
		glEnd ();
	} else {
		glBegin (GL_TRIANGLE_STRIP);
			glVertex3iv (v1);
			glVertex3iv (v2);
			glVertex3iv (v4);
			glVertex3iv (v3);
		glEnd ();
	}
}
void Nu_PutTwinklyCircle ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_TWINKLYCIRCLE);
	znode_wrlong (GetReg (REG_D2));
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrcolor (GetReg (REG_D6));
}
void Nu_DrawTwinklyCircle (void **data)
{
	int v1[3];
	unsigned int dreg2, isize;
	float size;
	int r, g, b;

	dreg2 = znode_rdlong (data);
	znode_rdvertex (data, v1);
	znode_rdcolor (data, &r, &g, &b);
	
	glColor3ub (r, g, b);

	isize = (dreg2 << 16) | (dreg2 >> 16);
	//printf ("%x (%x)\n", GetReg (2), isize);
	
	size = -0.002*((short)dreg2)*v1[2];
	
	/* billboard: always face the camera, otherwise this twinkle/star
	 * dot degenerates to an invisible line when viewed edge-on. */
	billboard_begin (v1[0], v1[1], v1[2]);
	
	if (size > 0.0f) gluDisk (qobj, 0.0, size, 32, 1);
	
	size = -0.002*((short) dreg2)*v1[2] - 0.016*v1[2];
	
	//printf ("Size %.2f\n", size);
	if (size > 0.0f) {
		glBegin (GL_LINES);
			glVertex3f (-size, 0.0f, 0.0f);
			glVertex3f (+size, 0.0f, 0.0f);
			glVertex3f (0.0f, -size, 0.0f);
			glVertex3f (0.0f, +size, 0.0f);
		glEnd ();
	}
	billboard_end ();
}

void Nu_Put2DLine ()
{
	if (use_renderer == R_OLD) return;

	if (znode_start == NULL) {
		add_node (&znode_start, 0);
	} else {
		znode_insert (znode_start, 0);
	}
	znode_wrlong (NU_2DLINE);
	znode_wrword (GetReg (REG_D0));
	znode_wrword (GetReg (REG_D1));
	znode_wrword (GetReg (REG_D2));
	znode_wrword (GetReg (REG_D3));
	znode_wrword (GetReg (REG_D4));
	///printf ("%x\n",(GetReg (REG_D4)&0xffff)>>2);
	/* what about color!!!!!! */
}
void Nu_Draw2DLine (void **data)
{
	short x1,y1,x2,y2;
	int col;

	x1 = znode_rdword (data);
	y1 = znode_rdword (data);
	x2 = znode_rdword (data);
	y2 = znode_rdword (data);
	col = MainRGBPalette[(znode_rdword (data)&0xffff)>>2];

	//printf ("%x,%x,%x,%x\n", col, col&0xff, (col>>8)&0xff, (col>>16)&0xff);
	
	push_ortho ();
	set_ctrl_viewport ();
	//glColor3ub (col&0xff, (col>>8)&0xff, (col>>16)&0xff);
	glColor3ub (0,255,0);
	glBegin (GL_LINES);
		glVertex2i (x1, 199-y1);
		glVertex2i (x2, 199-y2);
	glEnd ();
	set_main_viewport ();
	pop_ortho ();
}

#define NUSPHERE_SLICES	48
#define NUSPHERE_STACKS	32

/* The ST software renderer shades planets with a small palette of discrete
 * colour steps (an 8-ish step ramp for the lit hemisphere, another for the
 * dark one - see fe2.s: L3d5dc_PushPlanetCol / "lit side color"/"dark side
 * color"), which is why it looks "banded"/patchy rather than smoothly
 * shaded. Nu_PutPlanet only gives us two colours (object colour, light
 * colour) rather than the actual ramp tables, so we approximate the same
 * visual style: quantize the diffuse term into a handful of discrete
 * bands instead of doing continuous GL Gouraud shading. */
#define PLANET_SHADE_BANDS	8

static void planet_transform_normal (const GLfloat rot_matrix[16], const float n[3], float out[3])
{
	/* Matches the glRotatef(180,1,0,0); glRotatef(180,0,1,0); pair
	 * applied before glMultMatrixf(rot_matrix) in Nu_DrawPlanet: that
	 * combination flips X and Y and keeps Z. */
	float fx = -n[0], fy = -n[1], fz = n[2];

	out[0] = rot_matrix[0]*fx + rot_matrix[4]*fy + rot_matrix[8]*fz;
	out[1] = rot_matrix[1]*fx + rot_matrix[5]*fy + rot_matrix[9]*fz;
	out[2] = rot_matrix[2]*fx + rot_matrix[6]*fy + rot_matrix[10]*fz;
}

static void planet_banded_color (const float n_world[3], const float light_dir[3],
				  const int dark[3], const int lit[3])
{
	float ndotl = n_world[0]*light_dir[0] + n_world[1]*light_dir[1] + n_world[2]*light_dir[2];
	int step;
	float t;

	if (ndotl < 0.0f) ndotl = 0.0f;
	if (ndotl > 1.0f) ndotl = 1.0f;

	step = (int) (ndotl * PLANET_SHADE_BANDS);
	if (step >= PLANET_SHADE_BANDS) step = PLANET_SHADE_BANDS - 1;
	/* band-centered brightness, so each band is a flat, visible step
	 * rather than a smooth ramp */
	t = (step + 0.5f) / PLANET_SHADE_BANDS;

	glColor3ub ((GLubyte) (dark[0] + t*(lit[0]-dark[0])),
		    (GLubyte) (dark[1] + t*(lit[1]-dark[1])),
		    (GLubyte) (dark[2] + t*(lit[2]-dark[2])));
}

/* Manually generated UV-sphere (rather than gluSphere) so each vertex can
 * get its own quantized/banded colour - gluSphere only supports GL's own
 * continuous per-pixel lighting. Combined with glShadeModel(GL_FLAT) this
 * gives clearly visible shading steps like the ST software renderer,
 * instead of a smooth GL-lit sphere. */
static void draw_banded_sphere (float size, const GLfloat rot_matrix[16], const float light_dir[3],
				 const int dark[3], const int lit[3])
{
	int i, j;

	glShadeModel (GL_FLAT);

	for (i = 0; i < NUSPHERE_STACKS; i++) {
		float lat0 = (float) M_PI * (-0.5f + (float) i / NUSPHERE_STACKS);
		float lat1 = (float) M_PI * (-0.5f + (float) (i+1) / NUSPHERE_STACKS);
		float z0 = sin (lat0), zr0 = cos (lat0);
		float z1 = sin (lat1), zr1 = cos (lat1);

		glBegin (GL_QUAD_STRIP);
		for (j = 0; j <= NUSPHERE_SLICES; j++) {
			float lng = 2.0f * (float) M_PI * (float) j / NUSPHERE_SLICES;
			float x = cos (lng), y = sin (lng);
			float n0[3] = { x*zr0, y*zr0, z0 };
			float n1[3] = { x*zr1, y*zr1, z1 };
			float world0[3], world1[3];

			planet_transform_normal (rot_matrix, n0, world0);
			planet_banded_color (world0, light_dir, dark, lit);
			glVertex3f (n0[0]*size, n0[1]*size, n0[2]*size);

			planet_transform_normal (rot_matrix, n1, world1);
			planet_banded_color (world1, light_dir, dark, lit);
			glVertex3f (n1[0]*size, n1[1]*size, n1[2]*size);
		}
		glEnd ();
	}

	glShadeModel (GL_SMOOTH);
}

/* not finished by a long shot */
void Nu_PutPlanet ()
{
	if (use_renderer == R_OLD) return;
	
	/*{
		int cunt, i;
		cunt = GetReg (REG_A6);
		cunt -= 36;
		printf ("Cuntrix:");
		for (i=0; i<9; i++) {
			if (((i)%3) == 0) printf ("\n");
			printf ("%04hx ", STMemory_ReadWord (cunt));
			cunt += 2;
		}
		printf ("\n");
	}*/
	
	znode_wrlong (NU_PLANET);
	znode_wrlong (GetReg (REG_D6));
	znode_wrlong (GetReg (REG_D1));
	znode_wrlong (GetReg (REG_D0));
	/* lighting vector */
	znode_wrlightsource (GetReg (REG_A1));
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrmatrix (GetReg (REG_A6)-36);
}
void Nu_DrawPlanet (void **data)
{
	int v1[3];
	int size;
	int obj_col_raw, light_col_raw;
	int dark[3], lit[3];
	float light_vec[4], light_dir[3], len;
	GLfloat rot_matrix[16];

	obj_col_raw = znode_rdlong (data);
	light_col_raw = znode_rdlong (data);
	size = znode_rdlong (data);
	
	znode_rdvertexf (data, light_vec);
	light_vec[3] = 0.0f;

	len = sqrt (light_vec[0]*light_vec[0] + light_vec[1]*light_vec[1] + light_vec[2]*light_vec[2]);
	if (len > 0.0001f) {
		light_dir[0] = light_vec[0]/len;
		light_dir[1] = light_vec[1]/len;
		light_dir[2] = light_vec[2]/len;
	} else {
		light_dir[0] = 0.0f; light_dir[1] = 0.0f; light_dir[2] = 1.0f;
	}

	/* dark side = the object's base colour on its own; lit side = base
	 * colour plus the light's colour contribution (clamped). This
	 * matches the two endpoints the previous GL_LIGHT1-based ambient/
	 * diffuse setup produced, but quantized into visible bands instead
	 * of interpolated smoothly. */
	split_rgb444b (obj_col_raw, &dark[0], &dark[1], &dark[2]);
	split_rgb444b (light_col_raw, &lit[0], &lit[1], &lit[2]);
	lit[0] += dark[0]; if (lit[0] > 255) lit[0] = 255;
	lit[1] += dark[1]; if (lit[1] > 255) lit[1] = 255;
	lit[2] += dark[2]; if (lit[2] > 255) lit[2] = 255;

	glDisable (GL_LIGHTING);

	znode_rdvertex (data, v1);
	znode_rdmatrix (data, rot_matrix);

	//printf ("planet size %d, pos (%d,%d,%d)\n", size,v1[0],v1[1],v1[2]);
	
	glPushMatrix ();
	glTranslatef (v1[0], v1[1], v1[2]);
	glRotatef (180.0f, 1, 0, 0);
	glRotatef (180.0f, 0, 1, 0);
	glMultMatrixf (rot_matrix);
	glCullFace (GL_BACK);
	glEnable (GL_CULL_FACE);
	draw_banded_sphere ((float) size, rot_matrix, light_dir, dark, lit);
	glDisable (GL_CULL_FACE);
	glPopMatrix ();
}

void Nu_PutCircle ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_CIRCLE);
	znode_wrlong (GetReg (REG_D2));
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrcolor (GetReg (REG_D6));
}
void Nu_DrawCircle (void **data)
{
	int v1[3];
	unsigned int dreg2, isize;
	float size;
	int r, g, b;

	dreg2 = znode_rdlong (data);
	znode_rdvertex (data, v1);
	znode_rdcolor (data, &r, &g, &b);
	
	glColor3ub (r, g, b);

	isize = (dreg2 << 16) | (dreg2 >> 16);
	//printf ("%x (%x)\n", GetReg (2), isize);
	
	size = -0.002*((short)dreg2)*v1[2];
	
	/* billboard: this is used for distant planets/stars rendered as a
	 * flat shaded dot, which must always face the camera. */
	billboard_begin (v1[0], v1[1], v1[2]);
	gluDisk (qobj, 0.0, size, 32, 1);
	billboard_end ();
}

/* life is so strange */
void Nu_PutCylinder ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_CYLINDER);
	znode_wrlightsource (GetReg (REG_A4));
	znode_wrlong (GetReg (REG_D3));
	znode_wrlong (GetReg (REG_D2));
	znode_wrlong (GetReg (REG_D6));
	znode_wrvertex (GetReg (REG_A2)+4);
	znode_wrvertex (GetReg (REG_A3)+4);
	znode_wrlong (GetReg (REG_D0));
	znode_wrlong (GetReg (REG_D1));
	znode_wrlong (GetReg (REG_D5));
	znode_wrlong (GetReg (REG_D4));
}
void Nu_DrawCylinder (void **data)
{
	float light_vec[4];
	int v1[3], v2[3];
	float vdiff[3];
	int rad1, rad2;
	int light_col, obj_col, extra_col;
	float h;

	znode_rdvertexf (data, light_vec);
	light_col = znode_rdlong (data);
	obj_col = znode_rdlong (data);
	extra_col = znode_rdlong (data);

	znode_rdvertex (data, v1);
	znode_rdvertex (data, v2);

	vdiff[0] = v2[0] - v1[0];
	vdiff[1] = v2[1] - v1[1];
	vdiff[2] = v2[2] - v1[2];
	
	h = sqrt (vdiff[0]*vdiff[0] + vdiff[1]*vdiff[1] + vdiff[2]*vdiff[2]);
	
	rad1 = znode_rdlong (data) & 0xffff;
	rad2 = znode_rdlong (data) & 0xffff;
	
	glShadeModel (GL_SMOOTH);
	
	glPushMatrix ();
	glTranslatef (v1[0], v1[1], v1[2]);
	glRotatef (-RAD_2_DEG * (atan2 (vdiff[2], vdiff[0]) - M_PI/2), 0.0f, 1.0f, 0.0f);
	glRotatef (-RAD_2_DEG * asin (vdiff[1]/h), 1.0f, 0.0f, 0.0f);
#define CYLINDER_POOP	20
	
	lighting_on (light_vec, light_col, extra_col, znode_rdlong (data));
	gluDisk (qobj, 0.0, rad1, CYLINDER_POOP, 1);
	glTranslatef (0, 0, h);
	
	lighting_on (light_vec, light_col, extra_col, znode_rdlong (data));
	gluDisk (qobj, 0.0, rad2, CYLINDER_POOP, 1);
	glTranslatef (0, 0, -h);
	
	glEnable (GL_CULL_FACE);
	lighting_on (light_vec, light_col, extra_col, obj_col);
	gluCylinder (qobj, rad1, rad2, h, CYLINDER_POOP, 1);
	glDisable (GL_CULL_FACE);
		
	glPopMatrix ();
	lighting_off ();
}

/*
 * NU_OVALTHINGY is used by the 68k code to draw planet rings (see
 * fe2.s: L3d648_PutPlanetRings / L3b9aa_FilledOvalThingy /
 * L37fb2_ProjectOvalXYZ, and the "gas giant ring colours" comment
 * nearby). The hostcall never passes an actual colour register, so
 * previously this always drew a solid black filled disc. We now draw
 * a translucent, neutral-tinted *annulus* (hollow in the middle, like
 * a real ring) instead.
 *
 * (d, e) are BAM-style angles (32768 = half turn) used to orient the
 * ring's tilt, matching the angle convention used elsewhere in this
 * file. (f) looked like a third rotation in the same family, but a
 * flat, uniformly coloured, rotationally-symmetric ring is invariant
 * under rotation around its own normal, so it has no visible effect
 * here and is intentionally not applied. If this still looks wrong
 * in-game, the sign/axis of (d, e) is the first thing to try flipping.
 */
void Nu_PutOval ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_OVALTHINGY);
	znode_wrvertex (GetReg (REG_A0)+4);
	
	znode_wrlong (GetReg (REG_D3));
	znode_wrlong (GetReg (REG_D4));
	znode_wrlong (GetReg (REG_D5));

	znode_wrlong (GetReg (REG_D6));
}
void Nu_DrawOval (void **data)
{
	int v1[3];
	int rad;
	short d, e, f;

	znode_rdvertex (data, v1);

	d = (short) znode_rdlong (data);
	e = (short) znode_rdlong (data);
	f = (short) znode_rdlong (data);
	(void) f;
	rad = (short) znode_rdlong (data);

	/* No colour data is available for this primitive; use a neutral,
	 * slightly translucent tint typical of planetary rings instead of
	 * the previous solid black. */
	glEnable (GL_BLEND);
	/* Translucent surfaces shouldn't write to the depth buffer (only
	 * test against it), otherwise they can wrongly occlude things drawn
	 * after them that are actually behind their bounding geometry. */
	glDepthMask (GL_FALSE);
	glColor4ub (200, 190, 170, 160);

	glPushMatrix ();
	glTranslatef (v1[0], v1[1], v1[2]);
	glRotatef (180.0f * (d / 32768.0f), 0.0f, 1.0f, 0.0f);
	glRotatef (180.0f * (e / 32768.0f), 1.0f, 0.0f, 0.0f);
	/* Hollow annulus (not a filled disc): a ring should not cover the
	 * body it surrounds. */
	gluDisk (qobj, rad * 0.6, rad, 48, 1);
	glPopMatrix ();
	glDepthMask (GL_TRUE);
	glDisable (GL_BLEND);
}

void Nu_PutBlob ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_BLOB);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrlong (GetReg (REG_D0));
	znode_wrlong (GetReg (REG_D1));
}
void Nu_DrawBlob (void **data)
{
	int v1[3];
	unsigned int r, g, b;
	int rad;
	int edges;
	
	znode_rdvertex (data, v1);
	split_rgb444i (znode_rdlong (data), &r, &g, &b);
	rad = znode_rdlong (data) & 0xffff;
	edges = rad+4;
	
	glColor3ui (r, g, b);
	if (rad < 3) {
		glPointSize ((rad/2)+1);
		glBegin (GL_POINTS);
			glVertex3iv (v1);
		glEnd ();
	} else {
		/* billboard: keep the glow disk facing the camera. */
		billboard_begin (v1[0], v1[1], v1[2]);
		gluDisk (qobj, 0.0, -0.002*(rad)*v1[2], edges, 1);
		billboard_end ();
	}
}
void Nu_PutColoredPoint ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_POINT);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrcolor (GetReg (REG_D0));
	znode_wrlong (2);
}

void Nu_PutPoint ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_POINT);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrcolor (0xfff);
	znode_wrlong (1);
}
void Nu_DrawPoint (void **data)
{
	int v1[3];
	int point_size, r, g, b;

	if (use_renderer == R_OLD) return;
	znode_rdvertex (data, v1);
	znode_rdcolor (data, &r, &g, &b);
	point_size = znode_rdlong (data);

	glPointSize (point_size);
	glColor3ub (r, g, b);
	glBegin (GL_POINTS);
		glVertex3iv (v1);
	glEnd ();
}

void Nu_PutLine ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_LINE);
	znode_wrvertex (GetReg (REG_A0)+4);
	znode_wrvertex (GetReg (REG_A1)+4);
	znode_wrcolor (GetReg (REG_D6));
}
void Nu_DrawLine (void **data)
{
	int v1[3], v2[3];
	int r, g, b;
	
	znode_rdvertex (data, v1);
	znode_rdvertex (data, v2);
	znode_rdcolor (data, &r, &g, &b);
	
	glColor3ub (r, g, b);
	glBegin (GL_LINES);
		glVertex3iv (v1);
		glVertex3iv (v2);
	glEnd ();
}

void Nu_IsGLRenderer ()
{
	if (use_renderer == R_OLD) {
		SetReg (0, 0);
	} else {
		SetReg (0, 1);
	}
}

void Nu_GLClearArea ()
{
	unsigned char *screen, *screen2;
	int x,y,x1,x2,y1,y2;

	if (use_renderer == R_OLD) return;
	x1 = GetReg (0)&0xffff;
	y1 = GetReg (1)&0xffff;
	x2 = GetReg (2)&0xffff;
	y2 = GetReg (3)&0xffff;
	
	push_ortho ();
	set_ctrl_viewport ();
	glColor3f (0.0f, 0.0f, 0.0f);
	glBegin (GL_TRIANGLE_STRIP);
		glVertex3f (x1, 200-y1, 0);
		glVertex3f (x2, 200-y1, 0);
		glVertex3f (x1, 200-y2, 0);
		glVertex3f (x2, 200-y2, 0);
	glEnd ();
	set_main_viewport ();
	pop_ortho ();

	/* and then we wipe the bit of the ST framebuffer (on both buffers)
	 * to transparent/unset */
	screen = (unsigned char *)PHYSCREEN;
	screen += SCREENBYTES_LINE * y1;
	screen2 = (unsigned char *)LOGSCREEN;
	screen2 += SCREENBYTES_LINE * y1;

	for (y=y1; y<y2; y++) {
		for (x=x1; x<x2; x++) {
			*(screen+x) = 255;
			*(screen2+x) = 255;
		}
		screen += SCREENBYTES_LINE;
		screen2 += SCREENBYTES_LINE;
	}
}

typedef void (*NU_DRAWFUNC) (void **);
NU_DRAWFUNC nu_drawfuncs[NU_MAX] = {
	NULL,
	&Nu_DrawTriangle,
	&Nu_DrawQuad,
	&Nu_DrawLine,
	&Nu_DrawBezierLine,
	&Nu_DrawTeardrop,
	&Nu_DrawComplexSNext, // 6
	&Nu_DrawComplexStart,
	&Nu_DrawComplexEnd,
	&Nu_DrawComplexStartInner, // 9
	&Nu_DrawComplexBezier,
	&Nu_DrawTwinklyCircle,
	&Nu_DrawPlanet,
	&Nu_DrawCircle,
	&Nu_DrawCylinder,
	&Nu_DrawBlob,
	&Nu_DrawOval,
	&Nu_DrawPoint,
	&Nu_Draw2DLine
};

static void Nu_DrawPrimitive (void *data)
{
	int fnum;
	
	for (;;) {
		fnum = znode_rdlong (&data);
		//fprintf (stderr, "%d ", fnum);
		if (!fnum) return;
		nu_drawfuncs[fnum] (&data);
	}
}

/*
 * znode_start is the head of a btree of znodes, each with a linked list
 * of GL display lists to draw (in list order).
 *
 * Draw this crap starting from biggest value znodes.
 *
 * The z ordering BETWEEN nodes still comes purely from this tree walk
 * (painter's algorithm on the game-supplied zval, not real 3D distance -
 * some primitives, e.g. distant backdrop/cloud layers, are deliberately
 * given small/arbitrary vertex Z regardless of their intended "far away"
 * zval, so trusting a single global depth buffer across *all* nodes
 * makes far-away things wrongly occlude near ones). Real GL depth
 * testing is only applied *within* a single node's own primitive(s), by
 * clearing the depth buffer before each node: that fixes local
 * self-intersection between primitives batched into the same node (e.g.
 * interleaved ground quads/triangles from the same terrain chunk)
 * without disturbing the coarse inter-node order that already worked.
 */
static void draw_3dview (struct ZNode *node)
{
	if (node == NULL) return;
	if (node->more) draw_3dview (node->more);
	
	if (use_renderer) {
		//fprintf (stderr, "Z=%d ", node->z);
		glClear (GL_DEPTH_BUFFER_BIT);
		Nu_DrawPrimitive (node->data);
	}

	if (node->less) draw_3dview (node->less);
}

static void set_gl_clear_col (int rgb)
{
	float r,g,b;
	r = (rgb&0xff)/255.0f;
	g = (rgb&0xff00)/65280.0f;
	b = (rgb&0xff0000)/16711680.0f;
	glClearColor (r,g,b,0);
}

/*
 * Approximate sky/ground backdrop for atmosphere flight.
 *
 * The real horizon rendering lives entirely in the ST software renderer's
 * own primitive-list interpreter (see fe2.s: Fn_Draw3DView), which is
 * skipped outright when the GL renderer is active and was never given a
 * GL-side replacement. Precisely replicating it would need a dedicated
 * new hostcall + a matching change to fe2.s. As a reasonable first cut
 * that doesn't touch the 68k code at all, draw a two-tone vertical
 * gradient (sky above the horizon, ground below) using the one colour
 * value we do have every frame (fe2_bgcol, set by Call_Memset/
 * Call_MemsetBlue) instead of a single flat glClearColor.
 */
static void draw_atmosphere_backdrop (void)
{
	unsigned int horizon = MainRGBPalette[fe2_bgcol];
	float hr = (horizon & 0xff) / 255.0f;
	float hg = ((horizon >> 8) & 0xff) / 255.0f;
	float hb = ((horizon >> 16) & 0xff) / 255.0f;
	/* zenith: darker/richer version of the horizon colour */
	float zr = hr * 0.35f, zg = hg * 0.35f, zb = hb * 0.55f;
	/* ground: warm tone blended with the horizon colour, so it stays
	 * thematically consistent with whatever the sky is doing */
	float gr = hr*0.5f + 0.425f, gg = hg*0.5f + 0.225f, gb = hb*0.5f + 0.275f;

	glDisable (GL_DEPTH_TEST);
	glMatrixMode (GL_PROJECTION);
	glPushMatrix ();
	glLoadIdentity ();
	glOrtho (0, 320, 0, 200, -1, 1);
	glMatrixMode (GL_MODELVIEW);
	glPushMatrix ();
	glLoadIdentity ();

	/* sky: zenith (top) -> horizon colour (middle) */
	glBegin (GL_QUADS);
		glColor3f (zr, zg, zb);
		glVertex2i (0, 200);
		glVertex2i (320, 200);
		glColor3f (hr, hg, hb);
		glVertex2i (320, 100);
		glVertex2i (0, 100);
	glEnd ();

	/* ground: horizon colour (middle) -> ground tone (bottom) */
	glBegin (GL_QUADS);
		glColor3f (hr, hg, hb);
		glVertex2i (0, 100);
		glVertex2i (320, 100);
		glColor3f (gr, gg, gb);
		glVertex2i (320, 0);
		glVertex2i (0, 0);
	glEnd ();

	glMatrixMode (GL_PROJECTION);
	glPopMatrix ();
	glMatrixMode (GL_MODELVIEW);
	glPopMatrix ();
	glEnable (GL_DEPTH_TEST);
}

void Nu_DrawScreen ()
{
	/* build RGB palettes */
	_BuildRGBPalette (MainRGBPalette, MainPalette, len_main_palette);
	_BuildRGBPalette (CtrlRGBPalette, CtrlPalette, 16);
	
	if (in_atmosphere && use_renderer != R_GLWIRE) {
		draw_atmosphere_backdrop ();
	}
	
	//fprintf (stderr, "Render: ");
	if (znode_cur) end_node ();
	//printf ("Frame: %d znodes.\n", znode_buf_pos);
	draw_3dview (znode_start);
	//fprintf (stderr, "\n");

#if PRIM_DEBUG
	prim_debug_frame_end ();
#endif

	if (mouse_shown) {
		SDL_ShowCursor (SDL_ENABLE);
		mouse_shown = 0;
	} else {
		SDL_ShowCursor (SDL_DISABLE);
	}
	draw_control_panel ();
	glFlush ();
	
	SDL_GL_SwapBuffers ();

	/* frontier background color... */
	if (use_renderer == R_GLWIRE) {
		glClearColor (0,0,0,0);
	} else {
		set_gl_clear_col (MainRGBPalette[fe2_bgcol]);
	}
	
	glMatrixMode (GL_MODELVIEW);
	glClear (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glLoadIdentity ();
	
	set_main_viewport ();
}
