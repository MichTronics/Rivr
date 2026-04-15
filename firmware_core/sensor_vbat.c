/**
 * @file  sensor_vbat.c
 * @brief Battery voltage measurement via ESP-IDF 5.x ADC oneshot driver.
 *
 * Uses adc_oneshot + hardware calibration (curve-fitting on S3, line-fitting
 * on classic ESP32) so the raw counts are converted to calibrated millivolts
 * before the divider ratio is applied.
 *
 * The measured voltage at the ADC pin is:
 *   V_adc = V_bat × R_low / (R_high + R_low)
 *
 * so to recover V_bat:
 *   V_bat = V_adc × (R_high + R_low) / R_low
 *         = V_adc × RIVR_VBAT_DIV_NUM / RIVR_VBAT_DIV_DEN
 *
 * For a 100 kΩ / 100 kΩ divider, NUM=2, DEN=1.
 */

#include "sensor_vbat.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include <stdbool.h>

#define TAG "VBAT"

/* ── Module state ─────────────────────────────────────────────────────────── */

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t         s_cali_handle = NULL;
static adc_channel_t             s_channel     = 0;
static int                       s_div_num     = 2;
static int                       s_div_den     = 1;
static bool                      s_ready       = false;

/* ── Public API ───────────────────────────────────────────────────────────── */

void vbat_init(int gpio_num, int div_num, int div_den)
{
    adc_unit_t    unit;
    adc_channel_t channel;

    /* Map GPIO → ADC unit + channel */
    esp_err_t err = adc_oneshot_io_to_channel(gpio_num, &unit, &channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d is not a valid ADC pin (err=0x%x)", gpio_num, err);
        return;
    }

    /*
     * Only ADC1 is safe to use alongside WiFi/BLE.
     * ADC2 sharing with the RF co-processor introduces read errors and
     * requires a lock that we do not want to add to this thin driver.
     */
    if (unit != ADC_UNIT_1) {
        ESP_LOGW(TAG, "GPIO%d maps to ADC2 — not supported (use an ADC1 pin)",
                 gpio_num);
        return;
    }

    s_channel = channel;
    s_div_num = div_num;
    s_div_den = (div_den != 0) ? div_den : 1;   /* guard against /0 */

    /* Initialise ADC1 unit */
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_1,
    };
    if (adc_oneshot_new_unit(&init_cfg, &s_adc_handle) != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed");
        return;
    }

    /* Configure the channel: 12 dB attenuation gives a 0–3.3 V input range */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten    = ADC_ATTEN_DB_12,
    };
    if (adc_oneshot_config_channel(s_adc_handle, channel, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed");
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return;
    }

    /* Calibration — prefer curve-fitting (ESP32-S3), fall back to line-fitting */
    bool cali_ok = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    {
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id  = ADC_UNIT_1,
            .chan     = channel,
            .atten    = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle) == ESP_OK) {
            cali_ok = true;
            ESP_LOGI(TAG, "ADC calibration: curve fitting");
        }
    }
#endif /* ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED */

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if (!cali_ok) {
        adc_cali_line_fitting_config_t line_cfg = {
            .unit_id  = ADC_UNIT_1,
            .atten    = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_cali_create_scheme_line_fitting(&line_cfg, &s_cali_handle) == ESP_OK) {
            cali_ok = true;
            ESP_LOGI(TAG, "ADC calibration: line fitting");
        }
    }
#endif /* ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED */

    if (!cali_ok) {
        ESP_LOGW(TAG, "ADC calibration unavailable — raw reading may be ±5%%");
    }

    s_ready = true;
    ESP_LOGI(TAG, "vbat_init OK (gpio=%d ch=%d div=%d/%d)",
             gpio_num, (int)channel, div_num, s_div_den);
}

int32_t vbat_read_mv(void)
{
    if (!s_ready || !s_adc_handle) {
        return 0;
    }

    int raw = 0;
    if (adc_oneshot_read(s_adc_handle, s_channel, &raw) != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_read failed");
        return 0;
    }

    int adc_mv = 0;
    if (s_cali_handle) {
        adc_cali_raw_to_voltage(s_cali_handle, raw, &adc_mv);
    } else {
        /* Linear fallback for 12-bit ADC on 3.3 V supply */
        adc_mv = (raw * 3300) / 4095;
    }

    /* Apply voltage divider: V_bat = V_adc × num / den */
    return (int32_t)((adc_mv * s_div_num) / s_div_den);
}
