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
#include <vector>
#include <fstream>
#include <string>
#include <d3d11.h>
#include <dxgi1_2.h>
#include "nvEncodeAPI.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <opus/opus.h>
#include <thread>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

std::atomic<bool> g_running{ true };
std::atomic<bool> g_clientConnected{ false }; // <-- ADD THIS

SOCKET g_sendSocket;
sockaddr_in g_destAddr;

SOCKET g_inputRecvSocket;

struct PacketRecord {
    uint32_t seq;
    int size;
    char data[1500]; // Max UDP payload size
};
std::vector<PacketRecord> g_packetHistory(1024); // Ring buffer of the last 1024 packets
uint32_t g_sequenceCounter = 0;

BOOL WINAPI ConsoleHandler(DWORD signal) {
    if (signal == CTRL_CLOSE_EVENT || signal == CTRL_C_EVENT) {
        std::cout << "\n[SYSTEM] Shutdown signal received. Cleaning up..." << std::endl;
        g_running = false;
        Sleep(500); // Give the main loop half a second to exit cleanly
        return TRUE;
    }
    return FALSE;
}

void AudioStreamThread() {
    // COM requires initialization on every new thread
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* pEnumerator = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;
    IAudioCaptureClient* pCaptureClient = nullptr;
    WAVEFORMATEX* pwfx = nullptr;

    // 1. Find the Default Windows Playback Device (Speakers/Headphones)
    CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);

    // 2. Activate the Audio Client
    pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pAudioClient);

    // 3. Get the raw audio format Windows is currently using
    pAudioClient->GetMixFormat(&pwfx);
    std::cout << "[AUDIO] Intercepting: " << pwfx->nSamplesPerSec << " Hz, "
        << pwfx->nChannels << " Channels, " << pwfx->wBitsPerSample << " Bits\n";

    // 4. Initialize WASAPI in LOOPBACK mode (The magic trick)
    // We request a 10ms buffer (100,000 units of 100-nanoseconds) for ultra-low latency
    HRESULT hrInit = pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        100000, 0, pwfx, NULL);

    if (FAILED(hrInit)) {
        std::cerr << "[AUDIO] Failed to initialize loopback! Error: 0x" << std::hex << hrInit << "\n";
        return;
    }

    // 5. Get the Capture tool to extract the PCM bytes
    pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCaptureClient);

    // 6. Initialize the Opus Encoder
    // OPUS_APPLICATION_RESTRICTED_LOWDELAY is designed specifically for gaming/VoIP
    int opusErr;
    OpusEncoder* opusEnc = opus_encoder_create(48000, 2, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &opusErr);
    std::cout << "[AUDIO] Opus Encoder Booted. Status: " << opusErr << " (0 is Success)\n";

    // --- THE WASAPI + OPUS CAPTURE LOOP ---
    pAudioClient->Start();

    UINT32 packetLength = 0;
    std::vector<float> pcmBuffer;

    // 960 frames = exactly 20ms of audio at 48000 Hz. 
    // Opus loves 20ms frames for gaming.
    const int OPUS_FRAME_SIZE = 960;
    const int CHANNELS = 2;
    std::vector<unsigned char> opusOut(4000);

    std::cout << "[AUDIO] Live. Intercepting motherboard audio...\n";

    while (g_running) {
        Sleep(2); // Yield thread lightly

        HRESULT hr = pCaptureClient->GetNextPacketSize(&packetLength);

        // 1. DRAIN THE WASAPI BUFFER
        while (packetLength != 0 && SUCCEEDED(hr)) {
            BYTE* pData;
            UINT32 numFramesAvailable;
            DWORD flags;

            pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                // If the PC is quiet, fill our buffer with pure silence (0.0f)
                pcmBuffer.insert(pcmBuffer.end(), numFramesAvailable * CHANNELS, 0.0f);
            }
            else {
                // Copy the raw 32-bit floats straight from the motherboard
                float* pFloatData = (float*)pData;
                pcmBuffer.insert(pcmBuffer.end(), pFloatData, pFloatData + (numFramesAvailable * CHANNELS));
            }

            pCaptureClient->ReleaseBuffer(numFramesAvailable);
            pCaptureClient->GetNextPacketSize(&packetLength);
        }

        // 2. ENCODE AND SEND OPUS FRAMES
        // Only encode when we have a full 20ms block ready!
        while (pcmBuffer.size() >= OPUS_FRAME_SIZE * CHANNELS) {

            // Compress the float data directly
            int encodedBytes = opus_encode_float(opusEnc, pcmBuffer.data(), OPUS_FRAME_SIZE, opusOut.data(), opusOut.size());

            if (encodedBytes > 0) {
                // Multiplex it: Add the 0x02 AUDIO header
                std::vector<char> udpPacket(encodedBytes + 1);
                udpPacket[0] = 0x02; // 0x02 = AUDIO DATA
                memcpy(udpPacket.data() + 1, opusOut.data(), encodedBytes);

                sendto(g_sendSocket, udpPacket.data(), udpPacket.size(), 0, (struct sockaddr*)&g_destAddr, sizeof(g_destAddr));
            }

            // Delete the 20ms we just encoded from the front of the queue
            pcmBuffer.erase(pcmBuffer.begin(), pcmBuffer.begin() + (OPUS_FRAME_SIZE * CHANNELS));
        }
    }

    pAudioClient->Stop();
    // --------------------------------------

    // Cleanup memory when the thread eventually dies
    if (opusEnc) opus_encoder_destroy(opusEnc);
    if (pwfx) CoTaskMemFree(pwfx);
    if (pCaptureClient) pCaptureClient->Release();
    if (pAudioClient) pAudioClient->Release();
    if (pDevice) pDevice->Release();
    if (pEnumerator) pEnumerator->Release();
    CoUninitialize();
}

void InputInjectionThread() {
    sockaddr_in localAddr = { 0 };
    localAddr.sin_family = AF_INET;
    localAddr.sin_port = htons(9999); // Input port
    localAddr.sin_addr.s_addr = INADDR_ANY;

    g_inputRecvSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    bind(g_inputRecvSocket, (struct sockaddr*)&localAddr, sizeof(localAddr));

    char buf[64];
    sockaddr_in clientAddr;
    int clientLen = sizeof(clientAddr);

    std::cout << "[INPUT] Receiver Live on Port 9999\n";

    while (g_running) {
        int bytes = recvfrom(g_inputRecvSocket, buf, sizeof(buf), 0, (struct sockaddr*)&clientAddr, &clientLen);
        if (bytes > 0) {
            if (!g_clientConnected) {
                // We got our first packet! Lock onto this IP.
                g_destAddr.sin_family = AF_INET;
                g_destAddr.sin_port = htons(8888); // Target the Client's video/audio port

                g_destAddr.sin_addr = clientAddr.sin_addr; // Copy the IP from the incoming packet

                char ipStr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(g_destAddr.sin_addr), ipStr, INET_ADDRSTRLEN);
                std::cout << "\n[NETWORK] Wild Client Appeared! Locked onto IP: " << ipStr << "\n";

                g_clientConnected = true; // Release the GPU pipeline!
            }
            // ---------------------------------------

            // PROTOCOL: [TYPE (1b)] [X (4b)] [Y (4b)] [K_DATA (1b)]
            uint8_t type = buf[0];

            if (type == 0xFF) continue; // Ignore the ping packet, we just needed it for the IP!

            // =======================================================
            // --- NEW: CATCH NACK REQUESTS ---
            // =======================================================
            if (type == 0x08) {
                uint32_t missingSeq = *(uint32_t*)(buf + 1);
                int idx = missingSeq % 1024; // Find where it lives in the ring buffer

                // Sanity check: Ensure the history hasn't overwritten it yet
                if (g_packetHistory[idx].seq == missingSeq) {
                    sendto(g_sendSocket, g_packetHistory[idx].data, g_packetHistory[idx].size, 0, (struct sockaddr*)&g_destAddr, sizeof(g_destAddr));
                    std::cout << "[NACK] Rescued dropped packet: " << missingSeq << "\n";
                }
                continue; // We are done, skip the mouse/keyboard checks
            }
            // =======================================================

            if (type == 0x03) { // MOUSE MOVE (Absolute)
                // FIX: Read the incoming bytes as Integers!
                int scaledX = *(int*)(buf + 1);
                int scaledY = *(int*)(buf + 5);

                INPUT input = { 0 };
                input.type = INPUT_MOUSE;
                input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;

                // Pass the integer directly to the mouse hardware
                input.mi.dx = scaledX;
                input.mi.dy = scaledY;
                SendInput(1, &input, sizeof(INPUT));
            }
            else if (type == 0x05) { // MOUSE CLICK
                uint8_t button = buf[1];
                uint8_t isDown = buf[2];

                INPUT input = { 0 };
                input.type = INPUT_MOUSE;

                if (button == 0) // Left
                    input.mi.dwFlags = isDown ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
                else // Right
                    input.mi.dwFlags = isDown ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;

                SendInput(1, &input, sizeof(INPUT));
            }
            else if (type == 0x04) { // KEYBOARD
                uint8_t vk = buf[1];
                bool isDown = buf[2] == 1;

                INPUT input = { 0 };
                input.type = INPUT_KEYBOARD;
                input.ki.wVk = vk;
                input.ki.dwFlags = isDown ? 0 : KEYEVENTF_KEYUP;
                SendInput(1, &input, sizeof(INPUT));
            }
            else if (type == 0x06) { // RELATIVE MOVE
                int dx = *(int*)(buf + 1);
                int dy = *(int*)(buf + 5);

                INPUT input = { 0 };
                input.type = INPUT_MOUSE;
                // NOTICE: No MOUSEEVENTF_ABSOLUTE here!
                input.mi.dwFlags = MOUSEEVENTF_MOVE;
                input.mi.dx = dx;
                input.mi.dy = dy;
                SendInput(1, &input, sizeof(INPUT));
            }
        }
    }
}

int main() {
    // Prevent resolution misreads from Windows DPI scaling
    SetProcessDPIAware();
    timeBeginPeriod(1);

    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    std::cout << "Booting GameStream Host..." << std::endl;

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    // FIX: Removed the type declarations so we update the GLOBALS!
    g_sendSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    // Clear out the struct properly
    memset(&g_destAddr, 0, sizeof(g_destAddr));

    g_destAddr.sin_family = AF_INET;
    g_destAddr.sin_port = htons(8888);

    std::thread inputThread(InputInjectionThread);
    inputThread.detach();

    // 2. NOW WE CAN GO TO SLEEP AND WAIT!
    // =======================================================
    // --- NEW: WAIT FOR CLIENT TO PUNCH FIRST ---
    // =======================================================
    std::cout << "\n[NETWORK] Engine sleeping. Waiting for a Client to connect...\n";

    while (g_running && !g_clientConnected) {
        Sleep(100); // Sleep lightly until the Input Thread flips the flag
    }

    if (!g_running) return 0; // Safely exit if user pressed Ctrl+C while waiting
    // =======================================================

    std::cout << "Connected! Initializing Encoder..." << std::endl;

    std::thread audioThread(AudioStreamThread);
    audioThread.detach();

    // Hide the local cursor on the host
    ShowCursor(FALSE);

    while (g_running) {
        std::cout << "\n[SYSTEM] Initializing GPU & Encoder Pipeline..." << std::endl;

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
        if (FAILED(dxgiOutput1->DuplicateOutput(d3dDevice, &deskDupl))) {
            std::cout << "[DXGI] Awaiting Secure Desktop exit (UAC prompt active)..." << std::endl;

            // Clean up the half-built DirectX pointers so we don't leak memory
            dxgiOutput1->Release();
            dxgiOutput->Release();
            dxgiAdapter->Release();
            dxgiDevice->Release();
            d3dContext->Release();
            d3dDevice->Release();

            // Wait 1 second and loop back to the top of Phase A to try again
            Sleep(1000);
            continue;
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
        bool pipelineBroken = false;
        std::cout << "Phase D: Streaming. Press ESCAPE to stop." << std::endl;

        while (g_running && !pipelineBroken) {
            IDXGIResource* desktopResource = nullptr;
            DXGI_OUTDUPL_FRAME_INFO frameInfo;

            HRESULT hr = deskDupl->AcquireNextFrame(100, &frameInfo, &desktopResource);

            // --- THE SIMPLE RECOVERY FIX ---
            if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_ACCESS_DENIED || hr == DXGI_ERROR_SESSION_DISCONNECTED) {
                std::cout << "[DXGI] Session lost (UAC/MUX Switch). Rebooting pipeline..." << std::endl;
                pipelineBroken = true;
                continue;
            }

            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            if (FAILED(hr)) break;

            // =========================================================================
            // --- NEW: CURSOR SHAPE EXTRACTION (0x07) ---
            // =========================================================================
            if (frameInfo.PointerShapeBufferSize > 0) {
                UINT sizeReq;
                DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo;
                std::vector<uint8_t> shapeBuffer(frameInfo.PointerShapeBufferSize);

                if (SUCCEEDED(deskDupl->GetFramePointerShape(frameInfo.PointerShapeBufferSize, shapeBuffer.data(), &sizeReq, &shapeInfo))) {

                    // Build Packet 0x07: [Type (1b)] [W (4b)] [H (4b)] [Pitch (4b)] [Format (4b)] [Pixels...]
                    // A standard 32x32 ARGB cursor is exactly 4096 bytes. 
                    // UDP's absolute size limit is 65,507 bytes. 
                    // This means we DON'T need to write a chunker! We can blast it in one shot!
                    std::vector<char> cursorPacket(1 + 16 + sizeReq);
                    cursorPacket[0] = 0x07;
                    memcpy(cursorPacket.data() + 1, &shapeInfo.Width, 4);
                    memcpy(cursorPacket.data() + 5, &shapeInfo.Height, 4);
                    memcpy(cursorPacket.data() + 9, &shapeInfo.Pitch, 4);
                    memcpy(cursorPacket.data() + 13, &shapeInfo.Type, 4);
                    memcpy(cursorPacket.data() + 17, shapeBuffer.data(), sizeReq);

                    sendto(g_sendSocket, cursorPacket.data(), cursorPacket.size(), 0, (struct sockaddr*)&g_destAddr, sizeof(g_destAddr));

                    std::cout << "[CURSOR] Shape Intercepted & Sent! (" << shapeInfo.Width << "x" << shapeInfo.Height << ")\n";
                }
            }
            // =========================================================================

            if (frameInfo.LastPresentTime.QuadPart == 0) {
                if (desktopResource) desktopResource->Release(); // <--- FIX: Prevent Null Pointer Crash!
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

                    // --- THE UDP CHUNKING FIX (MULTIPLEXED) ---
                    const int CHUNK_SIZE = 1400;
                    int bytesRemaining = (int)totalSize;
                    int offset = 0;

                    while (bytesRemaining > 0) {
                        int payloadSize = (bytesRemaining > CHUNK_SIZE) ? CHUNK_SIZE : bytesRemaining;

                        // NEW CUSTOM PROTOCOL: [0x01] [SEQ (4b)] [Payload...]
                        std::vector<char> packet(1 + 4 + payloadSize);
                        packet[0] = 0x01;
                        memcpy(packet.data() + 1, &g_sequenceCounter, 4); // Stamp it!
                        memcpy(packet.data() + 5, dataPtr + offset, payloadSize);

                        // 1. Save a copy to the History Bank FIRST
                        int idx = g_sequenceCounter % 1024;
                        g_packetHistory[idx].seq = g_sequenceCounter;
                        g_packetHistory[idx].size = packet.size();
                        memcpy(g_packetHistory[idx].data, packet.data(), packet.size());

                        // 2. Fire it over the network
                        sendto(g_sendSocket, packet.data(), packet.size(), 0, (struct sockaddr*)&g_destAddr, sizeof(g_destAddr));

                        g_sequenceCounter++; // Increment the master stamper
                        offset += payloadSize;
                        bytesRemaining -= payloadSize;

                        if (offset % (CHUNK_SIZE * 2) == 0) Sleep(0);
                    }

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
    

        if (encoderSession) {
            nvenc.nvEncUnregisterResource(encoderSession, regRes.registeredResource);
            nvenc.nvEncDestroyBitstreamBuffer(encoderSession, bitstreamBuffer.bitstreamBuffer);
            nvenc.nvEncDestroyEncoder(encoderSession);
        }

        if (hwBridgeTexture) hwBridgeTexture->Release();
        if (deskDupl)        deskDupl->Release();
        if (dxgiOutput1)     dxgiOutput1->Release();
        if (dxgiOutput)      dxgiOutput->Release();
        if (dxgiAdapter)     dxgiAdapter->Release();
        if (dxgiDevice)      dxgiDevice->Release();
        if (d3dContext)      d3dContext->Release();
        if (d3dDevice)       d3dDevice->Release();
        if (hNVENC)          FreeLibrary(hNVENC);

    // If g_running is still true, the loop will now jump back to Phase A!
    }

    closesocket(g_sendSocket);
    WSACleanup();

    std::cout << "Host shut down cleanly." << std::endl;
    return 0;
}