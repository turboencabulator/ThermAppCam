// SPDX-FileCopyrightText: 2015 Alexander G <pidbip@gmail.com>
// SPDX-FileCopyrightText: 2019-2025 Kyle Guinn <elyk03@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THERMAPP_H
#define THERMAPP_H

#include <libusb.h>
#include <stdint.h>

#define VENDOR  0x1772
#define PRODUCT 0x0002

#define HEADER_SIZE       64
#define FRAME_WIDTH_MIN   80
#define FRAME_WIDTH_MAX  640
#define FRAME_HEIGHT_MIN  80
#define FRAME_HEIGHT_MAX 480
#define FRAME_PIXELS_MIN (FRAME_WIDTH_MIN * FRAME_HEIGHT_MIN)
#define FRAME_PIXELS_MAX (FRAME_WIDTH_MAX * FRAME_HEIGHT_MAX)

// Device apparently only works with wMaxPacketSize (512-byte) packets of data.
// Note the frame is padded to a multiple of 512 bytes.
#define PACKET_SIZE      512
#define BULK_SIZE_MIN    ((HEADER_SIZE + 2*FRAME_PIXELS_MIN + PACKET_SIZE-1) & ~(PACKET_SIZE-1))
#define BULK_SIZE_MAX    ((HEADER_SIZE + 2*FRAME_PIXELS_MAX + PACKET_SIZE-1) & ~(PACKET_SIZE-1))

enum thermapp_cal_set {
	CAL_SET_NV,
	CAL_SET_LO,
	CAL_SET_MED,
	CAL_SET_HI,
	CAL_SETS,
};

#define CAL_FILES 23

// AD5628 DAC in Therm App is for generating control voltage
// VREF = 2.5 volts 11 Bit
union thermapp_cfg {
	uint16_t word[32];
	struct {
		uint16_t preamble[4];
		uint16_t modes;// 0xXXXM  Modes set last nibble
		uint16_t serial_num_lo;
		uint16_t serial_num_hi;
		uint16_t hardware_num;
		uint16_t firmware_num;
		uint16_t fpa_h;
		uint16_t fpa_w;
		uint16_t data_h;
		uint16_t data_w;
		uint16_t data_0d;
		int16_t  temp_thermistor;
		uint16_t temp_fpa_diode;
		uint16_t VoutA; //DCoffset;// AD5628 VoutA, Range: 0V - 2.45V, max 2048
		uint16_t data_11;
		uint16_t VoutC;//gain;// AD5628 VoutC, Range: 0V - 3.59V, max 2984 ??????
		uint16_t VoutD;// AD5628 VoutD, Range: 0V - 2.895V, max 2394 ??????
		uint16_t VoutE;// AD5628 VoutE, Range: 0V - 3.63V, max 2997, FPA VBUS
		uint16_t data_15;
		uint16_t data_16;
		uint16_t data_17;
		uint16_t data_18;
		uint16_t data_offset; // or header_size?
		uint16_t frame_num_lo;
		uint16_t frame_num_hi;
		uint16_t data_1c;
		uint16_t data_1d;
		uint16_t data_1e;
		uint16_t data_1f;
	};
};

union thermapp_frame {
	union thermapp_cfg header;
	unsigned char bytes[BULK_SIZE_MAX];
};

struct thermapp_usb_dev {
	libusb_context *ctx;
	libusb_device_handle *usb;
	struct libusb_transfer *transfer_in;
	struct libusb_transfer *transfer_out;

	unsigned char *cfg_fill;
	unsigned char *cfg_out;
	unsigned char *frame_in;
	unsigned char *frame_done;
	size_t cfg_fill_sz;
	size_t frame_in_sz;
	size_t frame_done_sz;
};

struct thermapp_cal {
	uint32_t serial_num;
	uint16_t hardware_num;
	uint16_t firmware_num;

	size_t img_w;
	size_t img_h;
	size_t nuc_w;
	size_t nuc_h;
	size_t ofs_x;
	size_t ofs_y;
	size_t bpr_i;

	enum thermapp_cal_set cur_set;

	// non-owning pointers to per-pixel arrays
	const float *nuc_good;         // 1.bin
	const float *nuc_offset;       // 6{,a,b,c}.bin
	const float *nuc_px;           // 5{,a,b,c}.bin
	const float *nuc_px2;          // 7{,a,b,c}.bin
	const float *nuc_px3;          // 18{a,b,c}.bin
	const float *nuc_px4;          // 19{a,b,c}.bin
	const float *nuc_tfpa;         // 2{,a,b,c}.bin
	const float *nuc_tfpa2;        // 3{,a,b,c}.bin
	const float *nuc_tfpa_px;      // 4{,a,b,c}.bin
	const float *nuc_tfpa2_px2;    // 20{a,b,c}.bin
	const float *nuc_vgsk;         // 8.bin
	const float *nuc_vgsk2;        // 9.bin
	const float *nuc_vgsk_px;      // 10.bin
	const float *transient_offset; // 22{a,b,c}.bin
	const float *transient_delta;  // 21{a,b,c}.bin

	uint16_t vgsk_min;
	uint16_t vgsk_max;
	double histogram_peak_target;
	const double *delta_thermistor;
	const float *dist_param;

	// 0.bin
	uint16_t ver_format;
	uint16_t ver_data;
	uint16_t cal_type;
	char model[20 + 1];
	char lens[10 + 1];
	char description[30 + 1];
	char cal_date[6 + 1]; // DDMMYY format
	float cal_temp_min; // celsius
	float cal_temp_max; // celsius
	double coeffs_fpa_diode[2];
	double coeffs_thermistor[6];
	double beta_fpa_diode;
	double beta_thermistor;
	float thresh_med_to_lo; // celsius
	float thresh_lo_to_med; // celsius
	float thresh_hi_to_med; // celsius
	float thresh_med_to_hi; // celsius
	float transient_oper_time; // minutes
	float delta_temp_max;
	float delta_temp_min;
	float transient_step_time; // seconds

	// 11{,a,b,c}.bin
	struct {
		union thermapp_cfg cfg;
		uint16_t vgsk_min;
		uint16_t vgsk_max;
		double histogram_peak_target;
		double delta_thermistor[3];
		float dist_param[5];
	} header[CAL_SETS];

	char *path_buf;
	char *leaf_ptr;
	size_t leaf_len;

	unsigned char *raw_buf[CAL_SETS][CAL_FILES];
	size_t raw_len[CAL_SETS][CAL_FILES];
	uint32_t valid[CAL_SETS];

	// storage for auto-generated calibration
	float auto_good[FRAME_PIXELS_MAX];
	float auto_offset[FRAME_PIXELS_MAX];
};


struct thermapp_usb_dev *thermapp_usb_open(void);
void thermapp_usb_start(struct thermapp_usb_dev *);
int thermapp_usb_transfers_pending(struct thermapp_usb_dev *);
void thermapp_usb_handle_events(struct thermapp_usb_dev *);
size_t thermapp_usb_frame_read(struct thermapp_usb_dev *, void *, size_t);
size_t thermapp_usb_cfg_write(struct thermapp_usb_dev *, const void *, size_t, size_t);
void thermapp_usb_close(struct thermapp_usb_dev *);

struct thermapp_cal *thermapp_cal_open(const char *, const union thermapp_cfg *);
void thermapp_cal_bpr_init(struct thermapp_cal *);
int thermapp_cal_select(struct thermapp_cal *, enum thermapp_cal_set);
void thermapp_cal_close(struct thermapp_cal *);

int thermapp_img_vgsk(const struct thermapp_cal *, const union thermapp_frame *);
void thermapp_img_nuc(const struct thermapp_cal *, const union thermapp_frame *, float *);
void thermapp_img_bpr(const struct thermapp_cal *, float *);
void thermapp_img_minmax(const struct thermapp_cal *, const float *, float *, float *);
void thermapp_img_quantize(const struct thermapp_cal *, const float *, uint16_t *);
void thermapp_img_hpf(const struct thermapp_cal *, uint16_t *, float);
void thermapp_img_lut(const struct thermapp_cal *, const uint16_t *, uint8_t *, float, float);

#endif /* THERMAPP_H */
