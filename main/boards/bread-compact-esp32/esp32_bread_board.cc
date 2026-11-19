#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "codecs/max9814_pcm5102a_codec.h"
#include "config.h"
#include "display/oled_display.h"

#include <driver/i2c_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>

#define TAG "CompactWifiBoard"

class CompactWifiBoard : public WifiBoard {
private:
    Display* display_ = nullptr;
    Button boot_button_{GPIO_NUM_0};
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    void InitializeOledDisplay() {
        // SSD1306 0.96-inch I2C OLED: SDA=GPIO4, SCL=GPIO15, address=0x3C.
        i2c_master_bus_config_t bus_config = {};
        bus_config.i2c_port = I2C_NUM_0;
        bus_config.sda_io_num = DISPLAY_SDA_PIN;
        bus_config.scl_io_num = DISPLAY_SCL_PIN;
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.flags.enable_internal_pullup = true;

        esp_err_t err = i2c_new_master_bus(&bus_config, &i2c_bus_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OLED I2C bus init failed: %s; continuing without display",
                     esp_err_to_name(err));
            display_ = new NoDisplay();
            return;
        }

        esp_lcd_panel_io_i2c_config_t io_config = {};
        io_config.dev_addr = 0x3C;
        io_config.scl_speed_hz = 400000;
        io_config.control_phase_bytes = 1;
        io_config.dc_bit_offset = 6;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;

        err = esp_lcd_new_panel_io_i2c(i2c_bus_, &io_config, &panel_io_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OLED panel IO init failed: %s; continuing without display",
                     esp_err_to_name(err));
            i2c_del_master_bus(i2c_bus_);
            i2c_bus_ = nullptr;
            display_ = new NoDisplay();
            return;
        }

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        // The SSD1306 driver needs the selected panel height in vendor_config.
        esp_lcd_panel_ssd1306_config_t ssd1306_config = {};
        ssd1306_config.height = static_cast<uint8_t>(DISPLAY_HEIGHT);
        panel_config.vendor_config = &ssd1306_config;

        err = esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_);
        if (err == ESP_OK) {
            err = esp_lcd_panel_reset(panel_);
        }
        if (err == ESP_OK) {
            err = esp_lcd_panel_init(panel_);
        }
        if (err == ESP_OK) {
            err = esp_lcd_panel_disp_on_off(panel_, true);
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SSD1306 init failed: %s; continuing without display",
                     esp_err_to_name(err));
            if (panel_ != nullptr) {
                esp_lcd_panel_del(panel_);
                panel_ = nullptr;
            }
            esp_lcd_panel_io_del(panel_io_);
            panel_io_ = nullptr;
            i2c_del_master_bus(i2c_bus_);
            i2c_bus_ = nullptr;
            display_ = new NoDisplay();
            return;
        }

        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                   DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        ESP_LOGI(TAG, "SSD1306 OLED ready: %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    }

public:
    CompactWifiBoard() : WifiBoard() {
        // Initialize only the display here. The MAX9814/PCM5102A audio codec
        // configuration and runtime path are intentionally left unchanged.
        InitializeOledDisplay();

        // Original ESP32 has no supported on-device wake word in this no-PSRAM build.
        // BOOT button (GPIO0) starts/stops chat.
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
