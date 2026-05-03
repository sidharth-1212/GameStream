#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WINSOCKAPI_
#define _WINSOCKAPI_
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <iostream>
#include <d3d11.h>
#include <dxgi1_2.h>
#include "nvEncodeAPI.h"
#include <thread>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

std::atomic<bool> g_running{ true };

BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_CLOSE_EVENT || signal == CTRL_C_EVENT) {
        std::cout << "\n[SYSTEM] Shutdown signal received. Cleaning up..." << std::endl;
        g_running = false;
        Sleep(500); // Give the main loop half a second to exit cleanly
        return TRUE;
    }
    return FALSE;
}

int main() {
    // Prevent resolution misreads from Windows DPI scaling
    SetProcessDPIAware();
    timeBeginPeriod(1);

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    std::cout << "Booting GameStream Host..." << std::endl;

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    SOCKET sendSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in destAddr = { 0 };
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(8888);
    inet_pton(AF_INET, "192.168.1.24", &destAddr.sin_addr); //192.168.1.24 / 127.0.0.1

    std::cout << "Connecting to Client at 127.0.0.1:8888..." << std::endl;
    while (connect(sendSocket, (struct sockaddr*)&destAddr, sizeof(destAddr)) == SOCKET_ERROR) {
        Sleep(500);
    }
    std::cout << "Connected! Initializing Encoder..." << std::endl;

    // =========================================================================
    // Phase A: D3D11 device + Desktop Duplication
    // =========================================================================
    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &d3dDevice, nullptr, &d3dContext);

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* dxgiAdapter = nullptr;
    IDXGIOutput* dxgiOutput = nullptr;
    IDXGIOutput1* dxgiOutput1 = nullptr;

    d3dDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    dxgiDevice->GetAdapter(&dxgiAdapter);
    dxgiAdapter->EnumOutputs(0, &dxgiOutput);
    dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&dxgiOutput1);

    IDXGIOutputDuplication* deskDupl = nullptr;
    HRESULT hrDupl = dxgiOutput1->DuplicateOutput(d3dDevice, &deskDupl);
    if (FAILED(hrDupl)) {
        std::cerr << "DuplicateOutput failed: 0x" << std::hex << hrDupl << std::endl;
        return 1;
    }

    // Detect the desktop's actual pixel format so NVENC gets the right buffer format
    DXGI_OUTDUPL_DESC duplDesc;
    deskDupl->GetDesc(&duplDesc);
    int activeWidth = duplDesc.ModeDesc.Width;
    int activeHeight = duplDesc.ModeDesc.Height;

    std::cout << "[DXGI] Capturing at: " << activeWidth << "x" << activeHeight << std::endl;
    DXGI_FORMAT desktopFormat = duplDesc.ModeDesc.Format;

    // Map DXGI format → NVENC buffer format
    // DXGI_FORMAT_B8G8R8A8_UNORM → ARGB in NVENC terminology (B=byte0, G=byte1, R=byte2, A=byte3)
    // DXGI_FORMAT_R8G8B8A8_UNORM → ABGR in NVENC terminology
    NV_ENC_BUFFER_FORMAT nvencFormat =
        (desktopFormat == DXGI_FORMAT_R8G8B8A8_UNORM)
        ? NV_ENC_BUFFER_FORMAT_ABGR
        : NV_ENC_BUFFER_FORMAT_ARGB;

    std::cout << "Desktop format: "
        << (nvencFormat == NV_ENC_BUFFER_FORMAT_ABGR ? "RGBA->ABGR" : "BGRA->ARGB")
        << std::endl;

    // =========================================================================
    // Phase B: NVENC setup
    // =========================================================================
    HMODULE hNVENC = LoadLibrary(TEXT("nvEncodeAPI64.dll"));
    if (!hNVENC) {
        std::cerr << "Failed to load nvEncodeAPI64.dll" << std::endl;
        return 1;
    }

    typedef NVENCSTATUS(NVENCAPI* NvEncodeAPICreateInstance_Type)(NV_ENCODE_API_FUNCTION_LIST*);
    auto NvEncodeAPICreateInstance =
        (NvEncodeAPICreateInstance_Type)GetProcAddress(hNVENC, "NvEncodeAPICreateInstance");

    NV_ENCODE_API_FUNCTION_LIST nvenc = { NV_ENCODE_API_FUNCTION_LIST_VER };
    NvEncodeAPICreateInstance(&nvenc);

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sessionParams = { NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER };
    sessionParams.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    sessionParams.device = (void*)d3dDevice;
    sessionParams.apiVersion = NVENCAPI_VERSION;

    void* encoderSession = nullptr;
    NVENCSTATUS nvStatus = nvenc.nvEncOpenEncodeSessionEx(&sessionParams, &encoderSession);
    if (nvStatus != NV_ENC_SUCCESS) {
        std::cerr << "nvEncOpenEncodeSessionEx failed: " << nvStatus << std::endl;
        return 1;
    }

    NV_ENC_INITIALIZE_PARAMS initParams = { NV_ENC_INITIALIZE_PARAMS_VER };
    GUID encodeGUID = NV_ENC_CODEC_H264_GUID;
    GUID presetGUID = NV_ENC_PRESET_P3_GUID;
    initParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;

    NV_ENC_PRESET_CONFIG presetConfig = { NV_ENC_PRESET_CONFIG_VER };
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
    nvenc.nvEncGetEncodePresetConfigEx(encoderSession, encodeGUID, presetGUID,
        initParams.tuningInfo, &presetConfig);

    NV_ENC_CONFIG encodeConfig = presetConfig.presetCfg;
    initParams.encodeConfig = &encodeConfig;
    initParams.encodeGUID = encodeGUID;
    initParams.presetGUID = presetGUID;
    initParams.encodeWidth = activeWidth;
    initParams.encodeHeight = activeHeight;

    
    initParams.frameRateNum = 60;
    initParams.frameRateDen = 1;
    initParams.enablePTD = 1;
    encodeConfig.gopLength = 60;
    encodeConfig.encodeCodecConfig.h264Config.idrPeriod = 60;

    // Refresh 1/30th of the screen every frame
    encodeConfig.encodeCodecConfig.h264Config.enableIntraRefresh = 0;
    encodeConfig.encodeCodecConfig.h264Config.intraRefreshPeriod = 30;
    encodeConfig.encodeCodecConfig.h264Config.intraRefreshCnt = 1;

    // Send headers with EVERY frame so the client never gets lost
    encodeConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1;

    // Lower the bitrate slightly for stability while testing loopback
    encodeConfig.rcParams.averageBitRate = 8000000;
    encodeConfig.rcParams.maxBitRate = 12000000;

    // The VBV Buffer is the "bank" NVENC draws from during alt-tabs.
    // Set it to hold exactly 1 second of maximum data.
    encodeConfig.rcParams.vbvBufferSize = 12000000;
    
    encodeConfig.rcParams.enableAQ = 1;

    nvenc.nvEncInitializeEncoder(encoderSession, &initParams);
    std::cout << "NVENC encoder initialized." << std::endl;

    NV_ENC_CREATE_BITSTREAM_BUFFER bitstreamBuffer = { NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
    nvenc.nvEncCreateBitstreamBuffer(encoderSession, &bitstreamBuffer);

    // =========================================================================
    // Phase C: Hardware bridge texture
    // D3D11_USAGE_DEFAULT keeps it in VRAM. NVENC reads directly from there
    // without any CPU round-trip.
    // =========================================================================
    D3D11_TEXTURE2D_DESC hwDesc = { 0 };
    hwDesc.Width = activeWidth;   // 👈 Match monitor width (e.g., 1920)
    hwDesc.Height = activeHeight;
    hwDesc.MipLevels = 1;
    hwDesc.ArraySize = 1;
    hwDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // FIX: Force BGRA for GDI compatibility
    hwDesc.SampleDesc.Count = 1;
    hwDesc.Usage = D3D11_USAGE_DEFAULT;
    hwDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    // --- THE CURSOR HACK FLAG ---
    hwDesc.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
    // ----------------------------

    ID3D11Texture2D* hwBridgeTexture = nullptr;
    d3dDevice->CreateTexture2D(&hwDesc, nullptr, &hwBridgeTexture);

    NV_ENC_REGISTER_RESOURCE regRes = { NV_ENC_REGISTER_RESOURCE_VER };
    regRes.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    regRes.resourceToRegister = (void*)hwBridgeTexture;
    regRes.width = activeWidth;   // 👈 Update this!
    regRes.height = activeHeight;
    // Since we forced BGRA above, tell NVENC to expect ARGB (NVENC's name for BGRA)
    regRes.bufferFormat = NV_ENC_BUFFER_FORMAT_ARGB;
    nvenc.nvEncRegisterResource(encoderSession, &regRes);

    // =========================================================================
    // Phase D: Capture + encode loop
    // =========================================================================

    // Timing governor: cap at exactly 60 fps using QueryPerformanceCounter
    LARGE_INTEGER frequency, frameStart, current;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&frameStart);
    const LONGLONG targetTicks = frequency.QuadPart / 60;

    int framesCaptured = 0;
    std::cout << "Phase D: Streaming. Press ESCAPE to stop." << std::endl;

    while (g_running) {
        IDXGIResource* desktopResource = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo;

        HRESULT hr = deskDupl->AcquireNextFrame(100, &frameInfo, &desktopResource);

        // --- THE FULLSCREEN RECOVERY FIX ---
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_ACCESS_DENIED || hr == DXGI_ERROR_SESSION_DISCONNECTED) {
            std::cout << "[DXGI] Display mode changed. Recovering..." << std::endl;

            if (deskDupl) { deskDupl->Release(); deskDupl = nullptr; }

            // FIX: We must also release the old bridge texture if the resolution might change
            if (hwBridgeTexture) { hwBridgeTexture->Release(); hwBridgeTexture = nullptr; }

            bool recovered = false;
            while (!recovered && g_running) {
                Sleep(500);

                HRESULT reinitHr = dxgiOutput1->DuplicateOutput(d3dDevice, &deskDupl);
                if (SUCCEEDED(reinitHr)) {
                    // --- THE FIX: Refresh actual monitor resolution ---
                    deskDupl->GetDesc(&duplDesc);
                    activeWidth = duplDesc.ModeDesc.Width;
                    activeHeight = duplDesc.ModeDesc.Height;
                    std::cout << "[DXGI] Recovered at new resolution: " << activeWidth << "x" << activeHeight << std::endl;

                    // --- RE-CREATE BRIDGE TEXTURE ---
                    D3D11_TEXTURE2D_DESC hwDesc = { 0 };
                    hwDesc.Width = activeWidth;
                    hwDesc.Height = activeHeight;
                    hwDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    hwDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                    hwDesc.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
                    hwDesc.SampleDesc.Count = 1;
                    hwDesc.Usage = D3D11_USAGE_DEFAULT;
                    d3dDevice->CreateTexture2D(&hwDesc, nullptr, &hwBridgeTexture);

                    recovered = true;
                }
            }
            continue;
        }
        // ------------------------------------

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (FAILED(hr)) break;

        // If the OS says there was no update to the image, skip encoding to save bandwidth.
        if (frameInfo.LastPresentTime.QuadPart == 0) {
            desktopResource->Release();
            deskDupl->ReleaseFrame();
            continue;
        }

        ID3D11Texture2D* desktopTexture = nullptr;
        desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTexture);

        // Clamp copy box to actual source dimensions (defensive, handles resolution edge cases)
        D3D11_TEXTURE2D_DESC srcDesc;
        desktopTexture->GetDesc(&srcDesc);
        D3D11_BOX srcBox;
        srcBox.left = 0;
        srcBox.right = activeWidth;
        srcBox.top = 0;
        srcBox.bottom = activeHeight; // 👈 No more hardcoded 1200u
        srcBox.front = 0;
        srcBox.back = 1;

        // GPU-to-GPU copy: desktop texture → unprotected bridge texture
        d3dContext->CopySubresourceRegion(hwBridgeTexture, 0, 0, 0, 0,
            desktopTexture, 0, &srcBox);

        // --- VERY QUICK CURSOR DRAW ---
        // Request a GDI "Device Context" (DC) for our GPU texture
        IDXGISurface1* gdiSurface = nullptr;
        if (SUCCEEDED(hwBridgeTexture->QueryInterface(__uuidof(IDXGISurface1), (void**)&gdiSurface))) {
            HDC hdc;
            if (SUCCEEDED(gdiSurface->GetDC(FALSE, &hdc))) {

                // Get the current cursor state and position
                CURSORINFO ci = { sizeof(CURSORINFO) };
                if (GetCursorInfo(&ci) && ci.flags == CURSOR_SHOWING) {
                    // Draw the icon directly into the GPU texture
                    DrawIcon(hdc, ci.ptScreenPos.x, ci.ptScreenPos.y, ci.hCursor);
                }

                // Release the DC so NVENC can use the texture
                gdiSurface->ReleaseDC(nullptr);
            }
            gdiSurface->Release();
        }
        // ------------------------------

        // Release the OS-owned desktop texture immediately
        desktopTexture->Release();
        desktopResource->Release();
        deskDupl->ReleaseFrame();

        // Map bridge texture into NVENC
        NV_ENC_MAP_INPUT_RESOURCE mapRes = { NV_ENC_MAP_INPUT_RESOURCE_VER };
        mapRes.registeredResource = regRes.registeredResource;
        nvenc.nvEncMapInputResource(encoderSession, &mapRes);

        // Encode
        NV_ENC_PIC_PARAMS picParams = { NV_ENC_PIC_PARAMS_VER };
        picParams.inputWidth = activeWidth;
        picParams.inputHeight = activeHeight;
        picParams.inputBuffer = mapRes.mappedResource;
        picParams.outputBitstream = bitstreamBuffer.bitstreamBuffer;
        picParams.bufferFmt = nvencFormat;
        picParams.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;

        if (nvenc.nvEncEncodePicture(encoderSession, &picParams) == NV_ENC_SUCCESS) {
            NV_ENC_LOCK_BITSTREAM lockParams = { NV_ENC_LOCK_BITSTREAM_VER };
            lockParams.outputBitstream = bitstreamBuffer.bitstreamBuffer;

            if (nvenc.nvEncLockBitstream(encoderSession, &lockParams) == NV_ENC_SUCCESS) {
                uint32_t totalSize = lockParams.bitstreamSizeInBytes;
                uint8_t* dataPtr = (uint8_t*)lockParams.bitstreamBufferPtr;

                // --- THE UDP CHUNKING FIX ---
                const int CHUNK_SIZE = 1400; // Stay under the 1500 byte MTU limit
                int bytesRemaining = (int)totalSize;
                int offset = 0;

                while (bytesRemaining > 0) {
                    int packetSize = (bytesRemaining > CHUNK_SIZE) ? CHUNK_SIZE : bytesRemaining;
                    send(sendSocket, (char*)(dataPtr + offset), packetSize, 0);

                    offset += packetSize;
                    bytesRemaining -= packetSize;

                    // The Discord standard: Space UDP packets out across the 16.6ms frame window
                    // to prevent router queue overflow.
                    if (offset % (CHUNK_SIZE * 2) == 0) {
                        // Using Sleep(0) yields the thread slice safely without 
                        // forcing the massive 1-2ms delay of Sleep(1).
                        Sleep(0);
                    }
                }
                // ----------------------------

                if (framesCaptured % 60 == 0) {
                    std::cout << "Streaming (UDP)... Frame " << framesCaptured << std::endl;
                }

                nvenc.nvEncUnlockBitstream(encoderSession, bitstreamBuffer.bitstreamBuffer);
                framesCaptured++;
            }
        }

        // Unmap so the bridge texture is free to be written next frame
        nvenc.nvEncUnmapInputResource(encoderSession, mapRes.mappedResource);
    }

done:
    std::cout << "Shutting down..." << std::endl;

    // Flush and destroy encoder
    if (encoderSession) {
        NV_ENC_PIC_PARAMS flushParams = { NV_ENC_PIC_PARAMS_VER };
        flushParams.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        nvenc.nvEncEncodePicture(encoderSession, &flushParams);

        nvenc.nvEncUnregisterResource(encoderSession, regRes.registeredResource);
        nvenc.nvEncDestroyBitstreamBuffer(encoderSession, bitstreamBuffer.bitstreamBuffer);
        nvenc.nvEncDestroyEncoder(encoderSession);
    }

    if (hwBridgeTexture) hwBridgeTexture->Release();
    if (hNVENC)          FreeLibrary(hNVENC);
    if (deskDupl)        deskDupl->Release();
    if (dxgiOutput1)     dxgiOutput1->Release();
    if (dxgiOutput)      dxgiOutput->Release();
    if (dxgiAdapter)     dxgiAdapter->Release();
    if (dxgiDevice)      dxgiDevice->Release();
    if (d3dContext)      d3dContext->Release();
    if (d3dDevice)       d3dDevice->Release();

    closesocket(sendSocket);
    WSACleanup();

    std::cout << "Host shut down cleanly. " << framesCaptured << " frames sent." << std::endl;
    return 0;
}