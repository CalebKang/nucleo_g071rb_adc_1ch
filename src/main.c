/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <stm32_ll_adc.h>

#define ADC_NODE DT_PATH(zephyr_user)
#define ADC_READ_INTERVAL_MS 1000
#define STM32_ADC_CHANNEL_ID DT_IO_CHANNELS_INPUT_BY_IDX(ADC_NODE, 0)
#define STM32_ADC_LL_CHANNEL __LL_ADC_DECIMAL_NB_TO_CHANNEL(STM32_ADC_CHANNEL_ID)
#define STM32_ADC_OVERSAMPLING_RATIO 256U
#define STM32_ADC_OVERSAMPLING_SHIFT 4U
#define STM32_ADC_READY_TIMEOUT_US 10000U
#define DAC_NODE DT_NODELABEL(dac1)
#define DAC_CHANNEL_ID 1U
#define DAC_RESOLUTION 12U
#define DAC_OUTPUT_MV 3U
#define DAC_VREF_MV 3300U
#define DAC_RAW_MAX ((1U << DAC_RESOLUTION) - 1U)

#if !DT_NODE_EXISTS(ADC_NODE) || !DT_NODE_HAS_PROP(ADC_NODE, io_channels)
#error "No ADC channel configured in /zephyr,user io-channels"
#endif

static const struct adc_dt_spec adc_channel =
	ADC_DT_SPEC_GET_BY_IDX(ADC_NODE, 0);

static const struct dac_channel_cfg dac_channel_cfg = {
	.channel_id = DAC_CHANNEL_ID,
	.resolution = DAC_RESOLUTION,
	.buffered = false,
};

static const struct device *const dac_dev = DEVICE_DT_GET(DAC_NODE);

static int dac_set_test_voltage(uint32_t output_mv, uint32_t *raw)
{
	int err;
	uint32_t value;

	if (!device_is_ready(dac_dev)) {
		printk("DAC controller %s is not ready\n", dac_dev->name);
		return -ENODEV;
	}

	err = dac_channel_setup(dac_dev, &dac_channel_cfg);
	if (err < 0) {
		printk("Could not setup DAC channel %u (%d)\n",
		       DAC_CHANNEL_ID, err);
		return err;
	}

	value = ((output_mv * DAC_RAW_MAX) + (DAC_VREF_MV / 2U)) / DAC_VREF_MV;

	err = dac_write_value(dac_dev, DAC_CHANNEL_ID, value);
	if (err < 0) {
		printk("Could not write DAC channel %u (%d)\n",
		       DAC_CHANNEL_ID, err);
		return err;
	}

	*raw = value;
	return 0;
}

static void dac_print_registers(void)
{
	printk("DAC registers: DHR12R1=%lu DOR1=%lu\n",
	       DAC1->DHR12R1, DAC1->DOR1);
}

static int stm32_ll_adc_disable_for_config(void)
{
	uint32_t timeout = STM32_ADC_READY_TIMEOUT_US;

	if (LL_ADC_REG_IsConversionOngoing(ADC1)) {
		LL_ADC_REG_StopConversion(ADC1);
		while (LL_ADC_REG_IsConversionOngoing(ADC1) ||
		       LL_ADC_REG_IsStopConversionOngoing(ADC1)) {
			if (--timeout == 0U) {
				return -ETIMEDOUT;
			}
			k_busy_wait(1);
		}
	}

	if (LL_ADC_IsEnabled(ADC1)) {
		timeout = STM32_ADC_READY_TIMEOUT_US;
		LL_ADC_Disable(ADC1);
		while (LL_ADC_IsEnabled(ADC1)) {
			if (--timeout == 0U) {
				return -ETIMEDOUT;
			}
			k_busy_wait(1);
		}
	}

	return 0;
}

static int stm32_ll_adc_enable(void)
{
	uint32_t timeout = STM32_ADC_READY_TIMEOUT_US;

	if (LL_ADC_IsEnabled(ADC1)) {
		return 0;
	}

	LL_ADC_ClearFlag_ADRDY(ADC1);
	LL_ADC_Enable(ADC1);
	while (!LL_ADC_IsActiveFlag_ADRDY(ADC1)) {
		if (--timeout == 0U) {
			return -ETIMEDOUT;
		}
		k_busy_wait(1);
	}

	return 0;
}

static int stm32_ll_adc_configure(void)
{
	int err;

	err = stm32_ll_adc_disable_for_config();
	if (err < 0) {
		return err;
	}

	LL_ADC_SetResolution(ADC1, LL_ADC_RESOLUTION_12B);
	LL_ADC_REG_SetTriggerSource(ADC1, LL_ADC_REG_TRIG_SOFTWARE);
	LL_ADC_REG_SetSequencerLength(ADC1, LL_ADC_REG_SEQ_SCAN_DISABLE);
	LL_ADC_REG_SetSequencerRanks(ADC1, LL_ADC_REG_RANK_1,
				     STM32_ADC_LL_CHANNEL);
	LL_ADC_SetOverSamplingScope(ADC1, LL_ADC_OVS_GRP_REGULAR_CONTINUED);
	LL_ADC_ConfigOverSamplingRatioShift(ADC1, LL_ADC_OVS_RATIO_256,
					    LL_ADC_OVS_SHIFT_RIGHT_4);

	return stm32_ll_adc_enable();
}

static int stm32_ll_adc_read_raw16(uint16_t *raw_16)
{
	uint32_t timeout = STM32_ADC_READY_TIMEOUT_US;
	uint32_t value;

	LL_ADC_ClearFlag_EOC(ADC1);
	LL_ADC_ClearFlag_EOS(ADC1);
	LL_ADC_ClearFlag_OVR(ADC1);
	LL_ADC_REG_StartConversion(ADC1);

	while (!LL_ADC_IsActiveFlag_EOC(ADC1) && !LL_ADC_IsActiveFlag_EOS(ADC1)) {
		if (--timeout == 0U) {
			return -ETIMEDOUT;
		}
		k_busy_wait(1);
	}

	value = LL_ADC_REG_ReadConversionData32(ADC1);
	*raw_16 = (uint16_t)value;

	return 0;
}

static const char *stm32_ll_sampling_cycles(void)
{
	uint32_t sampling_time;

	sampling_time = LL_ADC_GetChannelSamplingTime(ADC1, STM32_ADC_LL_CHANNEL);

	switch (sampling_time) {
#ifdef LL_ADC_SAMPLINGTIME_1CYCLE_5
	case LL_ADC_SAMPLINGTIME_1CYCLE_5:
		return "1.5";
#endif
#ifdef LL_ADC_SAMPLINGTIME_2CYCLES_5
	case LL_ADC_SAMPLINGTIME_2CYCLES_5:
		return "2.5";
#endif
#ifdef LL_ADC_SAMPLINGTIME_3CYCLES_5
	case LL_ADC_SAMPLINGTIME_3CYCLES_5:
		return "3.5";
#endif
#ifdef LL_ADC_SAMPLINGTIME_6CYCLES_5
	case LL_ADC_SAMPLINGTIME_6CYCLES_5:
		return "6.5";
#endif
#ifdef LL_ADC_SAMPLINGTIME_7CYCLES_5
	case LL_ADC_SAMPLINGTIME_7CYCLES_5:
		return "7.5";
#endif
#ifdef LL_ADC_SAMPLINGTIME_12CYCLES_5
	case LL_ADC_SAMPLINGTIME_12CYCLES_5:
		return "12.5";
#endif
#ifdef LL_ADC_SAMPLINGTIME_19CYCLES_5
	case LL_ADC_SAMPLINGTIME_19CYCLES_5:
		return "19.5";
#endif
#ifdef LL_ADC_SAMPLINGTIME_39CYCLES_5
	case LL_ADC_SAMPLINGTIME_39CYCLES_5:
		return "39.5";
#endif
#ifdef LL_ADC_SAMPLINGTIME_79CYCLES_5
	case LL_ADC_SAMPLINGTIME_79CYCLES_5:
		return "79.5";
#endif
#ifdef LL_ADC_SAMPLINGTIME_160CYCLES_5
	case LL_ADC_SAMPLINGTIME_160CYCLES_5:
		return "160.5";
#endif
	default:
		return "unknown";
	}
}

static uint32_t stm32_ll_oversampling_ratio(void)
{
	switch (LL_ADC_GetOverSamplingRatio(ADC1)) {
	case LL_ADC_OVS_RATIO_256:
		return 256U;
	default:
		return 0U;
	}
}

static uint32_t stm32_ll_oversampling_shift(void)
{
	switch (LL_ADC_GetOverSamplingShift(ADC1)) {
	case LL_ADC_OVS_SHIFT_RIGHT_4:
		return 4U;
	default:
		return 0U;
	}
}

static void stm32_ll_print_calibration(void)
{
	printk("STM32 LL ADC calibration: factor=%lu CALFACT=0x%08lx\n",
	       LL_ADC_GetCalibrationFactor(ADC1), ADC1->CALFACT);
}

int main(void)
{
	int err;
	uint32_t count = 0;
	uint32_t dac_raw;

	if (!adc_is_ready_dt(&adc_channel)) {
		printk("ADC controller %s is not ready\n", adc_channel.dev->name);
		return 0;
	}

	err = dac_set_test_voltage(DAC_OUTPUT_MV, &dac_raw);
	if (err < 0) {
		return 0;
	}

	err = adc_channel_setup_dt(&adc_channel);
	if (err < 0) {
		printk("Could not setup ADC channel %d (%d)\n",
		       adc_channel.channel_id, err);
		return 0;
	}

	err = stm32_ll_adc_configure();
	if (err < 0) {
		printk("Could not configure ADC with STM32 LL (%d)\n", err);
		return 0;
	}

	printk("NUCLEO-G071RB ADC sample started: %s channel %d\n",
	       adc_channel.dev->name, adc_channel.channel_id);
	printk("DAC test output: %s channel=%u raw=%u target=%u mV "
	       "pin=PA4/Arduino A2, jumper A2 to A0\n",
	       dac_dev->name, DAC_CHANNEL_ID, dac_raw, DAC_OUTPUT_MV);
	dac_print_registers();
	printk("STM32 LL ADC config: channel=%u sampling=%s cycles "
	       "oversampling_ratio=%u shift=%u\n",
	       STM32_ADC_CHANNEL_ID, stm32_ll_sampling_cycles(),
	       stm32_ll_oversampling_ratio(), stm32_ll_oversampling_shift());
	stm32_ll_print_calibration();

	while (true) {
		uint16_t raw_12;
		uint16_t raw_16;
		int32_t val_mv;

		err = stm32_ll_adc_read_raw16(&raw_16);
		if (err < 0) {
			printk("[%u] ADC read failed (%d)\n", count++, err);
			k_msleep(ADC_READ_INTERVAL_MS);
			continue;
		}

		raw_12 = raw_16 >> STM32_ADC_OVERSAMPLING_SHIFT;
		val_mv = raw_12;

		printk("[%u] ll_channel=%u sampling=%s cycles ovs_ratio=%u "
		       "shift=%u raw16=%u raw12=%u dac_dor1=%lu",
		       count++, STM32_ADC_CHANNEL_ID, stm32_ll_sampling_cycles(),
		       STM32_ADC_OVERSAMPLING_RATIO, STM32_ADC_OVERSAMPLING_SHIFT,
		       raw_16, raw_12, DAC1->DOR1);

		err = adc_raw_to_millivolts_dt(&adc_channel, &val_mv);
		if (err < 0) {
			printk(" mV=N/A\n");
		} else {
			printk(" voltage=%" PRId32 " mV\n", val_mv);
		}

		k_msleep(ADC_READ_INTERVAL_MS);
	}

	return 0;
}
