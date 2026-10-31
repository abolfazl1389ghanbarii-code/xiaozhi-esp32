#include "max9814_pcm5102a_codec.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#define TAG "Max9814Pcm5102a"

Max9814Pcm5102aCodec::Max9814Pcm5102aCodec(
    int input_sample_rate, int output_sample_rate, gpio_num_t adc_gpio,
    gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout)
    : adc_gpio_(adc_gpio) {
    configASSERT(adc_gpio_ == GPIO_NUM_34);
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_channels_ = 1;
    output_channels_ = 1;
    duplex_ = false;
    input_reference_ = false;

    // MAX9814 is an analog microphone: use ADC1 so Wi-Fi does not reserve the ADC.
    adc_oneshot_unit_init_cfg_t adc_init = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&adc_init, &adc_handle_));

    adc_oneshot_chan_cfg_t adc_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, adc_channel_, &adc_config));

    // ADC oneshot does not consume an I2S controller. Reserve I2S1 for PCM5102A.
    i2s_chan_config_t chan_cfg = {
        .id = XIAOZHI_I2S_PORT(1),
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, nullptr));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = static_cast<uint32_t>(output_sample_rate_),
            .clk_src = I2S_CLK_SRC_DEFAULT,
#if SOC_I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
#if SOC_I2S_HW_VERSION_2
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_LOGI(TAG, "MAX9814 ADC1 GPIO%d, PCM5102A I2S1 initialized", adc_gpio_);
}

Max9814Pcm5102aCodec::~Max9814Pcm5102aCodec() {
    if (tx_handle_ != nullptr) {
        i2s_channel_disable(tx_handle_);
        i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
    }
    if (adc_handle_ != nullptr) {
        adc_oneshot_del_unit(adc_handle_);
        adc_handle_ = nullptr;
    }
}

int Max9814Pcm5102aCodec::Read(int16_t* dest, int samples) {
    if (!input_enabled_ || dest == nullptr || samples <= 0) return 0;

    // 16 kHz sampling. ADC1 oneshot avoids the I2S0/ADC-DMA controller conflict.
    const int sample_period_us = 1000000 / input_sample_rate_;
    int raw = 0;
    for (int i = 0; i < samples; ++i) {
        const int64_t start_us = esp_timer_get_time();
        if (adc_oneshot_read(adc_handle_, adc_channel_, &raw) != ESP_OK) {
            dest[i] = 0;
        } else {
            // Slowly follow MAX9814's DC bias, then convert the AC signal to signed PCM.
            dc_offset_ += (static_cast<float>(raw) - dc_offset_) * 0.002f;
            float centered = (static_cast<float>(raw) - dc_offset_) * 16.0f;
            centered *= (input_gain_ > 0.0f ? input_gain_ : 1.0f);
            centered = std::max(-32768.0f, std::min(32767.0f, centered));
            dest[i] = static_cast<int16_t>(centered);
        }
        const int32_t elapsed = static_cast<int32_t>(esp_timer_get_time() - start_us);
        if (elapsed < sample_period_us) {
            esp_rom_delay_us(sample_period_us - elapsed);
        }
    }
    return samples;
}

int Max9814Pcm5102aCodec::Write(const int16_t* data, int samples) {
    if (!output_enabled_ || data == nullptr || samples <= 0) return 0;
    std::lock_guard<std::mutex> lock(io_mutex_);
    std::vector<int32_t> buffer(samples);
    const int32_t volume_factor =
        static_cast<int32_t>(std::pow(static_cast<double>(output_volume_) / 100.0, 2) * 65536.0);
    for (int i = 0; i < samples; ++i) {
        const int64_t scaled = static_cast<int64_t>(data[i]) * volume_factor;
        buffer[i] = static_cast<int32_t>(std::clamp<int64_t>(scaled, INT32_MIN, INT32_MAX));
    }
    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(tx_handle_, buffer.data(),
        samples * sizeof(int32_t), &bytes_written, portMAX_DELAY);
    if (err != ESP_OK) return 0;
    return bytes_written / sizeof(int32_t);
}

void Max9814Pcm5102aCodec::EnableInput(bool enable) {
    AudioCodec::EnableInput(enable);
}

void Max9814Pcm5102aCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) return;
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
    AudioCodec::EnableOutput(enable);
}
