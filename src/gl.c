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
 * list of feature points and plots them as small line segments in a
 * "continent" grey (L3ddc0/L3dec8, "move.l #$777,d7" and friends). That
 * path used to bypass every Nu_Put* hostcall we could hook into: it
 * projects points and clips lines by hand straight onto the ST's own 2D
 * screen buffer (L3ddc4's Cohen-Sutherland-style clip against L3de18).
 *
 * Two dedicated hcalls, Nu_PutPlanetFeatureStart/Nu_PutPlanetFeature (see
 * below), now capture that per-planet feature vertex list before fe2.s
 * projects/clips it, so the real coastline geometry is drawn as actual
 * line segments from Nu_DrawPlanet (see draw_planet_features).
 *
 * The rest of the surface still doesn't have per-pixel land/sea shading
 * data available (Nu_PutPlanet only ever gives us a size, position,
 * rotation and two colours), so we still approximate the *look* of that
 * shading procedurally: a seamless 3D value-noise field evaluated directly
 * on the sphere's surface normal (no UV seams/pole pinching), thresholded
 * into continent-shaped patches, tinted with the same grey (0x777) the
 * original code uses for its feature points. The noise is seeded from the
 * planet's own colours+size, so a given planet's "continents" stay fixed
 * from frame to frame and rotate together with the planet itself (the
 * normal it is sampled at is already in the planet's own rotated space). */
#define PLANET_LAND_TINT_R	112
#define PLANET_LAND_TINT_G	112
#define PLANET_LAND_TINT_B	112
#define PLANET_LAND_MIX		0.45f

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
 * transform (position + rotation matrix) is known. */
#define MAX_PLANET_FEATURE_VERTS	16384
static float planet_feature_dir[MAX_PLANET_FEATURE_VERTS][3];
static char planet_feature_newchain[MAX_PLANET_FEATURE_VERTS];
static int planet_feature_count;

static void planet_feature_push (int rawx, int rawy, int rawz, int newchain)
{
	float x, y, z, len;

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

	planet_feature_dir[planet_feature_count][0] = x;
	planet_feature_dir[planet_feature_count][1] = y;
	planet_feature_dir[planet_feature_count][2] = z;
	planet_feature_newchain[planet_feature_count] = newchain;
	planet_feature_count++;
}

/* Draws the buffered coastline vertices as connected line segments and
 * clears the buffer - call from inside the same glPushMatrix/glTranslatef/
 * glRotatef/glMultMatrixf block used for the planet's sphere, so the lines
 * are transformed exactly like draw_banded_sphere()'s mesh vertices. */
static void draw_planet_features (float size)
{
	int i;

	if (planet_feature_count < 2) { planet_feature_count = 0; return; }

	glColor3ub (PLANET_LAND_TINT_R, PLANET_LAND_TINT_G, PLANET_LAND_TINT_B);
	glLineWidth (2.0f);
	glBegin (GL_LINES);
	for (i = 1; i < planet_feature_count; i++) {
		if (planet_feature_newchain[i]) continue;
		glVertex3f (planet_feature_dir[i-1][0]*size, planet_feature_dir[i-1][1]*size, planet_feature_dir[i-1][2]*size);
		glVertex3f (planet_feature_dir[i][0]*size, planet_feature_dir[i][1]*size, planet_feature_dir[i][2]*size);
	}
	glEnd ();
	glLineWidth (1.0f);

	planet_feature_count = 0;
}

static unsigned int planet_hash_u (int x, int y, int z, unsigned int seed)
{
	unsigned int h = seed;
	h ^= (unsigned int) x * 374761393u;
	h ^= (unsigned int) y * 668265263u;
	h ^= (unsigned int) z * 2147483647u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h ^= (h >> 16);
	return h;
}

static float planet_hash_f (int x, int y, int z, unsigned int seed)
{
	return (float) (planet_hash_u (x, y, z, seed) & 0xffffff) / (float) 0xffffff;
}

/* Trilinearly interpolated value noise, sampled at an arbitrary 3D point
 * (not a 2D UV): evaluating it directly on points of the unit sphere gives
 * a pattern with no seam and no pole pinching. */
static float planet_noise3 (float x, float y, float z, unsigned int seed)
{
	int x0 = (int) floorf (x), y0 = (int) floorf (y), z0 = (int) floorf (z);
	float fx = x - x0, fy = y - y0, fz = z - z0;
	float sx = fx*fx*(3.0f - 2.0f*fx);
	float sy = fy*fy*(3.0f - 2.0f*fy);
	float sz = fz*fz*(3.0f - 2.0f*fz);
	float c000, c100, c010, c110, c001, c101, c011, c111;
	float c00, c10, c01, c11, c0, c1;

	c000 = planet_hash_f (x0,   y0,   z0,   seed);
	c100 = planet_hash_f (x0+1, y0,   z0,   seed);
	c010 = planet_hash_f (x0,   y0+1, z0,   seed);
	c110 = planet_hash_f (x0+1, y0+1, z0,   seed);
	c001 = planet_hash_f (x0,   y0,   z0+1, seed);
	c101 = planet_hash_f (x0+1, y0,   z0+1, seed);
	c011 = planet_hash_f (x0,   y0+1, z0+1, seed);
	c111 = planet_hash_f (x0+1, y0+1, z0+1, seed);

	c00 = c000 + sx*(c100-c000);
	c10 = c010 + sx*(c110-c010);
	c01 = c001 + sx*(c101-c001);
	c11 = c011 + sx*(c111-c011);
	c0 = c00 + sy*(c10-c00);
	c1 = c01 + sy*(c11-c01);
	return c0 + sz*(c1-c0);
}

/* Two extra octaves on top of the base frequency give the continent
 * outlines some coastline-like irregularity instead of perfectly smooth
 * blobs, without hiding the underlying banded shading. */
static float planet_land_factor (const float n[3], unsigned int seed)
{
	float x = n[0]*3.0f, y = n[1]*3.0f, z = n[2]*3.0f;
	float v = planet_noise3 (x, y, z, seed) * 0.6f
		+ planet_noise3 (x*2.3f, y*2.3f, z*2.3f, seed + 1) * 0.3f
		+ planet_noise3 (x*4.7f, y*4.7f, z*4.7f, seed + 2) * 0.1f;
	/* soft threshold around the noise's mean so land covers roughly a
	 * third of the surface instead of a hard 50/50 cut */
	float land = (v - 0.55f) * 4.0f;
	if (land < 0.0f) land = 0.0f;
	if (land > 1.0f) land = 1.0f;
	return land;
}

static void planet_banded_color (const float n_world[3], const float light_dir[3],
				  const int dark[3], const int lit[3], unsigned int seed)
{
	float ndotl = n_world[0]*light_dir[0] + n_world[1]*light_dir[1] + n_world[2]*light_dir[2];
	int step;
	float t, land;
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

	land = planet_land_factor (n_world, seed) * PLANET_LAND_MIX;
	r += land * (PLANET_LAND_TINT_R - r);
	g += land * (PLANET_LAND_TINT_G - g);
	b += land * (PLANET_LAND_TINT_B - b);

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
				 const int dark[3], const int lit[3], unsigned int seed)
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
			planet_banded_color (world0, light_dir, dark, lit, seed);
			glVertex3f (n0[0]*size, n0[1]*size, n0[2]*size);

			planet_transform_normal (rot_matrix, n1, world1);
			planet_banded_color (world1, light_dir, dark, lit, seed);
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
			      const float light_dir[3], const int dark[3], const int lit[3],
			      unsigned int seed)
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
			planet_banded_color (nf, light_dir, dark, lit, seed);
			glVertex3f ((float) (GROUND_DOME_RADIUS*v0[0]),
				    (float) (GROUND_DOME_RADIUS*v0[1]),
				    (float) (GROUND_DOME_RADIUS*v0[2]));

			nf[0] = (float) n1[0];
			nf[1] = (float) n1[1];
			nf[2] = (float) n1[2];
			planet_banded_color (nf, light_dir, dark, lit, seed);
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
	znode_wrlong (NU_PLANETFEATURESTART);
	znode_wrlong (reg_word_s16 (REG_D3));
	znode_wrlong (reg_word_s16 (REG_D4));
	znode_wrlong (reg_word_s16 (REG_D5));
}
void Nu_DrawPlanetFeatureStart (void **data)
{
	int x, y, z;
	x = znode_rdlong (data);
	y = znode_rdlong (data);
	z = znode_rdlong (data);
	planet_feature_push (x, y, z, 1);
}

void Nu_PutPlanetFeature ()
{
	if (use_renderer == R_OLD) return;
	znode_wrlong (NU_PLANETFEATURE);
	znode_wrlong (reg_word_s16 (REG_D3));
	znode_wrlong (reg_word_s16 (REG_D4));
	znode_wrlong (reg_word_s16 (REG_D5));
}
void Nu_DrawPlanetFeature (void **data)
{
	int x, y, z;
	x = znode_rdlong (data);
	y = znode_rdlong (data);
	z = znode_rdlong (data);
	planet_feature_push (x, y, z, 0);
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
	unsigned int seed;

	obj_col_raw = znode_rdlong (data);
	light_col_raw = znode_rdlong (data);
	size = znode_rdlong (data);

	/* Stable per-planet identity for the procedural continent pattern
	 * (see planet_land_factor): derived only from quantities that do not
	 * change frame to frame for a given planet (its colours and radius),
	 * never from position, which would make the "continents" slide
	 * around as the camera/planet move. */
	seed = ((unsigned int) obj_col_raw * 2654435761u)
		^ ((unsigned int) light_col_raw * 40503u)
		^ (unsigned int) size;

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
			/* We are close enough to a planet that its surface is the
			 * ground under us - that is exactly the condition for the
			 * sky backdrop too, so derive it from here rather than
			 * from a game flag. Picked up by the next frame. */
			planet_ground_seen = 1;
			draw_horizon_cap (centre, R, d, light_dir, dark, lit, seed);
			/* Not drawn from this close-up "standing on the ground"
			 * view (the coastline silhouette only makes sense seen
			 * from afar); discard so it doesn't leak into the next
			 * planet's feature list. */
			planet_feature_count = 0;
			return;
		}
	}

	glPushMatrix ();
	glTranslatef (v1[0], v1[1], v1[2]);
	glRotatef (180.0f, 1, 0, 0);
	glRotatef (180.0f, 0, 1, 0);
	glMultMatrixf (rot_matrix);
	glCullFace (GL_BACK);
	glEnable (GL_CULL_FACE);
	draw_banded_sphere ((float) size, rot_matrix, light_dir, dark, lit, seed);
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
	&Nu_DrawPlanetFeature
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
