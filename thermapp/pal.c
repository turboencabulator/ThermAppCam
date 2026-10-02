// SPDX-FileCopyrightText: 2026 Kyle Guinn <elyk03@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thermapp.h"

#include <math.h>
#include <string.h>

// Macros to construct colors/change the memory layout.  Currently:
// B' G' R' X  (V4L2_PIX_FMT_XBGR32) on little-endian machines
// X  R' G' B' (V4L2_PIX_FMT_XRGB32) on big-endian machines
#define SHIFT_R 16
#define SHIFT_G  8
#define SHIFT_B  0
#define RGB(rrggbb) rrggbb

// All palette_* functions can either modify and return their `buf` argument, or
// can return a constant array.  All palette arrays currently have 256 elements.
// Modify & return is preferred in case this length needs to change.

static const uint32_t *
palette_whitehot(uint32_t *buf)
{
	// R' = G' = B' = i
	for (size_t i = 0; i < UINT8_MAX+1; ++i) {
		buf[i] = i << SHIFT_R
		       | i << SHIFT_G
		       | i << SHIFT_B;
	}
	return buf;
}

static const uint32_t *
palette_blackhot(uint32_t *buf)
{
	// R' = G' = B' = 255 - i
	for (size_t i = 0; i < UINT8_MAX+1; ++i) {
		buf[i] = (UINT8_MAX - i) << SHIFT_R
		       | (UINT8_MAX - i) << SHIFT_G
		       | (UINT8_MAX - i) << SHIFT_B;
	}
	return buf;
}

// These next palettes are defined as (piecewise) linear in the
// non-linear R'G'B' space and therefore may not be perceptually uniform.
// https://blog.johnnovak.net/2016/09/21/what-every-coder-should-know-about-gamma/#gradients
// https://en.wikipedia.org/wiki/Mach_bands

static const uint32_t *
palette_vivid(uint32_t *buf)
{
	// R' = i
	// B' = 255 - R'
	// G' = 255 - (R' * B' / 64)
	for (size_t i = 0; i < UINT8_MAX+1; ++i) {
		buf[i] = i                                        << SHIFT_R
		       | (UINT8_MAX - (i * (UINT8_MAX - i) >> 6)) << SHIFT_G
		       | (UINT8_MAX - i)                          << SHIFT_B;
	}
	return buf;
}

static const uint32_t *
palette_iron(uint32_t *buf)
{
	// Linear interpolate in R'G'B' space:
	//   i    [0] [256*]
	//   R'   86   253
	//   G'    0   250
	//   B'  154     0
	// Round to nearest.
	for (size_t i = 0; i < UINT8_MAX+1; ++i) {
		size_t j = UINT8_MAX+1 - i;
		buf[i] = (( 86 * j) + (253 * i) + 0x80) >> 8 << SHIFT_R
		       | ((  0 * j) + (250 * i) + 0x80) >> 8 << SHIFT_G
		       | ((154 * j) + (  0 * i) + 0x80) >> 8 << SHIFT_B;
	}
	return buf;
}

static const uint32_t *
palette_rainbow(uint32_t *buf)
{
	// Linear interpolate in R'G'B' space:
	//   i    [0]  [31]  [95] [159] [223] [255]
	//   R'    0     0     0   255   255   127
	//   G'    0     0   255   255     0     0
	//   B'  131   255   255     0     0     0
	// All segments have a slope of +/- 4 except where clamped to 255 or 0 at the right endpoint.
	size_t i = 0;
	buf[i] = (131 << SHIFT_B); i += 1;
	while (i < 32) {
		buf[i] = buf[i - 1] + (4 << SHIFT_B); i += 1;
	}
	while (i < 95) {
		buf[i] = buf[i - 1] + (4 << SHIFT_G); i += 1;
	}
	buf[i] = (255 << SHIFT_B) | (255 << SHIFT_G); i += 1;
	while (i < 159) {
		buf[i] = buf[i - 1] + (4 << SHIFT_R) - (4 << SHIFT_B); i += 1;
	}
	buf[i] = (255 << SHIFT_G) | (255 << SHIFT_R); i += 1;
	while (i < 223) {
		buf[i] = buf[i - 1] - (4 << SHIFT_G); i += 1;
	}
	buf[i] = (255 << SHIFT_R); i += 1;
	while (i < 256) {
		buf[i] = buf[i - 1] - (4 << SHIFT_R); i += 1;
	}
	return buf;
}

static const uint32_t *
palette_psy(uint32_t *buf)
{
	// Linear interpolate in R'G'B' space:
	//   i    [0]  [16]  [32]  [48]  [80] [112] [128] [144] [160] [176] [192] [240] [256*]
	//   R'    0   130   100    60     0   ...   ...     0   130   255   ...   255   100
	//   G'    0   ...   ...   ...     0   180   ...   200   230   255   200    50    50
	//   B'    0   130   ...   ...   150   180   100   ...    50   ...   ...   ...    50
	// Points on segments with positive/negative slope are rounded down/up, respectively.
	static const float slope[16][3] = {
		{  130 / 16.0f,   0 / 16.0f, 130 / 16.0f },
		{  -30 / 16.0f,   0 / 16.0f,   5 / 16.0f },
		{  -40 / 16.0f,   0 / 16.0f,   5 / 16.0f },
		{  -30 / 16.0f,   0 / 16.0f,   5 / 16.0f },
		{  -30 / 16.0f,   0 / 16.0f,   5 / 16.0f },
		{    0 / 16.0f,  90 / 16.0f,  15 / 16.0f },
		{    0 / 16.0f,  90 / 16.0f,  15 / 16.0f },
		{    0 / 16.0f,  10 / 16.0f, -80 / 16.0f },
		{    0 / 16.0f,  10 / 16.0f, -25 / 16.0f },
		{  130 / 16.0f,  30 / 16.0f, -25 / 16.0f },
		{  125 / 16.0f,  25 / 16.0f,   0 / 16.0f },
		{    0 / 16.0f, -55 / 16.0f,   0 / 16.0f },
		{    0 / 16.0f, -50 / 16.0f,   0 / 16.0f },
		{    0 / 16.0f, -50 / 16.0f,   0 / 16.0f },
		{    0 / 16.0f, -50 / 16.0f,   0 / 16.0f },
		{ -155 / 16.0f,   0 / 16.0f,   0 / 16.0f },
	};
	int r = 0, g = 0, b = 0;
	buf[0] = 0;
	for (size_t i = 0; i < 255; ++i) {
		size_t j = i >> 4;
		size_t k = (i & 0xf) + 1;
		int dr = (int)(k * slope[j][0]);
		int dg = (int)(k * slope[j][1]);
		int db = (int)(k * slope[j][2]);
		buf[i+1] = (r + dr) << SHIFT_R
		         | (g + dg) << SHIFT_G
		         | (b + db) << SHIFT_B;
		if (k == 16) {
			r += dr;
			g += dg;
			b += db;
		}
	}
	return buf;
}

static const uint32_t *
palette_lava(void)
{
	static const uint32_t lava[] = {
		RGB(0x100002), RGB(0x110105), RGB(0x110108), RGB(0x12010b), RGB(0x13020f), RGB(0x130213), RGB(0x140217), RGB(0x14031b),
		RGB(0x150320), RGB(0x160424), RGB(0x160429), RGB(0x17042e), RGB(0x180533), RGB(0x190538), RGB(0x1a063e), RGB(0x1a0643),
		RGB(0x1b0749), RGB(0x1c074e), RGB(0x1d0854), RGB(0x1e095a), RGB(0x1f0960), RGB(0x200a65), RGB(0x200a6b), RGB(0x220b71),
		RGB(0x230b77), RGB(0x240c7c), RGB(0x250d82), RGB(0x250d88), RGB(0x270e8d), RGB(0x270f93), RGB(0x291098), RGB(0x2a119d),
		RGB(0x2b11a3), RGB(0x2c12a8), RGB(0x2d13ac), RGB(0x2e14b1), RGB(0x2f15b5), RGB(0x3016b9), RGB(0x3216be), RGB(0x3317c1),
		RGB(0x3418c5), RGB(0x3519c8), RGB(0x361acb), RGB(0x371bce), RGB(0x381cd0), RGB(0x391dd2), RGB(0x3a1ed3), RGB(0x3b1fd5),
		RGB(0x3c20d6), RGB(0x3d21d8), RGB(0x3e22d9), RGB(0x3f23d9), RGB(0x4024d9), RGB(0x4126d9), RGB(0x4227d9), RGB(0x4328d9),
		RGB(0x442ad9), RGB(0x452bd9), RGB(0x462cd9), RGB(0x472ed9), RGB(0x482fd9), RGB(0x4930d9), RGB(0x4a32d9), RGB(0x4a33d9),
		RGB(0x4c35d9), RGB(0x4d36d9), RGB(0x4e38d9), RGB(0x4f39d9), RGB(0x503bd9), RGB(0x513cd9), RGB(0x523ed9), RGB(0x533fd9),
		RGB(0x5441d9), RGB(0x5542d9), RGB(0x5644d9), RGB(0x5745d9), RGB(0x5946d9), RGB(0x5a48d9), RGB(0x5b49d9), RGB(0x5c4bd9),
		RGB(0x5d4cd9), RGB(0x5e4dd9), RGB(0x5f4fd9), RGB(0x6150d9), RGB(0x6251d9), RGB(0x6352d9), RGB(0x6453d9), RGB(0x6655d9),
		RGB(0x6756d9), RGB(0x6857d9), RGB(0x6958d9), RGB(0x6b59d9), RGB(0x6c5ad9), RGB(0x6d5bd9), RGB(0x6e5bd9), RGB(0x705cd9),
		RGB(0x715dd9), RGB(0x735ed9), RGB(0x745ed9), RGB(0x755ed9), RGB(0x775fd9), RGB(0x7860d9), RGB(0x7960d9), RGB(0x7c60d8),
		RGB(0x7e60d7), RGB(0x8160d5), RGB(0x8360d4), RGB(0x8660d2), RGB(0x895fd0), RGB(0x8b5fcd), RGB(0x8f5ecb), RGB(0x925dc9),
		RGB(0x955cc7), RGB(0x985ac4), RGB(0x9b59c1), RGB(0x9e58bf), RGB(0xa156bc), RGB(0xa455b9), RGB(0xa853b6), RGB(0xab52b3),
		RGB(0xae50b0), RGB(0xb24ead), RGB(0xb54daa), RGB(0xb84ba7), RGB(0xbb49a4), RGB(0xbe48a1), RGB(0xc1479d), RGB(0xc4459a),
		RGB(0xc74497), RGB(0xca4294), RGB(0xcd4191), RGB(0xcf408e), RGB(0xd23f8c), RGB(0xd43e89), RGB(0xd73e86), RGB(0xd93d84),
		RGB(0xdb3d81), RGB(0xde3d7f), RGB(0xe03d7d), RGB(0xe23d7a), RGB(0xe33d78), RGB(0xe53e76), RGB(0xe63e74), RGB(0xe83f72),
		RGB(0xe94070), RGB(0xeb416e), RGB(0xec426c), RGB(0xed436a), RGB(0xef4469), RGB(0xf04667), RGB(0xf14765), RGB(0xf24863),
		RGB(0xf24a61), RGB(0xf44b60), RGB(0xf44d5e), RGB(0xf54f5c), RGB(0xf6505b), RGB(0xf65259), RGB(0xf75457), RGB(0xf85656),
		RGB(0xf85854), RGB(0xf95a53), RGB(0xfa5c51), RGB(0xfa5e50), RGB(0xfa604e), RGB(0xfb624d), RGB(0xfb644c), RGB(0xfc664a),
		RGB(0xfc6849), RGB(0xfd6a48), RGB(0xfd6c46), RGB(0xfd6e45), RGB(0xfe7044), RGB(0xfe7242), RGB(0xfe7542), RGB(0xff7740),
		RGB(0xff783f), RGB(0xff7a3e), RGB(0xff7b3e), RGB(0xff7d3c), RGB(0xff7e3c), RGB(0xff7f3b), RGB(0xff8139), RGB(0xff8338),
		RGB(0xff8537), RGB(0xff8636), RGB(0xff8835), RGB(0xff8a34), RGB(0xff8b33), RGB(0xff8d31), RGB(0xff8f30), RGB(0xff912f),
		RGB(0xff932e), RGB(0xff952c), RGB(0xff962b), RGB(0xff982a), RGB(0xff9a29), RGB(0xff9c28), RGB(0xff9e28), RGB(0xffa028),
		RGB(0xffa228), RGB(0xffa428), RGB(0xffa628), RGB(0xffa728), RGB(0xffa928), RGB(0xffab28), RGB(0xffad28), RGB(0xffaf28),
		RGB(0xffb128), RGB(0xffb328), RGB(0xffb428), RGB(0xffb628), RGB(0xffb828), RGB(0xffba28), RGB(0xffbb28), RGB(0xffbd28),
		RGB(0xffbf28), RGB(0xffc128), RGB(0xffc228), RGB(0xffc428), RGB(0xffc628), RGB(0xffc728), RGB(0xffc928), RGB(0xffca28),
		RGB(0xffcc28), RGB(0xffcd28), RGB(0xffcf28), RGB(0xffd028), RGB(0xffd128), RGB(0xffd329), RGB(0xffd52e), RGB(0xffd833),
		RGB(0xffda39), RGB(0xffdc40), RGB(0xffdf47), RGB(0xffe14e), RGB(0xffe356), RGB(0xffe55f), RGB(0xffe668), RGB(0xffe872),
		RGB(0xffea7c), RGB(0xffec86), RGB(0xffee90), RGB(0xffef9a), RGB(0xfff1a3), RGB(0xfff2ac), RGB(0xfff4b6), RGB(0xfff5c0),
		RGB(0xfff6c9), RGB(0xfff8d2), RGB(0xfff9da), RGB(0xfffae2), RGB(0xfffbe9), RGB(0xfffcef), RGB(0xfffdf5), RGB(0xfffefb),
	};
	return lava;
}

static const uint32_t *
palette_green(void)
{
	static const uint32_t green[] = {
		RGB(0x100002), RGB(0x000000), RGB(0x000100), RGB(0x000300), RGB(0x000500), RGB(0x000700), RGB(0x000900), RGB(0x000a00),
		RGB(0x000b00), RGB(0x010c01), RGB(0x010d01), RGB(0x010e01), RGB(0x010f01), RGB(0x001000), RGB(0x001100), RGB(0x011201),
		RGB(0x011301), RGB(0x011401), RGB(0x011501), RGB(0x011601), RGB(0x011701), RGB(0x011801), RGB(0x011901), RGB(0x021a02),
		RGB(0x021b02), RGB(0x021c02), RGB(0x021d02), RGB(0x021e02), RGB(0x021f02), RGB(0x022002), RGB(0x022102), RGB(0x022202),
		RGB(0x022302), RGB(0x032303), RGB(0x032303), RGB(0x032403), RGB(0x032403), RGB(0x032403), RGB(0x032503), RGB(0x032603),
		RGB(0x032703), RGB(0x042704), RGB(0x042804), RGB(0x042804), RGB(0x042804), RGB(0x042804), RGB(0x042904), RGB(0x052a05),
		RGB(0x052b05), RGB(0x052e05), RGB(0x052f05), RGB(0x053005), RGB(0x053105), RGB(0x063106), RGB(0x063206), RGB(0x063206),
		RGB(0x063306), RGB(0x063406), RGB(0x063506), RGB(0x073607), RGB(0x073707), RGB(0x073807), RGB(0x073a07), RGB(0x073b07),
		RGB(0x073b07), RGB(0x083c08), RGB(0x083c08), RGB(0x083c08), RGB(0x083d08), RGB(0x083e08), RGB(0x083f08), RGB(0x084008),
		RGB(0x084108), RGB(0x094209), RGB(0x094309), RGB(0x094409), RGB(0x094509), RGB(0x094609), RGB(0x094709), RGB(0x094809),
		RGB(0x0a490a), RGB(0x0a490a), RGB(0x0a490a), RGB(0x0a4a0a), RGB(0x0a4a0a), RGB(0x0a4a0a), RGB(0x0a4a0a), RGB(0x0b4b0b),
		RGB(0x0b4b0b), RGB(0x0b4b0b), RGB(0x0b4b0b), RGB(0x0b4b0b), RGB(0x0b4c0b), RGB(0x0c4c0c), RGB(0x0c4c0c), RGB(0x0c4c0c),
		RGB(0x0c4d0c), RGB(0x0c4d0c), RGB(0x0c4d0c), RGB(0x0d4d0d), RGB(0x0d4d0d), RGB(0x0d4e0d), RGB(0x0d4e0d), RGB(0x0d4e0d),
		RGB(0x0d4e0d), RGB(0x0e4e0e), RGB(0x0e4f0e), RGB(0x0e4f0e), RGB(0x0e4f0e), RGB(0x0e4f0e), RGB(0x0e4f0e), RGB(0x0e500e),
		RGB(0x0f510f), RGB(0x0f520f), RGB(0x0f520f), RGB(0x0f530f), RGB(0x105410), RGB(0x105510), RGB(0x105610), RGB(0x105710),
		RGB(0x105810), RGB(0x115911), RGB(0x115a11), RGB(0x115b11), RGB(0x115c11), RGB(0x115d11), RGB(0x115e11), RGB(0x125f12),
		RGB(0x126012), RGB(0x126112), RGB(0x126212), RGB(0x126312), RGB(0x126412), RGB(0x136413), RGB(0x136513), RGB(0x136613),
		RGB(0x136713), RGB(0x136813), RGB(0x136913), RGB(0x146a14), RGB(0x146b14), RGB(0x146b14), RGB(0x146c14), RGB(0x146d14),
		RGB(0x146e14), RGB(0x156f15), RGB(0x156f15), RGB(0x157015), RGB(0x157115), RGB(0x157215), RGB(0x157315), RGB(0x167416),
		RGB(0x167516), RGB(0x167616), RGB(0x167716), RGB(0x167816), RGB(0x167916), RGB(0x167a16), RGB(0x177b17), RGB(0x177c17),
		RGB(0x177d17), RGB(0x177e17), RGB(0x177f17), RGB(0x178017), RGB(0x178117), RGB(0x178217), RGB(0x178317), RGB(0x178417),
		RGB(0x188518), RGB(0x188618), RGB(0x188618), RGB(0x188818), RGB(0x188b18), RGB(0x198d19), RGB(0x198f19), RGB(0x199119),
		RGB(0x1a931a), RGB(0x1a951a), RGB(0x1a971a), RGB(0x1b991b), RGB(0x1b9b1b), RGB(0x1c9e1c), RGB(0x1ca01c), RGB(0x1da21d),
		RGB(0x1da41d), RGB(0x1ea61e), RGB(0x1ea81e), RGB(0x1eaa1e), RGB(0x1fac1f), RGB(0x1fae1f), RGB(0x20b020), RGB(0x20b220),
		RGB(0x21b421), RGB(0x21b621), RGB(0x22b822), RGB(0x22ba22), RGB(0x23bc23), RGB(0x23be23), RGB(0x24c024), RGB(0x24c224),
		RGB(0x25c325), RGB(0x26c326), RGB(0x28c428), RGB(0x29c529), RGB(0x2ac62a), RGB(0x2bc72b), RGB(0x2cc82c), RGB(0x2dc92d),
		RGB(0x2eca2e), RGB(0x31cb31), RGB(0x33cc33), RGB(0x35cd35), RGB(0x37ce37), RGB(0x39cf39), RGB(0x3bd03b), RGB(0x3dd13d),
		RGB(0x41d241), RGB(0x44d244), RGB(0x46d346), RGB(0x48d448), RGB(0x4bd54b), RGB(0x4dd64d), RGB(0x50d750), RGB(0x55d855),
		RGB(0x58d958), RGB(0x5ada5a), RGB(0x5cdb5c), RGB(0x5fdc5f), RGB(0x62dd62), RGB(0x64dd64), RGB(0x69de69), RGB(0x6ede6e),
		RGB(0x73de73), RGB(0x7adf7a), RGB(0x80df80), RGB(0x82e082), RGB(0x87e087), RGB(0x8ce18c), RGB(0x96e196), RGB(0x99e199),
		RGB(0x9fe29f), RGB(0xa2e2a2), RGB(0xa5e3a5), RGB(0xa8e3a8), RGB(0xaae4aa), RGB(0xace4ac), RGB(0xafe4af), RGB(0xb4e5b4),
		RGB(0xb9e5b9), RGB(0xbce6bc), RGB(0xc3e6c3), RGB(0xc6e6c6), RGB(0xc8e7c8), RGB(0xcde7cd), RGB(0xd2e8d2), RGB(0xd3e8d3),
	};
	return green;
}

static const uint32_t *
palette_ironbow(uint32_t *buf)
{
	// https://stackoverflow.com/questions/28495390/thermal-imaging-palette
	static const double coeffs[3][6] = {
		{-39.1125,  2.19321,  -0.00532377, 4.18485e-6,  0.0,        0.0,        },
		{ -1.61706, 0.280643, -0.00808127, 6.73208e-5, -1.64251e-7, 1.28826e-10,},
		{ 30.466,   3.24907,  -0.0232532,  4.19544e-5, -1.05015e-8, 9.48804e-12,},
	};
	for (size_t i = 0; i < UINT8_MAX+1; ++i) {
		size_t j = 5;
		double x = (double)(433 * i) / (double)(UINT8_MAX+1);
		double r = coeffs[0][j];
		double g = coeffs[1][j];
		double b = coeffs[2][j];
		while (j-- > 0) {
			r = fma(r, x, coeffs[0][j]);
			g = fma(g, x, coeffs[1][j]);
			b = fma(b, x, coeffs[2][j]);
		}
		r = (r < 0.0) ? 0.0 : (r >= 255.0) ? 255.0 : floor(r);
		g = (g < 0.0) ? 0.0 : (g >= 255.0) ? 255.0 : floor(g);
		b = (b < 0.0) ? 0.0 : (b >= 255.0) ? 255.0 : floor(b);
		buf[i] = (int)r << SHIFT_R
		       | (int)g << SHIFT_G
		       | (int)b << SHIFT_B;
	}
	return buf;
}

const uint32_t *
thermapp_pal_select(const char *name, uint32_t *buf)
{
	if (!name
	 || strcmp(name, "whitehot") == 0) {
		return palette_whitehot(buf);
	} else if (strcmp(name, "blackhot") == 0) {
		return palette_blackhot(buf);
	} else if (strcmp(name, "vivid") == 0) {
		return palette_vivid(buf);
	} else if (strcmp(name, "iron") == 0) {
		return palette_iron(buf);
	} else if (strcmp(name, "rainbow") == 0) {
		return palette_rainbow(buf);
	} else if (strcmp(name, "psy") == 0) {
		return palette_psy(buf);
	} else if (strcmp(name, "lava") == 0) {
		return palette_lava();
	} else if (strcmp(name, "green") == 0) {
		return palette_green();
	} else if (strcmp(name, "ironbow") == 0) {
		return palette_ironbow(buf);
	} else {
		return NULL;
	}
}
