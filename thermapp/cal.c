// SPDX-FileCopyrightText: 2025 Kyle Guinn <elyk03@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thermapp.h"

#include <endian.h>
#include <fcntl.h>
#include <unistd.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t
read_word(unsigned char *src)
{
	uint16_t word;
	memcpy(&word, src, sizeof word);
	return le16toh(word);
}

static float
read_float(unsigned char *src)
{
	union {
		uint32_t word;
		float f;
	} u;
	memcpy(&u.word, src, sizeof u.word);
	u.word = le32toh(u.word);
	return u.f;
}

static double
read_double(unsigned char *src)
{
	union {
		uint64_t word;
		double d;
	} u;
	memcpy(&u.word, src, sizeof u.word);
	u.word = le64toh(u.word);
	return u.d;
}

static void
read_string(char *dst, unsigned char *src, size_t len)
{
	memcpy(dst, src, len);
	dst[len] = '\0';
}

static void
parse_params(struct thermapp_cal *cal)
{
	size_t set = 0;
	size_t id = 0;
	unsigned char *src = cal->raw_buf[set][id];
	size_t len = cal->raw_len[set][id];

	// TODO: Do all fields exist in all versions?
	if (!src || len != 0x98) {
		return;
	}

	// ver_format:
	//   0:  All data in other files encoded as (double).
	//   1:  NUC tables now encoded as (float), header now encoded as (int16_t), new extra fields following header.
	//   2:  NUC tables should be 640x480 for the ThermApp-PRO, header now encoded as (uint16_t).
	// cal_type:
	//   2:  ThermApp-TH device, calibration sets 1-3 are present.
	cal->ver_format = read_word(src); src += 2;
	cal->ver_data   = read_word(src); src += 2;
	cal->cal_type   = read_word(src); src += 2;

	read_string(cal->model,       src, 20); src += 20;
	read_string(cal->lens,        src, 10); src += 10;
	read_string(cal->description, src, 30); src += 30;
	read_string(cal->cal_date,    src,  6); src +=  6;

	cal->cal_temp_min = read_float(src); src += 4;
	cal->cal_temp_max = read_float(src); src += 4;

	for (size_t i = 0; i < 2; ++i) {
		cal->coeffs_fpa_diode[i] = read_float(src); src += 4;
	}
	for (size_t i = 0; i < 6; ++i) {
		cal->coeffs_thermistor[i] = read_float(src); src += 4;
	}
	cal->alpha_fpa_diode  = read_float(src); src += 4;
	cal->alpha_thermistor = read_float(src); src += 4;

	cal->thresh_med_to_lo = read_float(src); src += 4;
	cal->thresh_lo_to_med = read_float(src); src += 4;
	cal->thresh_hi_to_med = read_float(src); src += 4;
	cal->thresh_med_to_hi = read_float(src); src += 4;

	cal->transient_oper_time = read_float(src); src += 4;
	cal->delta_temp_max      = read_float(src); src += 4;
	cal->delta_temp_min      = read_float(src); src += 4;
	cal->transient_step_time = read_float(src); src += 4;

	cal->valid[set] |= 1 << id;
}

static void
parse_header(struct thermapp_cal *cal, size_t set)
{
	size_t id = 11;
	unsigned char *src = cal->raw_buf[set][id];
	size_t len = cal->raw_len[set][id];

	if (!src) {
		return;
	}

	if (cal->ver_format == 0) {
		if (len == 0x100) {
			for (size_t i = 0; i < 32; ++i) {
				cal->header[set].cfg.word[i] = (int16_t)read_double(src); src += 8;
			}
			cal->valid[set] |= 1 << id;
		}
	} else {
		// TODO: Do all fields exist in all versions?
		if (len == 0x68) {
			for (size_t i = 0; i < 32; ++i) {
				cal->header[set].cfg.word[i] = read_word(src); src += 2;
			}
			cal->valid[set] |= 1 << id;

			cal->header[set].gsk_voltage_min = read_word(src); src += 2;
			cal->header[set].gsk_voltage_max = read_word(src); src += 2;
			cal->header[set].histogram_peak_target = read_float(src); src += 4;

			for (size_t i = 0; i < 3; ++i) {
				cal->header[set].delta_thermistor[i] = read_float(src); src += 4;
			}
			for (size_t i = 0; i < 5; ++i) {
				cal->header[set].dist_param[i] = read_float(src); src += 4;
			}
			cal->valid[set] |= 1 << CAL_FILES;
		}
	}
}

static void
parse_nuc(struct thermapp_cal *cal, size_t set, size_t id)
{
	unsigned char *src = cal->raw_buf[set][id];
	float *dst = (float *)src;
	size_t len = cal->raw_len[set][id];

	if (!src) {
		return;
	}

	if (cal->ver_format == 0) {
		if (len == 384 * 288 * 8) {
			for (size_t i = 0; i < 384 * 288; ++i) {
				*dst++ = (float)read_double(src); src += 8;
			}
			cal->valid[set] |= 1 << id;
		}
	} else if (cal->ver_format == 1) {
		if (len == 384 * 288 * 4) {
#if __BYTE_ORDER != __LITTLE_ENDIAN
			for (size_t i = 0; i < 384 * 288; ++i) {
				*dst++ = read_float(src); src += 4;
			}
#endif
			cal->valid[set] |= 1 << id;
		}
	} else if (cal->ver_format == 2) {
		if (len == 640 * 480 * 4) {
#if __BYTE_ORDER != __LITTLE_ENDIAN
			for (size_t i = 0; i < 640 * 480; ++i) {
				*dst++ = read_float(src); src += 4;
			}
#endif
			cal->valid[set] |= 1 << id;
		}
	}
}

static const char *leaf_names[CAL_FILES][CAL_SETS] = {
	{ "0.bin",  NULL,      NULL,      NULL,      }, // Parameters
	{ "1.bin",  NULL,      NULL,      NULL,      }, // Bad pixel map (1.0 = good, 0.0 = bad)
	{ "2.bin",  "2a.bin",  "2b.bin",  "2c.bin",  }, // NUC coefficents: cfg[15]
	{ "3.bin",  "3a.bin",  "3b.bin",  "3c.bin",  }, // NUC coefficents: cfg[15]^2
	{ "4.bin",  "4a.bin",  "4b.bin",  "4c.bin",  }, // NUC coefficents: cfg[15] * pixel
	{ "5.bin",  "5a.bin",  "5b.bin",  "5c.bin",  }, // NUC coefficents: pixel
	{ "6.bin",  "6a.bin",  "6b.bin",  "6c.bin",  }, // NUC coefficents: 1
	{ "7.bin",  "7a.bin",  "7b.bin",  "7c.bin",  }, // NUC coefficents: pixel^2
	{ "8.bin",  NULL,      NULL,      NULL,      }, // NUC coefficents: cfg[18]
	{ "9.bin",  NULL,      NULL,      NULL,      }, // NUC coefficents: cfg[18]^2
	{ "10.bin", NULL,      NULL,      NULL,      }, // NUC coefficents: cfg[18] * pixel
	{ "11.bin", "11a.bin", "11b.bin", "11c.bin", }, // Header
	{ NULL,     NULL,      NULL,      NULL,      },
	{ NULL,     NULL,      NULL,      NULL,      },
	{ NULL,     NULL,      NULL,      NULL,      },
	{ NULL,     NULL,      NULL,      NULL,      },
	{ NULL,     NULL,      NULL,      NULL,      },
	{ NULL,     NULL,      NULL,      NULL,      },
	{ NULL,     "18a.bin", "18b.bin", "18c.bin", }, // NUC coefficents: pixel^3
	{ NULL,     "19a.bin", "19b.bin", "19c.bin", }, // NUC coefficents: pixel^4
	{ NULL,     "20a.bin", "20b.bin", "20c.bin", }, // NUC coefficents: cfg[15]^2 * pixel^2
	{ NULL,     "21a.bin", "21b.bin", "21c.bin", }, // Transient coefficents: Thermistor temp - FPA temp
	{ NULL,     "22a.bin", "22b.bin", "22c.bin", }, // Transient coefficents: 1
};

static void
read_leaf(struct thermapp_cal *cal, size_t set, size_t id)
{
	int fd = -1;
	unsigned char *buf = NULL;
	const char *leaf_name = leaf_names[id][set];

	if (!leaf_name) {
		return;
	}

	*cal->leaf_ptr = '\0';
	strncat(cal->leaf_ptr, leaf_name, cal->leaf_len - 1);
	printf("Reading %s\n", cal->path_buf);

	fd = open(cal->path_buf, O_RDONLY);
	if (fd < 0) {
		perror("open");
		goto err;
	}

	off_t len = lseek(fd, 0, SEEK_END);
	if (len < 0) {
		perror("lseek");
		goto err;
	} else if (len == 0) {
		fprintf(stderr, "%s: %s\n", "lseek", "Empty file");
		goto err;
	}

	buf = malloc(len);
	if (!buf) {
		perror("malloc");
		goto err;
	}

	if (lseek(fd, 0, SEEK_SET) != 0) {
		perror("lseek");
		goto err;
	}

	unsigned char *ptr = buf;
	off_t rem = len;
	while (rem) {
		ssize_t bytes_read = read(fd, ptr, rem);
		if (bytes_read < 0) {
			perror("read");
			goto err;
		} else if (bytes_read == 0) {
			fprintf(stderr, "%s: %s\n", "read", "Unexpected EOF");
			goto err;
		}
		ptr += bytes_read;
		rem -= bytes_read;
	}

	close(fd);
	cal->raw_buf[set][id] = buf;
	cal->raw_len[set][id] = len;
	return;

err:
	free(buf);
	if (fd >= 0)
		close(fd);
}

struct thermapp_cal *
thermapp_cal_open(const char *dir, const union thermapp_cfg *header)
{
	struct thermapp_cal *cal = calloc(1, sizeof *cal);
	if (!cal) {
		perror("calloc");
		return cal;
	}

	cal->serial_num   = header->serial_num_lo
	                  | header->serial_num_hi << 16;
	cal->hardware_num = header->hardware_num;
	cal->firmware_num = header->firmware_num;
	if (cal->firmware_num == 256) {
		cal->firmware_num = 7; // ???
	}

	// Optional: Everything between here and err attempts to read the factory calibration files.

	if (!dir || !*dir) {
		goto err;
	}

	// Set up a buffer for path manipulation, for read_leaf.
	// Fill out the (const) stem portion; the leaf portion can be substituted.
	const char *format = dir[strlen(dir) - 1] == '/'
	                   ? "%s%" PRIu32 "/%s"
	                   : "%s/%" PRIu32 "/%s";
	const char *longest_leaf = "11a.bin";
	char *path_buf;
	int path_len, stem_len;
	path_len = snprintf(NULL, 0, format, dir, cal->serial_num, longest_leaf);
	if (path_len <= 0) {
		goto err;
	}
	path_len += 1;
	path_buf = malloc(path_len);
	if (!path_buf) {
		perror("malloc");
		goto err;
	}
	stem_len = snprintf(path_buf, path_len, format, dir, cal->serial_num, "");
	if (stem_len <= 0) {
		free(path_buf);
		goto err;
	}
	cal->path_buf = path_buf;
	cal->leaf_ptr = path_buf + stem_len;
	cal->leaf_len = path_len - stem_len;

	// Attempt to read and parse each leaf file.
	// Missing/empty/failures result in cal->raw_buf[set][id] == NULL on a per-file basis.
	for (size_t set = 0; set < CAL_SETS; ++set) {
		for (size_t id = 0; id < CAL_FILES; ++id) {
			read_leaf(cal, set, id);

			if (id == 0 && set == 0) {
				parse_params(cal);

				// Interpretation of all other files depend on version constants in this first file.
				// Abort if first file is missing/corrupt.
				if (!(cal->valid[set] & (1 << id))) {
					goto err;
				}

				// Ensure the reported FPA size matches the expected NUC table size.
				if (cal->ver_format == 2) {
					if (header->fpa_w != 640 || header->fpa_h != 480) {
						goto err;
					}
				} else {
					if (header->fpa_w != 384 || header->fpa_h != 288) {
						goto err;
					}
				}
			} else if (id == 11) {
				parse_header(cal, set);
			} else {
				parse_nuc(cal, set, id);
			}
		}

		// Only set 0 is expected to exist for non-TH devices.
		// Sets 1-3 are for TH devices in thermography mode.
		if (cal->cal_type != 2) {
			break;
		}
	}

	if (cal->valid[0] & (1 << 1)) {
		cal->nuc_px_live = (const float *)cal->raw_buf[0][1];
	}

err:
	// Provide experimental defaults if any calibration data is missing.

	if (!(cal->valid[0] & (1 << 0))) {
		cal->coeffs_fpa_diode[0] = 0.00652 * -14336;
		cal->coeffs_fpa_diode[1] = 0.00652;
	}

	return cal;
}

void
thermapp_cal_close(struct thermapp_cal *cal)
{
	if (!cal)
		return;

	for (size_t set = 0; set < CAL_SETS; ++set)
		for (size_t id = 0; id < CAL_FILES; ++id)
			free(cal->raw_buf[set][id]);
	free(cal->path_buf);
	free(cal);
}
