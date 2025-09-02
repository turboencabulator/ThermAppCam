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
	size_t id = 0;
	unsigned char *src = cal->raw_buf[id];
	size_t len = cal->raw_len[id];

	// TODO: Do all fields exist in all versions?
	if (!src || len != 0x98) {
		return;
	}

	// ver_format:
	//   0:  All data in other files encoded as (double).
	//   1:  NUC tables now encoded as (float), header now encoded as (int16_t), new extra fields following header.
	//   2:  NUC tables should be 640x480 for the ThermApp-PRO, header now encoded as (uint16_t).
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

	cal->valid |= 1 << id;
}

static void
parse_header(struct thermapp_cal *cal)
{
	size_t id = 11;
	unsigned char *src = cal->raw_buf[id];
	size_t len = cal->raw_len[id];

	if (!src || !(cal->valid & (1 << 0))) {
		return;
	}

	if (cal->ver_format == 0) {
		if (len == 0x100) {
			for (size_t i = 0; i < 32; ++i) {
				cal->cfg.word[i] = (int16_t)read_double(src); src += 8;
			}
			cal->valid |= 1 << id;
		}
	} else {
		// TODO: Do all fields exist in all versions?
		if (len == 0x68) {
			for (size_t i = 0; i < 32; ++i) {
				cal->cfg.word[i] = read_word(src); src += 2;
			}
			cal->valid |= 1 << id;

			cal->gsk_voltage_min = read_word(src); src += 2;
			cal->gsk_voltage_max = read_word(src); src += 2;
			cal->histogram_peak_target = read_float(src); src += 4;

			for (size_t i = 0; i < 3; ++i) {
				cal->delta_thermistor[i] = read_float(src); src += 4;
			}
			for (size_t i = 0; i < 5; ++i) {
				cal->dist_param[i] = read_float(src); src += 4;
			}
			cal->valid |= 1 << CAL_FILES;
		}
	}
}

static void
parse_nuc(struct thermapp_cal *cal, size_t id)
{
	unsigned char *src = cal->raw_buf[id];
	float *dst = (float *)src;
	size_t len = cal->raw_len[id];

	if (!src || !(cal->valid & (1 << 0))) {
		return;
	}

	if (cal->ver_format == 0) {
		if (len == 384 * 288 * 8) {
			for (size_t i = 0; i < 384 * 288; ++i) {
				*dst++ = (float)read_double(src); src += 8;
			}
			cal->valid |= 1 << id;
		}
	} else if (cal->ver_format == 1) {
		if (len == 384 * 288 * 4) {
#if __BYTE_ORDER != __LITTLE_ENDIAN
			for (size_t i = 0; i < 384 * 288; ++i) {
				*dst++ = read_float(src); src += 4;
			}
#endif
			cal->valid |= 1 << id;
		}
	} else if (cal->ver_format == 2) {
		if (len == 640 * 480 * 4) {
#if __BYTE_ORDER != __LITTLE_ENDIAN
			for (size_t i = 0; i < 640 * 480; ++i) {
				*dst++ = read_float(src); src += 4;
			}
#endif
			cal->valid |= 1 << id;
		}
	}
}

static const char *leaf_names[CAL_FILES] = {
	"0.bin",
	"1.bin",
	"2.bin",
	"3.bin",
	"4.bin",
	"5.bin",
	"6.bin",
	"7.bin",
	"8.bin",
	"9.bin",
	"10.bin",
	"11.bin",
};

static void
read_leaf(struct thermapp_cal *cal, size_t id)
{
	int fd = -1;
	unsigned char *buf = NULL;

	*cal->leaf_ptr = '\0';
	strncat(cal->leaf_ptr, leaf_names[id], cal->leaf_len - 1);
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
	cal->raw_buf[id] = buf;
	cal->raw_len[id] = len;
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

	// Everything beyond this point is optional.
	// Caller is responsible for handling missing data.
	if (!dir || !*dir) {
		return cal;
	}

	// Set up a buffer for path manipulation, for read_leaf.
	// Fill out the (const) stem portion; the leaf portion can be substituted.
	const char *format = dir[strlen(dir) - 1] == '/'
	                   ? "%s%" PRIu32 "/%s"
	                   : "%s/%" PRIu32 "/%s";
	const char *longest_leaf = "11.bin";
	char *path_buf;
	int path_len, stem_len;
	path_len = snprintf(NULL, 0, format, dir, cal->serial_num, longest_leaf);
	if (path_len <= 0) {
		return cal;
	}
	path_len += 1;
	path_buf = malloc(path_len);
	if (!path_buf) {
		perror("malloc");
		return cal;
	}
	stem_len = snprintf(path_buf, path_len, format, dir, cal->serial_num, "");
	if (stem_len <= 0) {
		free(path_buf);
		return cal;
	}
	cal->path_buf = path_buf;
	cal->leaf_ptr = path_buf + stem_len;
	cal->leaf_len = path_len - stem_len;

	// Attempt to read each leaf file.
	// Missing/empty/failures result in cal->raw_buf[i] == NULL on a per-file basis.
	for (size_t i = 0; i < CAL_FILES; ++i)
		read_leaf(cal, i);

	// Convert data to host endianness, update valid flags where successful.
	parse_params(cal);
	parse_header(cal);
	for (size_t i = 1; i < 11; ++i) {
		parse_nuc(cal, i);
	}

	return cal;
}

void
thermapp_cal_close(struct thermapp_cal *cal)
{
	if (!cal)
		return;

	for (size_t i = 0; i < CAL_FILES; ++i)
		free(cal->raw_buf[i]);
	free(cal->path_buf);
	free(cal);
}
