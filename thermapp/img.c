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
thermapp_img_nuc(const struct thermapp_cal *cal, const union thermapp_frame *frame, float *out, int transient_enabled, float temp_delta)
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
	} else if (cal->cur_set < CAL_SETS) {
		const float *nuc_offset       = &cal->nuc_offset[nuc_start];
		const float *nuc_px           = &cal->nuc_px[nuc_start];
		const float *nuc_px2          = &cal->nuc_px2[nuc_start];
		const float *nuc_px3          = &cal->nuc_px3[nuc_start];
		const float *nuc_px4          = &cal->nuc_px4[nuc_start];
		const float *nuc_tfpa         = &cal->nuc_tfpa[nuc_start];
		const float *nuc_tfpa2        = &cal->nuc_tfpa2[nuc_start];
		const float *nuc_tfpa_px      = &cal->nuc_tfpa_px[nuc_start];
		const float *nuc_tfpa2_px2    = &cal->nuc_tfpa2_px2[nuc_start];
		const float *transient_offset = &cal->transient_offset[nuc_start];
		const float *transient_delta  = &cal->transient_delta[nuc_start];

		for (size_t y = cal->img_h; y; --y) {
			for (size_t x = cal->img_w; x; --x) {
				float px = *pixels++;
				float tp = tfpa * px;
				float td = *transient_delta++ * temp_delta + *transient_offset++;
				float t2 = *nuc_tfpa2++ * tfpa + *nuc_tfpa++;
				float tp2 = *nuc_tfpa2_px2++ * tp + *nuc_tfpa_px++;
				float sum = *nuc_px4++ * px + *nuc_px3++;
				sum = sum * px + *nuc_px2++;
				sum = sum * px + *nuc_px++;
				sum = sum * px + *nuc_offset++;
				sum += t2 * tfpa;
				sum += tp2 * tp;

				if (transient_enabled) {
					sum += td;
				}

				if (sum < cal->dist_param[4]) {
					sum = sum * cal->dist_param[0] + cal->dist_param[1];
				} else {
					sum = sum * cal->dist_param[2] + cal->dist_param[3];
				}

				*out++ = sum;
			}
			nuc_offset       += nuc_row_adj;
			nuc_px           += nuc_row_adj;
			nuc_px2          += nuc_row_adj;
			nuc_px3          += nuc_row_adj;
			nuc_px4          += nuc_row_adj;
			nuc_tfpa         += nuc_row_adj;
			nuc_tfpa2        += nuc_row_adj;
			nuc_tfpa_px      += nuc_row_adj;
			nuc_tfpa2_px2    += nuc_row_adj;
			transient_offset += nuc_row_adj;
			transient_delta  += nuc_row_adj;
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
thermapp_img_minmax(const struct thermapp_cal *cal, const float *in_px, float *out_px_min, float *out_px_max, size_t *out_i_min, size_t *out_i_max)
{
	float px_min, px_max;
	size_t i_min, i_max;

	px_min = px_max = *in_px++;
	i_min = i_max = 0;
	for (size_t i = 1; i < cal->img_w * cal->img_h; ++i) {
		float px = *in_px++;
		if (px_min > px) {
			px_min = px;
			i_min = i;
		}
		if (px_max < px) {
			px_max = px;
			i_max = i;
		}
	}
	if (out_px_min) *out_px_min = px_min;
	if (out_px_max) *out_px_max = px_max;
	if (out_i_min) *out_i_min = i_min;
	if (out_i_max) *out_i_max = i_max;
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
thermapp_img_hpf(const struct thermapp_cal *cal, uint16_t *io, float enhanced_ratio)
{
	// Compute HPF(image) as image - LPF(image).
	// LPF is computed as an exponential-weighted moving average across the image's pixels,
	// first over each row (left-to-right, then right-to-left, initial state from left column),
	// then over each column (top-to-bottom, then bottom-to-top, initial state from top row).
	// enhanced_ratio: range = [0.25f:5.0f], default = 1.25f to match the app.

	float alpha = 8.0f * enhanced_ratio / 100.0f;
	if (alpha < 0.0f || 1.0f < alpha) return;
#define LPF_SCALE 8 // Fixed-point scale factor
	uint32_t alpha_scaled = alpha * (float)(1 << LPF_SCALE);
	uint32_t beta_scaled = (1 << LPF_SCALE) - alpha_scaled;

	size_t w = cal->img_w;
	size_t h = cal->img_h;
#define LPF_RES 2 // RES:1 input downsampling during LPF
	size_t w_div = (w + LPF_RES - 1) / LPF_RES;
	size_t h_div = (h + LPF_RES - 1) / LPF_RES;
	if (!w_div || !h_div) return;
	size_t w_mod = w - (w_div - 1) * LPF_RES;
	size_t h_mod = h - (h_div - 1) * LPF_RES;

#define LPF_WIDTH_MAX  ((FRAME_WIDTH_MAX  + LPF_RES - 1) / LPF_RES)
#define LPF_HEIGHT_MAX ((FRAME_HEIGHT_MAX + LPF_RES - 1) / LPF_RES)
	uint32_t sy_buf[LPF_WIDTH_MAX];
	uint16_t lpf_buf[LPF_WIDTH_MAX * LPF_HEIGHT_MAX];

	uint16_t *lpf = lpf_buf;
	for (size_t y = 0; y < h_div; ++y) {
		// First column passed as-is to init the row filter state,
		*lpf = *io;
		uint32_t sx_scaled = *lpf << LPF_SCALE;

		io += LPF_RES;
		lpf += 1;

		for (size_t x = 1; x < w_div; ++x) {
			// Left-to-right pass on this row.
			sx_scaled = ((beta_scaled * sx_scaled) >> LPF_SCALE) + (alpha_scaled * *io);
			*lpf = sx_scaled >> LPF_SCALE;

			io += LPF_RES;
			lpf += 1;
		}

		uint32_t *sy_scaled = sy_buf + w_div;
		for (size_t x = 0; x < w_div; ++x) {
			io -= LPF_RES;
			lpf -= 1;
			sy_scaled -= 1;

			// Right-to-left pass on this row.
			sx_scaled = ((beta_scaled * sx_scaled) >> LPF_SCALE) + (alpha_scaled * *lpf);
			// Skip the write-back to lpf, the filter output is used directly below.

			// Top-to-bottom pass on each column.
			// Init the column filter state on the first row.
			*sy_scaled = y ? ((beta_scaled * *sy_scaled) + (alpha_scaled * sx_scaled)) >> LPF_SCALE : sx_scaled;
			*lpf = *sy_scaled >> LPF_SCALE;
		}

		io += LPF_RES * w;
		lpf += w_div;
	}

	for (size_t y = 0; y < h_div; ++y) {
		io -= LPF_RES * (w - w_div);

		uint32_t *sy_scaled = sy_buf + w_div;
		for (size_t x = 0; x < w_div; ++x) {
			io -= LPF_RES;
			lpf -= 1;
			sy_scaled -= 1;

			// Bottom-to-top pass on each column.
			*sy_scaled = ((beta_scaled * *sy_scaled) >> LPF_SCALE) + (alpha_scaled * *lpf);
			// Skip the write-back to lpf, the filter output is used directly below.

			// s is the low-frequency component.
			// Subtract it out to leave the high-frequency component.
			// Result will be centered around 0, shift it to the middle of the range of uint16_t.
			uint32_t s = *sy_scaled >> LPF_SCALE;
			s -= UINT16_MAX / 2;

			size_t rows = y ? LPF_RES : h_mod;
			size_t cols = x ? LPF_RES : w_mod;
			for (size_t j = 0; j < rows; ++j) {
				for (size_t i = 0; i < cols; ++i) {
					io[j * w + i] -= s;
				}
			}
		}
	}
}

void
thermapp_img_lut(const struct thermapp_cal *cal, const uint16_t *in, uint8_t *lut, float ignore_ratio, float max_gain)
{
	unsigned bins[UINT16_MAX+1];
	memset(bins, 0, sizeof bins);

	// Compute histogram.
	for (size_t i = cal->img_w * cal->img_h; i; --i) {
		bins[*in++] += 1;
	}

	// Optionally discard outlier bins.
	// ignore_ratio: range = [0.0f:1.0f), default = 0.0f to match the app,
	// but in practice should be [0.0f:0.5f) otherwise all bins are discarded.
	unsigned ignore_px = ignore_ratio * (cal->img_w * cal->img_h);
	unsigned n = 0;
	size_t lo = 0, hi = UINT16_MAX+1;
	while (n < ignore_px && hi) {
		hi -= 1;
		n += bins[hi];
		bins[hi] = 0;
	}
	n = 0;
	while (n < ignore_px && lo < hi) {
		n += bins[lo];
		bins[lo] = 0;
		lo += 1;
	}

	// Number the non-empty bins from 1 to n.
	n = 0;
	for (size_t i = lo; i < UINT16_MAX+1; ++i) {
		if (bins[i]) {
			n += 1;
		}
		bins[i] = n;
	}

	// Scale the bins range-axis from [0:n] to [0:UINT8_MAX], then filter.
	// max_gain, when enabled: 3.0f (TH, Enhanced), 0.45f (TH, Thermography), 1.0f (otherwise) to match the app.
#define LUT_SCALE 8
#define LUT_RANGE_SCALED (((UINT8_MAX+1) << LUT_SCALE) - 1)
	unsigned offset_scaled = 0;
	unsigned gain_scaled = n ? LUT_RANGE_SCALED / n : 0;
	unsigned max_gain_scaled = max_gain * (float)(1 << LUT_SCALE);
	if (max_gain_scaled && gain_scaled > max_gain_scaled) {
		gain_scaled = max_gain_scaled;
		offset_scaled = (LUT_RANGE_SCALED - (n * gain_scaled)) / 2;
	}
	for (size_t i = 0; i < UINT16_MAX+1; ++i) {
		unsigned new = (gain_scaled * bins[i] + offset_scaled) >> LUT_SCALE;
#define LUT_ALPHA 26                     //  26/256 ~= 0.1
#define LUT_BETA ((1 << 8) - LUT_ALPHA)  // 230/256 ~= 0.9
		lut[i] = (LUT_BETA * lut[i] + LUT_ALPHA * new) >> 8;
	}
}
