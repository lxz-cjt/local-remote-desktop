#include "desktop_capture.h"
#include <chrono>
#include <cstring>
#include <iostream>

DesktopCapture::DesktopCapture() = default;
DesktopCapture::~DesktopCapture() = default;

bool DesktopCapture::Initialize() {
    // 清理旧对象
    duplication_.Reset();
    context_.Reset();
    device_.Reset();

    D3D_FEATURE_LEVEL featureLevel;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr, 0, D3D11_SDK_VERSION,
        &device_, &featureLevel, &context_
    );
    if (FAILED(hr)) {
        std::cerr << "[Capture] D3D11CreateDevice failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = device_.As(&dxgiDevice);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetParent(IID_PPV_ARGS(&dxgiAdapter));
    if (FAILED(hr)) return false;

    ComPtr<IDXGIOutput> dxgiOutput;
    hr = dxgiAdapter->EnumOutputs(0, &dxgiOutput);
    if (FAILED(hr)) {
        std::cerr << "[Capture] EnumOutputs failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    DXGI_OUTPUT_DESC outputDesc;
    dxgiOutput->GetDesc(&outputDesc);
    screen_width_ = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
    screen_height_ = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

    ComPtr<IDXGIOutput1> dxgiOutput1;
    hr = dxgiOutput.As(&dxgiOutput1);
    if (FAILED(hr)) return false;

    hr = dxgiOutput1->DuplicateOutput(device_.Get(), &duplication_);
    if (FAILED(hr)) {
        std::cerr << "[Capture] DuplicateOutput failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    return true;
}

void DesktopCapture::GetScreenSize(uint32_t& width, uint32_t& height) const {
    width = screen_width_;
    height = screen_height_;
}

bool DesktopCapture::CaptureFrame(CapturedFrame& frame) {
    if (!duplication_) {
        if (!Initialize()) return false;
    }

    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    ComPtr<IDXGIResource> resource;

    HRESULT hr = duplication_->AcquireNextFrame(100, &frameInfo, &resource);

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;
    }

    if (FAILED(hr)) {
        // 设备丢失或重置，尝试重建
        std::cerr << "[Capture] AcquireNextFrame failed: 0x" << std::hex << hr
                  << ", reinitializing..." << std::endl;
        duplication_.Reset();
        context_.Reset();
        device_.Reset();
        Initialize();
        return false;
    }

    ComPtr<ID3D11Texture2D> texture;
    hr = resource.As(&texture);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        return false;
    }

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    // 创建 staging texture 用于 CPU 读取
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> stagingTexture;
    hr = device_->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        return false;
    }

    context_->CopyResource(stagingTexture.Get(), texture.Get());

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context_->Map(stagingTexture.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        return false;
    }

    frame.width = desc.Width;
    frame.height = desc.Height;
    frame.rgba_data.resize(frame.width * frame.height * 4);
    frame.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // DXGI 输出通常是 BGRA，复制到目标缓冲区
    for (uint32_t y = 0; y < frame.height; ++y) {
        std::memcpy(
            frame.rgba_data.data() + y * frame.width * 4,
            static_cast<uint8_t*>(mapped.pData) + y * mapped.RowPitch,
            frame.width * 4
        );
    }

    context_->Unmap(stagingTexture.Get(), 0);
    duplication_->ReleaseFrame();
    return true;
}
