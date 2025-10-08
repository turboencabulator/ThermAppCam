// SPDX-FileCopyrightText: 2025 Kyle Guinn <elyk03@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thermapp.h"

#include <limits.h>
#include <string.h>

static void
histogram(int *bins, const uint16_t *pixels, size_t len, int bpp16)
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
center_of_mass(const int *buf, size_t len)
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

		int bins[256];
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
thermapp_img_nuc(const struct thermapp_cal *cal, const union thermapp_frame *frame, int *out, int *min, int *max)
{
	float tfpa = frame->header.temp_fpa_diode;
	float vgsk = frame->header.VoutC;
	const uint16_t *pixels = (const uint16_t *)&frame->bytes[frame->header.data_offset];
	size_t nuc_start = cal->ofs_y * cal->nuc_w + cal->ofs_x;
	size_t nuc_row_adj = cal->nuc_w - cal->img_w;
	int frame_min = INT_MAX;
	int frame_max = INT_MIN;

	if (cal->cur_set == CAL_SET_NV) {
		const float *nuc_live    = &cal->nuc_live[nuc_start];
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

				int new = (int)sum;
				*out++ = new;

				// only bother updating min/max if the pixel isn't dead
				if (*nuc_live++) {
					if (new > frame_max) {
						frame_max = new;
					}
					if (new < frame_min) {
						frame_min = new;
					}
				}
			}
			nuc_live    += nuc_row_adj;
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
		const float *nuc_live      = &cal->nuc_live[nuc_start];
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

				int new = (int)sum;
				*out++ = new;

				// only bother updating min/max if the pixel isn't dead
				if (*nuc_live++) {
					if (new > frame_max) {
						frame_max = new;
					}
					if (new < frame_min) {
						frame_min = new;
					}
				}
			}
			nuc_live      += nuc_row_adj;
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
		const float *nuc_live   = &cal->nuc_live[nuc_start];
		const float *nuc_offset = &cal->nuc_offset[nuc_start];

		for (size_t y = cal->img_h; y; --y) {
			for (size_t x = cal->img_w; x; --x) {
				float px = *pixels++;
				float sum = px + *nuc_offset++;

				int new = (int)sum;
				*out++ = new;

				// only bother updating min/max if the pixel isn't dead
				if (*nuc_live++) {
					if (new > frame_max) {
						frame_max = new;
					}
					if (new < frame_min) {
						frame_min = new;
					}
				}
			}
			nuc_live   += nuc_row_adj;
			nuc_offset += nuc_row_adj;
		}
	}
	*min = frame_min;
	*max = frame_max;
}
