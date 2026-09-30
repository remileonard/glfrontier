/*
 * Planet surface maps for the GL renderer.
 *
 * How the ST renderer draws a planet surface
 * ------------------------------------------
 *
 * fe2.s L3cd9c_ProjectPlanet does not texture anything: after the planet
 * header (flags + base colours, atmosphere layers) the model carries a
 * byte coded list of surface features (read from L3d3f0 on):
 *
 *	feature := code frac vertex+ 0		polygon (continent, sea...)
 *		 | code|$80 x y z angle		circle (ice cap, crater...)
 *	list	:= feature* 0
 *	vertex	:= x y z			signed bytes, model space
 *		 | x y z 0 r			bounding circle, not drawn
 *
 * Every feature is projected onto the planet (L3da98_ProjectPlanetCoord),
 * turned into a 2D outline and scan converted with an even-odd rule where
 * each edge XORs its feature 'code' into the current colour index
 * (L35b28_DrawPlanet): the colour of a point is the XOR of the codes of
 * all the features containing it, so a lake inside a continent can give
 * the sea colour back. The index selects one of 16 colours prepared in
 * 0-30(a3), the base colours in the model plus the light's tints.
 *
 * Day and night work the same way (end of L3d3f0): when bit 7 of the
 * model flags is set the terminator is drawn as one more 'feature', a
 * great circle perpendicular to the light, XORing $20 over the sunward
 * hemisphere. Bit 6 adds a small circle 75.5 degrees from the sub-solar
 * point, and bit 5 another one at 82.8 degrees, giving the characteristic
 * bands of decreasing brightness towards the terminator. psurf_light_code
 * gives the code those circles XOR in for a given sun angle.
 *
 * Polygon edges are not straight: when it is on screen, an edge is split
 * recursively (L3de2e) and each midpoint is pushed around by a random
 * fraction of the planet's model axes, which is what makes the coast
 * lines fractal. The random numbers come from a simple generator seeded
 * by the planet object (118(object)): each edge takes the next value, and
 * each subdivision pushes and restores the generator state, so the
 * displacement of a given midpoint only depends on its position in the
 * subdivision tree and not on how far the tree was expanded. The ST only
 * expands it as far as the screen resolution needs, we expand it as far
 * as the texture resolution needs, and get the very same coast lines.
 *
 * The magnitude of the displacements works out independent of the view:
 * the level L midpoint of an edge is displaced by 32767 >> d6 along each
 * chosen axis, relative to endpoints of length R_units, with
 *	d6 = frac + s + L - 6 - detail/2	(+1 at random)
 *	R_units = m << 16 >> s
 * where m is the normalized radius word of the model and s the display
 * scale shift (192(a3)), which cancels out. We simply use s = 16.
 *
 * What this file does
 * -------------------
 *
 * psurf_get parses the feature list once per planet. psurf_raster then
 * rasterizes the XOR'ed feature codes over any square gnomonic patch of
 * the sphere (a cube face for the whole planet seen from space, or a
 * small patch around the point under the camera when flying low), using
 * the same even-odd XOR rule as the ST. In a gnomonic projection great
 * circle arcs are straight lines, so the (subdivided) edges are exact
 * line segments there.
 *
 * Which side of a spherical polygon is 'inside' is not defined by the
 * outline alone. The ST takes whatever encloses the screen projection;
 * we take the side that does not contain the point opposite the feature's
 * centre, which is the same thing for any feature smaller than a
 * hemisphere.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../m68000.h"
#include "planet.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* safety net only: the texel size stops the recursion long before */
#define MAX_LEVEL	28
#define MAX_POLYS	256
#define MAX_POLY_VERTS	256

struct PSPoly {
	int code;
	int frac;		/* 0: no fractal displacement */
	int circle;
	/* circle: points are coff + crad * (unit vector perpendicular to cdir) */
	double cdir[3], coff[3], crad;
	/* polygon: unit vertices and the generator state of each edge */
	int n;
	double (*v)[3];
	unsigned int *state;
	/* a point outside the feature, see the header comment */
	double ref[3];
};

struct PSurf {
	unsigned int feat_addr, seed;
	int radius_word, detail;
	/* normalized radius mantissa, 0x4000..0x7fff */
	int m;
	int npolys;
	struct PSPoly polys[MAX_POLYS];
};

/* ---- small vector helpers ------------------------------------------- */

static inline double dot3 (const double a[3], const double b[3])
{
	return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

static inline void cross3d (const double a[3], const double b[3], double o[3])
{
	o[0] = a[1]*b[2] - a[2]*b[1];
	o[1] = a[2]*b[0] - a[0]*b[2];
	o[2] = a[0]*b[1] - a[1]*b[0];
}

static inline double norm3 (double a[3])
{
	double l = sqrt (dot3 (a, a));
	if (l > 0.0) { a[0] /= l; a[1] /= l; a[2] /= l; }
	return l;
}

static inline double angle3 (const double a[3], const double b[3])
{
	/* atan2 form: accurate for tiny angles, unlike acos */
	double c[3];
	cross3d (a, b, c);
	return atan2 (sqrt (dot3 (c, c)), dot3 (a, b));
}

/* any unit vector perpendicular to unit vector n */
static void perp3 (const double n[3], double o[3])
{
	double t[3] = { 1.0, 0.0, 0.0 };
	if (fabs (n[0]) > 0.6) { t[0] = 0.0; t[1] = 1.0; }
	cross3d (n, t, o);
	norm3 (o);
}

/* ---- the ST's random number generator (L3ddc4, L3de2e) --------------- */

static inline unsigned int rng_step (unsigned int s)
{
	return s + ((s << 16) | (s >> 16));
}

/* ---- parsing --------------------------------------------------------- */

static inline int rdbyte (unsigned int *p)
{
	return (signed char) STMemory_ReadByte ((*p)++);
}

static void poly_finish (struct PSPoly *p)
{
	double c[3] = { 0.0, 0.0, 0.0 };
	int i;

	for (i = 0; i < p->n; i++) {
		c[0] += p->v[i][0]; c[1] += p->v[i][1]; c[2] += p->v[i][2];
	}
	if (norm3 (c) == 0.0) c[2] = 1.0;
	p->ref[0] = -c[0]; p->ref[1] = -c[1]; p->ref[2] = -c[2];
}

static void add_circle (struct PSurf *s, int code, const double dir[3], int angle_byte,
			unsigned int *state)
{
	struct PSPoly *p;
	/* BAM, 65536 = full turn */
	double a = (double) (angle_byte << 6) * 2.0 * M_PI / 65536.0;
	double sn = sin (a), cs = cos (a);
	double u[3], w[3];
	int i;

	/* L3dc2c: circles smaller than 100 units (planet radius <= 0x4000)
	 * are not drawn, and then do not use random numbers either. The
	 * display radius varies a little with distance; use a typical one. */
	if (sn * s->m * 0.5 * 1.0215 < 100.0) return;
	/* four edges, never displaced (174(a3) is cleared for circles) */
	for (i = 0; i < 4; i++) *state = rng_step (*state);

	if (s->npolys >= MAX_POLYS) return;
	p = &s->polys[s->npolys++];
	memset (p, 0, sizeof (*p));
	p->code = code;
	p->circle = 1;
	memcpy (p->cdir, dir, sizeof (p->cdir));
	p->coff[0] = dir[0]*cs; p->coff[1] = dir[1]*cs; p->coff[2] = dir[2]*cs;
	p->crad = sn;
	/* 4 base points, like L3dcbc */
	p->n = 4;
	p->v = malloc (4 * sizeof (*p->v));
	perp3 (dir, u);
	cross3d (dir, u, w);
	for (i = 0; i < 3; i++) {
		p->v[0][i] = p->coff[i] + sn*u[i];
		p->v[1][i] = p->coff[i] - sn*w[i];
		p->v[2][i] = p->coff[i] - sn*u[i];
		p->v[3][i] = p->coff[i] + sn*w[i];
	}
	/* the side of the circle away from its centre is outside, as long as
	 * the circle is smaller than a hemisphere */
	p->ref[0] = -dir[0]; p->ref[1] = -dir[1]; p->ref[2] = -dir[2];
	if (cs < 0.0) {
		/* larger than a hemisphere: the centre is outside instead */
		memcpy (p->ref, dir, sizeof (p->ref));
	}
}

static void parse (struct PSurf *s)
{
	unsigned int p = s->feat_addr;
	unsigned int state = s->seed;
	double verts[MAX_POLY_VERTS][3];
	int d7;

	for (;;) {
		d7 = rdbyte (&p);
		if (d7 == 0) break;

		if (d7 & 0x80) {
			/* l3db7c: circle(s) on the planet surface */
			double dir[3];
			int ang;
			dir[0] = rdbyte (&p);
			dir[1] = rdbyte (&p);
			dir[2] = rdbyte (&p);
			ang = rdbyte (&p) & 0xff;
			if (norm3 (dir) == 0.0) continue;
			if (d7 & 0x40) {
				/* the opposite circle too (both polar caps), first */
				double o[3] = { -dir[0], -dir[1], -dir[2] };
				add_circle (s, d7 & 0x1c, o, ang, &state);
			}
			add_circle (s, d7 & 0x1c, dir, ang, &state);
		} else {
			/* L3d41e: polygon */
			struct PSPoly *poly;
			int frac, n = 0, x, y, z, i;

			frac = rdbyte (&p);
			x = rdbyte (&p);
			for (;;) {
				y = rdbyte (&p);
				z = rdbyte (&p);
				if (n == 0 && STMemory_ReadByte (p) == 0) {
					/* bounding circle of the feature, used by the ST to
					 * tell whether the camera is inside it */
					p += 2;
					x = rdbyte (&p);
					continue;
				}
				if (n < MAX_POLY_VERTS) {
					verts[n][0] = x; verts[n][1] = y; verts[n][2] = z;
					if (norm3 (verts[n]) > 0.0) n++;
				}
				x = rdbyte (&p);
				if (x == 0) break;
			}
			if (n < 2) {
				state = rng_step (state);
				continue;
			}
			if (s->npolys >= MAX_POLYS) break;
			poly = &s->polys[s->npolys++];
			memset (poly, 0, sizeof (*poly));
			poly->code = d7 & 0x3c;
			poly->frac = frac;
			poly->n = n;
			poly->v = malloc (n * sizeof (*poly->v));
			poly->state = malloc (n * sizeof (*poly->state));
			memcpy (poly->v, verts, n * sizeof (*poly->v));
			/* one generator step per edge, closing edge included */
			for (i = 0; i < n; i++) {
				poly->state[i] = state;
				state = rng_step (state);
			}
			poly_finish (poly);
		}
	}
}

#define PSURF_CACHE	8
static struct PSurf *cache[PSURF_CACHE];
static int cache_next;

static void psurf_free (struct PSurf *s)
{
	int i;
	for (i = 0; i < s->npolys; i++) {
		free (s->polys[i].v);
		free (s->polys[i].state);
	}
	free (s);
}

struct PSurf *psurf_get (unsigned int feat_addr, unsigned int seed, int radius_word, int detail)
{
	struct PSurf *s;
	int i, m;

	for (i = 0; i < PSURF_CACHE; i++) {
		s = cache[i];
		if (s && s->feat_addr == feat_addr && s->seed == seed &&
		    s->radius_word == radius_word && s->detail == detail)
			return s;
	}

	s = calloc (1, sizeof (*s));
	s->feat_addr = feat_addr;
	s->seed = seed;
	s->radius_word = radius_word;
	s->detail = detail;
	/* l3cdd8: normalize the radius word to 0x4000..0x7fff */
	m = radius_word & 0xffff;
	if (m == 0) m = 0x4000;
	while (!(m & 0x4000)) m <<= 1;
	s->m = m;
	parse (s);

	if (cache[cache_next]) psurf_free (cache[cache_next]);
	cache[cache_next] = s;
	cache_next = (cache_next + 1) % PSURF_CACHE;
	return s;
}

/* ---- outline generation ---------------------------------------------- */

struct Leaf {
	double a[3], b[3];
};

struct Ctx {
	struct PSurf *s;
	/* patch cone: centre and half angle */
	double c[3], cone;
	/* stop splitting below this arc length */
	double leaf_ang;
	struct Leaf *leaves;
	int nleaves, maxleaves;
};

static void emit (struct Ctx *x, const double a[3], const double b[3])
{
	if (x->nleaves == x->maxleaves) {
		x->maxleaves = x->maxleaves ? 2*x->maxleaves : 4096;
		x->leaves = realloc (x->leaves, x->maxleaves * sizeof (struct Leaf));
	}
	memcpy (x->leaves[x->nleaves].a, a, sizeof (x->leaves[0].a));
	memcpy (x->leaves[x->nleaves].b, b, sizeof (x->leaves[0].b));
	x->nleaves++;
}

/* Can an arc from a to b (plus a displacement of up to 'slack' radians)
 * come near the patch? */
static int arc_near_patch (struct Ctx *x, const double a[3], const double b[3],
			   double len, double slack)
{
	double mid[3] = { a[0]+b[0], a[1]+b[1], a[2]+b[2] };
	if (norm3 (mid) == 0.0) return 1;
	return angle3 (mid, x->c) < x->cone + 0.5*len + slack;
}

/* L3de2e: split a polygon edge, with the ST's fractal displacement */
static void split_edge (struct Ctx *x, const struct PSPoly *p,
			const double a[3], const double b[3], unsigned int state, int level)
{
	const struct PSurf *s = x->s;
	double len = angle3 (a, b);
	double slack = 0.0, amp = 0.0, mid[3];
	unsigned int s1, s2;
	int d6pre = 0;

	if (p->frac) {
		d6pre = 2*(p->frac + 16 + level) - 12 - s->detail;
		if (d6pre >= 0) {
			/* largest possible displacement here and below it:
			 * sqrt(3) axes, halving at each level */
			amp = 32767.0 / ldexp (1.0, d6pre >> 1);
			slack = 2.0 * 1.7320508 * amp / (2.0 * s->m);
		}
	}

	if (len < x->leaf_ang || level >= MAX_LEVEL ||
	    !arc_near_patch (x, a, b, len, slack)) {
		emit (x, a, b);
		return;
	}

	/* L3ddc4 steps the generator on every edge */
	s1 = rng_step (state);
	s2 = s1;
	mid[0] = s->m * (a[0] + b[0]);
	mid[1] = s->m * (a[1] + b[1]);
	mid[2] = s->m * (a[2] + b[2]);

	if (p->frac && d6pre >= 0) {
		unsigned int d7 = (s1 << 16) | (s1 >> 16);
		unsigned int w = d7 & 0xffff;
		int d6 = d6pre >> 1, axis;
		double step;

		s2 = s1 + d7;
		/* N flag of the swap */
		if (d7 & 0x80000000) d6++;
		step = 32767.0 / ldexp (1.0, d6);
		for (axis = 0; axis < 3; axis++) {
			int bit = (w >> 14) & 1;
			w = (w << 1) & 0xffff;
			if (bit) {
				int neg = (w >> 14) & 1;
				w = (w << 1) & 0xffff;
				/* the rows of the rotation matrix at -36(a6) are the
				 * model axes seen in viewing space */
				mid[axis] += neg ? -step : step;
			}
		}
	}
	if (norm3 (mid) == 0.0) {
		emit (x, a, b);
		return;
	}

	split_edge (x, p, a, mid, s2, level + 1);
	split_edge (x, p, mid, b, rng_step (s2), level + 1);
}

/* circles are split along the small circle, without displacement */
static void split_circle (struct Ctx *x, const struct PSPoly *p,
			  const double a[3], const double b[3], int level)
{
	double len = angle3 (a, b);
	double mid[3], d[3];
	int i;

	if (len < x->leaf_ang || level >= MAX_LEVEL ||
	    !arc_near_patch (x, a, b, len, 0.5*len)) {
		emit (x, a, b);
		return;
	}
	for (i = 0; i < 3; i++) d[i] = (a[i] - p->coff[i]) + (b[i] - p->coff[i]);
	if (norm3 (d) == 0.0) {
		emit (x, a, b);
		return;
	}
	for (i = 0; i < 3; i++) mid[i] = p->coff[i] + p->crad * d[i];
	norm3 (mid);
	split_circle (x, p, a, mid, level + 1);
	split_circle (x, p, mid, b, level + 1);
}

/* ---- rasterization ----------------------------------------------------- */

/* Do the great circle arcs p1-p2 and q1-q2 (each shorter than half a
 * circle) cross? */
static int arcs_cross (const double p1[3], const double p2[3],
		       const double q1[3], const double q2[3])
{
	double n1[3], n2[3], X[3], sp[3], sq[3];
	double d1, d2, d3, d4;

	cross3d (p1, p2, n1);
	d1 = dot3 (n1, q1);
	d2 = dot3 (n1, q2);
	if ((d1 > 0.0) == (d2 > 0.0)) return 0;
	cross3d (q1, q2, n2);
	d3 = dot3 (n2, p1);
	d4 = dot3 (n2, p2);
	if ((d3 > 0.0) == (d4 > 0.0)) return 0;
	/* each arc crosses the other's great circle once, at +X or -X */
	cross3d (n1, n2, X);
	sp[0] = p1[0]+p2[0]; sp[1] = p1[1]+p2[1]; sp[2] = p1[2]+p2[2];
	sq[0] = q1[0]+q2[0]; sq[1] = q1[1]+q2[1]; sq[2] = q1[2]+q2[2];
	return (dot3 (X, sp) > 0.0) == (dot3 (X, sq) > 0.0);
}

/* Is direction q inside the (approximated) outline leaves[first..last)? */
static int inside (const struct Leaf *leaves, int first, int last,
		   const double q[3], const double ref[3])
{
	double mid[3], path[3][3];
	int i, j, n = 0;

	/* walk from q to ref, via the midpoint so both arcs stay short */
	mid[0] = q[0] + ref[0]; mid[1] = q[1] + ref[1]; mid[2] = q[2] + ref[2];
	if (norm3 (mid) < 1e-6) perp3 (q, mid);
	memcpy (path[0], q, sizeof (path[0]));
	memcpy (path[1], mid, sizeof (path[1]));
	memcpy (path[2], ref, sizeof (path[2]));

	for (i = first; i < last; i++)
		for (j = 0; j < 2; j++)
			n += arcs_cross (leaves[i].a, leaves[i].b, path[j], path[j+1]);
	return n & 1;
}

struct Seg {
	double x0, y0, x1, y1;
	int code;
};

struct Cross {
	float x;
	int code;
};

static int cmp_cross (const void *a, const void *b)
{
	float xa = ((const struct Cross *) a)->x, xb = ((const struct Cross *) b)->x;
	return (xa > xb) - (xa < xb);
}

struct YCross {
	double y;
	int code;
};

static int cmp_ycross (const void *a, const void *b)
{
	double ya = ((const struct YCross *) a)->y, yb = ((const struct YCross *) b)->y;
	return (ya > yb) - (ya < yb);
}

void psurf_raster (struct PSurf *s, const struct PPatch *pt, unsigned char *codes)
{
	struct Ctx x;
	struct Seg *segs = NULL;
	struct Cross *cross;
	struct YCross *yc = NULL;
	int *row_count, *row_start;
	int nsegs = 0, maxsegs = 0, nyc = 0, maxyc = 0;
	int n = pt->n, i, j, k, pi, base_code = 0;
	double texel = 2.0 * pt->half / n;
	double xl = -pt->half - texel, yb = -pt->half - texel;
	double q0[3];
	int *left;

	memset (&x, 0, sizeof (x));
	x.s = s;
	memcpy (x.c, pt->c, sizeof (x.c));
	/* the extended square [xl, half]^2 has to fit in the cone */
	x.cone = atan ((pt->half + 2*texel) * 1.4142136) + 0.01;
	/* texel size at the patch centre, the smallest one */
	x.leaf_ang = texel;

	for (i = 0; i < 3; i++) q0[i] = pt->c[i] + xl*pt->u[i] + yb*pt->v[i];
	norm3 (q0);

	for (pi = 0; pi < s->npolys; pi++) {
		const struct PSPoly *p = &s->polys[pi];
		int first = x.nleaves;

		for (i = 0; i < p->n; i++) {
			const double *a = p->v[i], *b = p->v[(i+1) % p->n];
			if (p->circle) split_circle (&x, p, a, b, 0);
			else split_edge (&x, p, a, b, p->state[i], 0);
		}
		if (inside (x.leaves, first, x.nleaves, q0, p->ref))
			base_code ^= p->code;

		/* project what can reach the square into the patch plane */
		for (i = first; i < x.nleaves; i++) {
			const double *a = x.leaves[i].a, *b = x.leaves[i].b;
			double ca = dot3 (a, pt->c), cb = dot3 (b, pt->c);
			struct Seg *sg;

			if (ca < 0.05 || cb < 0.05) continue;
			if (nsegs == maxsegs) {
				maxsegs = maxsegs ? 2*maxsegs : 4096;
				segs = realloc (segs, maxsegs * sizeof (*segs));
			}
			sg = &segs[nsegs++];
			sg->x0 = dot3 (a, pt->u) / ca;
			sg->y0 = dot3 (a, pt->v) / ca;
			sg->x1 = dot3 (b, pt->u) / cb;
			sg->y1 = dot3 (b, pt->v) / cb;
			sg->code = p->code;
		}
	}
	free (x.leaves);

	/* colour at the start of every row: walk up the line x = xl from q0 */
	left = malloc (n * sizeof (int));
	for (i = 0; i < nsegs; i++) {
		struct Seg *sg = &segs[i];
		double y;
		if ((sg->x0 <= xl) == (sg->x1 <= xl)) continue;
		y = sg->y0 + (xl - sg->x0) * (sg->y1 - sg->y0) / (sg->x1 - sg->x0);
		if (y <= yb) continue;
		if (nyc == maxyc) {
			maxyc = maxyc ? 2*maxyc : 256;
			yc = realloc (yc, maxyc * sizeof (*yc));
		}
		yc[nyc].y = y;
		yc[nyc++].code = sg->code;
	}
	qsort (yc, nyc, sizeof (*yc), cmp_ycross);
	{
		int c = base_code;
		k = 0;
		for (j = 0; j < n; j++) {
			double y = -pt->half + (j + 0.5) * texel;
			while (k < nyc && yc[k].y < y) c ^= yc[k++].code;
			left[j] = c;
		}
	}
	free (yc);

	/* bucket the crossings of every row's centre line, then fill */
	row_count = calloc (n + 1, sizeof (int));
	row_start = malloc ((n + 1) * sizeof (int));
	for (k = 0; k < 2; k++) {
		for (i = 0; i < nsegs; i++) {
			struct Seg *sg = &segs[i];
			double ylo = sg->y0 < sg->y1 ? sg->y0 : sg->y1;
			double yhi = sg->y0 < sg->y1 ? sg->y1 : sg->y0;
			int j0 = (int) ceil ((ylo + pt->half) / texel - 0.5);
			int j1 = (int) floor ((yhi + pt->half) / texel - 0.5);
			if (j0 < 0) j0 = 0;
			if (j1 > n-1) j1 = n-1;
			for (j = j0; j <= j1; j++) {
				double y = -pt->half + (j + 0.5) * texel;
				double xc;
				if ((sg->y0 <= y) == (sg->y1 <= y)) continue;
				xc = sg->x0 + (y - sg->y0) * (sg->x1 - sg->x0) / (sg->y1 - sg->y0);
				/* left of the start line: already in left[j] */
				if (xc <= xl) continue;
				if (k == 0) {
					row_count[j]++;
					continue;
				}
				cross[row_start[j]].x = (float) xc;
				cross[row_start[j]++].code = sg->code;
			}
		}
		if (k == 0) {
			int tot = 0;
			for (j = 0; j < n; j++) {
				row_start[j] = tot;
				tot += row_count[j];
			}
			cross = malloc ((tot + 1) * sizeof (*cross));
		}
	}
	/* row_start[j] now points at the end of row j */
	for (j = 0; j < n; j++) {
		/* row j's crossings run from the previous row's end to its own */
		int start = j ? row_start[j-1] : 0, end = row_start[j];
		int c = left[j], xi = 0;
		unsigned char *row = codes + j*n;

		qsort (cross + start, end - start, sizeof (*cross), cmp_cross);
		for (i = start; i < end; i++) {
			int xe = (int) ceil ((cross[i].x + pt->half) / texel - 0.5);
			if (xe > n) xe = n;
			for (; xi < xe; xi++) row[xi] = c;
			c ^= cross[i].code;
		}
		for (; xi < n; xi++) row[xi] = c;
	}

	free (cross);
	free (row_count);
	free (row_start);
	free (left);
	free (segs);
}

int psurf_light_code (int flags, double cos_sun)
{
	if (!(flags & 0x80)) return 0;
	if (cos_sun <= 0.0) return 0;
	if (!(flags & 0x40)) return 0x20;
	/* small circles 75.5 and 82.8 degrees from the sub-solar point */
	if (flags & 0x20) {
		if (cos_sun > 0.25) return 0x30;
		if (cos_sun > 0.125) return 0x10;
		return 0x20;
	}
	if (cos_sun > 0.25) return 0x10;
	return 0x20;
}
