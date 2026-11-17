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
#include <esp_http_server.h>
#include <esp_netif.h>
#include <lwip/inet.h>
#include <cstdlib>

#define TAG "CompactWifiBoard"

class CompactWifiBoard : public WifiBoard {
private:
    Display* display_ = nullptr;
    Button boot_button_{GPIO_NUM_0};
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    httpd_handle_t volume_http_server_ = nullptr;

    static esp_err_t VolumePageHandler(httpd_req_t* req) {
        static const char page[] = R"PAGE(<!doctype html><html lang="fa" dir="rtl"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#142033"><title>کنترل صدای XiaoZhi</title><style>
        *{box-sizing:border-box}body{margin:0;min-height:100vh;display:grid;place-items:center;padding:20px;background:linear-gradient(145deg,#101827,#20364b);color:#f5f7fb;font-family:Tahoma,Arial,sans-serif}
        main{width:min(100%,430px);padding:28px;border:1px solid #40536b;border-radius:26px;background:#1d2b3c;box-shadow:0 18px 55px #0005}
        small{color:#8fc9ff}h1{font-size:24px;margin:12px 0 28px}.value{text-align:center;font-size:58px;font-weight:bold;margin:20px 0 4px}.unit{text-align:center;color:#adbdcf;font-size:13px}
        input{width:100%;margin:32px 0;accent-color:#58b5ff;height:10px;direction:ltr}.buttons{display:flex;gap:12px;margin-top:12px}
        button{flex:1;border:0;border-radius:14px;padding:16px;background:#304760;color:white;font-size:17px;font-weight:bold}.mute{background:#a33c50}.hint{text-align:center;color:#adbdcf;font-size:12px;margin-top:24px}
        </style></head><body><main><small>XIAOZHI · DIGITAL AUDIO</small><h1>کنترل صدای دستگاه</h1><div class="value" id="v">--</div><div class="unit">درصد صدا</div>
        <input id="s" type="range" min="0" max="100" value="70"><div class="buttons"><button id="down">− کاهش</button><button id="up">افزایش +</button></div>
        <div class="buttons"><button class="mute" id="mute">قطع صدا</button></div><div class="hint">گوشی و XiaoZhi باید به یک Wi-Fi متصل باشند.</div></main>
        <script>
        const s=document.getElementById('s'),v=document.getElementById('v'),m=document.getElementById('mute');let t=null,last=70;
        function show(n){s.value=n;v.textContent=n;m.textContent=Number(n)===0?'فعال‌کردن صدا':'قطع صدا'}
        async function send(n){n=Math.max(0,Math.min(100,Number(n)||0));show(n);try{await fetch('/api/volume?value='+n,{method:'POST',cache:'no-store'})}catch(e){v.textContent='خطا'}}
        s.addEventListener('input',()=>show(s.value));s.addEventListener('change',()=>send(s.value));
        document.getElementById('down').onclick=()=>send(Number(s.value)-5);document.getElementById('up').onclick=()=>send(Number(s.value)+5);
        m.onclick=()=>{if(Number(s.value)>0){last=Number(s.value);send(0)}else send(last||50)};
        fetch('/api/volume').then(r=>r.json()).then(d=>{show(d.volume);if(d.volume>0)last=d.volume}).catch(()=>show(70));
        </script></body></html>)PAGE";
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
    }

    static esp_err_t VolumeApiHandler(httpd_req_t* req) {
        AudioCodec* codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Audio codec unavailable");
            return ESP_FAIL;
        }
        if (req->method == HTTP_GET) {
            char response[40];
            snprintf(response, sizeof(response), "{\"volume\":%d}", codec->output_volume());
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
        }
        char query[32] = {};
        char value[8] = {};
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
            httpd_query_key_value(query, "value", value, sizeof(value)) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing volume value");
            return ESP_FAIL;
        }
        char* end = nullptr;
        long volume = strtol(value, &end, 10);
        if (end == value || *end != 0 || volume < 0 || volume > 100) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Volume must be 0..100");
            return ESP_FAIL;
        }
        codec->SetOutputVolume(static_cast<int>(volume));
        httpd_resp_set_type(req, "application/json");
        char response[40];
        snprintf(response, sizeof(response), "{\"volume\":%ld}", volume);
        return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    }

    void StartVolumeWebServer() {
        if (volume_http_server_ != nullptr) return;
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.server_port = 80;
        config.max_uri_handlers = 4;
        if (httpd_start(&volume_http_server_, &config) != ESP_OK) {
            volume_http_server_ = nullptr;
            ESP_LOGE(TAG, "Could not start phone volume-control web server");
            return;
        }
        httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = VolumePageHandler, .user_ctx = nullptr};
        httpd_uri_t get_volume = {.uri = "/api/volume", .method = HTTP_GET, .handler = VolumeApiHandler, .user_ctx = nullptr};
        httpd_uri_t set_volume = {.uri = "/api/volume", .method = HTTP_POST, .handler = VolumeApiHandler, .user_ctx = nullptr};
        httpd_register_uri_handler(volume_http_server_, &root);
        httpd_register_uri_handler(volume_http_server_, &get_volume);
        httpd_register_uri_handler(volume_http_server_, &set_volume);
        ESP_LOGI(TAG, "Phone volume-control server started on port 80");
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info = {};
        if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            char url[48];
            snprintf(url, sizeof(url), "http://" IPSTR, IP2STR(&ip_info.ip));
            ESP_LOGI(TAG, "Open %s on a phone connected to the same Wi-Fi", url);
            if (display_ != nullptr) display_->ShowNotification(url, 15000);
        }
    }

    void InitializeOledDisplay() {
        // SSD1306 uses I2C address 0x3C on most 0.96-inch modules.
        i2c_master_bus_config_t bus_config = {};
        bus_config.i2c_port = I2C_NUM_0;
        bus_config.sda_io_num = DISPLAY_SDA_PIN;
        bus_config.scl_io_num = DISPLAY_SCL_PIN;
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.flags.enable_internal_pullup = true;

        esp_err_t err = i2c_new_master_bus(&bus_config, &i2c_bus_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OLED I2C bus initialization failed: %s", esp_err_to_name(err));
            display_ = new NoDisplay();
            return;
        }

        // ESP-IDF 6.1 no longer provides the legacy I2C config macro.
        // SSD1306 uses one control byte; bit 6 selects command/data.
        esp_lcd_panel_io_i2c_config_t io_config = {};
        io_config.dev_addr = 0x3C;
        io_config.scl_speed_hz = 400000;
        io_config.control_phase_bytes = 1;
        io_config.dc_bit_offset = 6;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        err = esp_lcd_new_panel_io_i2c(i2c_bus_, &io_config, &panel_io_);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "OLED I2C panel IO initialization failed: %s", esp_err_to_name(err));
            i2c_del_master_bus(i2c_bus_);
            i2c_bus_ = nullptr;
            display_ = new NoDisplay();
            return;
        }

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;

        err = esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_);
        if (err == ESP_OK) {
            err = esp_lcd_panel_reset(panel_);
        }
        if (err == ESP_OK) {
            err = esp_lcd_panel_init(panel_);
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SSD1306 initialization failed: %s; continuing without display",
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
        ESP_LOGI(TAG, "SSD1306 OLED initialized (%dx%d)", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    }

public:
    CompactWifiBoard() : WifiBoard() {
        // Diagnostic stage 0: keep the known-good XiaoZhi/audio startup path intact.
        // OLED initialization is deliberately disabled to isolate whether the display
        // stack is preventing the application from booting.
        display_ = new NoDisplay();

        // The original ESP32 has no supported on-device wake word in this
        // no-PSRAM build. Use the BOOT button (GPIO0) to start/stop chat.
        boot_button_.OnClick([]() {
            Application::GetInstance().ToggleChatState();
        });
    }


    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override {
        WifiBoard::SetNetworkEventCallback([this, callback = std::move(callback)](NetworkEvent event, const std::string& data) {
            if (callback) callback(event, data);
            if (event == NetworkEvent::Connected) StartVolumeWebServer();
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
