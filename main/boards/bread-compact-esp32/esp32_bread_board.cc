#include "wifi_board.h"
#include "codecs/max9814_pcm5102a_codec.h"
#include "config.h"
#include "display/oled_display.h"

class CompactWifiBoard : public WifiBoard {
private:
    Display* display_ = nullptr;

public:
    CompactWifiBoard() : WifiBoard() {
        // Diagnostic build: skip OLED/I2C and BOOT-button initialization.
        // This isolates startup from optional display and button hardware.
        display_ = new NoDisplay();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Max9814Pcm5102aCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_ADC_MIC_GPIO, AUDIO_I2S_SPK_GPIO_BCLK,
            AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(CompactWifiBoard);
