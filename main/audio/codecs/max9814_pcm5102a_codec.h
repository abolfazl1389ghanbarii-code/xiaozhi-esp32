#ifndef _MAX9814_PCM5102A_CODEC_H
#define _MAX9814_PCM5102A_CODEC_H

#include "audio_codec.h"
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <mutex>

class Max9814Pcm5102aCodec : public AudioCodec {
private:
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_channel_t adc_channel_ = ADC_CHANNEL_6; // GPIO34 = ADC1_CH6 on classic ESP32
    gpio_num_t adc_gpio_;
    std::mutex io_mutex_;
    float dc_offset_ = 2048.0f;
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
