/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "audio_codec.h"

#include <zephyr/kernel.h>

#include "macros_common.h"
#include "streamctrl.h"
#include "sw_codec_select.h"
#include "audio_datapath.h"
#include "audio_i2s.h"
#include "data_fifo.h"
#include "led.h"
#include "hw_codec.h"
#include "tone.h"
#include "contin_array.h"
#include "pcm_stream_channel_modifier.h"
#include "audio_usb.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(audio_codec, CONFIG_LOG_AUDIO_CODEC_LEVEL);

#define FIFO_TX_BLOCK_COUNT (CONFIG_FIFO_FRAME_SPLIT_NUM * CONFIG_FIFO_TX_FRAME_COUNT)
#define FIFO_RX_BLOCK_COUNT (CONFIG_FIFO_FRAME_SPLIT_NUM * CONFIG_FIFO_RX_FRAME_COUNT)

#define DEBUG_INTERVAL_NUM 1000

K_THREAD_STACK_DEFINE(encoder_thread_stack, CONFIG_ENCODER_STACK_SIZE);

DATA_FIFO_DEFINE(fifo_tx, FIFO_TX_BLOCK_COUNT, WB_UP(BLOCK_SIZE_BYTES));
DATA_FIFO_DEFINE(fifo_rx, FIFO_RX_BLOCK_COUNT, WB_UP(BLOCK_SIZE_BYTES));

static struct k_thread encoder_thread_data;
static k_tid_t encoder_thread_id;

static struct sw_codec_config sw_codec_cfg;
/* Buffer which can hold max 1 period test tone at 1000 Hz */
static int16_t test_tone_buf[CONFIG_AUDIO_SAMPLE_RATE_HZ / 1000];
static size_t test_tone_size;
static bool audio_codec_started;

static void encoder_thread(void *arg1, void *arg2, void *arg3)
{
	int ret;
	uint32_t blocks_alloced_num;
	uint32_t blocks_locked_num;

	int debug_trans_count = 0;
	size_t encoded_data_size = 0;

	void *tmp_pcm_raw_data[CONFIG_FIFO_FRAME_SPLIT_NUM];
	char pcm_raw_data[FRAME_SIZE_BYTES];

	static uint8_t *encoded_data;
	static size_t pcm_block_size;
	static uint32_t test_tone_finite_pos;

	while (1) {
		/* Get PCM data from I2S */
		/* Since one audio frame is divided into a number of
		 * blocks, we need to fetch the pointers to all of these
		 * blocks before copying it to a continuous area of memory
		 * before sending it to the encoder
		 */
		for (int i = 0; i < CONFIG_FIFO_FRAME_SPLIT_NUM; i++) {
			ret = data_fifo_pointer_last_filled_get(&fifo_rx, &tmp_pcm_raw_data[i],
								&pcm_block_size, K_FOREVER);
			ERR_CHK(ret);
			memcpy(pcm_raw_data + (i * BLOCK_SIZE_BYTES), tmp_pcm_raw_data[i],
			       pcm_block_size);

			ret = data_fifo_block_free(&fifo_rx, &tmp_pcm_raw_data[i]);
			ERR_CHK(ret);
		}

		if (sw_codec_cfg.encoder.enabled) {
			if (test_tone_size) {
				/* Test tone takes over audio stream */
				uint32_t num_bytes;
				char tmp[FRAME_SIZE_BYTES / 2];

				ret = contin_array_create(tmp, FRAME_SIZE_BYTES / 2, test_tone_buf,
							  test_tone_size, &test_tone_finite_pos);
				ERR_CHK(ret);

				ret = pscm_copy_pad(tmp, FRAME_SIZE_BYTES / 2,
						    CONFIG_AUDIO_BIT_DEPTH_BITS, pcm_raw_data,
						    &num_bytes);
				ERR_CHK(ret);
			}

			ret = sw_codec_encode(pcm_raw_data, FRAME_SIZE_BYTES, &encoded_data,
					      &encoded_data_size);

			ERR_CHK_MSG(ret, "Encode failed");
		}

		/* Print block usage */
		if (debug_trans_count == DEBUG_INTERVAL_NUM) {
			ret = data_fifo_num_used_get(&fifo_rx, &blocks_alloced_num,
						     &blocks_locked_num);
			ERR_CHK(ret);
			LOG_DBG(COLOR_CYAN "RX alloced: %d, locked: %d" COLOR_RESET,
				blocks_alloced_num, blocks_locked_num);
			debug_trans_count = 0;
		} else {
			debug_trans_count++;
		}

		if (sw_codec_cfg.encoder.enabled) {
			/* Send encoded data over IPM */
			streamctrl_encoded_data_send(encoded_data, encoded_data_size);
		}
		STACK_USAGE_PRINT("encoder_thread", &encoder_thread_data);
	}
}

/**@brief Initializes the fifos, the codec, and starts the i2s
 */
static void audio_codec_start(void)
{
	int ret;

	if (audio_codec_started) {
		LOG_WRN("Audio codec already started");
		return;
	}

	if (!fifo_tx.initialized) {
		ret = data_fifo_init(&fifo_tx);
		ERR_CHK_MSG(ret, "Failed to set up tx FIFO");
	}

	if (!fifo_rx.initialized) {
		ret = data_fifo_init(&fifo_rx);
		ERR_CHK_MSG(ret, "Failed to set up rx FIFO");
	}

	ret = sw_codec_init(sw_codec_cfg);
	ERR_CHK_MSG(ret, "Failed to set up codec");

	sw_codec_cfg.initialized = true;

	if (sw_codec_cfg.encoder.enabled) {
		encoder_thread_id =
			k_thread_create(&encoder_thread_data, encoder_thread_stack,
					CONFIG_ENCODER_STACK_SIZE, (k_thread_entry_t)encoder_thread,
					NULL, NULL, NULL,
					K_PRIO_PREEMPT(CONFIG_ENCODER_THREAD_PRIO), 0, K_NO_WAIT);
		ret = k_thread_name_set(encoder_thread_id, "ENCODER");
		ERR_CHK(ret);
	}

	audio_codec_started = true;
}

int audio_encode_test_tone_set(uint32_t freq)
{
	int ret;

	if (freq == 0) {
		test_tone_size = 0;
		return 0;
	}

	ret = tone_gen(test_tone_buf, &test_tone_size, freq, CONFIG_AUDIO_SAMPLE_RATE_HZ, 1);
	ERR_CHK(ret);

	if (test_tone_size > sizeof(test_tone_buf)) {
		return -ENOMEM;
	}

	return 0;
}

/* This function is only used on gateway using USB as audio source and bidirectional stream */
int audio_decode(void const *const encoded_data, size_t encoded_data_size, bool bad_frame)
{
	int ret;
	uint32_t blocks_alloced_num;
	uint32_t blocks_locked_num;
	static int debug_trans_count;
	static void *tmp_pcm_raw_data[CONFIG_FIFO_FRAME_SPLIT_NUM];
	static void *pcm_raw_data;
	size_t pcm_block_size;

	if (!sw_codec_cfg.initialized) {
		/* Throw away data */
		/* This can happen when using play/pause since there might be
		 * some packages left in the buffers
		 */
		LOG_DBG("Trying to decode while codec is not initialized");
		return -EPERM;
	}

	ret = data_fifo_num_used_get(&fifo_tx, &blocks_alloced_num, &blocks_locked_num);
	if (ret) {
		return ret;
	}

	uint8_t free_blocks_num = FIFO_TX_BLOCK_COUNT - blocks_locked_num;

	/* If not enough space for a full frame, remove oldest samples to make room */
	if (free_blocks_num < CONFIG_FIFO_FRAME_SPLIT_NUM) {
		void *old_data;
		size_t size;

		for (int i = 0; i < (CONFIG_FIFO_FRAME_SPLIT_NUM - free_blocks_num); i++) {
			ret = data_fifo_pointer_last_filled_get(&fifo_tx, &old_data, &size,
								K_NO_WAIT);
			if (ret == -ENOMSG) {
				/* If there are no more blocks in FIFO, break */
				break;
			}
			ret = data_fifo_block_free(&fifo_tx, &old_data);
			if (ret) {
				return ret;
			}
		}
	}

	for (int i = 0; i < CONFIG_FIFO_FRAME_SPLIT_NUM; i++) {
		ret = data_fifo_pointer_first_vacant_get(&fifo_tx, &tmp_pcm_raw_data[i], K_FOREVER);
		if (ret) {
			return ret;
		}
	}

	ret = sw_codec_decode(encoded_data, encoded_data_size, bad_frame, &pcm_raw_data,
			      &pcm_block_size);
	if (ret) {
		LOG_ERR("Failed to decode");
		return ret;
	}

	/* Split decoded frame into CONFIG_FIFO_FRAME_SPLIT_NUM blocks */
	for (int i = 0; i < CONFIG_FIFO_FRAME_SPLIT_NUM; i++) {
		memcpy(tmp_pcm_raw_data[i], (char *)pcm_raw_data + (i * (BLOCK_SIZE_BYTES)),
		       BLOCK_SIZE_BYTES);

		ret = data_fifo_block_lock(&fifo_tx, &tmp_pcm_raw_data[i], BLOCK_SIZE_BYTES);
		if (ret) {
			LOG_ERR("Failed to lock block");
			return ret;
		}
	}
	if (debug_trans_count == DEBUG_INTERVAL_NUM) {
		ret = data_fifo_num_used_get(&fifo_tx, &blocks_alloced_num, &blocks_locked_num);
		if (ret) {
			return ret;
		}
		LOG_DBG(COLOR_MAGENTA "TX alloced: %d, locked: %d" COLOR_RESET, blocks_alloced_num,
			blocks_locked_num);
		debug_trans_count = 0;
	} else {
		debug_trans_count++;
	}

	return 0;
}

void audio_gateway_start(void)
{
	int ret;

	if (IS_ENABLED(CONFIG_SW_CODEC_SBC)) {
		sw_codec_cfg.sw_codec = SW_CODEC_SBC;
	} else if (IS_ENABLED(CONFIG_SW_CODEC_LC3)) {
		sw_codec_cfg.sw_codec = SW_CODEC_LC3;
	}

#if (CONFIG_STREAM_BIDIRECTIONAL)
	sw_codec_cfg.decoder.enabled = true;
	sw_codec_cfg.decoder.channel_mode = SW_CODEC_MONO;
#endif /* (CONFIG_STREAM_BIDIRECTIONAL) */

	if (IS_ENABLED(CONFIG_SW_CODEC_SBC)) {
		sw_codec_cfg.encoder.bitrate = SBC_MONO_BITRATE;
	} else if (IS_ENABLED(CONFIG_SW_CODEC_LC3)) {
		sw_codec_cfg.encoder.bitrate = CONFIG_LC3_MONO_BITRATE;
	}

#if (CONFIG_TRANSPORT_CIS)
	sw_codec_cfg.encoder.channel_mode = SW_CODEC_STEREO;
#else
	sw_codec_cfg.encoder.channel_mode = SW_CODEC_MONO;
#endif /* (CONFIG_TRANSPORT_CIS) */
	sw_codec_cfg.encoder.enabled = true;

	audio_codec_start();

#if (CONFIG_AUDIO_SOURCE_USB)
	ret = audio_usb_start(&fifo_tx, &fifo_rx);
	ERR_CHK(ret);
#elif CONFIG_AUDIO_SOURCE_I2S
	ret = hw_codec_default_conf_enable();
	ERR_CHK(ret);

	ret = audio_datapath_start(&fifo_rx);
	ERR_CHK(ret);
#endif /* (CONFIG_AUDIO_SOURCE_USB) */
}

void audio_headset_start(void)
{
	int ret;

	if (IS_ENABLED(CONFIG_SW_CODEC_SBC)) {
		sw_codec_cfg.sw_codec = SW_CODEC_SBC;
	} else if (IS_ENABLED(CONFIG_SW_CODEC_LC3)) {
		sw_codec_cfg.sw_codec = SW_CODEC_LC3;
	}

#if (CONFIG_STREAM_BIDIRECTIONAL)
	sw_codec_cfg.encoder.enabled = true;
	sw_codec_cfg.encoder.channel_mode = SW_CODEC_MONO;

	if (IS_ENABLED(CONFIG_SW_CODEC_SBC)) {
		sw_codec_cfg.encoder.bitrate = CONFIG_SBC_MONO_BITRATE;
	} else if (IS_ENABLED(CONFIG_SW_CODEC_LC3)) {
		sw_codec_cfg.encoder.bitrate = CONFIG_LC3_MONO_BITRATE;
	}
#endif /* (CONFIG_STREAM_BIDIRECTIONAL) */

	sw_codec_cfg.decoder.channel_mode = SW_CODEC_MONO;
	sw_codec_cfg.decoder.enabled = true;

	audio_codec_start();

	ret = hw_codec_default_conf_enable();
	ERR_CHK(ret);

	ret = audio_datapath_start(&fifo_rx);
	ERR_CHK(ret);
}

void audio_stop(void)
{
	int ret;

	if (!audio_codec_started) {
		LOG_WRN("Audio codec not started");
		return;
	}

	audio_codec_started = false;

	if (!sw_codec_cfg.initialized) {
		LOG_WRN("Codec already unitialized");
		return;
	}

	LOG_DBG("Stopping codec");
	/* Aborting encoder thread before uninitializing */
	if (sw_codec_cfg.encoder.enabled) {
		k_thread_abort(encoder_thread_id);
	}

#if ((CONFIG_AUDIO_DEV == GATEWAY) && CONFIG_AUDIO_SOURCE_USB)
	audio_usb_stop();
#else
	ret = hw_codec_soft_reset();
	ERR_CHK(ret);

	ret = audio_datapath_stop();
	ERR_CHK(ret);
#endif /* ((CONFIG_AUDIO_DEV == GATEWAY) && CONFIG_AUDIO_SOURCE_USB) */

	ret = sw_codec_uninit(sw_codec_cfg);
	ERR_CHK_MSG(ret, "Failed to uninit codec");
	sw_codec_cfg.initialized = false;

	/* This will force a reinit when starting audio again, ensuring that
	 * previous data is overwritten
	 */
	fifo_tx.initialized = false;
	fifo_rx.initialized = false;
}
