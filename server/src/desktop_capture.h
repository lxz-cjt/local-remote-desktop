#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include <memory>

using Microsoft::WRL::ComPtr;

struct CapturedFrame {
    std::vector<uint8_t> rgba_data;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t timestamp_us = 0;
};

class DesktopCapture {
public:
    DesktopCapture();
    ~DesktopCapture();

    bool Initialize();
    bool CaptureFrame(CapturedFrame& frame);
    void GetScreenSize(uint32_t& width, uint32_t& height) const;

private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    uint32_t screen_width_ = 0;
    uint32_t screen_height_ = 0;
};
