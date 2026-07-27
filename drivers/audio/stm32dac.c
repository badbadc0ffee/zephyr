/*
 * Copyright (c) 2026 Draeger
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT st_stm32_dac_audio

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/dma/dma_stm32.h>
#include <zephyr/audio/codec.h>

#include <soc.h>
#include <stm32_ll_dac.h>
#include <stm32_ll_tim.h>

#define LOG_LEVEL CONFIG_LOG_DEFAULT_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(stm32_dac);

/* Trigger source encoding (LL_DAC_TRIG_EXT_TIM*_TRGO constants depend on STM32 family) */
#define STM32_DAC_TRIG_TIM6 LL_DAC_TRIG_EXT_TIM6_TRGO
#define STM32_DAC_TRIG_TIM7 LL_DAC_TRIG_EXT_TIM7_TRGO

struct stm32_dac_cfg {
	DAC_TypeDef *dac_base;
	uint32_t dac_ll_channel;
	const char *dac_trigger_source;
	TIM_TypeDef *tim_base;
	const struct device *counter_dev;
	const struct device *dma_dev;
	uint32_t dma_channel;
	uint32_t dma_slot;
};

struct stm32_dac_data {
	struct dma_config dma_cfg;
	struct dma_block_config dma_block;
	bool playing;
	audio_codec_tx_done_callback_t tx_cb;
	void *tx_cb_user_data;
	uint32_t sample_rate;
	uint32_t dac_trigger;
};

static void stm32_dac_dma_callback(const struct device *dev, void *user_data,
				   uint32_t channel, int status)
{
	const struct device *codec_dev = (const struct device *)user_data;
	const struct stm32_dac_cfg *cfg = codec_dev->config;
	struct stm32_dac_data *data = codec_dev->data;

	counter_stop(cfg->counter_dev);
	dma_stop(cfg->dma_dev, cfg->dma_channel);
	data->playing = false;

	if (data->tx_cb != NULL) {
		data->tx_cb(codec_dev, data->tx_cb_user_data);
	}
}

static uint32_t stm32_dac_parse_trigger_source(const char *source_str)
{
	if (source_str == NULL) {
		return 0;
	}

	if (strcmp(source_str, "TIM6_TRGO") == 0) {
		return STM32_DAC_TRIG_TIM6;
	} else if (strcmp(source_str, "TIM7_TRGO") == 0) {
		return STM32_DAC_TRIG_TIM7;
	}

	return 0;
}

static int stm32_dac_configure(const struct device *dev, struct audio_codec_cfg *cfg)
{
	const struct stm32_dac_cfg *dac_cfg = dev->config;
	struct stm32_dac_data *data = dev->data;
	uint32_t freq;
	uint32_t ticks;
	int ret;

	if (!cfg) {
		return -EINVAL;
	}

	data->sample_rate = cfg->dai_cfg.pcm.samplerate;

	freq = counter_get_frequency(dac_cfg->counter_dev);
	if (freq == 0) {
		LOG_ERR("Counter frequency is 0");
		return -EINVAL;
	}

	ticks = freq / cfg->dai_cfg.pcm.samplerate - 1U;
	LOG_DBG("Counter freq: %u Hz, sample rate: %u Hz, ARR: %u",
		freq, cfg->dai_cfg.pcm.samplerate, ticks);

	struct counter_top_cfg top_cfg = {
		.ticks = ticks,
		.callback = NULL,
		.flags = 0,
	};

	ret = counter_set_top_value(dac_cfg->counter_dev, &top_cfg);
	if (ret != 0) {
		LOG_ERR("counter_set_top_value failed: %d", ret);
		return ret;
	}

	return 0;
}

static void stm32_dac_start_output(const struct device *dev)
{
	const struct stm32_dac_cfg *cfg = dev->config;
	struct stm32_dac_data *data = dev->data;

	if (data->playing) {
		LOG_WRN("Already playing");
		return;
	}

	LL_DAC_SetOutputBuffer(cfg->dac_base, cfg->dac_ll_channel, LL_DAC_OUTPUT_BUFFER_ENABLE);

#if defined(LL_DAC_OUTPUT_CONNECT_INTERNAL)
	LL_DAC_SetOutputConnection(cfg->dac_base, cfg->dac_ll_channel,
				   LL_DAC_OUTPUT_CONNECT_EXTERNAL);
#endif

	LL_DAC_SetTriggerSource(cfg->dac_base, cfg->dac_ll_channel, data->dac_trigger);
	LL_DAC_EnableTrigger(cfg->dac_base, cfg->dac_ll_channel);
	LL_DAC_EnableDMAReq(cfg->dac_base, cfg->dac_ll_channel);
	LL_DAC_Enable(cfg->dac_base, cfg->dac_ll_channel);

	data->playing = true;
	counter_start(cfg->counter_dev);

	LOG_DBG("DAC playback started");
}

static void stm32_dac_stop_output(const struct device *dev)
{
	const struct stm32_dac_cfg *cfg = dev->config;
	struct stm32_dac_data *data = dev->data;

	if (!data->playing) {
		return;
	}

	counter_stop(cfg->counter_dev);
	dma_stop(cfg->dma_dev, cfg->dma_channel);
	data->playing = false;

	LOG_DBG("DAC playback stopped");
}

static int stm32_dac_start(const struct device *dev, uint8_t dir)
{
	if (dir != AUDIO_DAI_DIR_TX) {
		return -ENOTSUP;
	}
	stm32_dac_start_output(dev);
	return 0;
}

static int stm32_dac_stop(const struct device *dev, uint8_t dir)
{
	if (dir != AUDIO_DAI_DIR_TX) {
		return -ENOTSUP;
	}
	stm32_dac_stop_output(dev);
	return 0;
}

static int stm32_dac_set_property(const struct device *dev, audio_property_t property,
				  audio_channel_t channel, audio_property_value_t val)
{
	return -ENOTSUP;
}

static int stm32_dac_apply_properties(const struct device *dev)
{
	return 0;
}

static int stm32_dac_register_done_callback(const struct device *dev,
					    audio_codec_tx_done_callback_t tx_cb,
					    void *tx_cb_user_data,
					    audio_codec_rx_done_callback_t rx_cb,
					    void *rx_cb_user_data)
{
	struct stm32_dac_data *data = dev->data;

	if (rx_cb != NULL) {
		return -ENOTSUP;
	}

	data->tx_cb = tx_cb;
	data->tx_cb_user_data = tx_cb_user_data;

	return 0;
}

static int stm32_dac_write(const struct device *dev, uint8_t *data, size_t data_size)
{
	const struct stm32_dac_cfg *cfg = dev->config;
	struct stm32_dac_data *drv_data = dev->data;
	int ret;

	if (!data || data_size == 0) {
		return -EINVAL;
	}

	if (data_size > 0xffff) {
		LOG_ERR("Buffer size %zu exceeds max DMA items 0xffff", data_size);
		return -EINVAL;
	}

	if (drv_data->playing) {
		return -EBUSY;
	}

	drv_data->dma_block = (struct dma_block_config){
		.source_address = (uint32_t)data,
		.dest_address = LL_DAC_DMA_GetRegAddr(cfg->dac_base, cfg->dac_ll_channel,
						      LL_DAC_DMA_REG_DATA_8BITS_RIGHT_ALIGNED),
		.block_size = data_size,
		.source_addr_adj = DMA_ADDR_ADJ_INCREMENT,
		.dest_addr_adj = DMA_ADDR_ADJ_NO_CHANGE,
		.source_reload_en = 0,
		.dest_reload_en = 0,
	};

	drv_data->dma_cfg = (struct dma_config){
		.dma_slot = cfg->dma_slot,
		.channel_direction = MEMORY_TO_PERIPHERAL,
		.source_data_size = 1,
		.dest_data_size = 1,
		.block_count = 1,
		.head_block = &drv_data->dma_block,
		.dma_callback = stm32_dac_dma_callback,
		.user_data = (void *)dev,
		.complete_callback_en = 1,
		.half_complete_callback_en = 0,
		.error_callback_dis = 0,
	};

	ret = dma_config(cfg->dma_dev, cfg->dma_channel, &drv_data->dma_cfg);
	if (ret != 0) {
		LOG_ERR("dma_config failed: %d", ret);
		return ret;
	}

	ret = dma_start(cfg->dma_dev, cfg->dma_channel);
	if (ret != 0) {
		LOG_ERR("dma_start failed: %d", ret);
		return ret;
	}

	return 0;
}

static const struct audio_codec_driver_api stm32_dac_api = {
	.configure = stm32_dac_configure,
	.start_output = stm32_dac_start_output,
	.stop_output = stm32_dac_stop_output,
	.set_property = stm32_dac_set_property,
	.apply_properties = stm32_dac_apply_properties,
	.register_done_callback = stm32_dac_register_done_callback,
	.write = stm32_dac_write,
	.start = stm32_dac_start,
	.stop = stm32_dac_stop,
};

static int stm32_dac_init(const struct device *dev)
{
	const struct stm32_dac_cfg *cfg = dev->config;
	uint32_t dac_trigger;

	LOG_DBG("Initializing STM32 DAC driver on %s", dev->name);

	if (!device_is_ready(cfg->counter_dev)) {
		LOG_ERR("Counter device not ready");
		return -ENODEV;
	}

	if (!device_is_ready(cfg->dma_dev)) {
		LOG_ERR("DMA device not ready");
		return -ENODEV;
	}

	dac_trigger = stm32_dac_parse_trigger_source(cfg->dac_trigger_source);

	/* Prevent spurious TRGO pulses from counter update events */
	LL_TIM_SetUpdateSource(cfg->tim_base, LL_TIM_UPDATESOURCE_COUNTER);
	LL_TIM_SetTriggerOutput(cfg->tim_base, LL_TIM_TRGO_UPDATE);

	struct stm32_dac_data *data = dev->data;
	data->dac_trigger = dac_trigger;

	LOG_INF("STM32 DAC audio codec initialized");

	return 0;
}

#define STM32_DAC_INIT(index)                                                               \
	static const struct stm32_dac_cfg stm32_dac_cfg_##index = {                         \
		.dac_base = (DAC_TypeDef *)DT_REG_ADDR(DT_INST_IO_CHANNELS_CTLR_BY_IDX(index, 0)), \
		.dac_ll_channel = table_channels[DT_INST_IO_CHANNELS_OUTPUT_BY_IDX(index, 0) - 1], \
		.dac_trigger_source = DT_INST_PROP(index, dac_trigger_source),              \
		.tim_base = (TIM_TypeDef *)DT_REG_ADDR(DT_INST_PHANDLE(index, timer)),       \
		.counter_dev = DEVICE_DT_GET(DT_CHILD(DT_INST_PHANDLE(index, timer), counter)), \
		.dma_dev = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(index, tx)),              \
		.dma_channel = DT_INST_DMAS_CELL_BY_NAME(index, tx, channel),                \
		.dma_slot = DT_INST_DMAS_CELL_BY_NAME(index, tx, channel),                   \
	};                                                                                   \
                                                                                            \
	static struct stm32_dac_data stm32_dac_data_##index;                                \
                                                                                            \
	DEVICE_DT_INST_DEFINE(index, &stm32_dac_init, NULL, &stm32_dac_data_##index,       \
			      &stm32_dac_cfg_##index, POST_KERNEL,                             \
			      CONFIG_AUDIO_DAC_STM32_INIT_PRIORITY, &stm32_dac_api)

#define CHAN(n) LL_DAC_CHANNEL_##n
static const uint32_t table_channels[] = {
	CHAN(1),
#ifdef LL_DAC_CHANNEL_2
	CHAN(2),
#endif
};

DT_INST_FOREACH_STATUS_OKAY(STM32_DAC_INIT)
