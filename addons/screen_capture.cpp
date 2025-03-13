#include <napi.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <thread>
#include <atomic>
#include <vector>
#include <mutex>
#include <iostream>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")

class ScreenCapturer {
public:
    ScreenCapturer() : isRunning(false), useGDI(false), currentMonitorIndex(0) {
        InitializeDirectX();
        InitializeGDI();
        isRunning = true;
        captureThread = std::thread(&ScreenCapturer::CaptureLoop, this);
    }

    ~ScreenCapturer() {
        isRunning = false;
        if (captureThread.joinable()) captureThread.join();

        CleanupDirectX();
        CleanupGDI();
    }

    Napi::Object GetLatestFrame(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        std::lock_guard<std::mutex> lock(frameMutex);

        // Return frame data and dimensions
        Napi::Object result = Napi::Object::New(env);
        result.Set("data", Napi::Buffer<uint8_t>::Copy(env, latestFrame.data(), latestFrame.size()));
        result.Set("width", width);
        result.Set("height", height);
        return result;
    }

    Napi::Array ListMonitors(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Napi::Array monitorList = Napi::Array::New(env);

        IDXGIFactory1* pFactory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory))) {
            throw std::runtime_error("Failed to create DXGI factory");
        }

        IDXGIAdapter1* pAdapter = nullptr;
        for (UINT i = 0; pFactory->EnumAdapters1(i, &pAdapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            IDXGIOutput* pOutput = nullptr;
            for (UINT j = 0; pAdapter->EnumOutputs(j, &pOutput) != DXGI_ERROR_NOT_FOUND; ++j) {
                DXGI_OUTPUT_DESC outputDesc;
                pOutput->GetDesc(&outputDesc);

                // Convert WCHAR (wide character) to char (narrow character)
                char deviceName[32];
                WideCharToMultiByte(CP_UTF8, 0, outputDesc.DeviceName, -1, deviceName, sizeof(deviceName), nullptr, nullptr);

                Napi::Object monitorInfo = Napi::Object::New(env);
                monitorInfo.Set("deviceName", Napi::String::New(env, deviceName));
                monitorInfo.Set("index", Napi::Number::New(env, j));

                monitorList.Set(monitorList.Length(), monitorInfo);
                pOutput->Release();
            }
            pAdapter->Release();
        }
        pFactory->Release();

        return monitorList;
    }

    void SwitchMonitor(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (info.Length() < 1 || !info[0].IsNumber()) {
            throw Napi::Error::New(env, "Invalid argument: expected monitor index");
        }

        int newMonitorIndex = info[0].As<Napi::Number>().Int32Value();
        if (newMonitorIndex < 0) {
            throw Napi::Error::New(env, "Invalid monitor index");
        }

        std::lock_guard<std::mutex> lock(frameMutex); // Ensure thread safety

        try {
            currentMonitorIndex = newMonitorIndex;
            ReinitializeDirectX(); // Reinitialize DirectX resources for the new monitor
        } catch (const std::exception& e) {
            throw Napi::Error::New(env, std::string("Failed to switch monitor: ") + e.what());
        }
    }

private:
    void InitializeDirectX() {
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                       D3D11_SDK_VERSION, &pDevice, nullptr, nullptr);
        if (FAILED(hr)) throw std::runtime_error("Failed to create D3D11 device");

        IDXGIFactory1* pFactory = nullptr;
        hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory);
        if (FAILED(hr)) throw std::runtime_error("Failed to create DXGI factory");

        hr = pFactory->EnumAdapters1(0, &pAdapter);
        if (FAILED(hr)) throw std::runtime_error("Failed to enumerate adapters");

        hr = pAdapter->EnumOutputs(currentMonitorIndex, &pOutput);
        if (FAILED(hr)) throw std::runtime_error("Failed to enumerate outputs");

        hr = pOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&pOutput1);
        if (FAILED(hr)) throw std::runtime_error("Failed to query IDXGIOutput1 interface");

        hr = pOutput1->DuplicateOutput(pDevice, &pDuplication);
        if (FAILED(hr)) throw std::runtime_error("Failed to duplicate output");
    }

    void ReinitializeDirectX() {
        CleanupDirectX(); // Clean up existing DirectX resources
        InitializeDirectX(); // Reinitialize DirectX resources for the new monitor
    }

    void CleanupDirectX() {
        if (pDuplication) pDuplication->Release();
        if (pOutput1) pOutput1->Release();
        if (pOutput) pOutput->Release();
        if (pAdapter) pAdapter->Release();
        if (pDevice) pDevice->Release();
    }

    void InitializeGDI() {
        hdcScreen = GetDC(nullptr);
        hdcMem = CreateCompatibleDC(hdcScreen);
        width = GetSystemMetrics(SM_CXSCREEN);
        height = GetSystemMetrics(SM_CYSCREEN);
        hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
        SelectObject(hdcMem, hBitmap);
    }

    void CleanupGDI() {
        DeleteObject(hBitmap);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
    }

    bool IsUACActive() {
        HWND hwnd = GetForegroundWindow();
        if (hwnd == nullptr) return false;

        char className[256];
        GetClassName(hwnd, className, sizeof(className));

        // UAC prompts typically have the class name "Credential Dialog Xaml Host"
        return strcmp(className, "Credential Dialog Xaml Host") == 0;
    }

    void CaptureLoop() {
        while (isRunning) {
            bool uacActive = IsUACActive();

            if (uacActive && !useGDI) {
                // Switch to GDI mode
                std::lock_guard<std::mutex> lock(frameMutex); // Ensure thread safety
                useGDI = true;
                CleanupDirectX();
            } else if (!uacActive && useGDI) {
                // Switch back to DirectX mode
                std::lock_guard<std::mutex> lock(frameMutex); // Ensure thread safety
                useGDI = false;
                ReinitializeDirectX();
            }

            std::vector<uint8_t> frameData;

            if (useGDI) {
                // Use GDI to capture the screen
                BitBlt(hdcMem, 0, 0, width, height, hdcScreen, 0, 0, SRCCOPY);

                // Get the bitmap data
                BITMAPINFOHEADER bi = {0};
                bi.biSize = sizeof(BITMAPINFOHEADER);
                bi.biWidth = width;
                bi.biHeight = -height; // Negative height to ensure top-down DIB
                bi.biPlanes = 1;
                bi.biBitCount = 32;
                bi.biCompression = BI_RGB;

                frameData.resize(width * height * 4);
                GetDIBits(hdcMem, hBitmap, 0, height, frameData.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
            } else {
                // Use DirectX to capture the screen
                std::lock_guard<std::mutex> lock(frameMutex); // Ensure thread safety

                DXGI_OUTDUPL_FRAME_INFO frameInfo;
                IDXGIResource* pResource = nullptr;

                HRESULT hr = pDuplication->AcquireNextFrame(100, &frameInfo, &pResource);
                if (FAILED(hr)) continue;

                ID3D11Texture2D* pTexture = nullptr;
                hr = pResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pTexture);
                if (FAILED(hr)) {
                    pDuplication->ReleaseFrame();
                    pResource->Release();
                    continue;
                }

                D3D11_TEXTURE2D_DESC desc;
                pTexture->GetDesc(&desc);

                // Create a staging texture to copy the frame data
                ID3D11Texture2D* pStagingTexture = nullptr;
                desc.Usage = D3D11_USAGE_STAGING;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                desc.BindFlags = 0;
                desc.MiscFlags = 0;

                hr = pDevice->CreateTexture2D(&desc, nullptr, &pStagingTexture);
                if (FAILED(hr)) {
                    pDuplication->ReleaseFrame();
                    pResource->Release();
                    pTexture->Release();
                    continue;
                }

                // Copy the frame data to the staging texture
                pDevice->GetImmediateContext(&pContext);
                pContext->CopyResource(pStagingTexture, pTexture);

                // Map the staging texture to access the pixel data
                D3D11_MAPPED_SUBRESOURCE mappedResource;
                hr = pContext->Map(pStagingTexture, 0, D3D11_MAP_READ, 0, &mappedResource);
                if (FAILED(hr)) {
                    pDuplication->ReleaseFrame();
                    pResource->Release();
                    pTexture->Release();
                    pStagingTexture->Release();
                    continue;
                }

                // Copy the pixel data to a buffer
                frameData.resize(desc.Width * desc.Height * 4);
                memcpy(frameData.data(), mappedResource.pData, frameData.size());

                // Clean up
                pContext->Unmap(pStagingTexture, 0);
                pDuplication->ReleaseFrame();
                pResource->Release();
                pTexture->Release();
                pStagingTexture->Release();
            }

            // Convert BGRA to RGBA
            for (size_t i = 0; i < frameData.size(); i += 4) {
                std::swap(frameData[i], frameData[i + 2]); // Swap Red and Blue channels
            }

            // Update the latest frame
            {
                std::lock_guard<std::mutex> lock(frameMutex);
                latestFrame = std::move(frameData);
            }
        }
    }

    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pContext = nullptr;
    IDXGIAdapter1* pAdapter = nullptr;
    IDXGIOutput* pOutput = nullptr;
    IDXGIOutput1* pOutput1 = nullptr;
    IDXGIOutputDuplication* pDuplication = nullptr;

    HDC hdcScreen = nullptr;
    HDC hdcMem = nullptr;
    HBITMAP hBitmap = nullptr;
    int width = 0;
    int height = 0;

    std::thread captureThread;
    std::atomic<bool> isRunning;
    std::atomic<bool> useGDI;
    std::vector<uint8_t> latestFrame;
    std::mutex frameMutex;

    int currentMonitorIndex;
};

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    ScreenCapturer* capturer = new ScreenCapturer();
    exports.Set(Napi::String::New(env, "getLatestFrame"),
              Napi::Function::New(env, [capturer](const Napi::CallbackInfo& info) {
                  return capturer->GetLatestFrame(info);
              }));
    exports.Set(Napi::String::New(env, "listMonitors"),
              Napi::Function::New(env, [capturer](const Napi::CallbackInfo& info) {
                  return capturer->ListMonitors(info);
              }));
    exports.Set(Napi::String::New(env, "switchMonitor"),
              Napi::Function::New(env, [capturer, env](const Napi::CallbackInfo& info) {
                  capturer->SwitchMonitor(info);
                  return env.Undefined();
              }));
    return exports;
}

NODE_API_MODULE(screen_capture, Init);