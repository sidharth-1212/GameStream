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
#include <string>
#include <sstream>
#include <iomanip>

#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <opus/opus.h>
#include <xaudio2.h>
#include <queue>
#include <map>

#include <d2d1.h>
#include <dwrite.h>

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
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

extern "C" {
    __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

HCURSOR g_remoteCursor = NULL;

// ==========================================
// --- NEW: DYNAMIC BITRATE GLOBALS ---
// ==========================================
const uint32_t BITRATE_HIGH = 8000000; // 8 Mbps (Crisp & Clear)
const uint32_t BITRATE_LOW = 3000000; // 3 Mbps (Blurry but Fast)
uint32_t g_currentBitrate = BITRATE_HIGH;
int g_bitrateCooldown = 0; // The 5-second lock
// ==========================================

// ==========================================
// --- NEW: DIRECT2D HUD GLOBALS ---
// ==========================================
ID2D1Factory* g_pD2DFactory = nullptr;
ID2D1RenderTarget* g_pD2DRenderTarget = nullptr;
IDWriteFactory* g_pDWriteFactory = nullptr;
IDWriteTextFormat* g_pTextFormat = nullptr;
ID2D1SolidColorBrush* g_pBrushYellow = nullptr;
ID2D1SolidColorBrush* g_pBrushBackground = nullptr;
bool g_showHUD = true; // Toggle with F9
// ==========================================

bool timingStarted = false;
LARGE_INTEGER frameStart;

// --- globals ---
ID3D11Texture2D* g_pDisplayTexture = nullptr;     // RESTORED: The actual texture in GPU memory
ID3D11ShaderResourceView* g_pLumaSRV = nullptr;   // NEW: Brightness view for NV12
ID3D11ShaderResourceView* g_pChromaSRV = nullptr; // NEW: Color view for NV12
ID3D11ShaderResourceView* g_pDisplaySRV = nullptr;
ID3D11Buffer* g_pConstantBuffer = nullptr;
static std::vector<uint8_t> g_frameBuffer;
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

std::queue<AVFrame*> g_jitterBuffer;
const int MAX_BUFFER_SIZE = 2;
LARGE_INTEGER g_frameTimer;

std::map<uint32_t, std::vector<uint8_t>> g_packetStaging;
uint32_t g_expectedSeq = 0;
bool g_firstPacketReceived = false;
uint32_t g_highestNackSent = 0;

int g_packetsReceivedThisSecond = 0;
int g_packetsLostThisSecond = 0;
double g_currentPingMs = 0.0;
int g_displayFPS = 0;

DWORD g_lastHostContact = 0;
DWORD g_lastWakePing = 0;
bool g_hostIsAlive = false;

int g_streamW = 0;
int g_streamH = 0;

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
cbuffer SceneData : register(b0) {
    float streamHeight; // This is actually the video height!
};
Texture2D shaderTexture : register(t0);
SamplerState SampleType : register(s0);

struct PixelInputType {
    float4 position : SV_POSITION;
    float2 tex : TEXCOORD0;
};

float4 main(PixelInputType input) : SV_TARGET {
    // Math fix: Use the dynamic streamHeight to calculate the UV load
    float streamWidth = streamHeight * (1.6f); // Assume 16:10 or calculate properly
    uint x = (uint)(input.tex.x * (streamWidth - 1));
    uint y = (uint)(input.tex.y * (streamHeight - 1));
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

    typedef BOOL(WINAPI* SetProcessDpiAwarenessContextProc)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = LoadLibraryA("user32.dll");
    if (user32) {
        SetProcessDpiAwarenessContextProc setDpiCtx = (SetProcessDpiAwarenessContextProc)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (setDpiCtx) {
            setDpiCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
        else {
            SetProcessDPIAware(); // Fallback for older Windows
        }
        FreeLibrary(user32);
    }
    else {
        SetProcessDPIAware();
    }

    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    timeBeginPeriod(1);
    std::cout << "=== GameStream Client (Hardware Agnostic FFmpeg Build) ===\n\n";

    // Pre-allocate the frame buffer so it's ready for FFmpeg

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    SOCKET inputSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in hostInputAddr = { 0 };
    hostInputAddr.sin_family = AF_INET;
    hostInputAddr.sin_port = htons(9999);
    // =======================================================
    // --- NEW: DYNAMIC CONFIG LOGIC (CLIENT SIDE) ---
    // =======================================================
    std::string targetIP;
    std::ifstream configFile("host_ip.txt");

    if (configFile.is_open()) {
        std::getline(configFile, targetIP);
        configFile.close();
        std::cout << "[SYSTEM] Loaded Host IP from host_ip.txt: " << targetIP << "\n";
    }
    else {
        std::cout << "\n=========================================\n";
        std::cout << "  FIRST TIME SETUP (CLIENT)\n";
        std::cout << "=========================================\n";
        std::cout << "Enter the Host PC's IP Address (e.g., 192.168.1.17): ";
        std::cin >> targetIP;

        std::ofstream outFile("host_ip.txt");
        outFile << targetIP;
        outFile.close();
        std::cout << "[SYSTEM] Saved to host_ip.txt for future use!\n\n";
    }

    // Pass the string we just loaded/typed into the Winsock function
    inet_pton(AF_INET, targetIP.c_str(), &hostInputAddr.sin_addr);
    // =======================================================

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
    scd.BufferCount = 2; // FIX 1: Flip Model requires at least 2 buffers (Front & Back)
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hWnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT; // Crucial for Direct2D!
    
    ID3D11Device* dev = nullptr;
    HRESULT hrDev = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, createDeviceFlags, NULL, 0, D3D11_SDK_VERSION,
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

    // =======================================================
    // --- NEW: DIRECT2D OVERLAY INITIALIZATION ---
    // =======================================================
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_pD2DFactory);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_pDWriteFactory);

    // Create a nice, readable font (Consolas looks very "terminal/hacker")
    g_pDWriteFactory->CreateTextFormat(
        L"Consolas", NULL, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        24.0f, L"en-us", &g_pTextFormat
    );

    // Extract the raw glass surface from the SwapChain
    IDXGISurface* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, __uuidof(IDXGISurface), (void**)&pBackBuffer);

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    // Bind Direct2D to the DirectX 11 screen
    g_pD2DFactory->CreateDxgiSurfaceRenderTarget(pBackBuffer, &props, &g_pD2DRenderTarget);
    pBackBuffer->Release();

    // Create our paint colors
    g_pD2DRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Yellow), &g_pBrushYellow);
    g_pD2DRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.6f), &g_pBrushBackground); // Dark translucent box
    // =======================================================

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

    // =======================================================
    // --- NEW: SEND WAKE-UP PING TO HOST ---
    // =======================================================
    char pingPacket[9];
    pingPacket[0] = 0xFF;
    memcpy(pingPacket + 1, &clientW, 4); // 4 bytes for Width
    memcpy(pingPacket + 5, &clientH, 4); // 4 bytes for Height

    sendto(inputSocket, pingPacket, 9, 0, (struct sockaddr*)&hostInputAddr, sizeof(hostInputAddr));
    std::cout << "[NET] Sent Wake-Up Ping. Requested Resolution: " << clientW << "x" << clientH << "\n";
    // =======================================================
    

    bool isGameMode = false;
    std::cout << "\n=================================================\n";
    std::cout << "  HOTKEY: Press [ F8 ] to Toggle Game Mode\n";
    std::cout << "=================================================\n\n";

    auto DecodeVideoPacket = [&](uint8_t* data, int size) {
        while (size > 0) {
            int ret = av_parser_parse2(parser, codec_ctx, &pkt->data, &pkt->size,
                data, size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            data += ret;
            size -= ret;

            if (pkt->size) {
                if (avcodec_send_packet(codec_ctx, pkt) == 0) {
                    while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                        AVFrame* queuedFrame = av_frame_alloc();
                        av_frame_move_ref(queuedFrame, frame);
                        g_jitterBuffer.push(queuedFrame);

                        if (g_jitterBuffer.size() > MAX_BUFFER_SIZE) {
                            AVFrame* droppedFrame = g_jitterBuffer.front();
                            g_jitterBuffer.pop();
                            av_frame_free(&droppedFrame);
                        }
                    }
                }
                av_packet_unref(pkt);
            }
        }
        };

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
            else if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) && msg.wParam == VK_F9) {
                if ((msg.lParam & (1 << 30)) == 0) { // Prevent holding down auto-repeat
                    g_showHUD = !g_showHUD;
                }
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
            else if (msg.message == WM_MOUSEWHEEL) {
                // GET_WHEEL_DELTA_WPARAM extracts the scroll direction and distance
                // Positive = Scroll Up, Negative = Scroll Down
                int wheelDelta = GET_WHEEL_DELTA_WPARAM(msg.wParam);

                char packet[5];
                packet[0] = 0x0B;
                memcpy(packet + 1, &wheelDelta, 4);
                sendto(inputSocket, packet, 5, 0, (struct sockaddr*)&hostInputAddr, sizeof(hostInputAddr));
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
                    SetCursor(g_remoteCursor ? g_remoteCursor : LoadCursor(NULL, IDC_ARROW));
                    int mouseX = LOWORD(msg.lParam);
                    int mouseY = HIWORD(msg.lParam);

                    // Calculate actual video area on Client screen
                    int videoW = clientW, videoH = clientH;
                    int offsetX = 0, offsetY = 0;

                    if (g_streamW > 0 && g_streamH > 0) {
                        float streamRatio = (float)g_streamW / (float)g_streamH;
                        float windowRatio = (float)clientW / (float)clientH;

                        if (windowRatio > streamRatio) { // Pillarbox
                            videoW = (int)(clientH * streamRatio);
                            offsetX = (clientW - videoW) / 2;
                        }
                        else { // Letterbox
                            videoH = (int)(clientW / streamRatio);
                            offsetY = (clientH - videoH) / 2;
                        }
                    }

                    // Map mouse to the video area (ignore black bars)
                    int mappedX = (mouseX - offsetX < 0) ? 0 : (mouseX - offsetX > videoW ? videoW : mouseX - offsetX);
                    int mappedY = (mouseY - offsetY < 0) ? 0 : (mouseY - offsetY > videoH ? videoH : mouseY - offsetY);

                    int scaledX = (mappedX * 65535) / (videoW > 0 ? videoW : 1);
                    int scaledY = (mappedY * 65535) / (videoH > 0 ? videoH : 1);

                    char packet[9] = { 0x03 };
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

            g_lastHostContact = GetTickCount(); // <-- NEW: The Host is alive!
            g_hostIsAlive = true;

            uint8_t header = packetBuffer[0];
            uint8_t* payload = (uint8_t*)packetBuffer.data() + 1;
            int payloadSize = bytesRead - 1;

            if (header == 0x01) {
                // ==========================================
                // ROUTE 1: STAMPED VIDEO DATA & NACK REASSEMBLY
                // ==========================================
                g_packetsReceivedThisSecond++; // <-- FIX 1: PLUG IN THE SENSOR!

                if (!timingStarted) {
                    QueryPerformanceCounter(&frameStart);
                    timingStarted = true;
                }

                // 1. Unpack our new custom protocol
                uint32_t seq = *(uint32_t*)(payload);
                uint8_t* videoData = payload + 4;
                int videoSize = payloadSize - 4;

                if (!g_firstPacketReceived) {
                    g_expectedSeq = seq;
                    g_firstPacketReceived = true;
                }

                if (seq == g_expectedSeq) {
                    // --- SCENARIO A: Perfect Order ---
                    DecodeVideoPacket(videoData, videoSize);
                    g_expectedSeq++;

                    // Drain the staging area! Did the missing packets arrive?
                    while (g_packetStaging.count(g_expectedSeq)) {
                        DecodeVideoPacket(g_packetStaging[g_expectedSeq].data(), g_packetStaging[g_expectedSeq].size());
                        g_packetStaging.erase(g_expectedSeq);
                        g_expectedSeq++;
                    }
                }
                else if (seq > g_expectedSeq) {
                    // --- SCENARIO B: WE MISSED A PACKET! ---

                    // 1. Store this "packet from the future" safely
                    g_packetStaging[seq] = std::vector<uint8_t>(videoData, videoData + videoSize);

                    // 2. Scream for help (BUT NO SPAMMING!)
                    // Only NACK packets we haven't already screamed about
                    uint32_t startNack = (g_highestNackSent > g_expectedSeq) ? g_highestNackSent + 1 : g_expectedSeq;

                    for (uint32_t missing = startNack; missing < seq; ++missing) {
                        if (g_packetStaging.find(missing) == g_packetStaging.end()) {
                            char nackPacket[5];
                            nackPacket[0] = 0x08;
                            memcpy(nackPacket + 1, &missing, 4);
                            sendto(inputSocket, nackPacket, 5, 0, (struct sockaddr*)&hostInputAddr, sizeof(hostInputAddr));

                            g_packetsLostThisSecond++; // <-- FIX 2: LOG THE LOSS!
                            g_highestNackSent = missing; // Update the high-water mark
                        }
                    }

                    // 3. Safety Valve: Raise the limit to 500! (~700ms of safety)
                    if (g_packetStaging.size() > 500) {
                        std::cout << "[NET] Hopelessly desynced. Hard resetting sequence.\n";
                        g_packetStaging.clear();
                        g_expectedSeq = seq + 1;
                        DecodeVideoPacket(videoData, videoSize);
                    }
                }

                // --- SCENARIO C: seq < g_expectedSeq ---
                // It's a duplicate or arrived too late. We already moved on. Do nothing!

                if (timingStarted) {
                    LARGE_INTEGER decodeEnd;
                    QueryPerformanceCounter(&decodeEnd);
                    clientLatencyMs = ((double)(decodeEnd.QuadPart - frameStart.QuadPart) * 1000.0) / timerFreq.QuadPart;
                    timingStarted = false;
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
            else if (header == 0x07) {
                // ==========================================
                // ROUTE 3: CURSOR SHAPE DATA
                // ==========================================
                uint32_t width = *(uint32_t*)(payload);
                uint32_t height = *(uint32_t*)(payload + 4);
                uint32_t pitch = *(uint32_t*)(payload + 8);
                uint32_t type = *(uint32_t*)(payload + 12);
                uint8_t* pixels = payload + 16;

                HCURSOR newCursor = NULL;
                ICONINFO ii = { 0 };
                ii.fIcon = FALSE;
                ii.xHotspot = 0;
                ii.yHotspot = 0;

                // --- SCENARIO A: Text I-Beams & Crosshairs (1-bit Monochrome) ---
                if (type == 1) {
                    // The GPU gives us the AND mask (top) and XOR mask (bottom) stacked vertically.
                    // Windows CreateIconIndirect expects exactly this for monochrome cursors!
                    ii.hbmMask = CreateBitmap(width, height, 1, 1, pixels);
                    ii.hbmColor = NULL; // NULL tells Windows to treat it as Black & White

                    if (ii.hbmMask) {
                        newCursor = CreateIconIndirect(&ii);
                        DeleteObject(ii.hbmMask);
                    }
                }
                // --- SCENARIO B: Arrows & Hands (32-bit Color) ---
                else if (type == 2 || type == 4) {
                    // Type 2 = Standard Color. Type 4 = Masked Color (Crop the mask off the bottom).
                    uint32_t visualHeight = (type == 4) ? (height / 2) : height;

                    BITMAPINFO bmi = { 0 };
                    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                    bmi.bmiHeader.biWidth = width;
                    bmi.bmiHeader.biHeight = -((int)visualHeight); // Negative means Top-Down
                    bmi.bmiHeader.biPlanes = 1;
                    bmi.bmiHeader.biBitCount = 32;
                    bmi.bmiHeader.biCompression = BI_RGB;

                    void* dibBits = nullptr;
                    HBITMAP hbmColor = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &dibBits, NULL, 0);

                    if (hbmColor) {
                        // Copy ONLY the visual color data
                        for (uint32_t y = 0; y < visualHeight; ++y) {
                            memcpy((uint8_t*)dibBits + (y * width * 4), pixels + (y * pitch), width * 4);
                        }

                        // Dummy mask required for color cursors
                        HBITMAP hbmMask = CreateBitmap(width, visualHeight, 1, 1, NULL);

                        ii.hbmMask = hbmMask;
                        ii.hbmColor = hbmColor;

                        newCursor = CreateIconIndirect(&ii);

                        DeleteObject(hbmColor);
                        DeleteObject(hbmMask);
                    }
                }

                // If we successfully built the cursor, swap it out!
                if (newCursor) {
                    if (g_remoteCursor) DestroyCursor(g_remoteCursor);
                    g_remoteCursor = newCursor;
                }
            }
            else if (header == 0x09) {
                LARGE_INTEGER currentTime;
                QueryPerformanceCounter(&currentTime);

                LARGE_INTEGER sentTime;
                memcpy(&sentTime.QuadPart, payload, 8); // Extract the timestamp we sent

                // Calculate the Round Trip Time (RTT)
                g_currentPingMs = ((double)(currentTime.QuadPart - sentTime.QuadPart) * 1000.0) / timerFreq.QuadPart;
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
        // =========================================================================
        // --- NEW: THE 60Hz METRONOME ---
        // =========================================================================
        QueryPerformanceCounter(&frameEnd);

        // 16.666 milliseconds = exactly 60 FPS pacing
        double elapsedMs = ((double)(frameEnd.QuadPart - g_frameTimer.QuadPart) * 1000.0) / timerFreq.QuadPart;

        if (elapsedMs >= 16.666) {
            // Reset the metronome for the next beat
            g_frameTimer = frameEnd;

            // =======================================================
            // --- NEW: HOST DISCONNECT DETECTION ---
            // =======================================================
            if (GetTickCount() - g_lastHostContact > 3000) {
                if (g_hostIsAlive) {
                    std::cout << "\n[SYSTEM] Host connection lost. Initiating Hard Reset...\n";
                    g_hostIsAlive = false;
                }

                if (g_pDisplayTexture) {
                    if (g_pLumaSRV) { g_pLumaSRV->Release(); g_pLumaSRV = nullptr; }
                    if (g_pChromaSRV) { g_pChromaSRV->Release(); g_pChromaSRV = nullptr; }
                    g_pDisplayTexture->Release(); g_pDisplayTexture = nullptr;
                    if (g_pConstantBuffer) { g_pConstantBuffer->Release(); g_pConstantBuffer = nullptr; }
                    if (g_pVBuffer) { g_pVBuffer->Release(); g_pVBuffer = nullptr; }
                    g_streamW = 0; // Forces mouse math to fallback
                    g_streamH = 0;
                }

                while (!g_jitterBuffer.empty()) {
                    AVFrame* f = g_jitterBuffer.front();
                    g_jitterBuffer.pop();
                    av_frame_free(&f);
                }

                firstFrameReceived = false;     // KILLS THE METRICS & HEARTBEAT LOOP!
                g_firstPacketReceived = false;  // Resets the NACK sequence logic
                g_expectedSeq = 0;
                g_highestNackSent = 0;
                g_packetStaging.clear();
                timingStarted = false;
                fpsCounter = 0;
                g_packetsReceivedThisSecond = 0;
                g_packetsLostThisSecond = 0;

                float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                if (g_pRTV) {
                    g_pContext->ClearRenderTargetView(g_pRTV, black);
                    g_pSwapChain->Present(1, 0);
                }

                g_firstPacketReceived = false;
                g_packetStaging.clear();

                // --- SMART SEARCH PING (Includes resolution) ---
                if (GetTickCount() - g_lastWakePing > 1000) {
                    char pingPacket[9];
                    pingPacket[0] = 0xFF;
                    memcpy(pingPacket + 1, &clientW, 4);
                    memcpy(pingPacket + 5, &clientH, 4);
                    sendto(inputSocket, pingPacket, 9, 0, (struct sockaddr*)&hostInputAddr, sizeof(hostInputAddr));

                    g_lastWakePing = GetTickCount();
                    // Optional: uncomment below if you want a visual heartbeat in the console
                     std::cout << "[NET] Standby. Firing Smart Ping...\n"; 
                }
            }
            // =======================================================
            else {
                // =======================================================
                // THE HOST IS ALIVE! RENDER THE STREAM NORMALLY!
                // =======================================================

                // 1. Pull the oldest frame out of the Jitter Buffer
                AVFrame* frameToRender = nullptr;
                if (!g_jitterBuffer.empty()) {
                    frameToRender = g_jitterBuffer.front();
                    g_jitterBuffer.pop();
                }

                // 2. Upload and Render!
                if (frameToRender && g_pRTV) {

                    // --- Upload the frame to the GPU Pipeline ---
                    if (!g_pDisplayTexture) {
                        int streamW = frameToRender->width;
                        int streamH = frameToRender->height;
                        g_streamW = streamW; // <--- SAVE TO GLOBAL
                        g_streamH = streamH; // <--- SAVE TO GLOBAL
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

                        // 3. Dynamic Constant Buffer
                        struct SceneBuffer { float height; float padding[3]; };
                        SceneBuffer sb = { (float)streamH };
                        D3D11_BUFFER_DESC cbDesc = { 0 };
                        cbDesc.ByteWidth = sizeof(SceneBuffer);
                        cbDesc.Usage = D3D11_USAGE_DEFAULT;
                        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                        D3D11_SUBRESOURCE_DATA cbData = { &sb };
                        dev->CreateBuffer(&cbDesc, &cbData, &g_pConstantBuffer);
                        g_pContext->PSSetConstantBuffers(0, 1, &g_pConstantBuffer);

                        // =======================================================
                        // --- 4. ASPECT RATIO FIT-TO-SCREEN ---
                        // =======================================================
                        float streamRatio = (float)streamW / (float)streamH;
                        float windowRatio = (float)clientW / (float)clientH;

                        float scaleX = 1.0f;
                        float scaleY = 1.0f;

                        if (windowRatio > streamRatio) {
                            // Window is wider than video -> Pillarbox (Black bars on left/right)
                            scaleX = streamRatio / windowRatio;
                        }
                        else if (windowRatio < streamRatio) {
                            // Window is taller than video -> Letterbox (Black bars on top/bottom)
                            scaleY = windowRatio / streamRatio;
                        }

                        // Rebuild the geometry with the new math
                        Vertex scaledVertices[] = {
                            { -scaleX,  scaleY, 0.0f,  0.0f, 0.0f },
                            {  scaleX,  scaleY, 0.0f,  1.0f, 0.0f },
                            { -scaleX, -scaleY, 0.0f,  0.0f, 1.0f },
                            {  scaleX, -scaleY, 0.0f,  1.0f, 1.0f }
                        };

                        // Destroy the old stretched geometry and load the perfect one
                        if (g_pVBuffer) g_pVBuffer->Release();

                        D3D11_BUFFER_DESC bd = { 0 };
                        bd.ByteWidth = sizeof(scaledVertices);
                        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                        D3D11_SUBRESOURCE_DATA srd = { scaledVertices };
                        dev->CreateBuffer(&bd, &srd, &g_pVBuffer);

                        // Re-bind the new shape to the renderer
                        UINT stride = sizeof(Vertex), offset = 0;
                        g_pContext->IASetVertexBuffers(0, 1, &g_pVBuffer, &stride, &offset);
                    }

                    ID3D11Texture2D* hwTexture = (ID3D11Texture2D*)frameToRender->data[0];
                    UINT sliceIndex = (UINT)(uintptr_t)frameToRender->data[1];

                    g_pContext->CopySubresourceRegion(g_pDisplayTexture, 0, 0, 0, 0, hwTexture, sliceIndex, NULL);

                    // Free the FFmpeg memory immediately after we extract the DX11 texture
                    av_frame_free(&frameToRender);
                    firstFrameReceived = true;
                    fpsCounter++;

                    // --- Draw to the screen ---
                    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                    g_pContext->ClearRenderTargetView(g_pRTV, clearColor);

                    g_pContext->OMSetRenderTargets(1, &g_pRTV, NULL);

                    ID3D11ShaderResourceView* srvs[2] = { g_pLumaSRV, g_pChromaSRV };
                    g_pContext->PSSetShaderResources(0, 2, srvs);

                    g_pContext->Draw(4, 0);

                    ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
                    g_pContext->PSSetShaderResources(0, 2, nullSRVs);

                    if (g_showHUD && g_pD2DRenderTarget) {
                        g_pD2DRenderTarget->BeginDraw();

                        // 1. Math
                        double packetLossPct = 0.0;
                        int totalPackets = g_packetsReceivedThisSecond + g_packetsLostThisSecond;
                        if (totalPackets > 0) packetLossPct = ((double)g_packetsLostThisSecond / totalPackets) * 100.0;

                        // 2. Format the layout
                        std::wstringstream hudText;
                        hudText << L" FPS: " << g_displayFPS << L"\n"
                            << L" Ping: " << std::fixed << std::setprecision(1) << g_currentPingMs << L" ms\n"
                            << L" Decode: " << std::fixed << std::setprecision(2) << clientLatencyMs << L" ms\n"
                            << L" Loss: " << std::fixed << std::setprecision(2) << packetLossPct << L"%";
                        std::wstring finalStr = hudText.str();

                        // 3. Define the box in the top-left corner
                        D2D1_RECT_F textRect = D2D1::RectF(20.0f, 20.0f, 300.0f, 150.0f);

                        // 4. Paint!
                        g_pD2DRenderTarget->FillRectangle(textRect, g_pBrushBackground); // Translucent backdrop
                        g_pD2DRenderTarget->DrawTextW(
                            finalStr.c_str(), finalStr.length(), g_pTextFormat, textRect, g_pBrushYellow
                        ); // Yellow Text

                        g_pD2DRenderTarget->EndDraw();
                    }

                    // Because we are using Flip Discard, this bypasses DWM and hits the glass instantly
                    g_pSwapChain->Present(0, 0);
                }
            }
        }
        // =========================================================================

        // --- LATENCY MATH & WINDOW TITLE UPDATE ---
        if (firstFrameReceived) {
            QueryPerformanceCounter(&frameEnd);

            // Only calculate latency if we actually processed a network packet this loop
            

            // Update Window Title once per second
            if ((frameEnd.QuadPart - secondTimer.QuadPart) >= timerFreq.QuadPart) {

                // 1. Calculate Packet Loss %
                double packetLossPct = 0.0;
                int totalPackets = g_packetsReceivedThisSecond + g_packetsLostThisSecond;
                if (totalPackets > 0) {
                    packetLossPct = ((double)g_packetsLostThisSecond / totalPackets) * 100.0;
                }

                // 2. Print to Console
                std::cout << "[METRICS] FPS: " << std::setw(2) << fpsCounter
                    << " | Ping: " << std::fixed << std::setprecision(1) << std::setw(4) << g_currentPingMs << " ms"
                    << " | Decode Latency: " << std::setw(4) << clientLatencyMs << " ms"
                    << " | Packet Loss: " << packetLossPct << "%\n";

                if (g_bitrateCooldown > 0) {
                    g_bitrateCooldown--; // Tick down the lock
                }
                else {
                    // Define our thresholds
                    bool networkIsStruggling = (packetLossPct > 2.0 || g_currentPingMs > 60.0);
                    bool networkIsPerfect = (packetLossPct == 0.0 && g_currentPingMs < 30.0);

                    if (networkIsStruggling && g_currentBitrate == BITRATE_HIGH) {
                        // SHIFT DOWN! (Emergency survival mode)
                        g_currentBitrate = BITRATE_LOW;
                        g_bitrateCooldown = 5; // Lock it for 5 seconds to let the router breathe

                        char shiftPacket[5];
                        shiftPacket[0] = 0x0A;
                        memcpy(shiftPacket + 1, &g_currentBitrate, 4);
                        sendto(inputSocket, shiftPacket, 5, 0, (struct sockaddr*)&hostInputAddr, sizeof(hostInputAddr));

                        std::cout << "\n[GEAR SHIFT] Network struggling! Dropping video quality to 3 Mbps.\n\n";
                    }
                    else if (networkIsPerfect && g_currentBitrate == BITRATE_LOW) {
                        // SHIFT UP! (Coast is clear)
                        g_currentBitrate = BITRATE_HIGH;
                        g_bitrateCooldown = 5;

                        char shiftPacket[5];
                        shiftPacket[0] = 0x0A;
                        memcpy(shiftPacket + 1, &g_currentBitrate, 4);
                        sendto(inputSocket, shiftPacket, 5, 0, (struct sockaddr*)&hostInputAddr, sizeof(hostInputAddr));

                        std::cout << "\n[GEAR SHIFT] Network recovered! Restoring video quality to 8 Mbps.\n\n";
                    }
                }

                // 3. Fire the next Heartbeat
                LARGE_INTEGER pingTime;
                QueryPerformanceCounter(&pingTime);
                char pingPacket[9];
                pingPacket[0] = 0x09;
                memcpy(pingPacket + 1, &pingTime.QuadPart, 8);
                sendto(inputSocket, pingPacket, 9, 0, (struct sockaddr*)&hostInputAddr, sizeof(hostInputAddr));

                // 4. Reset for the next second
                g_displayFPS = fpsCounter;
                fpsCounter = 0;
                g_packetsReceivedThisSecond = 0;
                g_packetsLostThisSecond = 0;
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
    SetThreadExecutionState(ES_CONTINUOUS);
    return 0;
}