#ifndef NETWORK_CAMERA_H
#define NETWORK_CAMERA_H

#include "camera.h"

#include <cstddef>
#include <cstdint>
#include <string>

class NetworkCamera : public Camera {
public:
    NetworkCamera();
    ~NetworkCamera() override;

    void SetExplainUrl(const std::string& url, const std::string& token) override;
    bool SetCameraUrl(const std::string& url) override;
    bool Capture() override;
    bool SetHMirror(bool enabled) override;
    bool SetVFlip(bool enabled) override;
    std::expected<std::string, std::string> Explain(const std::string& question) override;

private:
    static constexpr size_t kMaxJpegSize = 60 * 1024;
    std::string camera_url_;
    std::string explain_url_;
    std::string explain_token_;
    uint8_t* jpeg_data_ = nullptr;
    size_t jpeg_size_ = 0;

    void ClearFrame();
};

#endif  // NETWORK_CAMERA_H
