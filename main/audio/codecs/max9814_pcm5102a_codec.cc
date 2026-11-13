#include "max9814_pcm5102a_codec.h"

#include <esp_check.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#define TAG "Max9814Pcm5102a"

Max9814Pcm5102aCodec::Max9814Pcm5102aCodec(
    int input_sample_rate, int output_sample_rate, gpio_num_t adc_gpio,
    gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout)
    : adc_gpio_(adc_gpio) {
    configASSERT(adc_gpio_ == GPIO_NUM_34);
    configASSERT(input_sample_rate == 16000);
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    input_channels_ = 1;
    output_channels_ = 1;
    duplex_ = false;
    input_reference_ = false;

    // GPIO34 is ADC1_CH6. Continuous ADC uses DMA and does not occupy I2S0.
    // Diagnostic build: periodically report captured PCM level and DMA timeouts.
    const adc_continuous_handle_cfg_t adc_handle_cfg = {
        .max_store_buf_size = 2048,
        .conv_frame_size = kAdcRawBufferBytes,
        .flags = {.flush_pool = 1},
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_handle_cfg, &adc_handle_));

    adc_digi_pattern_config_t pattern = {};
    pattern.atten = ADC_ATTEN_DB_12;
    pattern.channel = adc_channel_;
    pattern.unit = ADC_UNIT_1;
    pattern.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

    const adc_continuous_config_t adc_config = {
        .pattern_num = 1,
        .adc_pattern = &pattern,
        .sample_freq_hz = kAdcRateHz,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };
    ESP_ERROR_CHECK(adc_continuous_config(adc_handle_, &adc_config));
    ESP_ERROR_CHECK(adc_continuous_start(adc_handle_));

    // PCM5102A receives standard 32-bit I2S stereo frames on I2S1.
    const i2s_chan_config_t chan_cfg = {
        .id = XIAOZHI_I2S_PORT(1),
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, nullptr));

    const i2s_std_config_t std_cfg = {
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
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
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
    ESP_LOGI(TAG, "ADC1 DMA %lu Hz on GPIO%d; PCM5102A I2S1 %d Hz initialized",
             static_cast<unsigned long>(kAdcRateHz), adc_gpio_, output_sample_rate_);
}

Max9814Pcm5102aCodec::~Max9814Pcm5102aCodec() {
    if (tx_handle_ != nullptr) {
        if (output_enabled_) {
            i2s_channel_disable(tx_handle_);
        }
        i2s_del_channel(tx_handle_);
        tx_handle_ = nullptr;
    }
    if (adc_handle_ != nullptr) {
        adc_continuous_stop(adc_handle_);
        adc_continuous_deinit(adc_handle_);
        adc_handle_ = nullptr;
    }
}

int Max9814Pcm5102aCodec::Read(int16_t* dest, int samples) {
    if (!input_enabled_ || dest == nullptr || samples <= 0 || adc_handle_ == nullptr) {
        return 0;
    }

    int produced = 0;
    const int64_t read_started_us = esp_timer_get_time();
    while (produced < samples) {
        uint32_t bytes_read = 0;
        const esp_err_t read_err = adc_continuous_read(
            adc_handle_, adc_raw_buffer_.data(), adc_raw_buffer_.size(),
            &bytes_read, 100);

        if (read_err == ESP_ERR_TIMEOUT) {
            if (esp_timer_get_time() - read_started_us >= 1000000) {
                ESP_LOGW(TAG, "ADC DMA timeout: no samples received within 1 second");
                return produced;
            }
            continue;
        }
        if (read_err != ESP_OK) {
            ESP_LOGW(TAG, "ADC DMA read failed: %s", esp_err_to_name(read_err));
            return produced;
        }

        uint32_t parsed_count = 0;
        const esp_err_t parse_err = adc_continuous_parse_data(
            adc_handle_, adc_raw_buffer_.data(), bytes_read,
            adc_parsed_.data(), &parsed_count);
        if (parse_err != ESP_OK) {
            ESP_LOGW(TAG, "ADC DMA parse failed: %s", esp_err_to_name(parse_err));
            return produced;
        }

        for (uint32_t i = 0; i < parsed_count && produced < samples; ++i) {
            const auto& sample = adc_parsed_[i];
            if (!sample.valid || sample.unit != ADC_UNIT_1 ||
                sample.channel != adc_channel_) {
                continue;
            }

            // One-pole DC tracker followed by a two-sample box low-pass filter
            // before 32 kHz -> 16 kHz decimation.
            dc_offset_ += (static_cast<float>(sample.raw_data) - dc_offset_) * 0.0015f;
            float centered = (static_cast<float>(sample.raw_data) - dc_offset_) * 16.0f;
            centered *= (input_gain_ > 0.0f ? input_gain_ : 1.0f);
            centered = std::clamp(centered, -32768.0f, 32767.0f);
            const int16_t pcm = static_cast<int16_t>(centered);

            if (!decimation_phase_) {
                decimation_sample_ = pcm;
                decimation_phase_ = true;
            } else {
                const int32_t filtered =
                    (static_cast<int32_t>(decimation_sample_) + pcm) / 2;
                dest[produced++] = static_cast<int16_t>(filtered);
                decimation_phase_ = false;
            }
        }
    }

    if (produced > 0) {
        int32_t min_sample = 32767;
        int32_t max_sample = -32768;
        uint32_t peak = 0;
        double sum_squares = 0.0;
        for (int i = 0; i < produced; ++i) {
            const int32_t value = dest[i];
            min_sample = std::min(min_sample, value);
            max_sample = std::max(max_sample, value);
            peak = std::max(peak, static_cast<uint32_t>(std::abs(value)));
            sum_squares += static_cast<double>(value) * value;
        }
        const int64_t now_us = esp_timer_get_time();
        if (last_diag_time_us_ == 0 || now_us - last_diag_time_us_ >= 2000000) {
            const double rms = std::sqrt(sum_squares / produced);
            ESP_LOGI(TAG, "MIC PCM: samples=%d min=%ld max=%ld peak=%lu rms=%.1f gain=%.2f",
                     produced, static_cast<long>(min_sample), static_cast<long>(max_sample),
                     static_cast<unsigned long>(peak), rms, input_gain_);
            last_diag_time_us_ = now_us;
        }
    }
    return produced;
}

int Max9814Pcm5102aCodec::Write(const int16_t* data, int samples) {
    if (!output_enabled_ || data == nullptr || samples <= 0 || tx_handle_ == nullptr) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(io_mutex_);
    const float volume = std::clamp(output_volume_, 0, 100) / 100.0f;
    int written_samples = 0;

    while (written_samples < samples) {
        const int chunk = std::min(
            samples - written_samples, static_cast<int>(AUDIO_CODEC_DMA_FRAME_NUM));
        for (int i = 0; i < chunk; ++i) {
            const int32_t value = static_cast<int32_t>(
                static_cast<float>(data[written_samples + i]) * volume * 65536.0f);
            output_buffer_[2 * i] = value;
            output_buffer_[2 * i + 1] = value;
        }

        size_t bytes_written = 0;
        const size_t bytes_to_write = static_cast<size_t>(chunk) * 2 * sizeof(int32_t);
        const esp_err_t err = i2s_channel_write(
            tx_handle_, output_buffer_.data(), bytes_to_write,
            &bytes_written, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(err));
            break;
        }

        const int frame_bytes = 2 * sizeof(int32_t);
        const int completed = static_cast<int>(bytes_written / frame_bytes);
        written_samples += completed;
        if (completed < chunk) {
            break;
        }
    }
    return written_samples;
}

void Max9814Pcm5102aCodec::EnableInput(bool enable) {
    AudioCodec::EnableInput(enable);
    ESP_LOGI(TAG, "Microphone input %s", enable ? "enabled" : "disabled");
}

void Max9814Pcm5102aCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
    AudioCodec::EnableOutput(enable);
}
