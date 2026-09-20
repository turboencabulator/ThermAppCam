// SPDX-FileCopyrightText: 2026 Kyle Guinn <elyk03@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thermapp.h"

#include <math.h>
#include <time.h>

#define CLOCK_SOURCE CLOCK_MONOTONIC

static float
timespec_delta(struct timespec end, struct timespec start)
{
	struct timespec delta;
	delta.tv_nsec = end.tv_nsec - start.tv_nsec;
	delta.tv_sec  = end.tv_sec  - start.tv_sec;
	return (float)delta.tv_sec + (float)delta.tv_nsec / 1e9f;
}

void
thermapp_temp_init(struct thermapp_dev_temp *dt, const struct thermapp_cal *cal)
{
	clock_gettime(CLOCK_SOURCE, &dt->transient_start);
	dt->transient_step_start = dt->transient_start;
	dt->transient_steps = cal->transient_steps_max;
	dt->temp_settle_frame = 11;
	dt->old_temp_delta = NAN;
	dt->old_deriv_temp_delta = NAN;
}

void
thermapp_temp_update(struct thermapp_dev_temp *dt, const struct thermapp_cal *cal, const union thermapp_frame *frame)
{
	double raw_temp_fpa = frame->header.temp_fpa_diode;
	double cur_temp_fpa = cal->coeffs_fpa_diode[1];
	cur_temp_fpa = fma(cur_temp_fpa, raw_temp_fpa, cal->coeffs_fpa_diode[0]);

	double raw_temp_therm = frame->header.temp_thermistor;
	double cur_temp_therm = cal->coeffs_thermistor[5];
	cur_temp_therm = fma(cur_temp_therm, raw_temp_therm, cal->coeffs_thermistor[4]);
	cur_temp_therm = fma(cur_temp_therm, raw_temp_therm, cal->coeffs_thermistor[3]);
	cur_temp_therm = fma(cur_temp_therm, raw_temp_therm, cal->coeffs_thermistor[2]);
	cur_temp_therm = fma(cur_temp_therm, raw_temp_therm, cal->coeffs_thermistor[1]);
	cur_temp_therm = fma(cur_temp_therm, raw_temp_therm, cal->coeffs_thermistor[0]);

	double temp_fpa, temp_therm;
	if (dt->temp_settle_frame) {
		dt->temp_settle_frame -= 1;
		temp_fpa   = cur_temp_fpa;
		temp_therm = cur_temp_therm;
	} else {
		temp_fpa   = cal->beta_fpa_diode  * dt->temp_fpa   + (1.0 - cal->beta_fpa_diode)  * cur_temp_fpa;
		temp_therm = cal->beta_thermistor * dt->temp_therm + (1.0 - cal->beta_thermistor) * cur_temp_therm;
	}

	dt->cur_temp_fpa   = cur_temp_fpa;
	dt->cur_temp_therm = cur_temp_therm;
	dt->temp_fpa       = temp_fpa;
	dt->temp_therm     = temp_therm;
	dt->temp_delta     = temp_therm - temp_fpa;
}

void
thermapp_temp_transient(struct thermapp_dev_temp *dt, const struct thermapp_cal *cal)
{
	if (dt->transient_steps) {
		struct timespec now;
		clock_gettime(CLOCK_SOURCE, &now);
		if (timespec_delta(now, dt->transient_step_start) > cal->transient_step_time) {
			dt->transient_step_start = now;
			if (timespec_delta(now, dt->transient_start) >= cal->transient_oper_time
			 || fabs(dt->temp_delta) > cal->temp_delta_max) {
				dt->transient_steps = 0;
			} else if (!isnan(dt->old_temp_delta)) {
				double deriv_temp_delta = dt->temp_delta - dt->old_temp_delta;
				if (!isnan(dt->old_deriv_temp_delta)) {
					deriv_temp_delta = cal->beta_deriv_temp_delta * dt->old_deriv_temp_delta + (1.0 - cal->beta_deriv_temp_delta) * deriv_temp_delta;
					if (fabs(deriv_temp_delta) < cal->deriv_temp_delta_min) {
						dt->transient_steps -= 1;
					} else {
						dt->transient_steps = cal->transient_steps_max;
					}
				}
				dt->old_deriv_temp_delta = deriv_temp_delta;
			}
			dt->old_temp_delta = dt->temp_delta;
		}
	}
}
