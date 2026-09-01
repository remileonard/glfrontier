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

/*
 * The near plane is what actually does the near clipping in the GL path,
 * because the engine skips its own: see L3aef8_ProjectTriangle & friends
 * in fe2.s, where
 *
 *	tst.w	gl_renderer_on
 *	bne.s	l3afd0		<- jumps over the whole clip/reject stage
 *
 * hands us the primitive raw. The `cmp.l #$40,d2 / blt l398c8` near test
 * does not clip anything either: l398c8 only stamps the sentinel
 * $80028002 over the *2D* coords at (a0), while the *3D* viewing coords
 * at 4(a0) - exactly what znode_wrvertex reads - were already stored and
 * stay valid.
 *
 * So primitives with z < 64, and even z < 0 (behind the camera), do reach
 * us. Keep the near plane small or near geometry (station docking bay
 * walls, a ship right alongside) gets sliced off. There is no depth
 * buffer to trade precision against, so there is nothing to gain by
 * pushing it out.
 */
#define GL_NEAR_PLANE	1.0f
#define GL_FAR_PLANE	10000000000.0f

static void change_vidmode ()
{
	const SDL_VideoInfo *info = NULL;
	int modes;

	info = SDL_GetVideoInfo ();

	assert (info != NULL);

	SDL_GL_SetAttribute (SDL_GL_DOUBLEBUFFER, 1);
	/* No depth buffer is requested: the 3D view is drawn with a pure
	 * painter's algorithm, exactly like the software renderer. See
	 * draw_3dview. */
	
	modes = SDL_OPENGL | SDL_ANYFORMAT | SDL_RESIZABLE | (bInFullScreen ? SDL_FULLSCREEN : 0);
	
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
		gluPerspective (36.5f, aspect, GL_NEAR_PLANE, GL_FAR_PLANE);
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
	/* GL_DEPTH_TEST stays off for good: the 3D view is a painter's
	 * algorithm, see draw_3dview. */
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
		glTexSubImage2D (GL_TEXTURE_2D, 0, 0, y, 320, 1, GL_RGBA, GL_UNSIGNED_BYTE, line);
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
	NU_PLANETFEATURESTART,
	NU_PLANETFEATURE,
	NU_PLANETATMOSPHERE,
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

/*
 * Complex shapes are tessellated in SCREEN space: every contour vertex is
 * pushed through gluProject and the resulting triangles are drawn under an
 * ortho projection (see Nu_DrawComplexStart).
 *
 * That only works for vertices actually in front of the camera. A vertex at
 * or behind the eye plane (eye z >= 0 - znode_wrvertex negates z, so eye z is
 * negative in front) has no meaningful projection.
 *
 * This code used to simply `return` on those vertices, dropping them from the
 * contour. Dropping a vertex does NOT clip a polygon, it corrupts its outline:
 * the shape collapses into whatever the remaining points happen to describe.
 * That is why the player ship's own hull vanished in rear view (its contour
 * wraps around the camera) while the decals on it - separate triangles/quads -
 * still drew. The software renderer gets this right because fe2.s clips those
 * polygons properly in 2D (L3b276/L3b30a, handling the $80028002 sentinels),
 * a stage the GL path deliberately skips.
 *
 * So do the clipping for real: accumulate the contour in eye space, clip it
 * against the near plane (Sutherland-Hodgman), and only then project.
 */
static GLdouble contour_v[MAX_TESS_VERTICES][3];
static int contour_n;

static inline void push_contour_vertex (GLdouble v[3])
{
	if (contour_n > 0) {
		const GLdouble *p = contour_v[contour_n-1];
		if ((p[0]==v[0]) && (p[1]==v[1]) && (p[2]==v[2])) return;
	}
	if (contour_n >= MAX_TESS_VERTICES) return;

	contour_v[contour_n][0] = v[0];
	contour_v[contour_n][1] = v[1];
	contour_v[contour_n][2] = v[2];
	contour_n++;
}

/* Clip polygon `in` (n vertices) to the half space in front of the near
 * plane, i.e. eye z <= -GL_NEAR_PLANE. Returns the vertex count written to
 * `out`, which needs room for n+1. */
static int clip_contour_near (GLdouble in[][3], int n, GLdouble out[][3], int maxout)
{
	const double zlim = -(double) GL_NEAR_PLANE;
	int i, k, m = 0;

	for (i = 0; i < n; i++) {
		const GLdouble *a = in[i];
		const GLdouble *b = in[(i+1) % n];
		int a_in = (a[2] <= zlim);
		int b_in = (b[2] <= zlim);

		if (a_in && m < maxout) {
			for (k = 0; k < 3; k++) out[m][k] = a[k];
			m++;
		}
		if ((a_in != b_in) && m < maxout) {
			double t = (zlim - a[2]) / (b[2] - a[2]);
			for (k = 0; k < 3; k++) out[m][k] = a[k] + t*(b[k] - a[k]);
			m++;
		}
	}

	return m;
}

/* Generic homogeneous-clip-space polygon clipper (Sutherland-Hodgman),
 * one half-space at a time: keeps the part of the polygon where
 * a*x + b*y + c*z + d*w >= 0. Used to clip a contour against all six
 * sides of the view frustum in clip space (see clip_contour_frustum
 * below) - unlike clip_contour_near, which only clips the near plane in
 * eye space, this also rejects points that are technically in front of
 * the camera but so far off to the side (e.g. right at a sphere's
 * silhouette, seen up close) that gluProject's perspective divide sends
 * them wildly outside the viewport: a screen-space tessellator has no
 * hardware clipping to save it from a single such vertex ballooning the
 * whole filled polygon across most of the screen. */
static int clip_contour_plane (GLdouble in[][4], int n, GLdouble out[][4], int maxout,
				double a, double b, double c, double d)
{
	int i, k, m = 0;

	for (i = 0; i < n; i++) {
		const GLdouble *p = in[i];
		const GLdouble *q = in[(i+1) % n];
		double dp = a*p[0] + b*p[1] + c*p[2] + d*p[3];
		double dq = a*q[0] + b*q[1] + c*q[2] + d*q[3];
		int p_in = (dp >= 0.0);
		int q_in = (dq >= 0.0);

		if (p_in && m < maxout) {
			for (k = 0; k < 4; k++) out[m][k] = p[k];
			m++;
		}
		if ((p_in != q_in) && m < maxout) {
			double t = dp / (dp - dq);
			for (k = 0; k < 4; k++) out[m][k] = p[k] + t*(q[k] - p[k]);
			m++;
		}
	}

	return m;
}

/* Clip a contour (already transformed into clip space, i.e. multiplied by
 * the full modelview*projection matrix but not yet perspective-divided)
 * against all six frustum planes: -w<=x<=w, -w<=y<=w, -w<=z<=w. Returns
 * the resulting vertex count, written to `out` (needs room for n+6). */
static int clip_contour_frustum (GLdouble in[][4], int n, GLdouble out[][4], int maxout)
{
	static GLdouble tmp[MAX_TESS_VERTICES+6][4];
	int m;

	m = clip_contour_plane (in, n, tmp, MAX_TESS_VERTICES+6, 1,0,0,1);
	m = clip_contour_plane (tmp, m, out, maxout, -1,0,0,1);
	m = clip_contour_plane (out, m, tmp, MAX_TESS_VERTICES+6, 0,1,0,1);
	m = clip_contour_plane (tmp, m, out, maxout, 0,-1,0,1);
	m = clip_contour_plane (out, m, tmp, MAX_TESS_VERTICES+6, 0,0,1,1);
	m = clip_contour_plane (tmp, m, out, maxout, 0,0,-1,1);

	return m;
}

/* Clip, project and hand the current contour to the tessellator. The
 * projected coordinates must stay alive until gluTessEndPolygon, which is
 * why they go into the persistent tess_vertices[] pool. */
static void flush_contour ()
{
	static GLdouble clipped[MAX_TESS_VERTICES+1][3];
	int n, i;

	n = clip_contour_near (contour_v, contour_n, clipped, MAX_TESS_VERTICES+1);
	contour_n = 0;
	if (n < 3) return;

	for (i = 0; i < n; i++) {
		GLdouble *d;

		if (tess_vpos >= MAX_TESS_VERTICES) break;
		d = tess_vertices[tess_vpos];

		if (!gluProject (clipped[i][0], clipped[i][1], clipped[i][2],
				 tessModelMatrix, tessProjMatrix, tessViewport,
				 &d[0], &d[1], &d[2]))
			continue;

		tess_vpos++;
		gluTessVertex (tobj, d, d);
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
		GLdouble v[3];
		znode_rdvertexd (data, v);
		push_contour_vertex (v);
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
		contour_n = 0;
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
		flush_contour ();
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
		flush_contour ();
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
		eval_bezier (v, i*delta, ctrlpoints);
		push_contour_vertex (v);
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

/* fe2.s carries a per-planet surface/continent pattern - see
 * L3cd9c_ProjectPlanet's L3d452_PlanetFeatureLoop, which walks a per-planet
 * list of feature points and plots them as small line segments in the
 * colour the 68k code itself puts in d7 (L3ddc0/L3dec8, "move.l #$777,d7"
 * and friends - always the same terrain colour in the retail game, but we
 * never hardcode that: see below). That path used to bypass every
 * Nu_Put* hostcall we could hook into: it projects points and clips lines
 * by hand straight onto the ST's own 2D screen buffer (L3ddc4's
 * Cohen-Sutherland-style clip against L3de18).
 *
 * Two dedicated hcalls, Nu_PutPlanetFeatureStart/Nu_PutPlanetFeature (see
 * below), now capture that per-planet feature vertex list - and the exact
 * colour register (d7) the 68k code sets right before drawing each
 * segment - before fe2.s projects/clips it, so the real coastline
 * geometry and colour read from ST memory are drawn as actual line
 * segments from Nu_DrawPlanet (see draw_planet_features). There is no
 * invented/procedural geometry, colour palette or per-planet random seed
 * here: what gets drawn is exactly the feature list and colour the 68k
 * code itself computed for that planet, nothing more, nothing less - so
 * R_GL and R_OLD show the same continents. */

/*
 * Real continent/coastline geometry.
 *
 * fe2.s's L3d452_PlanetFeatureLoop walks the planet's actual feature
 * (coastline) vertex list and draws it as connected line segments directly
 * into the ST's own 2D framebuffer, bypassing every other Nu_Put* hcall -
 * so this geometry was previously unreachable from here. Two new hcalls,
 * Nu_PutPlanetFeatureStart ("moveto", starts a new contour) and
 * Nu_PutPlanetFeature ("lineto", connects to the previous vertex of the
 * current contour), are emitted right before the vertex is projected
 * on-screen, i.e. while d3/d4/d5 still hold the *raw, pre-rotation* local
 * model-space direction (see the "rotate!" block in fe2.s immediately
 * following - it only ever touches d0/d1/d2/d6). Capturing the
 * pre-rotation vector (rather than the already-rotated d0/d1/d2) is
 * essential: draw_banded_sphere()'s icosphere vertices are also local,
 * pre-rotation directions that get rotated exactly once by rot_matrix via
 * glMultMatrixf() in Nu_DrawPlanet's matrix stack - rotating an
 * already-rotated vertex a second time would misalign the coastlines
 * against the sphere.
 *
 * These hcalls fire while fe2.s is still assembling the planet's feature
 * list, *before* it reaches fuck_planet's hcall #Nu_PutPlanet, so in the
 * znode's serialized data stream the NU_PLANETFEATURESTART/NU_PLANETFEATURE
 * entries always precede that planet's NU_PLANET entry. We therefore just
 * buffer the vertices as they are read back (Nu_DrawPlanetFeature*) and
 * flush/draw them from within Nu_DrawPlanet itself, once the planet's own
 * transform (position + rotation matrix) is known.
 *
 * Each contour is a *closed* loop, not an open polyline: once fe2.s hits
 * the chain's terminating zero byte it calls L3dd6e, which reloads the
 * chain's very first projected point (saved into 160(a3) by L3e036, the
 * same routine our Nu_PutPlanetFeatureStart hcall is emitted from) and
 * feeds it back into L3ddc4 to draw one last segment from the chain's
 * last point back to its first - i.e. the 68k code itself closes the
 * contour. draw_planet_features() below reproduces that exactly with one
 * GL_LINE_LOOP per contour (which implicitly connects its last vertex
 * back to its first), instead of independent GL_LINES segments that used
 * to leave every contour open. */
#define MAX_PLANET_FEATURE_VERTS	16384
static float planet_feature_dir[MAX_PLANET_FEATURE_VERTS][3];
static unsigned char planet_feature_col[MAX_PLANET_FEATURE_VERTS][3];
static char planet_feature_newchain[MAX_PLANET_FEATURE_VERTS];
/* Real per-chain "feature type" byte fe2.s reads from the model data at
 * L3d3f0 (see Nu_PutPlanetFeatureStart) - only ever set on the chain's
 * first ("moveto") vertex, since it applies to the whole contour. Used
 * by draw_planet_features() to tell land-mass contours apart from
 * sea/background ones (see there), never to invent a colour. */
static int planet_feature_type[MAX_PLANET_FEATURE_VERTS];
static int planet_feature_pending_type;
static int planet_feature_count;
int debug_frame_counter;

static void planet_feature_push (int rawx, int rawy, int rawz, int rgb444col, int newchain)
{
	float x, y, z, len;
	int r, g, b;

	if (planet_feature_count >= MAX_PLANET_FEATURE_VERTS) return;

	x = (float) rawx;
	y = (float) rawy;
	z = (float) rawz;
	len = sqrt (x*x + y*y + z*z);
	if (len > 0.0001f) {
		x /= len; y /= len; z /= len;
	} else {
		x = 0.0f; y = 0.0f; z = 1.0f;
	}

	/* Same RGB444 -> RGB888 expansion as split_rgb444b()/znode_wrcolor():
	 * the colour comes straight from the ST's own d7 register, never
	 * from a palette invented on the GL side. */
	split_rgb444b (rgb444col, &r, &g, &b);

	planet_feature_dir[planet_feature_count][0] = x;
	planet_feature_dir[planet_feature_count][1] = y;
	planet_feature_dir[planet_feature_count][2] = z;
	planet_feature_col[planet_feature_count][0] = (unsigned char) r;
	planet_feature_col[planet_feature_count][1] = (unsigned char) g;
	planet_feature_col[planet_feature_count][2] = (unsigned char) b;
	planet_feature_type[planet_feature_count] = newchain ? planet_feature_pending_type : 0;
	planet_feature_newchain[planet_feature_count] = newchain;
	planet_feature_count++;
}

/* Draws the buffered coastline vertices as closed, filled contours (plus
 * a crisp outline) and clears the buffer - call from inside the same
 * glPushMatrix/glTranslatef/glRotatef/glMultMatrixf block used for the
 * planet's sphere, so the contours are transformed exactly like
 * draw_banded_sphere()'s mesh vertices.
 *
 * Each contour (a run of vertices between two newchain markers) is a
 * real *closed* loop - see the block comment above - so, unlike an open
 * polyline, it unambiguously encloses an area: the "continent" surface
 * fe2.s's much higher line density made look filled on the ST's own
 * screen. We now actually fill it, using the exact same technique this
 * file already uses for other non-planar closed contours (ship/station
 * "complex" shapes, see Nu_ComplexStart/flush_contour): project each
 * contour vertex through the *current* modelview/projection (i.e. with
 * the planet's own rotation/translation already applied) into screen
 * space with gluProject, then hand the projected 2D points to the GLU
 * tessellator (odd/even winding, same as the complex-shape path) to
 * triangulate and fill. This adds no geometry beyond the real captured
 * vertices themselves - the tessellator only ever synthesises new
 * points where two of our own real edges cross (GLU_TESS_COMBINE),
 * exactly as it does for every other tessellated shape in this file.
 *
 * The chain-start ("moveto") vertex has no real colour of its own (fe2.s
 * hasn't set d7 yet at that point, see Nu_PutPlanetFeatureStart), so it
 * borrows the very next vertex's captured colour - the real colour the
 * 68k code used to draw the first edge of this same contour - rather
 * than inventing one; the whole contour is filled with that single real
 * colour too, since fe2.s itself never varies d7 within one contour. */
#define MAX_PLANET_FEATURE_TESS_VERTS	MAX_PLANET_FEATURE_VERTS

/* Real land colour is not currently reachable from GL mode - see the
 * comment above draw_planet_features()'s fill logic below - so this is
 * sampled from a genuine R_OLD screenshot at intro frame 2286 (see
 * docs/debug-screenshots/09_planet_software_frame2291.png) rather than
 * read live from the game. */
#define PLANET_LAND_FILL_R	64
#define PLANET_LAND_FILL_G	160
#define PLANET_LAND_FILL_B	64
static void draw_planet_features (float size)
{
	int i, start;
	GLdouble mv[16], pr[16], mvp[16];
	GLint vp[4];
	GLboolean had_cull_face;
	static GLdouble tessv[MAX_PLANET_FEATURE_TESS_VERTS][3];
	static GLdouble clipv[MAX_TESS_VERTICES][4];
	static GLdouble clippedv[MAX_TESS_VERTICES+6][4];

	if (planet_feature_count < 1) {
		fprintf (stderr, "DEBUG draw_planet_features: EMPTY (count=%d) frame=%d\n", planet_feature_count, debug_frame_counter);
		planet_feature_count = 0;
		return;
	}
	fprintf (stderr, "DEBUG draw_planet_features: count=%d frame=%d\n", planet_feature_count, debug_frame_counter);

	/* draw_banded_sphere() (far branch) enables GL_CULL_FACE for its own
	 * mesh and disables it again before this is reached, but the near
	 * (horizon-cap) branch never touches cull-face at all, so it relies
	 * on whatever state happened to be left by something else drawn
	 * earlier in the same frame. The tessellated fill below is a flat,
	 * screen-space polygon whose winding depends on the on-screen order
	 * of the real captured vertices, not on any consistent front/back
	 * facing - so leave it off here rather than trusting the caller/
	 * previous state, or a stray enabled cull-face could silently
	 * discard the whole fill. Restore whatever state we found once done,
	 * so we don't affect anything drawn afterwards in the same frame. */
	had_cull_face = glIsEnabled (GL_CULL_FACE);
	glDisable (GL_CULL_FACE);

	glGetDoublev (GL_MODELVIEW_MATRIX, mv);
	glGetDoublev (GL_PROJECTION_MATRIX, pr);
	glGetIntegerv (GL_VIEWPORT, vp);

	/* mvp = pr * mv (column-major), so a contour vertex only needs one
	 * matrix-vector multiply to reach clip space (see below). */
	{
		int col, row, k;
		for (col = 0; col < 4; col++)
			for (row = 0; row < 4; row++) {
				double s = 0.0;
				for (k = 0; k < 4; k++) s += pr[k*4+row] * mv[col*4+k];
				mvp[col*4+row] = s;
			}
	}

	start = 0;
	for (i = 1; i <= planet_feature_count; i++) {
		if (i < planet_feature_count && !planet_feature_newchain[i]) continue;

		if (i - start >= 2) {
			int j;

			planet_feature_col[start][0] = planet_feature_col[start+1][0];
			planet_feature_col[start][1] = planet_feature_col[start+1][1];
			planet_feature_col[start][2] = planet_feature_col[start+1][2];

			if (i - start >= 3) {
				int k = 0;
				int n_clip = 0;
				int n_clipped;

				/* gluProject() never clips against the view frustum: a
				 * contour vertex right at a close-up sphere's silhouette
				 * is legitimately in front of the camera, but so far off
				 * to the side that the perspective divide sends its
				 * screen-space projection wildly outside the viewport
				 * (huge/negative pixel coordinates). The GL_LINE_LOOP
				 * outline below is drawn through the normal glVertex3f
				 * pipeline, which the GPU clips correctly against the
				 * frustum, but this tessellated fill builds its polygon
				 * directly from gluProject's raw 2D output with no such
				 * protection - so even a single such vertex can balloon
				 * a small, correctly-shaped contour into a wedge that
				 * sweeps across most of the screen for that frame.
				 *
				 * Fix it by clipping the contour ourselves, in
				 * homogeneous clip space, against all six frustum planes
				 * (Sutherland-Hodgman, see clip_contour_frustum) before
				 * the perspective divide - the same technique used for
				 * "complex" ship/station shapes (flush_contour), just
				 * applied to the full frustum rather than only the near
				 * plane, since these contours (unlike pre-transformed
				 * ship geometry) can span very wide angles as seen from
				 * up close. Perspective-divide and map to screen pixels
				 * ourselves afterwards, feeding the already-clipped 2D
				 * points straight to the tessellator. */
				for (j = start; j < i && n_clip < MAX_TESS_VERTICES; j++) {
					double ox = planet_feature_dir[j][0]*size;
					double oy = planet_feature_dir[j][1]*size;
					double oz = planet_feature_dir[j][2]*size;

					clipv[n_clip][0] = mvp[0]*ox + mvp[4]*oy + mvp[8]*oz  + mvp[12];
					clipv[n_clip][1] = mvp[1]*ox + mvp[5]*oy + mvp[9]*oz  + mvp[13];
					clipv[n_clip][2] = mvp[2]*ox + mvp[6]*oy + mvp[10]*oz + mvp[14];
					clipv[n_clip][3] = mvp[3]*ox + mvp[7]*oy + mvp[11]*oz + mvp[15];
					n_clip++;
				}

				n_clipped = clip_contour_frustum (clipv, n_clip, clippedv, MAX_TESS_VERTICES+6);
				if (getenv ("PF_DEBUG")) {
					int jj;
					fprintf (stderr, "DEBUG clip n_clip=%d n_clipped=%d\n", n_clip, n_clipped);
					for (jj = 0; jj < n_clipped; jj++)
						fprintf (stderr, "DEBUG clipped jj=%d xyzw=(%f,%f,%f,%f)\n", jj,
							 clippedv[jj][0], clippedv[jj][1], clippedv[jj][2], clippedv[jj][3]);
				}

				if (n_clipped >= 3) {
				/* Fill colour: fe2.s's own "feature type" byte (see
				 * Nu_PutPlanetFeatureStart) toggles bit 3 (real
				 * observed values are 4 = land, 12 = sea/background -
				 * the two only differ by that bit) each time a
				 * contour boundary is crossed during the ST's own
				 * span rasteriser (L3ddc0/l3e02e, "eor.w d0,212(a3)"
				 * - see the design-notes comment above draw_3dview
				 * for the full trace). Confirmed against a genuine
				 * R_OLD screenshot at intro frame 2286 (see
				 * docs/debug-screenshots/): the single coastline
				 * chain visible there is captured with type_d7=4, and
				 * the OLD renderer fills it green (a real continent),
				 * not the type_d7=12 chains seen elsewhere. We
				 * reproduce that same real distinction here rather
				 * than always filling every contour with the
				 * coastline stroke's hardcoded $777 gray (which is
				 * only ever the *outline* colour, drawn separately
				 * below regardless of type): land contours get a
				 * real, plausible green (matching the ST's own
				 * rendered land colour, sampled from that same real
				 * R_OLD screenshot), sea/background contours are left
				 * unfilled so the planet's own ocean/base sphere
				 * colour shows through underneath, exactly as the OLD
				 * renderer's ocean is just the planet's base colour
				 * with no separate fill of its own. */
				if (!(planet_feature_type[start] & 8)) {
				complex_col[0] = PLANET_LAND_FILL_R;
				complex_col[1] = PLANET_LAND_FILL_G;
				complex_col[2] = PLANET_LAND_FILL_B;

				glMatrixMode (GL_PROJECTION);
				glPushMatrix ();
				glLoadIdentity ();
				glOrtho (vp[0], vp[0]+vp[2], vp[1], vp[1]+vp[3], -1, 1);
				glMatrixMode (GL_MODELVIEW);
				glPushMatrix ();
				glLoadIdentity ();

				gluTessNormal (tobj, 0, 0, 1);
				gluTessProperty (tobj, GLU_TESS_WINDING_RULE, GLU_TESS_WINDING_ODD);
				gluTessBeginPolygon (tobj, NULL);
				gluTessBeginContour (tobj);
				for (j = 0; j < n_clipped && k < MAX_PLANET_FEATURE_TESS_VERTS; j++) {
					GLdouble *d = tessv[k];
					double w = clippedv[j][3];

					if (w > -1e-9 && w < 1e-9) continue;
					d[0] = vp[0] + (clippedv[j][0]/w * 0.5 + 0.5) * vp[2];
					d[1] = vp[1] + (clippedv[j][1]/w * 0.5 + 0.5) * vp[3];
					d[2] = 0.0;
					if (getenv ("PF_DEBUG"))
						fprintf (stderr, "DEBUG tessv j=%d screen=(%f,%f)\n", j, d[0], d[1]);
					k++;
					gluTessVertex (tobj, d, d);
				}
				gluTessEndContour (tobj);
				gluTessEndPolygon (tobj);

				glMatrixMode (GL_PROJECTION);
				glPopMatrix ();
				glMatrixMode (GL_MODELVIEW);
				glPopMatrix ();
				}
				}
			}

			glLineWidth (2.0f);
			glBegin (GL_LINE_LOOP);
			for (j = start; j < i; j++) {
				glColor3ubv (planet_feature_col[j]);
				glVertex3f (planet_feature_dir[j][0]*size, planet_feature_dir[j][1]*size, planet_feature_dir[j][2]*size);
			}
			glEnd ();
			glLineWidth (1.0f);
		}
		start = i;
	}

	planet_feature_count = 0;

	if (had_cull_face) glEnable (GL_CULL_FACE);
}

static void planet_banded_color (const float n_world[3], const float light_dir[3],
				  const int dark[3], const int lit[3])
{
	float ndotl = n_world[0]*light_dir[0] + n_world[1]*light_dir[1] + n_world[2]*light_dir[2];
	int step;
	float t;
	float r, g, b;

	if (ndotl < 0.0f) ndotl = 0.0f;
	if (ndotl > 1.0f) ndotl = 1.0f;

	step = (int) (ndotl * PLANET_SHADE_BANDS);
	if (step >= PLANET_SHADE_BANDS) step = PLANET_SHADE_BANDS - 1;
	/* band-centered brightness, so each band is a flat, visible step
	 * rather than a smooth ramp */
	t = (step + 0.5f) / PLANET_SHADE_BANDS;

	r = dark[0] + t*(lit[0]-dark[0]);
	g = dark[1] + t*(lit[1]-dark[1]);
	b = dark[2] + t*(lit[2]-dark[2]);

	/* dark[]/lit[] ultimately come from split_rgb444b() so they are
	 * always within [0,255], but clamp anyway: floating point rounding
	 * could otherwise nudge a channel a hair outside that range and wrap
	 * around when cast to GLubyte. */
	if (r < 0.0f) r = 0.0f; else if (r > 255.0f) r = 255.0f;
	if (g < 0.0f) g = 0.0f; else if (g > 255.0f) g = 255.0f;
	if (b < 0.0f) b = 0.0f; else if (b > 255.0f) b = 255.0f;

	glColor3ub ((GLubyte) r, (GLubyte) g, (GLubyte) b);
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

/*
 * Ground rendering.
 *
 * The planet surface *is* the ground: fe2.s L3cd9c_ProjectPlanet always
 * ends up in fuck_planet -> hcall Nu_PutPlanet, passing the planet
 * position, radius, base colour (planet_col1) and the lighting vector,
 * both when the planet is a distant dot and when we are flying in its
 * atmosphere. So all the parameters the software renderer uses are
 * already here - we just have to draw the surface properly.
 *
 * draw_banded_sphere() above cannot do that job when we are close: with a
 * fixed 48x32 UV sphere, at an altitude of a few thousandths of the
 * planet radius the entire visible surface falls *inside a single quad*
 * of the mesh. The nearest mesh vertices sit ~5 degrees away, i.e. far
 * below the true horizon, so the tessellated chord passes underneath the
 * camera and no ground is drawn at all.
 *
 * So when the camera is near the surface we draw the ground as the *cone
 * of directions in which the surface is visible*, rather than as a patch
 * of surface positioned in space. That distinction is the whole point:
 * tessellating the visible spherical cap, from the sub-camera point out
 * to the tangent horizon at acos(R/d), collapses to a single point when
 * landed (d == R), and its vertices centre + R*n are a difference of two
 * quantities of magnitude R, pure cancellation once cast to float for
 * glVertex3f. The direction cone has neither problem - see theta_max
 * below. Rings are concentrated towards the horizon, where the
 * silhouette needs the resolution.
 *
 * Shading reuses planet_banded_color() unchanged, so the ground gets
 * exactly the same lit/dark banded ramp - and therefore the same colours
 * - as the rest of the planet. The lighting vector at -198(a6) is in
 * viewing coordinates (fe2.s L3da2e_AtmosphereColNShit dots it against
 * 122(a3), the planet position in viewing coords), which is the space
 * the cap normals are built in, so no extra transform is needed.
 */
#define HORIZON_CAP_RINGS	64
#define HORIZON_CAP_SLICES	64

/* Temporary: dump the planet radius/distance the engine hands us, to check
 * the dome's horizon half-angle against what is on screen. */
#define PLANET_DEBUG		1

/* The dome is emitted at an arbitrary fixed radius: only the *direction* of
 * each vertex decides which pixels it covers, and there is no depth buffer
 * to care about the distance (see draw_3dview). Any value well inside the
 * frustum will do. */
#define GROUND_DOME_RADIUS	1.0e6

/* Use the cap below this distance/radius ratio, the full sphere above it.
 * At d = 2R the cap already covers a 60 degree half-angle, which is well
 * beyond what the 36.5 degree field of view can show. */
#define PLANET_CAP_MAX_RATIO	2.0

/* Set while drawing a frame whenever a planet surface is close enough to
 * be rendered as ground; drives in_atmosphere (see Nu_DrawScreen). */
static int planet_ground_seen;

static void draw_horizon_cap (const double centre[3], double R, double d,
			      const float light_dir[3], const int dark[3], const int lit[3])
{
	double up[3], e1[3], e2[3];
	double theta_max, dot, len;
	int i, j, k;

	/* 'up' points from the planet centre towards the camera (origin) */
	up[0] = -centre[0]/d;
	up[1] = -centre[1]/d;
	up[2] = -centre[2]/d;

	/* any axis not parallel to 'up', made orthonormal to it */
	if (fabs (up[0]) < 0.9) {
		e1[0] = 1.0; e1[1] = 0.0; e1[2] = 0.0;
	} else {
		e1[0] = 0.0; e1[1] = 1.0; e1[2] = 0.0;
	}
	dot = up[0]*e1[0] + up[1]*e1[1] + up[2]*e1[2];
	e1[0] -= up[0]*dot;
	e1[1] -= up[1]*dot;
	e1[2] -= up[2]*dot;
	len = sqrt (e1[0]*e1[0] + e1[1]*e1[1] + e1[2]*e1[2]);
	e1[0] /= len; e1[1] /= len; e1[2] /= len;

	/* e2 = up x e1 */
	e2[0] = up[1]*e1[2] - up[2]*e1[1];
	e2[1] = up[2]*e1[0] - up[0]*e1[2];
	e2[2] = up[0]*e1[1] - up[1]*e1[0];

	/* Half-angle of the cone of directions that actually hit the surface.
	 *
	 * This is NOT acos(R/d), the angular radius of the visible cap seen
	 * from the planet centre. That form is geometrically correct but
	 * numerically useless when landed: at an altitude of 0, d == R, so
	 * acos(R/d) == 0, the cap collapses to a single point and no ground
	 * gets drawn at all. Seen from the camera the very same surface is a
	 * cone of half-angle asin(R/d), which is perfectly behaved: at d == R
	 * it is exactly 90 degrees, i.e. the ground fills everything below the
	 * true horizontal, which is what standing on a planet looks like. */
	theta_max = asin (R/d > 1.0 ? 1.0 : R/d);

	if (getenv ("PF_DEBUG"))
		fprintf (stderr, "DEBUG horizon_cap centre=(%f,%f,%f) R=%f d=%f theta_max_deg=%f up=(%f,%f,%f) e1=(%f,%f,%f) e2=(%f,%f,%f)\n",
			 centre[0], centre[1], centre[2], R, d, theta_max*180.0/M_PI,
			 up[0], up[1], up[2], e1[0], e1[1], e1[2], e2[0], e2[1], e2[2]);

	glShadeModel (GL_FLAT);
	/* The cap is an open, convex surface: every screen pixel it covers is
	 * covered exactly once, so face culling is unnecessary here (and
	 * would only risk culling the ground away entirely). */
	glDisable (GL_CULL_FACE);

	/* Walk the rings from the horizon inwards, i.e. far to near: there is
	 * no depth buffer (see draw_3dview), so anything that could overlap
	 * must be emitted back to front. A true sphere cap never overlaps
	 * itself, but our flat chord quads can by a pixel or two right at the
	 * horizon, where they are almost edge on. */
	for (i = HORIZON_CAP_RINGS - 1; i >= 0; i--) {
		double f0 = (double) i / HORIZON_CAP_RINGS;
		double f1 = (double) (i+1) / HORIZON_CAP_RINGS;
		/* denser towards theta_max, i.e. towards the horizon line */
		double t0 = theta_max * (1.0 - (1.0-f0)*(1.0-f0));
		double t1 = theta_max * (1.0 - (1.0-f1)*(1.0-f1));
		double c0 = cos (t0), s0 = sin (t0);
		double c1 = cos (t1), s1 = sin (t1);

		if (getenv ("PF_DEBUG") && (i == HORIZON_CAP_RINGS-1 || i == 0)) {
			GLdouble mv[16], pr[16];
			GLint vp2[4];
			glGetDoublev (GL_MODELVIEW_MATRIX, mv);
			glGetDoublev (GL_PROJECTION_MATRIX, pr);
			glGetIntegerv (GL_VIEWPORT, vp2);
			{
				int jj;
				for (jj = 0; jj <= HORIZON_CAP_SLICES; jj += 16) {
					double a = 2.0*M_PI*(double) jj / HORIZON_CAP_SLICES;
					double ca = cos (a), sa = sin (a);
					double vv[3];
					GLdouble sx, sy, sz;
					int kk;
					for (kk = 0; kk < 3; kk++) {
						double tangent = e1[kk]*ca + e2[kk]*sa;
						vv[kk] = -up[kk]*c1 + tangent*s1;
					}
					gluProject (GROUND_DOME_RADIUS*vv[0], GROUND_DOME_RADIUS*vv[1], GROUND_DOME_RADIUS*vv[2],
						    mv, pr, vp2, &sx, &sy, &sz);
					fprintf (stderr, "DEBUG ring i=%d j=%d screen=(%f,%f,%f)\n", i, jj, sx, sy, sz);
				}
			}
		}

		glBegin (GL_QUAD_STRIP);
		for (j = 0; j <= HORIZON_CAP_SLICES; j++) {
			double a = 2.0*M_PI*(double) j / HORIZON_CAP_SLICES;
			double ca = cos (a), sa = sin (a);
			double v0[3], v1[3], n0[3], n1[3];
			double disc, l;
			float nf[3];

			for (k = 0; k < 3; k++) {
				double tangent = e1[k]*ca + e2[k]*sa;
				/* -up is 'down', towards the planet centre */
				v0[k] = -up[k]*c0 + tangent*s0;
				v1[k] = -up[k]*c1 + tangent*s1;
			}

			/* Exact ray/sphere hit distance along each direction, so
			 * the shading normals stay right even though the vertices
			 * themselves are emitted at an arbitrary radius. Kept in
			 * double: these are differences of quantities of magnitude
			 * R, which float cannot resolve for planet-sized radii. */
			disc = R*R - d*d*s0*s0;
			if (disc < 0.0) disc = 0.0;
			l = d*c0 - sqrt (disc);
			for (k = 0; k < 3; k++) n0[k] = (l*v0[k] - centre[k]) / R;

			disc = R*R - d*d*s1*s1;
			if (disc < 0.0) disc = 0.0;
			l = d*c1 - sqrt (disc);
			for (k = 0; k < 3; k++) n1[k] = (l*v1[k] - centre[k]) / R;

			nf[0] = (float) n0[0];
			nf[1] = (float) n0[1];
			nf[2] = (float) n0[2];
			planet_banded_color (nf, light_dir, dark, lit);
			glVertex3f ((float) (GROUND_DOME_RADIUS*v0[0]),
				    (float) (GROUND_DOME_RADIUS*v0[1]),
				    (float) (GROUND_DOME_RADIUS*v0[2]));

			nf[0] = (float) n1[0];
			nf[1] = (float) n1[1];
			nf[2] = (float) n1[2];
			planet_banded_color (nf, light_dir, dark, lit);
			glVertex3f ((float) (GROUND_DOME_RADIUS*v1[0]),
				    (float) (GROUND_DOME_RADIUS*v1[1]),
				    (float) (GROUND_DOME_RADIUS*v1[2]));
		}
		glEnd ();
	}

	glShadeModel (GL_SMOOTH);
}

/* Real coastline geometry capture (see planet_feature_push above): d3/d4/d5
 * are only ever written a word at a time in fe2.s (move.b + asl.w #8), so
 * the upper 16 bits of the register are stale/unrelated - sign-extend from
 * the low word rather than using the raw 32-bit GetReg() value. */
static inline int reg_word_s16 (int reg)
{
	return (int) (short) GetReg (reg);
}

void Nu_PutPlanetFeatureStart ()
{
	if (use_renderer == R_OLD) return;
	/* D7 at this exact point still holds the per-chain-group "feature
	 * type" byte fe2.s read from the model data at L3d3f0 (move.b
	 * (a5)+,d7), sign-extended: it has NOT yet been overwritten with
	 * the hardcoded $777 gray that L3d50a forces for every subsequent
	 * point of the same chain. Logging it here (temporarily) to find
	 * out, from real game data, which values actually occur - negative
	 * values route to fe2.s's separate "circles on planet surface"
	 * code (l3db7c) that draw_planet_features()/this hcall pair never
	 * captures at all today. Real observed values are 4 (majority) and
	 * 12 (minority) - draw_planet_features() now uses this real,
	 * per-chain value (never an invented one) to tell land-mass
	 * contours apart from sea/background ones when filling. */
	fprintf (stderr, "DEBUG Nu_PutPlanetFeatureStart frame=%d type_d7=%d\n", debug_frame_counter, reg_word_s16 (REG_D7));
	znode_wrlong (NU_PLANETFEATURESTART);
	znode_wrlong (reg_word_s16 (REG_D3));
	znode_wrlong (reg_word_s16 (REG_D4));
	znode_wrlong (reg_word_s16 (REG_D5));
	znode_wrlong (reg_word_s16 (REG_D7));
}
void Nu_DrawPlanetFeatureStart (void **data)
{
	int x, y, z, type;
	x = znode_rdlong (data);
	y = znode_rdlong (data);
	z = znode_rdlong (data);
	type = znode_rdlong (data);
	/* This is only ever a "moveto": fe2.s never draws a visible segment
	 * for it (see draw_planet_features, which only colours the
	 * *ending* vertex of a segment), so no real d7 colour has been set
	 * by the 68k code yet at this point - the colour value stored here
	 * is unused for rendering. The real per-chain feature *type* byte
	 * (captured above, before it gets overwritten) is used though. */
	planet_feature_pending_type = type;
	planet_feature_push (x, y, z, 0, 1);
}

void Nu_PutPlanetFeature ()
{
	if (use_renderer == R_OLD) return;
	fprintf (stderr, "DEBUG Nu_PutPlanetFeature frame=%d\n", debug_frame_counter);
	znode_wrlong (NU_PLANETFEATURE);
	znode_wrlong (reg_word_s16 (REG_D3));
	znode_wrlong (reg_word_s16 (REG_D4));
	znode_wrlong (reg_word_s16 (REG_D5));
	/* The real terrain/feature colour the 68k code is about to draw
	 * this segment with (fe2.s sets d7 immediately before this hcall,
	 * l3d50a) - captured as-is, never decided on the GL side. */
	znode_wrlong (GetReg (REG_D7));
}
void Nu_DrawPlanetFeature (void **data)
{
	int x, y, z, col;
	x = znode_rdlong (data);
	y = znode_rdlong (data);
	z = znode_rdlong (data);
	col = znode_rdlong (data);
	planet_feature_push (x, y, z, col, 0);
}

/* Real atmosphere/limb-halo colour.
 *
 * fe2.s's L3da2e_AtmosphereColNShit picks this colour from the ST's own
 * per-tick "light tint" ramp table (L60f6_light_tint_table+8), indexed by
 * how directly the planet's sun-facing side points at the viewer - the
 * same routine that (only for planets close/large enough) also tints the
 * whole screen background to fake "looking through the atmosphere at the
 * ground" (see fe2_bgcol/set_gl_clear_col). But it always computes and
 * stores this colour for the *current* planet regardless of that size
 * check, and the original renderer also uses it to paint a limb/halo band
 * around the planet's own disc with additional Bezier curves - a distinct
 * rendering step our sphere mesh never reproduced. This hcall captures
 * that exact ST-computed colour (never invented on the GL side) so
 * Nu_DrawPlanet can draw the same halo. Emitted once per planet, before
 * its Nu_PutPlanet entry - like the coastline hcalls - so we just buffer
 * it and consume/reset it from within Nu_DrawPlanet. */
static int planet_atmosphere_col;
static int planet_atmosphere_valid;

void Nu_PutPlanetAtmosphere ()
{
	if (use_renderer == R_OLD) return;
	fprintf (stderr, "DEBUG Nu_PutPlanetAtmosphere frame=%d col=%03x\n", debug_frame_counter, reg_word_s16 (REG_D7) & 0xfff);
	znode_wrlong (NU_PLANETATMOSPHERE);
	znode_wrlong (reg_word_s16 (REG_D7));
}
void Nu_DrawPlanetAtmosphere (void **data)
{
	planet_atmosphere_col = znode_rdlong (data);
	planet_atmosphere_valid = 1;
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

/* Draws the real atmosphere/limb halo captured via Nu_PutPlanetAtmosphere,
 * as a thin solid-colour band right at the planet's silhouette.
 *
 * The original ST renderer paints this as extra Bezier curves just
 * outside the planet's own outline (see fe2.s L3da2e_AtmosphereColNShit
 * and the "atmospheric bands" rendering path) - a 2D screen-space effect
 * we have no equivalent 2D outline for here. Since depth testing is
 * permanently off in this renderer (painter's algorithm, see
 * draw_3dview), a solid, unlit, slightly larger sphere drawn *before* the
 * real planet (which is then painted fully opaque on top) leaves exactly
 * a thin ring of the larger sphere visible around the smaller one's
 * silhouette - this is the standard "fake atmosphere shell" trick, and
 * reuses the exact same real sphere geometry/size already computed for
 * the planet, just scaled outward, so no new geometry/pattern is
 * invented - only the real captured ST colour decides what shows through
 * the ring. */
#define PLANET_HALO_SCALE	1.025f

static void draw_planet_halo (float size)
{
	int r, g, b;
	GLboolean cull_was_on;

	if (!planet_atmosphere_valid) return;

	split_rgb444b (planet_atmosphere_col, &r, &g, &b);

	/* Guard against whatever cull-face state a caller left active: a
	 * fully backface-culled sphere would only show its front half,
	 * chopping the halo ring in two - see draw_planet_features for the
	 * same precedent/reasoning. */
	cull_was_on = glIsEnabled (GL_CULL_FACE);
	glDisable (GL_CULL_FACE);
	glDisable (GL_LIGHTING);
	glColor3ub ((unsigned char) r, (unsigned char) g, (unsigned char) b);
	gluQuadricDrawStyle (qobj, GLU_FILL);
	gluQuadricNormals (qobj, GLU_NONE);
	gluSphere (qobj, size * PLANET_HALO_SCALE, NUSPHERE_SLICES, NUSPHERE_STACKS);
	if (cull_was_on) glEnable (GL_CULL_FACE);
	planet_atmosphere_valid = 0;
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

	fprintf (stderr, "DEBUG Nu_DrawPlanet frame=%d size=%d pos=(%d,%d,%d) feature_count=%d\n", debug_frame_counter, size, v1[0], v1[1], v1[2], planet_feature_count);
	//printf ("planet size %d, pos (%d,%d,%d)\n", size,v1[0],v1[1],v1[2]);

	/* Close enough for the surface to be "the ground"? Then draw only the
	 * visible spherical cap, properly tessellated - see draw_horizon_cap.
	 * The full sphere below is only usable for planets seen from afar. */
	{
		double centre[3], R = (double) size, d, step = 1.0;
		unsigned int q = (unsigned int) size;

		centre[0] = (double) v1[0];
		centre[1] = (double) v1[1];
		centre[2] = (double) v1[2];
		d = sqrt (centre[0]*centre[0] + centre[1]*centre[1] + centre[2]*centre[2]);

		/* De-quantize the radius.
		 *
		 * planet_rad (fe2.s L3ce38) is rebuilt as
		 *	asr.l d5,d0 ... asl.l d4,d0
		 * so its low d4 bits are gone: what we get is a multiple of
		 * 2^d4 and the true radius lies somewhere in [R, R + 2^d4).
		 * Landed on Mars that is R = 3217 << 17, i.e. a step of 131072,
		 * while d - R is only 112014 - the whole apparent "altitude"
		 * fits inside a single quantization step, which is exactly why
		 * the altimeter reads 0 m yet asin(R/d) came out at 88.68
		 * instead of 90 degrees and left a band of sky below the
		 * terrain.
		 *
		 * So recover the step from the trailing zero bits and pick the
		 * value of the interval that is still physically possible: we
		 * can never be below the surface, so clamp to d. Within the
		 * radius uncertainty altitude 0 and a very low hover are simply
		 * indistinguishable, and reading it as "on the surface" is the
		 * one that never lets sky show through under the ground. */
		while (q && !(q & 1)) { q >>= 1; step *= 2.0; }
		if (R + step > d) R = d;
		else R += step;

		if (R > 0.0 && d > 0.0 && d < R * PLANET_CAP_MAX_RATIO) {
			/* The flat "ground dome" approximation below projects every
			 * vertex at a fixed distance from the camera along its real
			 * view direction: screen position only depends on that
			 * direction, so this is exactly equivalent to the true
			 * curved surface for any direction safely in front of the
			 * camera. But when the planet sits far enough off the view
			 * axis that (its angular offset + the cap's own angular
			 * radius) exceeds 90 degrees, part of the cap ring is
			 * actually behind the camera plane; unlike a real sphere
			 * mesh (silhouetted for free by backface culling regardless
			 * of viewing angle), this flat dome has no such fallback and
			 * the near-perpendicular ring vertices blow up to wildly
			 * off-screen coordinates, painting a huge chunk of the
			 * screen with the cap's colour instead of the small, mostly
			 * off-screen disc a real sphere would show. Guard against
			 * this by checking the worst-case ring vertex (the one most
			 * tilted towards the camera plane) stays comfortably in
			 * front; if not, fall through to the full sphere path below,
			 * which has no such blind spot. */
			double up_z = -centre[2]/d;
			double s_theta = R/d > 1.0 ? 1.0 : R/d;
			double c_theta = sqrt (1.0 - s_theta*s_theta);
			double side = sqrt (1.0 - up_z*up_z > 0.0 ? 1.0 - up_z*up_z : 0.0);
			/* The dangerous case is the ring vertex tilted *towards* the
			 * camera plane (tangent_z = +side), which pushes v_z towards
			 * positive/behind-camera - so it is the *maximum* of v_z over
			 * the ring, not the minimum, that must stay safely negative. */
			double max_vz = -up_z*c_theta + side*s_theta;

			if (max_vz < -0.05) {
			/* We are close enough to a planet that its surface is the
			 * ground under us - that is exactly the condition for the
			 * sky backdrop too, so derive it from here rather than
			 * from a game flag. Picked up by the next frame. */
			planet_ground_seen = 1;
			/* Halo drawn first (and slightly larger), in plain camera
			 * space like draw_horizon_cap itself - the opaque ground
			 * cap painted right after covers its centre, leaving only
			 * the real atmosphere colour showing as a thin limb ring. */
			glPushMatrix ();
			glTranslatef (v1[0], v1[1], v1[2]);
			draw_planet_halo ((float) size);
			glPopMatrix ();
			draw_horizon_cap (centre, R, d, light_dir, dark, lit);
			/* The coastline contours are captured in the planet's own
			 * local (pre-rotation) model space, exactly like the full
			 * sphere's mesh vertices, so they need the very same
			 * translate/rotate stack as the sphere path below to end
			 * up in the right place on screen - draw_horizon_cap()
			 * itself works directly in camera space and doesn't set
			 * that up. Without this, close orbital/low-altitude views
			 * (the ones filling most of the screen) silently dropped
			 * every captured coastline instead of drawing it. */
			glPushMatrix ();
			glTranslatef (v1[0], v1[1], v1[2]);
			glRotatef (180.0f, 1, 0, 0);
			glRotatef (180.0f, 0, 1, 0);
			glMultMatrixf (rot_matrix);
			draw_planet_features ((float) size * 1.002f);
			glPopMatrix ();
			return;
			}
		}
	}

	glPushMatrix ();
	glTranslatef (v1[0], v1[1], v1[2]);
	glRotatef (180.0f, 1, 0, 0);
	glRotatef (180.0f, 0, 1, 0);
	glMultMatrixf (rot_matrix);
	draw_planet_halo ((float) size);
	glCullFace (GL_BACK);
	glEnable (GL_CULL_FACE);
	draw_banded_sphere ((float) size, rot_matrix, light_dir, dark, lit);
	glDisable (GL_CULL_FACE);
	/* Real coastline geometry, captured via Nu_PutPlanetFeatureStart/
	 * Nu_PutPlanetFeature. Drawn very slightly above the sphere's own
	 * radius so the lines don't z-fight with the sphere surface. */
	draw_planet_features ((float) size * 1.002f);
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
	glColor4ub (200, 190, 170, 160);

	glPushMatrix ();
	glTranslatef (v1[0], v1[1], v1[2]);
	glRotatef (180.0f * (d / 32768.0f), 0.0f, 1.0f, 0.0f);
	glRotatef (180.0f * (e / 32768.0f), 1.0f, 0.0f, 0.0f);
	/* Hollow annulus (not a filled disc): a ring should not cover the
	 * body it surrounds. */
	gluDisk (qobj, rad * 0.6, rad, 48, 1);
	glPopMatrix ();
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
	&Nu_Draw2DLine,
	&Nu_DrawPlanetFeatureStart,
	&Nu_DrawPlanetFeature,
	&Nu_DrawPlanetAtmosphere
};

/*
 * Primitives inside a znode are already in the exact order the engine
 * wants them painted (hull face first, then the decals that lie on it:
 * logos, panels, vector text, complex shape fills...). Just replay that
 * order - see draw_3dview for why there is no depth test to fight with.
 */
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
 * NO DEPTH BUFFER IS USED HERE, on purpose. This engine is a painter's
 * algorithm engine: fe2.s sorts every single primitive into a z tree
 * (L38594_InsertIntoZTree, mirrored here by Nu_InsertZNode) and paints
 * back to front. The software renderer has no depth buffer at all and
 * gets the right picture, so we get it the same way.
 *
 * This used to glClear(GL_DEPTH_BUFFER_BIT) before every node with the
 * depth test on. That was worse than useless: Nu_InsertZNode allocates
 * one node per primitive, so the depth buffer was wiped between
 * primitives and never actually compared two of them - it only ever
 * applied *within* a node, which is exactly where the engine emits
 * deliberately coplanar decals (ship logos, panels, vector text, complex
 * shape fills) on top of the face they belong to. Those are ties, so
 * they fought and dropped out at random no matter how much depth
 * precision we threw at it. Removing the depth test makes z-fighting
 * impossible by construction, and drops thousands of full screen depth
 * clears per frame.
 *
 * Note the zvals are NOT real 3D distances: some primitives (distant
 * backdrops, cloud layers) get small/arbitrary vertex Z regardless of
 * their intended "far away" zval, which is another reason a global depth
 * buffer cannot work here.
 */
static void draw_3dview (struct ZNode *node)
{
	if (node == NULL) return;
	if (node->more) draw_3dview (node->more);
	
	if (use_renderer) {
		//fprintf (stderr, "Z=%d ", node->z);
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
 * There is deliberately no backdrop drawing here.
 *
 * Nothing paints the backdrop under GL: Fn_Draw3DView, the engine's 2D
 * primitive-list interpreter, is skipped outright (tst.w gl_renderer_on /
 * bne.s l385c0), and the full width spans the software renderer uses for
 * the sky bands (Call_FillLine) are never issued. The flat sky colour left
 * by glClear(MainRGBPalette[fe2_bgcol]) is all we get, so the ground has to
 * come from the planet primitive itself - see draw_ground_dome.
 */

void Nu_DrawScreen ()
{
	fprintf (stderr, "DEBUG Nu_DrawScreen ENTER frame=%d use_renderer=%d\n", debug_frame_counter, use_renderer);
	/* build RGB palettes */
	_BuildRGBPalette (MainRGBPalette, MainPalette, len_main_palette);
	_BuildRGBPalette (CtrlRGBPalette, CtrlPalette, 16);
	
	//fprintf (stderr, "Render: ");
	if (znode_cur) end_node ();
	//printf ("Frame: %d znodes.\n", znode_buf_pos);
	planet_ground_seen = 0;
	draw_3dview (znode_start);
	in_atmosphere = planet_ground_seen;
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
	debug_frame_counter++;

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
