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
	int frame_min = INT_MAX;
	int frame_max = INT_MIN;

	if (cal->cur_set == CAL_SET_NV) {
		for (size_t i = 0; i < FRAME_PIXELS; ++i) {
			float px = pixels[i];
			float t2 = cal->nuc_tfpa2[i] * tfpa + cal->nuc_tfpa[i];
			float v2 = cal->nuc_vgsk2[i] * vgsk + cal->nuc_vgsk[i];
			float p2 = cal->nuc_px2[i] * px + cal->nuc_px[i];
			p2 += cal->nuc_tfpa_px[i] * tfpa;
			p2 += cal->nuc_vgsk_px[i] * vgsk;
			float sum = p2 * px + cal->nuc_offset[i];
			sum += t2 * tfpa;
			sum += v2 * vgsk;

			int new = (int)sum;
			out[i] = new;

			// only bother updating min/max if the pixel isn't dead
			if (cal->nuc_live[i]) {
				if (new > frame_max) {
					frame_max = new;
				}
				if (new < frame_min) {
					frame_min = new;
				}
			}
		}
	} else if (cal->cur_set != CAL_SETS) {
		for (size_t i = 0; i < FRAME_PIXELS; ++i) {
			float px = pixels[i];
			float tp = tfpa * px;
			float t2 = cal->nuc_tfpa2[i] * tfpa + cal->nuc_tfpa[i];
			float tp2 = cal->nuc_tfpa2_px2[i] * tp + cal->nuc_tfpa_px[i];
			float sum = cal->nuc_px4[i] * px + cal->nuc_px3[i];
			sum = sum * px + cal->nuc_px2[i];
			sum = sum * px + cal->nuc_px[i];
			sum = sum * px + cal->nuc_offset[i];
			sum += t2 * tfpa;
			sum += tp2 * tp;

			int new = (int)sum;
			out[i] = new;

			// only bother updating min/max if the pixel isn't dead
			if (cal->nuc_live[i]) {
				if (new > frame_max) {
					frame_max = new;
				}
				if (new < frame_min) {
					frame_min = new;
				}
			}
		}
	} else {
		for (size_t i = 0; i < FRAME_PIXELS; ++i) {
			float px = pixels[i];
			float sum = px + cal->nuc_offset[i];

			int new = (int)sum;
			out[i] = new;

			// only bother updating min/max if the pixel isn't dead
			if (cal->nuc_live[i]) {
				if (new > frame_max) {
					frame_max = new;
				}
				if (new < frame_min) {
					frame_min = new;
				}
			}
		}
	}
	*min = frame_min;
	*max = frame_max;
}
