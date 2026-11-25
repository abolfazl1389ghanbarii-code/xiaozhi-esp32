/*
 * XiaoZhi network camera node
 * Target: AI Thinker ESP32-CAM + OV2640 + PSRAM
 *
 * First boot (or after Wi-Fi credentials fail):
 *   Connect to Wi-Fi "XiaoZhiCam-Setup" (password: camsetup123)
 *   Open http://192.168.4.1 and enter the 2.4 GHz LAN credentials.
 *
 * On the same trusted LAN as XiaoZhi:
 *   GET /health  -> JSON status
 *   GET /capture -> one QVGA JPEG frame
 *
 * Do not expose this camera endpoint to the public internet.
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

static constexpr char kTag[] = "XiaoZhiCam";
static constexpr char kSetupSsid[] = "XiaoZhiCam-Setup";
static constexpr char kSetupPassword[] = "camsetup123";

WebServer server(80);
Preferences preferences;
static bool setupMode = false;

// AI Thinker ESP32-CAM pin map (OV2640)
static camera_config_t camera_config = {
    .pin_pwdn = 32,
    .pin_reset = -1,
    .pin_xclk = 0,
    .pin_sccb_sda = 26,
    .pin_sccb_scl = 27,
    .pin_d7 = 35,
    .pin_d6 = 34,
    .pin_d5 = 39,
    .pin_d4 = 36,
    .pin_d3 = 21,
    .pin_d2 = 19,
    .pin_d1 = 18,
    .pin_d0 = 5,
    .pin_vsync = 25,
    .pin_href = 23,
    .pin_pclk = 22,
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_QVGA,
    .jpeg_quality = 16,
    .fb_count = 1,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_LATEST,
};

static void handleRoot() {
    if (!setupMode) {
        server.send(200, "text/plain",
                    "XiaoZhi camera is online. Use /health or /capture.");
        return;
    }

    const String page =
        "<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<meta charset='utf-8'><title>XiaoZhi Camera Setup</title></head>"
        "<body style='font-family:sans-serif;max-width:420px;margin:40px auto;padding:16px'>"
        "<h2>XiaoZhi Camera Wi-Fi setup</h2>"
        "<p>Enter the 2.4 GHz Wi-Fi details used by your XiaoZhi board.</p>"
        "<form method='POST' action='/save'>"
        "<label>Wi-Fi name (SSID)</label><br><input name='ssid' maxlength='32' required style='width:100%;padding:10px'><br><br>"
        "<label>Wi-Fi password</label><br><input name='password' type='password' maxlength='64' style='width:100%;padding:10px'><br><br>"
        "<button style='padding:12px 20px'>Save and connect</button></form></body></html>";
    server.send(200, "text/html; charset=utf-8", page);
}

static void handleSave() {
    String ssid = server.arg("ssid");
    String password = server.arg("password");
    ssid.trim();

    if (ssid.length() == 0 || ssid.length() > 32 || password.length() > 64) {
        server.send(400, "text/plain; charset=utf-8",
                    "Invalid Wi-Fi name or password length.");
        return;
    }

    preferences.begin("xiaozhi-cam", false);
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    preferences.end();

    server.send(200, "text/html; charset=utf-8",
                "<html><meta charset='utf-8'><body style='font-family:sans-serif'>"
                "<h3>Saved.</h3><p>The camera is restarting and will join your Wi-Fi.</p>"
                "<p>Reconnect your phone to the normal Wi-Fi network.</p></body></html>");
    delay(1200);
    ESP.restart();
}

static void startSetupPortal() {
    setupMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(kSetupSsid, kSetupPassword);
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.begin();

    Serial.printf("[%s] Wi-Fi setup AP: %s
", kTag, kSetupSsid);
    Serial.printf("[%s] Setup password: %s
", kTag, kSetupPassword);
    Serial.println("[XiaoZhiCam] Open http://192.168.4.1 to configure Wi-Fi.");
}

static bool connectSavedWiFi() {
    preferences.begin("xiaozhi-cam", true);
    String ssid = preferences.getString("ssid", "");
    String password = preferences.getString("password", "");
    preferences.end();

    if (ssid.isEmpty()) return false;

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(ssid.c_str(), password.c_str());

    Serial.printf("[%s] Connecting to saved Wi-Fi", kTag);
    const unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 25000) {
        delay(400);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[XiaoZhiCam] Saved Wi-Fi connection failed; starting setup AP.");
        preferences.begin("xiaozhi-cam", false);
        preferences.clear();
        preferences.end();
        return false;
    }
    return true;
}

static void handleHealth() {
    server.send(200, "application/json",
                "{\"ok\":true,\"camera\":\"ov2640\",\"format\":\"jpeg\",\"resolution\":\"320x240\"}");
}

static void handleCapture() {
    camera_fb_t* frame = esp_camera_fb_get();
    if (frame == nullptr) {
        server.send(503, "text/plain", "Camera capture failed");
        return;
    }

    if (frame->format != PIXFORMAT_JPEG || frame->len < 4 ||
        frame->buf[0] != 0xFF || frame->buf[1] != 0xD8 ||
        frame->buf[frame->len - 2] != 0xFF || frame->buf[frame->len - 1] != 0xD9) {
        esp_camera_fb_return(frame);
        server.send(500, "text/plain", "Invalid JPEG frame");
        return;
    }

    server.setContentLength(frame->len);
    server.send(200, "image/jpeg", "");
    WiFiClient client = server.client();
    size_t sent = 0;
    while (client.connected() && sent < frame->len) {
        size_t n = client.write(frame->buf + sent, frame->len - sent);
        if (n == 0) break;
        sent += n;
        delay(0);
    }

    Serial.printf("[%s] JPEG %u bytes, sent %u bytes
",
                  kTag, static_cast<unsigned>(frame->len), static_cast<unsigned>(sent));
    esp_camera_fb_return(frame);
}

void setup() {
    Serial.begin(115200);
    delay(300);

    if (!psramFound()) {
        Serial.println("[XiaoZhiCam] ERROR: PSRAM not detected. Use an AI Thinker ESP32-CAM with PSRAM.");
        while (true) delay(1000);
    }

    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        Serial.printf("[XiaoZhiCam] Camera init failed: 0x%x
", err);
        while (true) delay(1000);
    }

    if (!connectSavedWiFi()) {
        startSetupPortal();
        return;
    }

    server.on("/", HTTP_GET, handleRoot);
    server.on("/health", HTTP_GET, handleHealth);
    server.on("/capture", HTTP_GET, handleCapture);
    server.begin();

    Serial.printf("[%s] Ready. Camera IP: %s
", kTag, WiFi.localIP().toString().c_str());
    Serial.printf("[%s] Health: http://%s/health
", kTag, WiFi.localIP().toString().c_str());
    Serial.printf("[%s] JPEG: http://%s/capture
", kTag, WiFi.localIP().toString().c_str());
}

void loop() {
    server.handleClient();
    delay(2);
}
