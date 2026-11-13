#ifndef _MAX9814_PCM5102A_CODEC_H
#define _MAX9814_PCM5102A_CODEC_H

#include "audio_codec.h"

#include <driver/gpio.h>
#include <esp_adc/adc_continuous.h>
#include <array>
#include <mutex>

class Max9814Pcm5102aCodec : public AudioCodec {
private:
    adc_continuous_handle_t adc_handle_ = nullptr;
    static constexpr adc_channel_t adc_channel_ = ADC_CHANNEL_6; // GPIO34 / ADC1_CH6
    gpio_num_t adc_gpio_;
    std::mutex io_mutex_;
    float dc_offset_ = 2048.0f;
    int16_t decimation_sample_ = 0;
    bool decimation_phase_ = false;
    int64_t last_diag_time_us_ = 0;

    static constexpr uint32_t kAdcRateHz = 32000;
    static constexpr size_t kAdcRawBufferBytes = 256;
    static constexpr size_t kAdcMaxResults =
        kAdcRawBufferBytes / SOC_ADC_DIGI_RESULT_BYTES;
    std::array<uint8_t, kAdcRawBufferBytes> adc_raw_buffer_{};
    std::array<adc_continuous_data_t, kAdcMaxResults> adc_parsed_{};
    std::array<int32_t, AUDIO_CODEC_DMA_FRAME_NUM * 2> output_buffer_{};

    int Read(int16_t* dest, int samples) override;
    int Write(const int16_t* data, int samples) override;
    void EnableInput(bool enable) override;
    void EnableOutput(bool enable) override;

public:
    Max9814Pcm5102aCodec(int input_sample_rate, int output_sample_rate,
                         gpio_num_t adc_gpio, gpio_num_t spk_bclk,
                         gpio_num_t spk_ws, gpio_num_t spk_dout);
    ~Max9814Pcm5102aCodec() override;
};

#endif
