/*
 * Planet surface maps for the GL renderer.
 *
 * See planet.c for how the ST renderer draws planet surfaces (fe2.s
 * L3cd9c_ProjectPlanet and friends) and how this reproduces it.
 */
#ifndef _PLANET_H
#define _PLANET_H

/* An opaque, parsed planet surface description (continents, seas, ice
 * caps...). Owned by the cache in planet.c. */
struct PSurf;

/* A square gnomonic patch of the planet surface, in model coordinates:
 * the texel at (s, t) in [0,1]^2 covers the direction
 *	c + ((2s-1)*half) * u + ((2t-1)*half) * v
 * c, u, v must be orthonormal. */
struct PPatch {
	double c[3], u[3], v[3];
	double half;
	int n;
};

/* Look up (or parse and cache) the surface features of a planet.
 * feat_addr:	68k address of the feature list (fe2.s planet_features)
 * seed:	the planet object's random seed (long at 118(object))
 * radius_word:	high word of the planet model's radius long
 * detail:	L60d4_optdetail2 */
struct PSurf *psurf_get (unsigned int feat_addr, unsigned int seed,
			 int radius_word, int detail);

/* Rasterize the XOR'ed feature codes (multiples of 4, 0..0x3c) of the
 * patch into codes[n*n], row major, row 0 at t = 0. */
void psurf_raster (struct PSurf *s, const struct PPatch *p, unsigned char *codes);

/* Light band code the ST renderer XORs over the feature codes for a surface
 * direction whose cosine to the sun is cos_sun (see planet.c). flags is the
 * high byte of the first word of the planet model. */
int psurf_light_code (int flags, double cos_sun);

#endif /* _PLANET_H */
