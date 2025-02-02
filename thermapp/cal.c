// SPDX-FileCopyrightText: 2025 Kyle Guinn <elyk03@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "thermapp.h"

#include <fcntl.h>
#include <unistd.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
read_leaf(struct thermapp_cal *cal, size_t i)
{
	int fd = -1;
	unsigned char *buf = NULL;

	*cal->leaf_ptr = '\0';
	strncat(cal->leaf_ptr, leaf_names[i], cal->leaf_len - 1);
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
	cal->raw_buf[i] = buf;
	cal->raw_len[i] = len;
	return;

err:
	free(buf);
	if (fd >= 0)
		close(fd);
}

struct thermapp_cal *
thermapp_cal_open(const char *dir, uint32_t serial_num)
{
	struct thermapp_cal *cal = calloc(1, sizeof *cal);
	if (!cal) {
		perror("calloc");
		return cal;
	}

	cal->serial_num = serial_num;

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
	path_len = snprintf(NULL, 0, format, dir, serial_num, longest_leaf);
	if (path_len <= 0) {
		return cal;
	}
	path_len += 1;
	path_buf = malloc(path_len);
	if (!path_buf) {
		perror("malloc");
		return cal;
	}
	stem_len = snprintf(path_buf, path_len, format, dir, serial_num, "");
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
