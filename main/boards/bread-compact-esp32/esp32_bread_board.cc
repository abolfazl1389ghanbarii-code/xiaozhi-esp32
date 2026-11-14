#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "codecs/max9814_pcm5102a_codec.h"
#include "config.h"
#include "display/oled_display.h"

class CompactWifiBoard : public WifiBoard {
private:
    Display* display_ = nullptr;
    Button boot_button_{GPIO_NUM_0};

public:
    CompactWifiBoard() : WifiBoard() {
        // Keep the OLED/I2C hardware bypassed while diagnosing audio capture.
        display_ = new NoDisplay();

        // The original ESP32 has no supported on-device wake word in this
        // no-PSRAM build. Use the BOOT button (GPIO0) to start/stop chat.
        boot_button_.OnClick([]() {
            Application::GetInstance().ToggleChatState();
        });
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
