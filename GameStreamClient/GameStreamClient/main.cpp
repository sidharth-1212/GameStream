#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <iostream>
#include <vector>
#include <atomic>
#include <fstream>
#include <sstream>
#include <iomanip>

#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <opus/opus.h>
#include <xaudio2.h>


extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

const int FRAME_WIDTH = 1920;
const int FRAME_HEIGHT = 1080;   // FIX: Changed from 1200 to 1080
const int CODED_HEIGHT = 1080;   // FIX: Changed from 1200 to 1080
const int TEXTURE_HEIGHT = 2160; // Y(1080) + U(540) + V(540)

bool timingStarted = false;
LARGE_INTEGER frameStart;

// --- globals ---
ID3D11Texture2D* g_pDisplayTexture = nullptr;     // RESTORED: The actual texture in GPU memory
ID3D11ShaderResourceView* g_pLumaSRV = nullptr;   // NEW: Brightness view for NV12
ID3D11ShaderResourceView* g_pChromaSRV = nullptr; // NEW: Color view for NV12
ID3D11ShaderResourceView* g_pDisplaySRV = nullptr;
ID3D11Buffer* g_pConstantBuffer = nullptr;
static std::vector<uint8_t> g_frameBuffer;
static size_t               g_frameRowPitch = FRAME_WIDTH;
static std::atomic<bool>    g_newFrameReady{ false };

IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11DeviceContext* g_pContext = nullptr;
ID3D11RenderTargetView* g_pRTV = nullptr;

struct Vertex { float x, y, z; float u, v; };
ID3D11InputLayout* g_pLayout = nullptr;
ID3D11VertexShader* g_pVS = nullptr;
ID3D11PixelShader* g_pPS = nullptr;
ID3D11PixelShader* pPSDbg = nullptr;
ID3D11Buffer* g_pVBuffer = nullptr;
ID3D11SamplerState* g_pSampler = nullptr;

// ============================================================
// DIAGNOSTIC HELPERS
// ============================================================

static void DumpBytes(const char* label, const uint8_t* data, size_t len) {
    size_t show = (len < 256) ? len : 256;
    std::ostringstream oss;
    oss << "\n[DUMP] " << label << " (first " << show << " of " << len << " bytes):\n";
    for (size_t i = 0; i < show; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i] << " ";
        if ((i + 1) % 16 == 0) oss << "\n";
    }
    oss << std::dec << "\n";
    std::cout << oss.str();
}

static void SaveFirstFrameYUV(const uint8_t* data, size_t rowPitch) {
    static bool saved = false;
    if (saved) return;
    saved = true;

    std::ofstream f("first_frame.yuv", std::ios::binary);
    if (!f) { std::cerr << "[DIAG] Could not write first_frame.yuv\n"; return; }

    for (int row = 0; row < FRAME_HEIGHT; row++)
        f.write((char*)(data + row * rowPitch), FRAME_WIDTH);

    for (int row = 0; row < CODED_HEIGHT / 2; row++)
        f.write((char*)(data + (CODED_HEIGHT + row) * rowPitch), FRAME_WIDTH);

    f.close();
    std::cout << "[DIAG] Saved first_frame.yuv (" << FRAME_WIDTH << "x" << FRAME_HEIGHT
        << " NV12). Open with ffplay.\n";
}

// ============================================================
// SHADER SOURCE
// ============================================================

const char* vertexShaderSrc = R"(
struct VOut {
    float4 position : SV_POSITION;
    float2 tex : TEXCOORD0;
};
VOut main(float4 position : POSITION, float2 tex : TEXCOORD) {
    VOut output;
    output.position = position;
    output.tex = tex;
    return output;
}
)";

const char* pixelShaderSrc = R"(
cbuffer SceneData : register(b0) {
    float screenHeight; 
};

Texture2D<float> lumaTexture : register(t0);
Texture2D<float2> chromaTexture : register(t1);
SamplerState SampleType : register(s0);

// I accidentally deleted this struct in the last step!
struct PixelInputType {
    float4 position : SV_POSITION;
    float2 tex : TEXCOORD0;
};

float4 main(PixelInputType input) : SV_TARGET {
    float luma = lumaTexture.Sample(SampleType, input.tex);
    
    float2 uv = chromaTexture.Sample(SampleType, input.tex) - 0.5f;

    float r = luma + 1.402f * uv.y;
    float g = luma - 0.344f * uv.x - 0.714f * uv.y;
    float b = luma + 1.772f * uv.x;

    return float4(r, g, b, 1.0f);
}
)";

const char* pixelShaderDebugSrc = R"(
Texture2D shaderTexture : register(t0);
SamplerState SampleType : register(s0);

struct PixelInputType {
    float4 position : SV_POSITION;
    float2 tex : TEXCOORD0;
};

float4 main(PixelInputType input) : SV_TARGET {
    uint x = clamp((uint)(input.tex.x * 1920.0f), 0u, 1919u);
    uint y = clamp((uint)(input.tex.y * 1200.0f), 0u, 1199u);
    float v = shaderTexture.Load(int3(x, y, 0)).r;
    return float4(v, v, v, 1.0f); 
}
)";

// ============================================================
// WINDOW
// ============================================================

static HWND CreateClientWindow() {
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"GameStreamClient";

    wc.hCursor = NULL;

    RegisterClass(&wc);

    // Get the physical monitor dimensions of the Client PC
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    // WS_POPUP creates a Borderless window so the title bar doesn't push the video down!
    return CreateWindowEx(0, L"GameStreamClient", L"GameStream Client",
        WS_POPUP | WS_VISIBLE, 0, 0,
        screenW, screenH, NULL, NULL, GetModuleHandle(NULL), NULL);
}

// ============================================================
// MAIN
// ============================================================

int main() {
    SetProcessDPIAware();
    timeBeginPeriod(1);
    std::cout << "=== GameStream Client (Hardware Agnostic FFmpeg Build) ===\n\n";

    // Pre-allocate the frame buffer so it's ready for FFmpeg

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    SOCKET inputSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in hostInputAddr = { 0 };
    hostInputAddr.sin_family = AF_INET;
    hostInputAddr.sin_port = htons(9999);
    inet_pton(AF_INET, "192.168.1.17", &hostInputAddr.sin_addr);

    SOCKET recvSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in recvAddr = { 0 };
    recvAddr.sin_family = AF_INET;
    recvAddr.sin_port = htons(8888);
    recvAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(recvSocket, (struct sockaddr*)&recvAddr, sizeof(recvAddr)) == SOCKET_ERROR) {
        std::cerr << "[NET] Bind failed: " << WSAGetLastError() << "\n";
        return 1;
    }

    // 2. THE "LANDING PAD" FIX
    // Increase the OS buffer to 16MB so it can hold the bursts of UDP chunks.
    int bufferSize = 1024 * 1024 * 16;
    setsockopt(recvSocket, SOL_SOCKET, SO_RCVBUF, (char*)&bufferSize, sizeof(int));

    // 3. Remove the TCP_NODELAY line (it will error on UDP)
    u_long mode = 1;
    ioctlsocket(recvSocket, FIONBIO, &mode);

    HWND hWnd = CreateClientWindow();

    // Get the actual size of our new borderless window
    RECT clientRect;
    GetClientRect(hWnd, &clientRect);
    int clientW = clientRect.right - clientRect.left;
    int clientH = clientRect.bottom - clientRect.top;

    // --- DX11 device ---
    DXGI_SWAP_CHAIN_DESC scd = { 0 };
    scd.BufferCount = 1;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hWnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;

    ID3D11Device* dev = nullptr;
    HRESULT hrDev = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
        &scd, &g_pSwapChain, &dev, NULL, &g_pContext);
    std::cout << "[DX11] D3D11CreateDeviceAndSwapChain: 0x" << std::hex << hrDev << std::dec << "\n";

    ID3D11Texture2D* backBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&backBuffer);
    dev->CreateRenderTargetView(backBuffer, NULL, &g_pRTV);
    backBuffer->Release();
    g_pContext->OMSetRenderTargets(1, &g_pRTV, NULL);

    D3D11_VIEWPORT vp = { 0 };
    vp.Width = (FLOAT)clientW;     // FIX: Match the monitor width
    vp.Height = (FLOAT)clientH;    // FIX: Match the monitor height
    vp.MaxDepth = 1.0f;
    g_pContext->RSSetViewports(1, &vp);

    D3D11_RASTERIZER_DESC rd = {}; rd.CullMode = D3D11_CULL_NONE; rd.FillMode = D3D11_FILL_SOLID;
    ID3D11RasterizerState* rs = nullptr;
    dev->CreateRasterizerState(&rd, &rs);
    g_pContext->RSSetState(rs);

    

    // 1. Luma SRV (Y Plane - mapped to t0 in shader)
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescY = {};
    srvDescY.Format = DXGI_FORMAT_R8_UNORM;
    srvDescY.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDescY.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(g_pDisplayTexture, &srvDescY, &g_pLumaSRV);

    // 2. Chroma SRV (UV Plane - mapped to t1 in shader)
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescUV = {};
    srvDescUV.Format = DXGI_FORMAT_R8G8_UNORM;
    srvDescUV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDescUV.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(g_pDisplayTexture, &srvDescUV, &g_pChromaSRV);

    // --- Compile shaders ---
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* psDbgBlob = nullptr;
    ID3DBlob* errBlob = nullptr;

    D3DCompile(vertexShaderSrc, strlen(vertexShaderSrc), NULL, NULL, NULL, "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);
    D3DCompile(pixelShaderSrc, strlen(pixelShaderSrc), NULL, NULL, NULL, "main", "ps_5_0", 0, 0, &psBlob, &errBlob);
    D3DCompile(pixelShaderDebugSrc, strlen(pixelShaderDebugSrc), NULL, NULL, NULL, "main", "ps_5_0", 0, 0, &psDbgBlob, &errBlob);

    dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), NULL, &g_pVS);
    dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), NULL, &g_pPS);
    dev->CreatePixelShader(psDbgBlob->GetBufferPointer(), psDbgBlob->GetBufferSize(), NULL, &pPSDbg);

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    dev->CreateInputLayout(ied, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &g_pLayout);

    Vertex vertices[] = {
        { -1.0f,  1.0f, 0.0f,  0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f,  1.0f, 0.0f },
        { -1.0f, -1.0f, 0.0f,  0.0f, 1.0f },
        {  1.0f, -1.0f, 0.0f,  1.0f, 1.0f }
    };
    D3D11_BUFFER_DESC bd = { 0 }; bd.ByteWidth = sizeof(vertices); bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA srd = { vertices };
    dev->CreateBuffer(&bd, &srd, &g_pVBuffer);

    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    dev->CreateSamplerState(&sampDesc, &g_pSampler);

    g_pContext->IASetInputLayout(g_pLayout);
    g_pContext->VSSetShader(g_pVS, 0, 0);
    g_pContext->PSSetSamplers(0, 1, &g_pSampler);
    UINT stride = sizeof(Vertex), offset = 0;
    g_pContext->IASetVertexBuffers(0, 1, &g_pVBuffer, &stride, &offset);
    g_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    bool useDebugShader = false;
    g_pContext->PSSetShader(g_pPS, 0, 0);
    std::cout << "[DIAG] Starting with FULL COLOUR YUV shader.\n       Press D to toggle to grayscale debug.\n\n";

    // --- FFMPEG setup ---
    // --- 1. Basic Decoder Setup ---
    // --- 1. Find the Hardware-Capable Decoder ---
    std::cout << "[FFMPEG] Initializing Zero-Copy Hardware Decoder...\n";
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) { std::cerr << "H.264 codec not found!\n"; return 1; }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);

    // --- 2. Hardware Linking (MUST follow context allocation) ---
    // Use the 'struct' prefix to help the C++ compiler identify these C types
    // --- 2. Hardware Linking ---
    AVBufferRef* hw_device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    AVHWDeviceContext* device_ptr = (AVHWDeviceContext*)hw_device_ctx->data;

    // FIX: Notice the "VA" in the middle of AVD3D11VADeviceContext!
    AVD3D11VADeviceContext* d3d11va_ctx = (AVD3D11VADeviceContext*)device_ptr->hwctx;

    // Link your existing DX11 device directly to the decoder
    d3d11va_ctx->device = dev;
    dev->AddRef();
    d3d11va_ctx->device_context = g_pContext;
    g_pContext->AddRef();

    av_hwdevice_ctx_init(hw_device_ctx);
    codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

    // Tell FFmpeg to keep the decoded frames in GPU memory (AV_PIX_FMT_D3D11)
    codec_ctx->get_format = [](struct AVCodecContext* s, const enum AVPixelFormat* fmt) {
        return AV_PIX_FMT_D3D11;
        };

    codec_ctx->thread_count = 1;
    codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    // FIX: Tell FFmpeg to explode (fail) if it detects missing UDP chunks,
    // rather than passing corrupted data to the hardware renderer.
    codec_ctx->err_recognition |= AV_EF_EXPLODE;

    codec_ctx->extra_hw_frames = 16;

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        std::cerr << "Could not open hardware codec!\n"; return 1;
    }

    AVCodecParserContext* parser = av_parser_init(codec->id);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* latestFrame = av_frame_alloc();

    std::vector<char> packetBuffer(65536);
    bool running = true;
    int  totalBytes = 0;
    int  frameCount = 0;
    bool firstFrameReceived = false;

    // --- FPS & Latency Timer Setup ---
    LARGE_INTEGER timerFreq, secondTimer, frameEnd;
    QueryPerformanceFrequency(&timerFreq);
    QueryPerformanceCounter(&secondTimer);
    int fpsCounter = 0;
    double clientLatencyMs = 0.0;
    // ---------------------------------

    // =========================================================================
    // AUDIO SETUP: XAudio2 + Opus
    // =========================================================================
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IXAudio2* pXAudio2 = nullptr;
    XAudio2Create(&pXAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);

    IXAudio2MasteringVoice* pMasterVoice = nullptr;
    pXAudio2->CreateMasteringVoice(&pMasterVoice);

    // We know the Host is sending 48000 Hz, 2 Channel, 32-bit Float audio
    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = 48000;
    wfx.wBitsPerSample = 32;
    wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    IXAudio2SourceVoice* pSourceVoice = nullptr;
    pXAudio2->CreateSourceVoice(&pSourceVoice, &wfx);
    pSourceVoice->Start(0);

    int opusErr;
    OpusDecoder* opusDec = opus_decoder_create(48000, 2, &opusErr);
    std::cout << "[AUDIO] XAudio2 & Opus Decoder Ready.\n";

    ShowCursor(TRUE);

    // XAudio2 reads memory asynchronously. We need a "Ring Buffer" to hold the 
    // decoded audio safely while the sound card plays it, otherwise it gets overwritten!
    const int AUDIO_BUFFERS = 32;
    std::vector<std::vector<float>> audioRing(AUDIO_BUFFERS, std::vector<float>(960 * 2));
    int ringIdx = 0;
    // =========================================================================

    

    bool isGameMode = false;
    std::cout << "\n=================================================\n";
    std::cout << "  HOTKEY: Press [ F8 ] to Toggle Game Mode\n";
    std::cout << "=================================================\n\n";

    while (running) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {

            // --- TOGGLE HOTKEY (F8) ---
            if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) && msg.wParam == VK_F8) {
                if ((msg.lParam & (1 << 30)) == 0) { // Prevent holding down auto-repeat
                    isGameMode = !isGameMode;
                    if (isGameMode) {
                        // Lock cursor perfectly to the Client window
                        RECT rect;
                        GetClientRect(hWnd, &rect);
                        POINT ptTopLeft = { rect.left, rect.top };
                        POINT ptBottomRight = { rect.right, rect.bottom };
                        ClientToScreen(hWnd, &ptTopLeft);
                        ClientToScreen(hWnd, &ptBottomRight);
                        rect.left = ptTopLeft.x; rect.top = ptTopLeft.y;
                        rect.right = ptBottomRight.x; rect.bottom = ptBottomRight.y;
                        ClipCursor(&rect);

                        // Snap cursor to dead center to begin
                        POINT center = { clientW / 2, clientH / 2 };
                        ClientToScreen(hWnd, &center);
                        SetCursorPos(center.x, center.y);

                        std::cout << "[INPUT] GAME MODE ON - 3D Camera Locked.\n";
                    }
                    else {
                        ShowCursor(TRUE); // Bring cursor back
                        ClipCursor(NULL);
                        std::cout << "[INPUT] DESKTOP MODE ON - Mouse Unlocked.\n";
                    }
                }
                continue; // Do NOT send F8 to the Host PC
            }

            // --- MOUSE CLICKS (Always Active) ---
            else if (msg.message == WM_LBUTTONDOWN || msg.message == WM_LBUTTONUP ||
                msg.message == WM_RBUTTONDOWN || msg.message == WM_RBUTTONUP) {
                uint8_t button = (msg.message == WM_LBUTTONDOWN || msg.message == WM_LBUTTONUP) ? 0 : 1;
                uint8_t isDown = (msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN) ? 1 : 0;

                char packet[3];
                packet[0] = 0x05;
                packet[1] = button;
                packet[2] = isDown;
                sendto(inputSocket, packet, 3, 0, (struct sockaddr*)&hostInputAddr, sizeof(hostInputAddr));
            }

            // --- MOUSE MOVEMENT (Split Logic) ---
            else if (msg.message == WM_MOUSEMOVE) {
                if (isGameMode) {
                    // 1. GAME MODE (Center-Locked Infinite Radius)
                    SetCursor(NULL); // Force the cursor to stay invisible!

                    POINT pt;
                    GetCursorPos(&pt);

                    POINT center = { clientW / 2, clientH / 2 };
                    ClientToScreen(hWnd, &center);

                    // Ignore the synthetic event generated by our own SetCursorPos
                    if (pt.x != center.x || pt.y != center.y) {
                        int dx = pt.x - center.x;
                        int dy = pt.y - center.y;

                        char packet[9] = { 0x06 };
                        memcpy(packet + 1, &dx, 4);
                        memcpy(packet + 5, &dy, 4);
                        sendto(inputSocket, packet, 9, 0, (struct sockaddr*)&hostInputAddr, sizeof(hostInputAddr));

                        // Force the physical cursor back to the dead center!
                        SetCursorPos(center.x, center.y);
                    }
                }
                else {
                    // 2. DESKTOP MODE (Absolute Scaling)
                    SetCursor(LoadCursor(NULL, IDC_ARROW)); // Bring the arrow back!

                    int scaledX = (LOWORD(msg.lParam) * 65535) / clientW;
                    int scaledY = (HIWORD(msg.lParam) * 65535) / clientH;

                    char packet[9];
                    packet[0] = 0x03;
                    memcpy(packet + 1, &scaledX, 4);
                    memcpy(packet + 5, &scaledY, 4);
                    sendto(inputSocket, packet, 9, 0, (struct sockaddr*)&hostInputAddr, sizeof(hostInputAddr));
                }
            }

            // --- KEYBOARD ---
            else if (msg.message == WM_KEYDOWN || msg.message == WM_KEYUP) {
                char packet[3];
                packet[0] = 0x04;
                packet[1] = (uint8_t)msg.wParam;
                packet[2] = (msg.message == WM_KEYDOWN) ? 1 : 0;
                sendto(inputSocket, packet, 3, 0, (struct sockaddr*)&hostInputAddr, sizeof(hostInputAddr));
            }

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        int bytesRead;
        // DRAIN the entire UDP buffer every single frame iteration
        while ((bytesRead = recv(recvSocket, packetBuffer.data(), (int)packetBuffer.size(), 0)) > 0) {

            uint8_t header = packetBuffer[0];
            uint8_t* payload = (uint8_t*)packetBuffer.data() + 1;
            int payloadSize = bytesRead - 1;

            if (header == 0x01) {
                // ==========================================
                // ROUTE 1: VIDEO DATA
                // ==========================================
                if (!timingStarted) {
                    QueryPerformanceCounter(&frameStart);
                    timingStarted = true;
                }

                while (payloadSize > 0) {
                    int ret = av_parser_parse2(parser, codec_ctx, &pkt->data, &pkt->size,
                        payload, payloadSize, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                    payload += ret;
                    payloadSize -= ret;

                    if (pkt->size) {
                        // Send packet to FFmpeg Decoder
                        if (avcodec_send_packet(codec_ctx, pkt) == 0) {
                            bool gotNewFrame = false;

                            // Drain all available frames
                            while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                                av_frame_move_ref(latestFrame, frame);
                                gotNewFrame = true;
                            }

                            if (gotNewFrame) {
                                // --- AUTOMATIC MODULAR SETUP ---
                                if (!g_pDisplayTexture) {
                                    int streamW = latestFrame->width;
                                    int streamH = latestFrame->height;
                                    std::cout << "\n[DX11] Incoming stream detected! Building GPU pipeline for " << streamW << "x" << streamH << "...\n";

                                    // 1. Dynamic Texture
                                    D3D11_TEXTURE2D_DESC texDesc = { 0 };
                                    texDesc.Width = streamW;
                                    texDesc.Height = streamH;
                                    texDesc.MipLevels = 1;
                                    texDesc.ArraySize = 1;
                                    texDesc.Format = DXGI_FORMAT_NV12;
                                    texDesc.SampleDesc.Count = 1;
                                    texDesc.Usage = D3D11_USAGE_DEFAULT;
                                    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                                    dev->CreateTexture2D(&texDesc, NULL, &g_pDisplayTexture);

                                    // 2. Dynamic SRVs
                                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescY = {};
                                    srvDescY.Format = DXGI_FORMAT_R8_UNORM;
                                    srvDescY.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                                    srvDescY.Texture2D.MipLevels = 1;
                                    dev->CreateShaderResourceView(g_pDisplayTexture, &srvDescY, &g_pLumaSRV);

                                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDescUV = {};
                                    srvDescUV.Format = DXGI_FORMAT_R8G8_UNORM;
                                    srvDescUV.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                                    srvDescUV.Texture2D.MipLevels = 1;
                                    dev->CreateShaderResourceView(g_pDisplayTexture, &srvDescUV, &g_pChromaSRV);

                                    // 3. Dynamic Constant Buffer (For the shader math)
                                    struct SceneBuffer { float height; float padding[3]; };
                                    SceneBuffer sb = { (float)streamH };
                                    D3D11_BUFFER_DESC cbDesc = { 0 };
                                    cbDesc.ByteWidth = sizeof(SceneBuffer);
                                    cbDesc.Usage = D3D11_USAGE_DEFAULT;
                                    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                                    D3D11_SUBRESOURCE_DATA cbData = { &sb };
                                    dev->CreateBuffer(&cbDesc, &cbData, &g_pConstantBuffer);
                                    g_pContext->PSSetConstantBuffers(0, 1, &g_pConstantBuffer);
                                }
                                // -------------------------------

                                ID3D11Texture2D* hwTexture = (ID3D11Texture2D*)latestFrame->data[0];
                                UINT sliceIndex = (UINT)(uintptr_t)latestFrame->data[1];

                                g_pContext->CopySubresourceRegion(g_pDisplayTexture, 0, 0, 0, 0,
                                    hwTexture, sliceIndex, NULL);
                                g_newFrameReady.store(true);

                                // Free the hardware surface immediately so FFmpeg doesn't crash!
                                av_frame_unref(latestFrame);

                                QueryPerformanceCounter(&frameEnd);
                                clientLatencyMs = ((double)(frameEnd.QuadPart - frameStart.QuadPart) * 1000.0) / timerFreq.QuadPart;
                                timingStarted = false;
                            }
                        }
                        av_packet_unref(pkt);
                    }
                }
            }
            else if (header == 0x02) {
                // ==========================================
                // ROUTE 2: AUDIO DATA
                // ==========================================
                // Decompress the Opus packet back into raw 32-bit floats
                int decodedSamples = opus_decode_float(opusDec, payload, payloadSize, audioRing[ringIdx].data(), 960, 0);

                if (decodedSamples > 0) {
                    XAUDIO2_BUFFER buffer = { 0 };
                    buffer.AudioBytes = decodedSamples * 2 * sizeof(float);
                    buffer.pAudioData = (const BYTE*)audioRing[ringIdx].data();

                    // Submit it to the sound card
                    pSourceVoice->SubmitSourceBuffer(&buffer);

                    // Move to the next slot in the ring buffer
                    ringIdx = (ringIdx + 1) % AUDIO_BUFFERS;
                }
            }
        }

        // Upload new frame from FFmpeg (always on main thread)
        if (g_newFrameReady.exchange(false)) {
            firstFrameReceived = true;
            fpsCounter++;
            if (frameCount++ % 60 == 0)
                std::cout << "[RENDER] Frame " << frameCount << " decoded and uploaded\n";
        }

        // Render
        // Render
        if (g_pRTV) {
            float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            g_pContext->ClearRenderTargetView(g_pRTV, clearColor);

            if (firstFrameReceived) {
                g_pContext->OMSetRenderTargets(1, &g_pRTV, NULL);

                // FIX: Pass both Luma and Chroma to the shader
                ID3D11ShaderResourceView* srvs[2] = { g_pLumaSRV, g_pChromaSRV };
                g_pContext->PSSetShaderResources(0, 2, srvs);

                g_pContext->Draw(4, 0);

                ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
                g_pContext->PSSetShaderResources(0, 2, nullSRVs);
            }
        }
        if (g_pSwapChain) g_pSwapChain->Present(0, 0);

        // --- LATENCY MATH & WINDOW TITLE UPDATE ---
        if (firstFrameReceived) {
            QueryPerformanceCounter(&frameEnd);

            // Only calculate latency if we actually processed a network packet this loop
            

            // Update Window Title once per second
            if ((frameEnd.QuadPart - secondTimer.QuadPart) >= timerFreq.QuadPart) {
                std::wstringstream title;
                title << L"GameStream Client | Video FPS: " << fpsCounter
                    << L" | Decode+Render Latency: " << std::fixed << std::setprecision(2) << clientLatencyMs << L" ms";

                SetWindowText(hWnd, title.str().c_str());

                fpsCounter = 0; // Reset for the next second
                secondTimer = frameEnd;
            }
        }
        // ------------------------------------------
    }

    // Cleanup FFmpeg
    av_parser_close(parser);
    avcodec_free_context(&codec_ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    av_frame_free(&latestFrame); // Don't forget to free the stable container too!

    // Cleanup DX11
    if (g_pLumaSRV)        g_pLumaSRV->Release();     // FIX: Release Luma
    if (g_pChromaSRV)      g_pChromaSRV->Release();   // FIX: Release Chroma
    if (g_pDisplayTexture) g_pDisplayTexture->Release();
    if (g_pSampler)        g_pSampler->Release();
    if (g_pVBuffer)        g_pVBuffer->Release();
    if (g_pLayout)         g_pLayout->Release();
    if (g_pVS)             g_pVS->Release();
    if (g_pPS)             g_pPS->Release();
    if (pPSDbg)            pPSDbg->Release();
    if (rs)                rs->Release();
    if (g_pRTV)            g_pRTV->Release();
    if (g_pSwapChain)      g_pSwapChain->Release();
    if (dev)               dev->Release();
    if (g_pConstantBuffer) g_pConstantBuffer->Release();
    // Cleanup Audio
    if (opusDec) opus_decoder_destroy(opusDec);
    if (pSourceVoice) pSourceVoice->DestroyVoice();
    if (pMasterVoice) pMasterVoice->DestroyVoice();
    if (pXAudio2) pXAudio2->Release();
    CoUninitialize();

    closesocket(recvSocket);
    WSACleanup();

    timeEndPeriod(1);

    return 0;
}