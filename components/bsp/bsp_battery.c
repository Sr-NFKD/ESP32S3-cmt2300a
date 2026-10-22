#include "bsp_battery.h"

#include "bsp_board.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t calibration;
static adc_channel_t channel;

esp_err_t bsp_battery_init(void)
{
    if (adc) return ESP_ERR_INVALID_STATE;
    adc_unit_t unit;
    esp_err_t err = adc_oneshot_io_to_channel(BSP_BATTERY_ADC, &unit, &channel);
    if (err != ESP_OK) return err;
    const adc_oneshot_unit_init_cfg_t init = {.unit_id = unit};
    err = adc_oneshot_new_unit(&init, &adc);
    if (err != ESP_OK) return err;
    // 4.2 V / 4.3 = 0.977 V; use the S3 2.5 dB range with headroom.
    const adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_2_5, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(adc, channel, &config);
    if (err == ESP_OK) {
        const adc_cali_curve_fitting_config_t cal_config = {
            .unit_id = unit, .chan = channel, .atten = config.atten,
            .bitwidth = config.bitwidth,
        };
        err = adc_cali_create_scheme_curve_fitting(&cal_config, &calibration);
    }
    if (err != ESP_OK) {
        adc_oneshot_del_unit(adc);
        adc = NULL;
    }
    return err;
}

esp_err_t bsp_battery_read_mv(uint16_t *millivolts, uint16_t *raw_average)
{
    if (!millivolts || !raw_average) return ESP_ERR_INVALID_ARG;
    if (!adc || !calibration) return ESP_ERR_INVALID_STATE;
    int total = 0, raw_total = 0;
    for (unsigned i = 0; i < 16; i++) {
        int raw, pin_mv;
        esp_err_t err = adc_oneshot_read(adc, channel, &raw);
        if (err != ESP_OK) return err; // Includes ADC2 contention with Wi-Fi.
        raw_total += raw;
        err = adc_cali_raw_to_voltage(calibration, raw, &pin_mv);
        if (err != ESP_OK) return err;
        if (pin_mv < 0) return ESP_ERR_INVALID_RESPONSE;
        total += pin_mv;
    }
    // Normalize the factory ADC curve to the measured battery calibration point.
    // The reference already includes the actual divider ratio; do not apply it twice.
    int reference_pin_mv;
    esp_err_t err = adc_cali_raw_to_voltage(calibration, BSP_BATTERY_CAL_RAW, &reference_pin_mv);
    if (err != ESP_OK) return err;
    if (reference_pin_mv <= 0) return ESP_ERR_INVALID_RESPONSE;
    const uint64_t denominator = 16ULL * (unsigned)reference_pin_mv;
    uint64_t battery_mv = ((uint64_t)total * BSP_BATTERY_CAL_MV + denominator / 2) / denominator;
    if (battery_mv > UINT16_MAX) return ESP_ERR_INVALID_RESPONSE;
    *millivolts = (uint16_t)battery_mv;
    *raw_average = (uint16_t)((raw_total + 8) / 16);
    return ESP_OK;
}
