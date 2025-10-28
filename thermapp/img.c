// SPDX-FileCopyrightText: 2025 Kyle Guinn <elyk03@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thermapp.h"

#include <math.h>
#include <string.h>

static void
histogram(unsigned *bins, const uint16_t *pixels, size_t len, int bpp16)
{
	memset(bins, 0, 256 * sizeof *bins);

	if (bpp16) {
		while (len--) {
			bins[*pixels++ >> 8 & 0xff] += 1;
		}
	} else {
		while (len--) {
			bins[*pixels++ >> 4 & 0xff] += 1;
		}
	}
}

static double
center_of_mass(const unsigned *buf, size_t len)
{
	double weight = 1.0, sum = 0.0, wsum = 0.0;
	while (len--) {
		double sample = (double)*buf++;
		wsum += weight * sample;
		sum += sample;
		weight += 1.0;
	}
	return (wsum / sum) - 1.0;
}

int
thermapp_img_vgsk(const struct thermapp_cal *cal, const union thermapp_frame *frame)
{
	const uint16_t *pixels = (const uint16_t *)&frame->bytes[frame->header.data_offset];
	int vgsk = frame->header.VoutC;
	int min = cal->vgsk_min;
	int max = cal->vgsk_max;
	double target = cal->histogram_peak_target;

	// XXX: The app skips over this calculation on 640x480 cameras,
	//      but the skipped code has decisions to handle 640x480 cameras?
	if (frame->header.fpa_w != 640 && target != 0.0) {
		if (frame->header.fpa_w == 640) {
			min = 1392;
			max = 2949;
		}

		unsigned bins[256];
		histogram(bins, pixels, frame->header.data_w * frame->header.data_h, frame->header.fpa_w == 640);
		double cm = center_of_mass(bins, 256);
		int delta = (int)(((target * 256.0) - cm) / 7.0);
		int new = vgsk + delta;
		if (min < new && new < max) {
			vgsk = new;
		}
	}
	return vgsk;
}

void
thermapp_img_nuc(const struct thermapp_cal *cal, const union thermapp_frame *frame, float *out)
{
	float tfpa = frame->header.temp_fpa_diode;
	float vgsk = frame->header.VoutC;
	const uint16_t *pixels = (const uint16_t *)&frame->bytes[frame->header.data_offset];
	size_t nuc_start = cal->ofs_y * cal->nuc_w + cal->ofs_x;
	size_t nuc_row_adj = cal->nuc_w - cal->img_w;

	if (cal->cur_set == CAL_SET_NV) {
		const float *nuc_offset  = &cal->nuc_offset[nuc_start];
		const float *nuc_px      = &cal->nuc_px[nuc_start];
		const float *nuc_px2     = &cal->nuc_px2[nuc_start];
		const float *nuc_tfpa    = &cal->nuc_tfpa[nuc_start];
		const float *nuc_tfpa2   = &cal->nuc_tfpa2[nuc_start];
		const float *nuc_tfpa_px = &cal->nuc_tfpa_px[nuc_start];
		const float *nuc_vgsk    = &cal->nuc_vgsk[nuc_start];
		const float *nuc_vgsk2   = &cal->nuc_vgsk2[nuc_start];
		const float *nuc_vgsk_px = &cal->nuc_vgsk_px[nuc_start];

		for (size_t y = cal->img_h; y; --y) {
			for (size_t x = cal->img_w; x; --x) {
				float px = *pixels++;
				float t2 = *nuc_tfpa2++ * tfpa + *nuc_tfpa++;
				float v2 = *nuc_vgsk2++ * vgsk + *nuc_vgsk++;
				float p2 = *nuc_px2++ * px + *nuc_px++;
				p2 += *nuc_tfpa_px++ * tfpa;
				p2 += *nuc_vgsk_px++ * vgsk;
				float sum = p2 * px + *nuc_offset++;
				sum += t2 * tfpa;
				sum += v2 * vgsk;

				*out++ = sum;
			}
			nuc_offset  += nuc_row_adj;
			nuc_px      += nuc_row_adj;
			nuc_px2     += nuc_row_adj;
			nuc_tfpa    += nuc_row_adj;
			nuc_tfpa2   += nuc_row_adj;
			nuc_tfpa_px += nuc_row_adj;
			nuc_vgsk    += nuc_row_adj;
			nuc_vgsk2   += nuc_row_adj;
			nuc_vgsk_px += nuc_row_adj;
		}
	} else if (cal->cur_set != CAL_SETS) {
		const float *nuc_offset    = &cal->nuc_offset[nuc_start];
		const float *nuc_px        = &cal->nuc_px[nuc_start];
		const float *nuc_px2       = &cal->nuc_px2[nuc_start];
		const float *nuc_px3       = &cal->nuc_px3[nuc_start];
		const float *nuc_px4       = &cal->nuc_px4[nuc_start];
		const float *nuc_tfpa      = &cal->nuc_tfpa[nuc_start];
		const float *nuc_tfpa2     = &cal->nuc_tfpa2[nuc_start];
		const float *nuc_tfpa_px   = &cal->nuc_tfpa_px[nuc_start];
		const float *nuc_tfpa2_px2 = &cal->nuc_tfpa2_px2[nuc_start];

		for (size_t y = cal->img_h; y; --y) {
			for (size_t x = cal->img_w; x; --x) {
				float px = *pixels++;
				float tp = tfpa * px;
				float t2 = *nuc_tfpa2++ * tfpa + *nuc_tfpa++;
				float tp2 = *nuc_tfpa2_px2++ * tp + *nuc_tfpa_px++;
				float sum = *nuc_px4++ * px + *nuc_px3++;
				sum = sum * px + *nuc_px2++;
				sum = sum * px + *nuc_px++;
				sum = sum * px + *nuc_offset++;
				sum += t2 * tfpa;
				sum += tp2 * tp;

				*out++ = sum;
			}
			nuc_offset    += nuc_row_adj;
			nuc_px        += nuc_row_adj;
			nuc_px2       += nuc_row_adj;
			nuc_px3       += nuc_row_adj;
			nuc_px4       += nuc_row_adj;
			nuc_tfpa      += nuc_row_adj;
			nuc_tfpa2     += nuc_row_adj;
			nuc_tfpa_px   += nuc_row_adj;
			nuc_tfpa2_px2 += nuc_row_adj;
		}
	} else {
		const float *nuc_offset = &cal->nuc_offset[nuc_start];

		for (size_t y = cal->img_h; y; --y) {
			for (size_t x = cal->img_w; x; --x) {
				float px = *pixels++;
				float sum = px + *nuc_offset++;

				*out++ = sum;
			}
			nuc_offset += nuc_row_adj;
		}
	}
}

void
thermapp_img_bpr(const struct thermapp_cal *cal, float *io)
{
	size_t nuc_start = cal->ofs_y * cal->nuc_w + cal->ofs_x;
	size_t nuc_row_adj = cal->nuc_w - cal->img_w;
	const float *nuc_good = &cal->nuc_good[nuc_start];

	// Relative indexes of nearby/neighboring pixels.
	// rel_0 is positive/forward-looking (reading from a known-good input pixel),
	// all others are negative/backward-looking (reading from good-or-repaired output).
	int rel_0 = cal->bpr_i;
	int rel_w = -1;
	int rel_n = -cal->img_w;
	int rel_nw = rel_n - 1;
	int rel_ne = rel_n + 1;

	// If a pixel is bad, replace it with the average of previously-encountered
	// neighboring pixels (on the west, northwest, north, and northeast if present).
	// If none (i.e. the first pixel is bad), copy from a known-good nearby pixel.
	for (size_t y = 0; y < cal->img_h; ++y) {
		for (size_t x = 0; x < cal->img_w; ++x) {
			if (!*nuc_good++) {
				if (!y) {
					if (!x) {
						*io = io[rel_0];
					} else {
						*io = io[rel_w];
					}
				} else {
					float avg;
					if (!x) {
						avg = io[rel_n]
						    + io[rel_ne];
						avg /= 2.0f;
					} else {
						avg = io[rel_w]
						    + io[rel_nw]
						    + io[rel_n];
						if (x != cal->img_w - 1) {
							avg += io[rel_ne];
							avg /= 4.0f;
						} else {
							avg /= 3.0f;
						}
					}
					*io = avg;
				}
			}
			io += 1;
		}
		nuc_good += nuc_row_adj;
	}
}

void
thermapp_img_minmax(const struct thermapp_cal *cal, const float *in, float *min, float *max)
{
	float frame_min = INFINITY;
	float frame_max = -INFINITY;
	for (size_t i = cal->img_w * cal->img_h; i; --i) {
		float px = *in++;
		frame_min = fminf(frame_min, px);
		frame_max = fmaxf(frame_max, px);
	}
	*min = frame_min;
	*max = frame_max;
}

void
thermapp_img_quantize(const struct thermapp_cal *cal, const float *in, uint16_t *out)
{
	for (size_t i = cal->img_w * cal->img_h; i; --i) {
		float px = *in++ + 5000;
		if (px > UINT16_MAX) {
			*out++ = UINT16_MAX;
		} else if (px < 0) {
			*out++ = 0;
		} else {
			*out++ = (int)px;
		}
	}
}

void
thermapp_img_lut(const struct thermapp_cal *cal, const uint16_t *in, uint8_t *lut)
{
	unsigned bins[UINT16_MAX+1];
	memset(bins, 0, sizeof bins);

	// Compute histogram.
	for (size_t i = cal->img_w * cal->img_h; i; --i) {
		bins[*in++] += 1;
	}

	// Number the non-empty bins from 1 to n.
	unsigned n = 0;
	for (size_t i = 0; i < UINT16_MAX+1; ++i) {
		if (bins[i]) {
			n += 1;
		}
		bins[i] = n;
	}

	// Scale the bins range-axis from [0:n] to [0:UINT8_MAX], then filter.
#define LUT_ALPHA 26                     //  26/256 ~= 0.1
#define LUT_BETA ((1 << 8) - LUT_ALPHA)  // 230/256 ~= 0.9
	unsigned factor = (UINT8_MAX << 8) / n;
	for (size_t i = 0; i < UINT16_MAX+1; ++i) {
		unsigned new = (bins[i] * factor) >> 8;
		lut[i] = (LUT_BETA * lut[i] + LUT_ALPHA * new) >> 8;
	}
}
