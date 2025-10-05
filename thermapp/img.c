// SPDX-FileCopyrightText: 2025 Kyle Guinn <elyk03@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thermapp.h"

#include <limits.h>

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
