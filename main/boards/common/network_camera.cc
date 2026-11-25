#include "network_camera.h"

#include "board.h"
#include "settings.h"
#include "system_info.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <cJSON.h>

#define TAG "NetworkCamera"

NetworkCamera::NetworkCamera() {
    Settings settings("camera");
    camera_url_ = settings.GetString("url");
}

NetworkCamera::~NetworkCamera() {
    ClearFrame();
}

void NetworkCamera::ClearFrame() {
    if (jpeg_data_ != nullptr) {
        heap_caps_free(jpeg_data_);
        jpeg_data_ = nullptr;
    }
    jpeg_size_ = 0;
}

void NetworkCamera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool NetworkCamera::SetCameraUrl(const std::string& url) {
    if (url.rfind("http://", 0) != 0 || url.find("/capture") == std::string::npos) {
        ESP_LOGW(TAG, "Camera URL must be an http://.../capture URL");
        return false;
    }
    Settings settings("camera", true);
    settings.SetString("url", url);
    camera_url_ = url;
    ESP_LOGI(TAG, "Camera URL saved: %s", camera_url_.c_str());
    return true;
}

bool NetworkCamera::Capture() {
    ClearFrame();
    if (camera_url_.empty()) {
        ESP_LOGW(TAG, "Camera URL is not configured");
        return false;
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (auto opened = http->Open("GET", camera_url_); !opened) {
        ESP_LOGE(TAG, "Camera request failed: %s", opened.error().ToString().c_str());
        return false;
    }

    auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        ESP_LOGE(TAG, "Camera HTTP status invalid");
        http->Close();
        return false;
    }

    const size_t length = http->GetBodyLength();
    if (length < 4 || length > kMaxJpegSize) {
        ESP_LOGE(TAG, "JPEG size rejected: %u bytes (limit %u)",
                 static_cast<unsigned>(length), static_cast<unsigned>(kMaxJpegSize));
        http->Close();
        return false;
    }

    jpeg_data_ = static_cast<uint8_t*>(heap_caps_malloc(length, MALLOC_CAP_8BIT));
    if (jpeg_data_ == nullptr) {
        ESP_LOGE(TAG, "Not enough internal heap for %u-byte JPEG",
                 static_cast<unsigned>(length));
        http->Close();
        return false;
    }

    size_t received = 0;
    while (received < length) {
        auto result = http->Read(reinterpret_cast<char*>(jpeg_data_ + received), length - received);
        if (!result || *result == 0) {
            ESP_LOGE(TAG, "JPEG download ended early at %u/%u bytes",
                     static_cast<unsigned>(received), static_cast<unsigned>(length));
            http->Close();
            ClearFrame();
            return false;
        }
        received += *result;
    }
    http->Close();

    if (jpeg_data_[0] != 0xFF || jpeg_data_[1] != 0xD8 ||
        jpeg_data_[length - 2] != 0xFF || jpeg_data_[length - 1] != 0xD9) {
        ESP_LOGE(TAG, "Downloaded frame is not a valid JPEG");
        ClearFrame();
        return false;
    }

    jpeg_size_ = length;
    ESP_LOGI(TAG, "Captured network JPEG: %u bytes", static_cast<unsigned>(jpeg_size_));
    return true;
}

bool NetworkCamera::SetHMirror(bool enabled) {
    (void)enabled;
    return false;
}

bool NetworkCamera::SetVFlip(bool enabled) {
    (void)enabled;
    return false;
}

std::expected<std::string, std::string> NetworkCamera::Explain(const std::string& question) {
    if (explain_url_.empty()) {
        return std::unexpected("Vision explain URL is not configured by the server");
    }
    if (jpeg_data_ == nullptr || jpeg_size_ == 0) {
        return std::unexpected("No JPEG frame is available; capture a photo first");
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    const std::string boundary = "----XIAOZHI_NETWORK_CAMERA";

    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");

    if (auto opened = http->Open("POST", explain_url_); !opened) {
        return std::unexpected("Failed to connect to vision service: " +
                               opened.error().ToString());
    }

    auto write = [&http](const char* data, size_t size) -> bool {
        auto result = http->Write(data, size);
        return !result;
    };

    const std::string vision_question =
        "Please answer in Persian (Farsi). " + question;
    std::string question_part = "--" + boundary +
        "\r\nContent-Disposition: form-data; name=\"question\"\r\n\r\n" +
        vision_question + "\r\n";
    std::string file_header = "--" + boundary +
        "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\""
        "\r\nContent-Type: image/jpeg\r\n\r\n";
    std::string footer = "\r\n--" + boundary + "--\r\n";

    if (write(question_part.data(), question_part.size()) ||
        write(file_header.data(), file_header.size()) ||
        write(reinterpret_cast<const char*>(jpeg_data_), jpeg_size_) ||
        write(footer.data(), footer.size()) || write("", 0)) {
        http->Close();
        return std::unexpected("Failed while uploading JPEG to vision service");
    }

    auto status = http->GetStatusCode();
    if (!status) {
        http->Close();
        return std::unexpected("Failed to read vision service response status");
    }
    if (*status != 200) {
        http->Close();
        return std::unexpected("Vision service returned HTTP " + std::to_string(*status));
    }

    std::string response = http->ReadAll();
    http->Close();

    if (response.empty()) {
        return std::unexpected("Vision service returned an empty response");
    }

    // The vision endpoint returns JSON with the natural-language answer in
    // its "response" field. Return that text to MCP, not the JSON envelope.
    // Keep plain-text responses compatible with alternate vision servers.
    cJSON* root = cJSON_Parse(response.c_str());
    if (root != nullptr) {
        const cJSON* answer = cJSON_GetObjectItemCaseSensitive(root, "response");
        if (cJSON_IsString(answer) && answer->valuestring != nullptr) {
            std::string answer_text(answer->valuestring);
            cJSON_Delete(root);
            if (answer_text.empty()) {
                return std::unexpected("Vision service returned an empty answer");
            }
            return answer_text;
        }
        cJSON_Delete(root);
        return std::unexpected(
            "Vision service JSON does not contain a string 'response' field");
    }

    return response;
}
