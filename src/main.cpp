// src/main.cpp
#include <windows.h>
#include <windowsx.h>       // for GET_X_LPARAM, GET_Y_LPARAM
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <d3dx12.h>         // from third_party/d3dx12/d3dx12.h
#include <commdlg.h>        // GetOpenFileNameW
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <thread>
#include <algorithm>        // for std::clamp
#include <filesystem>       // C++17
#include <cstdio>           // for FILE*
#include <chrono>    // for steady_clock

#include <windows.h>
#include <shobjidl.h>   // IFileDialog
#include <wrl/client.h>
#include <filesystem>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <ShellScalingAPI.h>   // or <Shcore.h> on some SDKs
#pragma comment(lib, "Shcore.lib")
using Microsoft::WRL::ComPtr;


#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE2_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_resize2.h"

#define STB_EASY_FONT_IMPLEMENTATION
#include "stb_easy_font.h"

#define NOMINMAX
#undef max
#undef min

using Microsoft::WRL::ComPtr;

// ---------------------------------------------
// DX12 globals
static const UINT FrameCount = 2;
ComPtr<ID3D12Device>         g_device;
ComPtr<IDXGISwapChain3>      g_swapChain;
ComPtr<ID3D12CommandQueue>   g_cmdQueue;
ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
ComPtr<ID3D12Resource>       g_renderTargets[FrameCount];
ComPtr<ID3D12DescriptorHeap> g_srvHeap;
ComPtr<ID3D12RootSignature>  g_rootSig;
ComPtr<ID3D12PipelineState>  g_pipelineState;
// Our loaded image
ComPtr<ID3D12Resource>       g_texture;
ComPtr<ID3D12Fence>          g_fence;
UINT                         g_rtvDescSize;
UINT                         g_frameIndex = 0;
UINT64                       g_fenceValue = 0;
HANDLE                       g_fenceEvent = nullptr;
// text overlay pipeline objects
Microsoft::WRL::ComPtr<ID3D12RootSignature> g_textRootSig;
Microsoft::WRL::ComPtr<ID3D12PipelineState> g_textPSO;

// persistent overlay buffers
Microsoft::WRL::ComPtr<ID3D12Resource> g_textVB, g_textIB;
void* g_textVBMapped = nullptr;
void* g_textIBMapped = nullptr;
UINT  g_textVBCapacity = 0;
UINT  g_textIBCapacity = 0;




// Global variables for sorting image array
enum class SortMode { ByName, ByDateModified, ByDateCreated };
static SortMode g_sortMode = SortMode::ByName;

// ---- new globals for “next/prev” support ----
static std::vector<std::wstring> g_fileList;
static int                       g_currentFileIndex = 0;

// Track cursor movement time for hide
static std::chrono::steady_clock::time_point g_lastMouseMove;
static bool g_cursorHidden = false;

static std::wstring HumanSize(uint64_t bytes) {
    const wchar_t* u[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    double s = double(bytes); int i = 0;
    while (s >= 1024.0 && i < 4) { s /= 1024.0; ++i; }
    wchar_t buf[64]; swprintf_s(buf, L"%.2f %s", s, u[i]); return buf;
}

static std::wstring FileCreated(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return L"?";
    SYSTEMTIME stUTC{}, stLocal{};
    FileTimeToSystemTime(&fad.ftCreationTime, &stUTC);
    SystemTimeToTzSpecificLocalTime(nullptr, &stUTC, &stLocal);
    wchar_t buf[64];
    swprintf_s(buf, L"%02d/%02d/%04d %02d:%02d",
        stLocal.wMonth, stLocal.wDay, stLocal.wYear, stLocal.wHour, stLocal.wMinute);
    return buf;
}

// quick narrow (ASCII-only; non-ASCII -> '?') for stb_easy_font
static std::string NarrowAscii(const std::wstring& w) {
    std::string s; s.reserve(w.size());
    for (wchar_t c : w) s.push_back((c >= 32 && c < 127) ? char(c) : '?');
    return s;
}

static std::string BuildInfoLine(const std::wstring& path) {
    uint64_t sz = 0; try { sz = (uint64_t)std::filesystem::file_size(path); } catch(...) {}
    std::wstring info = L"Filename: " +
                        std::filesystem::path(path).filename().wstring() +
                        L"  |  Size: " + HumanSize(sz) +
                        L"  |  Date: " + FileCreated(path);
    return NarrowAscii(info);
}


// Image data globals
std::vector<uint8_t>         g_pixels;
int                          g_imgW = 0, g_imgH = 0;

// Track zoom interval and mouse position
float g_zoom       = 1.0f;    // current, used for rendering
float g_targetZoom = 1.0f;    // goal, set by wheel
float oldZoom = g_targetZoom; // before you change g_targetZoom
float g_offX = 0.0f;    // offset in clip space (-1…1)
float g_offY = 0.0f;
float g_targetOffX = 0.0f;
float g_targetOffY = 0.0f;

// Panning state
static bool g_isPanning = false;
static int  g_panLastX  = 0;
static int  g_panLastY  = 0;


int g_screenW = 0;
int g_screenH = 0;

// Letterbox scales (computed once at load)
float  g_baseScaleX = 1.0f;
float  g_baseScaleY = 1.0f;

static void UpdateClientSize(HWND hWnd) {
    RECT rc; GetClientRect(hWnd, &rc);
    g_screenW = std::max<int>(1, rc.right  - rc.left);
    g_screenH = std::max<int>(1, rc.bottom - rc.top);
}

// Full‐screen triangle shaders (will draw your image later)
static const char* g_VS = R"(
cbuffer TransformCB : register(b0)
{
    float scaleX;
    float scaleY;
    float offX;
    float offY;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    float2 quadPos[4] = {
        float2(-scaleX, -scaleY),
        float2( scaleX, -scaleY),
        float2(-scaleX,  scaleY),
        float2( scaleX,  scaleY)
    };
    float2 quadUV[4] = {
        float2(0, 1),
        float2(1, 1),
        float2(0, 0),
        float2(1, 0)
    };

    VSOut o;
    // apply zoom‐center translation
    o.pos = float4(quadPos[vid] + float2(offX, offY), 0, 1);
    o.uv  = quadUV[vid];
    return o;
}
)";

static const char* g_PS = R"(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };

Texture2D    tex  : register(t0);
SamplerState samp : register(s0);

float4 PSMain(VSOut vsIn) : SV_TARGET
{
    // sample with the UVs we generated in the VS
    return tex.Sample(samp, vsIn.uv);
}
)";

// ==== text overlay shaders (pixel-space triangles, solid color) ====
static const char* g_VS_Text = R"(
cbuffer ScreenCB : register(b0) { float2 invScreen; };
struct VSIn  { float2 pos : POSITION; float4 col : COLOR; };
struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };
VSOut VSMain(VSIn i) {
    float2 ndc = float2(i.pos.x * invScreen.x * 2.0f - 1.0f,
                        1.0f - i.pos.y * invScreen.y * 2.0f);
    VSOut o; o.pos = float4(ndc, 0, 1); o.col = i.col; return o;
}
)";

static const char* g_PS_Text = R"(
struct VSOut { float4 pos : SV_POSITION; float4 col : COLOR; };
float4 PSMain(VSOut i) : SV_TARGET { return i.col; }
)";

struct TextVertex { float x, y; float r, g, b, a; };
static bool g_drawText = false;

static D3D12_INPUT_ELEMENT_DESC g_TextIL[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
};

static void EnablePerMonitorV2DpiAwarenessEarly() {
    // Prefer Per-Monitor V2 on Win10+; fall back gracefully if unavailable.
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (user32) {
        using SetCtxFn = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (auto SetProcessDpiAwarenessContext =
                reinterpret_cast<SetCtxFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
            if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
            return;
        }
    }
    // Optional Win8.1 fallback (ok to omit if you only target Win10+):
    HMODULE shcore = LoadLibraryW(L"Shcore.dll");
    if (shcore) {
        using SetProcAwFn = HRESULT (WINAPI*)(PROCESS_DPI_AWARENESS);
        if (auto SetProcessDpiAwareness =
                reinterpret_cast<SetProcAwFn>(GetProcAddress(shcore, "SetProcessDpiAwareness"))) {
            SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
        }
    }
}

// Simple HRESULT checker (used by UpdateSubresources block)
inline void ThrowIfFailed(HRESULT hr) {
    if (hr == DXGI_ERROR_DEVICE_REMOVED) {
        // Grab the *actual* removal reason:
        HRESULT reason = g_device
            ? g_device->GetDeviceRemovedReason()
            : hr;
        wchar_t buf[128];
        swprintf_s(buf,
                   L"D3D12 device removed!\nGetDeviceRemovedReason = 0x%08X",
                   reason);
        MessageBoxW(nullptr, buf, L"DX12 DEVICE REMOVED", MB_OK|MB_ICONERROR);
        exit((int)reason);
    }
    if (FAILED(hr)) {
        wchar_t buf[64];
        swprintf_s(buf, L"HRESULT failed: 0x%08X", hr);
        MessageBoxW(nullptr, buf, L"Error", MB_OK|MB_ICONERROR);
        exit((int)hr);
    }
}


void CreateTextureFromPixels()
{
    if (g_imgW <= 0 || g_imgH <= 0 || g_pixels.empty())
        return;

    // 1) Clamp size (keep aspect) only if needed
    constexpr int kMaxTexDim = 16384;
    const int srcW = g_imgW, srcH = g_imgH;
    int dstW = srcW, dstH = srcH;

    if (srcW > kMaxTexDim || srcH > kMaxTexDim) {
        const double sx = double(kMaxTexDim) / double(srcW);
        const double sy = double(kMaxTexDim) / double(srcH);
        const double s  = (sx < sy) ? sx : sy;
        dstW = std::max(1, int(std::floor(srcW * s)));
        dstH = std::max(1, int(std::floor(srcH * s)));
    }

    // 2) Optional CPU resize (only if we actually clamped)
    const uint8_t* uploadData = g_pixels.data();
    std::vector<uint8_t> resized; // keep alive until copy completes
    if (dstW != srcW || dstH != srcH) {
        resized.resize(size_t(dstW) * size_t(dstH) * 4);

        // v2 API signature:
        // stbir_resize_uint8_srgb(in, w, h, strideB, out, W, H, strideB, STBIR_RGBA)
        stbir_resize_uint8_srgb(
            g_pixels.data(), srcW, srcH, srcW * 4,
            resized.data(),  dstW, dstH, dstW * 4,
            STBIR_RGBA
        );

        uploadData = resized.data();
    }

    OutputDebugStringA("CreateTextureFromPixels called\n");

    // 3) Create DEFAULT heap texture (dstW/dstH <= 16384)
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = static_cast<UINT64>(dstW);
    texDesc.Height           = static_cast<UINT>(dstH);
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1; // (set >1 if you add mips later)
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM; // keep your format
    texDesc.SampleDesc       = {1, 0};
    texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3D12Resource> tex;
    ThrowIfFailed(g_device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&tex)
    ));

    // 4) Create UPLOAD heap for the copy
    const UINT64 uploadSize = GetRequiredIntermediateSize(tex.Get(), 0, 1);
    Microsoft::WRL::ComPtr<ID3D12Resource> uploadHeap;
    ThrowIfFailed(g_device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(uploadSize),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadHeap)
    ));

    // 5) Record copy on a throwaway allocator/list (NO globals touched)
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    ThrowIfFailed(g_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    ThrowIfFailed(g_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        alloc.Get(), nullptr, IID_PPV_ARGS(&list)));

    D3D12_SUBRESOURCE_DATA sub = {};
    sub.pData      = uploadData;
    sub.RowPitch   = SIZE_T(dstW) * 4;
    sub.SlicePitch = SIZE_T(dstW) * SIZE_T(dstH) * 4;

    UpdateSubresources(list.Get(), tex.Get(), uploadHeap.Get(), 0, 0, 1, &sub);
    list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
    ThrowIfFailed(list->Close());

    ID3D12CommandList* lists[] = { list.Get() };
    g_cmdQueue->ExecuteCommandLists(1, lists);

    // 6) **Local** fence to wait for the copy (prevents stutter/deadlocks)
    Microsoft::WRL::ComPtr<ID3D12Fence> localFence;
    ThrowIfFailed(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&localFence)));
    HANDLE localEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!localEvent) { throw std::runtime_error("CreateEvent failed"); }

    const UINT64 fenceValue = 1;
    ThrowIfFailed(g_cmdQueue->Signal(localFence.Get(), fenceValue));
    ThrowIfFailed(localFence->SetEventOnCompletion(fenceValue, localEvent));
    WaitForSingleObject(localEvent, INFINITE);
    CloseHandle(localEvent);

    // 7) Create/overwrite SRV in your SRV heap
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format                  = texDesc.Format;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels     = 1;

    g_device->CreateShaderResourceView(
        tex.Get(), &srvDesc,
        g_srvHeap->GetCPUDescriptorHandleForHeapStart()
    );

    // 8) Publish texture
    g_texture = tex;

    // uploadHeap/alloc/list go out of scope here (safe after fence)
}

void CreateTextPipeline()
{
    // root sig: 2 float constants (invScreen)
    D3D12_ROOT_PARAMETER param{};
    param.ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    param.Constants.Num32BitValues = 2;
    param.Constants.ShaderRegister = 0; // b0
    param.ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_SIGNATURE_DESC rs{};
    rs.NumParameters = 1;
    rs.pParameters   = &param;
    rs.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> rsBlob, errBlob;
    ThrowIfFailed(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob));
    ThrowIfFailed(g_device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                                IID_PPV_ARGS(&g_textRootSig)));

    // compile shaders
    ComPtr<ID3DBlob> vs, ps, err;
    ThrowIfFailed(D3DCompile(g_VS_Text, strlen(g_VS_Text), nullptr, nullptr, nullptr,
                             "VSMain", "vs_5_0", 0, 0, &vs, &err));
    ThrowIfFailed(D3DCompile(g_PS_Text, strlen(g_PS_Text), nullptr, nullptr, nullptr,
                             "PSMain", "ps_5_0", 0, 0, &ps, &err));

    // PSO (alpha-blended triangles)
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature        = g_textRootSig.Get();
    pso.VS                    = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS                    = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout           = { g_TextIL, _countof(g_TextIL) };
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets      = 1;
    pso.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count      = 1;
    pso.SampleMask            = UINT_MAX;

    D3D12_RASTERIZER_DESC rast{};
    rast.FillMode = D3D12_FILL_MODE_SOLID;
    rast.CullMode = D3D12_CULL_MODE_NONE;
    rast.DepthClipEnable = TRUE;
    pso.RasterizerState = rast;

    D3D12_BLEND_DESC blend{};
    auto& rt = blend.RenderTarget[0];
    rt.BlendEnable           = TRUE;
    rt.SrcBlend              = D3D12_BLEND_SRC_ALPHA;
    rt.DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOp               = D3D12_BLEND_OP_ADD;
    rt.SrcBlendAlpha         = D3D12_BLEND_ONE;
    rt.DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.BlendState = blend;

    pso.DepthStencilState.DepthEnable   = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;

    ThrowIfFailed(g_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&g_textPSO)));
}

void CreateOrResizeTextBuffers(UINT vbBytesNeeded, UINT ibBytesNeeded)
{
    auto makeBuf = [&](UINT bytes, ComPtr<ID3D12Resource>& res, void*& mapped, UINT& cap)
    {
        if (res && cap >= bytes) return;
        cap = std::max(bytes, 256u * 1024u);
        mapped = nullptr;
        res.Reset();
        ThrowIfFailed(g_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(cap),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&res)));
        ThrowIfFailed(res->Map(0, nullptr, &mapped)); // keep mapped
    };

    makeBuf(vbBytesNeeded, g_textVB, g_textVBMapped, g_textVBCapacity);
    makeBuf(ibBytesNeeded, g_textIB, g_textIBMapped, g_textIBCapacity);
}

// Centered + scaled overlay (no index buffer)
void DrawOverlayText(ID3D12GraphicsCommandList* cl, const char* text,
                     float scale /*e.g. 2.0f*/, float centerX, float centerY, 
                     float r=1, float g=1, float b=1, float a=1)
{
    static char quadBuf[64 * 1024];
    int num_quads = stb_easy_font_print(0.0f, 0.0f, (char*)text, nullptr,
                                        quadBuf, sizeof(quadBuf));
    if (num_quads <= 0) return;

    struct V4 { float x,y,z,w; };
    auto qv = reinterpret_cast<V4*>(quadBuf);

    // 1) Compute the text bounding box in pixels (from stb output)
    float minx = FLT_MAX, miny = FLT_MAX;
    float maxx = -FLT_MAX, maxy = -FLT_MAX;
    for (int q = 0; q < num_quads; ++q) {
        for (int k = 0; k < 4; ++k) {
            float x = qv[q*4 + k].x;
            float y = qv[q*4 + k].y;
            minx = (x < minx) ? x : minx;
            miny = (y < miny) ? y : miny;
            maxx = (x > maxx) ? x : maxx;
            maxy = (y > maxy) ? y : maxy;
        }
    }
    const float w = (maxx - minx) * scale;
    const float h = (maxy - miny) * scale;

    // 2) Top-left so that the text's center sits at (centerX, centerY)
    const float baseX = centerX - w * 0.5f;
    const float baseY = centerY - h * 0.5f;

    // 3) Build 6 vertices per quad (two triangles), scaled & centered
    std::vector<TextVertex> verts;
    verts.reserve(size_t(num_quads) * 6);

    auto addTri = [&](float x0,float y0,float x1,float y1,float x2,float y2) {
        verts.push_back({ x0, y0, r,g,b,a });
        verts.push_back({ x1, y1, r,g,b,a });
        verts.push_back({ x2, y2, r,g,b,a });
    };

    for (int q = 0; q < num_quads; ++q) {
        float x0 = baseX + (qv[q*4 + 0].x - minx) * scale;
        float y0 = baseY + (qv[q*4 + 0].y - miny) * scale;
        float x1 = baseX + (qv[q*4 + 1].x - minx) * scale;
        float y1 = baseY + (qv[q*4 + 1].y - miny) * scale;
        float x2 = baseX + (qv[q*4 + 2].x - minx) * scale;
        float y2 = baseY + (qv[q*4 + 2].y - miny) * scale;
        float x3 = baseX + (qv[q*4 + 3].x - minx) * scale;
        float y3 = baseY + (qv[q*4 + 3].y - miny) * scale;

        // Tri 1: 0,1,2  |  Tri 2: 2,1,3
        addTri(x0,y0, x1,y1, x2,y2);
        addTri(x2,y2, x1,y1, x3,y3);
    }

    const UINT vbBytes = (UINT)(verts.size() * sizeof(TextVertex));

    // persistent upload VB (re-use across frames)
    if (!g_textVB || g_textVBCapacity < vbBytes) {
        g_textVBCapacity = (std::max)(vbBytes, 256u * 1024u);
        g_textVB.Reset();
        g_textVBMapped = nullptr;
        ThrowIfFailed(g_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(g_textVBCapacity),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&g_textVB)));
        ThrowIfFailed(g_textVB->Map(0, nullptr, &g_textVBMapped));
    }
    std::memcpy(g_textVBMapped, verts.data(), vbBytes);

    // set state for text
    cl->SetPipelineState(g_textPSO.Get());
    cl->SetGraphicsRootSignature(g_textRootSig.Get());

    float invScreen[2] = { 1.0f / float(g_screenW), 1.0f / float(g_screenH) };
    cl->SetGraphicsRoot32BitConstants(0, 2, invScreen, 0);

    D3D12_VERTEX_BUFFER_VIEW vbv{
        g_textVB->GetGPUVirtualAddress(), vbBytes, (UINT)sizeof(TextVertex)
    };

    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->IASetVertexBuffers(0, 1, &vbv);
    cl->DrawInstanced((UINT)verts.size(), 1, 0, 0);
}


// ------------------------------------------------
// Load an image from disk into g_pixels, g_imgW, g_imgH
bool LoadImage(const std::wstring& wpath) {
    // 1) Open the file as wide-char
    FILE* file = nullptr;
    if (_wfopen_s(&file, wpath.c_str(), L"rb") != 0 || !file) {
        MessageBoxW(nullptr,
                    L"Failed to open image file",
                    wpath.c_str(),
                    MB_OK | MB_ICONERROR);
        return false;
    }

    // 2) Let STB read from that FILE*
    int channels = 0;
    unsigned char* data = stbi_load_from_file(
        file,
        &g_imgW, &g_imgH,
        &channels, 4
    );
    fclose(file);

    // 3) Error-report if it failed
    if (!data) {
        const char* err = stbi_failure_reason();
        int wlen = MultiByteToWideChar(
            CP_UTF8, 0,
            err, -1,
            nullptr, 0
        );
        std::wstring werr(wlen, L'\0');
        MultiByteToWideChar(
            CP_UTF8, 0,
            err, -1,
            &werr[0], wlen
        );
        MessageBoxW(nullptr,
                    werr.c_str(),
                    L"LoadImage Error",
                    MB_OK | MB_ICONERROR);
        return false;
    }

    // 4) Copy into your pixel buffer
    size_t sz = size_t(g_imgW) * g_imgH * 4;
    try {
        g_pixels.assign(data, data + sz);
    } catch (const std::bad_alloc&) {
        MessageBoxW(nullptr,
                    L"Out of memory while copying image",
                    L"LoadImage Error",
                    MB_OK | MB_ICONERROR);
        stbi_image_free(data);
        return false;
    }
    stbi_image_free(data);

    return true;
}

// helper to get creation FILETIME for a path
static FILETIME GetCreationTime(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExW(path.c_str(),
                             GetFileExInfoStandard,
                             &data))
    {
        return data.ftCreationTime;
    }
    // on failure, return zero (earliest possible)
    return FILETIME{ 0, 0 };
}

auto sortFiles = [&](){
    namespace fs = std::filesystem;
    switch (g_sortMode) {
    case SortMode::ByName:
        std::sort(g_fileList.begin(), g_fileList.end(),
                  std::greater<std::wstring>()); 
        break;

    case SortMode::ByDateModified:
        std::sort(g_fileList.begin(), g_fileList.end(),
            [&](auto &a, auto &b){
                return fs::last_write_time(a)
                     > fs::last_write_time(b);  // descending
            });
        break;

    case SortMode::ByDateCreated:
        std::sort(g_fileList.begin(), g_fileList.end(),
            [&](auto &a, auto &b){
                FILETIME ca = GetCreationTime(a);
                FILETIME cb = GetCreationTime(b);
                ULARGE_INTEGER ua = {};
                ua.LowPart  = ca.dwLowDateTime;
                ua.HighPart = ca.dwHighDateTime;
                ULARGE_INTEGER ub = {};
                ub.LowPart  = cb.dwLowDateTime;
                ub.HighPart = cb.dwHighDateTime;
                return ua.QuadPart > ub.QuadPart;  // descending
            });
        break;
    }
};

static bool HasExt(const std::wstring& extLower) {
    static const std::unordered_set<std::wstring> kExts = {
        L".png", L".jpg", L".jpeg"
    };
    return kExts.count(extLower) != 0;
}

bool OpenFileDialogAndLoad()
{
    // Init COM for the file dialog
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool didInitCOM = SUCCEEDED(hr);

    ComPtr<IFileDialog> dlg;
    hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (FAILED(hr)) { if (didInitCOM) CoUninitialize(); return false; }

    // Filters: only PNG and JPG
    COMDLG_FILTERSPEC filters[] = {
        { L"Images (png, jpg)", L"*.png;*.jpg;*.jpeg" },
        { L"All Files", L"*.*" }
    };
    dlg->SetFileTypes(ARRAYSIZE(filters), filters);
    dlg->SetFileTypeIndex(1); // default to Images
    dlg->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);

    hr = dlg->Show(nullptr);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) { if (didInitCOM) CoUninitialize(); return false; }
    if (FAILED(hr)) { if (didInitCOM) CoUninitialize(); return false; }

    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) { if (didInitCOM) CoUninitialize(); return false; }

    PWSTR pszPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) { if (didInitCOM) CoUninitialize(); return false; }

    std::wstring selectedPath(pszPath);
    CoTaskMemFree(pszPath);

    // Enumerate PNG/JPG images in same folder
    namespace fs = std::filesystem;
    fs::path selected(selectedPath);
    fs::path folder = selected.parent_path();

    g_fileList.clear();
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        auto p = it->path();
        std::wstring ext = p.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (HasExt(ext)) g_fileList.push_back(p.wstring());
    }

    sortFiles();

    auto it = std::find(g_fileList.begin(), g_fileList.end(), selected.wstring());
    g_currentFileIndex = (it == g_fileList.end()) ? 0 : int(std::distance(g_fileList.begin(), it));

    bool ok = false;
    if (!g_fileList.empty()) ok = LoadImage(g_fileList[g_currentFileIndex]);

    if (didInitCOM) CoUninitialize();
    if (!ok) {
        MessageBoxW(nullptr, L"LoadImage failed", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

// Forward‐declare Win32 window proc
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wP, LPARAM lP)
{
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_MOUSEWHEEL:
    {
        using clock = std::chrono::steady_clock;
        g_lastMouseMove = clock::now();
        if (g_cursorHidden) {
            ShowCursor(TRUE);
            g_cursorHidden = false;
        }

        UpdateClientSize(hWnd);
        
        // 1) Compute mouse in NDC (–1…+1)
        POINT pt; 
        GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);
        float ndcX = (pt.x / float(g_screenW)) * 2.0f - 1.0f;
        float ndcY = 1.0f - (pt.y / float(g_screenH)) * 2.0f;

        // 2) Grab your *current* zoom & pan
        float oldZ    = g_zoom;
        float oldOffX = g_offX;
        float oldOffY = g_offY;

        // 3) Compute the new zoom level
        int   notches = GET_WHEEL_DELTA_WPARAM(wP) / WHEEL_DELTA;
        const float step = 0.3f;
        float newZ = std::clamp(
            oldZ * powf(1.0f + step, float(notches)),
            0.5f, 10.0f
        );

        // 4) Pivot around the cursor so that point stays fixed:
        //    newOff = oldOff * ratio + ndc*(1 - ratio)
        float ratio    = newZ / oldZ;
        float newOffX  = oldOffX * ratio + ndcX * (1.0f - ratio);
        float newOffY  = oldOffY * ratio + ndcY * (1.0f - ratio);

        // 5) Commit to the *target* values—your render‐loop lerp will smooth you there
        g_targetZoom   = newZ;
        g_targetOffX   = newOffX;
        g_targetOffY   = newOffY;

        return 0;
    }

    
    case WM_RBUTTONDOWN: {
        // Right click → move backward
        int dir = -1;
        int n   = int(g_fileList.size());
        g_currentFileIndex = (g_currentFileIndex + dir + n) % n;

        if (LoadImage(g_fileList[g_currentFileIndex])) {
            // Recompute aspect‐ratio letterboxing
            float imgAspect    = float(g_imgW) / float(g_imgH);
            float screenAspect = float(g_screenW) / float(g_screenH);
            g_baseScaleX = g_baseScaleY = 1.0f;
            if (imgAspect > screenAspect) {
                // image is wider → pillarbox vertically
                g_baseScaleY = screenAspect / imgAspect;
            } else {
                // image taller → letterbox horizontally
                g_baseScaleX = imgAspect / screenAspect;
            }

            // Upload to GPU
            CreateTextureFromPixels();
        }
        return 0;
    }
    
    case WM_LBUTTONDOWN: {
        // Left click → move forward
        int dir = +1;
        int n   = int(g_fileList.size());
        g_currentFileIndex = (g_currentFileIndex + dir + n) % n;

        if (LoadImage(g_fileList[g_currentFileIndex])) {
            // Recompute aspect‐ratio letterboxing
            float imgAspect    = float(g_imgW) / float(g_imgH);
            float screenAspect = float(g_screenW) / float(g_screenH);
            g_baseScaleX = g_baseScaleY = 1.0f;
            if (imgAspect > screenAspect) {
                // image is wider → pillarbox vertically
                g_baseScaleY = screenAspect / imgAspect;
            } else {
                // image taller → letterbox horizontally
                g_baseScaleX = imgAspect / screenAspect;
            }

            // Upload to GPU
            CreateTextureFromPixels();
        }
        return 0;
    }

    case WM_KEYDOWN:
    {
        if (wP == VK_ESCAPE) {
            // Cleanly close the window / exit message loop
            PostQuitMessage(0);
            return 0;
        }
        if ((wP == VK_RIGHT || wP == VK_LEFT) && !g_fileList.empty()) {
            int dir = (wP == VK_RIGHT) ? +1 : -1;
            int n   = int(g_fileList.size());
            g_currentFileIndex = (g_currentFileIndex + dir + n) % n;

            if (LoadImage(g_fileList[g_currentFileIndex])) {
                // Recompute aspect‐ratio letterboxing
                float imgAspect    = float(g_imgW) / float(g_imgH);
                float screenAspect = float(g_screenW) / float(g_screenH);
                g_baseScaleX = g_baseScaleY = 1.0f;
                if (imgAspect > screenAspect) {
                    // image is wider → pillarbox vertically
                    g_baseScaleY = screenAspect / imgAspect;
                } else {
                    // image taller → letterbox horizontally
                    g_baseScaleX = imgAspect / screenAspect;
                }

                // Upload to GPU
                CreateTextureFromPixels();
            }
            return 0;
        }
        if (wP == VK_UP || wP == VK_DOWN) {
            if (GetAsyncKeyState(VK_UP) < 0) {
                // 1) reset pan to center
                g_offX        = g_targetOffX = 0.0f;
                g_offY        = g_targetOffY = 0.0f;

                // 2) zoom in
                int notches = 1;
                const float zoomStep = 0.05f;
                float factorZ = powf(1.0f + zoomStep, float(notches));
                g_targetZoom = std::clamp(g_targetZoom * factorZ, 0.1f, 10.0f);

                // 3) perfect cursor-centric pivot, but with pivot at image center
                float oldZ = g_zoom;
                float newZ = g_targetZoom;
                float f    = newZ / oldZ;
                float centerX = 0.0f; // center of image in NDC
                float centerY = 0.0f; // center of image in NDC
                g_targetOffX = centerX * (1.0f - f) + g_targetOffX * f;
                g_targetOffY = centerY * (1.0f - f) + g_targetOffY * f;
            }

            if (GetAsyncKeyState(VK_DOWN) < 0) {
                // zoom out
                int notches = -1;
                const float zoomStep = 0.05f;
                float factorZ = powf(1.0f + zoomStep, float(notches));
                g_targetZoom = std::clamp(g_targetZoom * factorZ, 0.1f, 10.0f);

                // 3) perfect cursor-centric pivot, but with pivot at image center
                float oldZ = g_zoom;
                float newZ = g_targetZoom;
                float f    = newZ / oldZ;
                float centerX = 0.0f; // center of image in NDC
                float centerY = 0.0f; // center of image in NDC
                g_targetOffX = centerX * (1.0f - f) + g_targetOffX * f;
                g_targetOffY = centerY * (1.0f - f) + g_targetOffY * f;
            }
        }
        if (wP == 'R') {
            // Reset zoom & pan
            g_zoom        = g_targetZoom = 1.0f;
            g_offX        = g_targetOffX = 0.0f;
            g_offY        = g_targetOffY = 0.0f;
            return 0;
        }
        if (wP == 'T') {
            // cycle through Name → Modified → Created
            g_sortMode = SortMode((int(g_sortMode) + 1) % 3);
            // re-sort & reset index to the current file’s new position:
            std::wstring curr = g_fileList[g_currentFileIndex];
            sortFiles();
            auto it = std::find(g_fileList.begin(), g_fileList.end(), curr);
            g_currentFileIndex = it == g_fileList.end()
                ? 0
                : int(std::distance(g_fileList.begin(), it));
            return 0;
        }
        if (wP == 'O') {
            if (OpenFileDialogAndLoad()) {
                CreateTextureFromPixels();
            }

            return 0;
        }
        if (wP == 'I') {
            g_drawText = !g_drawText;
        }

        break;
    }

    case WM_MBUTTONDOWN:
    {
        SetCapture(hWnd);                // capture mouse to window
        g_isPanning = true;
        g_panLastX = GET_X_LPARAM(lP);
        g_panLastY = GET_Y_LPARAM(lP);
        // optional: show a “move” cursor while panning
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return 0;
    }

    case WM_MBUTTONUP:
    {
        g_isPanning = false;
        ReleaseCapture();
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        return 0;
    }

    // When mouse moves unhide cursor
    case WM_MOUSEMOVE:
    {
        using clock = std::chrono::steady_clock;
        g_lastMouseMove = clock::now();
        if (g_cursorHidden) {
            ShowCursor(TRUE);
            g_cursorHidden = false;
        }

        if (g_isPanning)
        {
            UpdateClientSize(hWnd); // keep g_screenW/H current

            const int mx = GET_X_LPARAM(lP);
            const int my = GET_Y_LPARAM(lP);
            const int dx = mx - g_panLastX;
            const int dy = my - g_panLastY;
            g_panLastX = mx;
            g_panLastY = my;

            // Convert pixel delta -> NDC delta (–1..+1 across the screen)
            const float ndc_dx =  2.0f * (float)dx / (float)g_screenW;
            const float ndc_dy = -2.0f * (float)dy / (float)g_screenH; // flip Y

            // Move the *target* offsets (your render loop already lerps to these)
            g_targetOffX += ndc_dx;
            g_targetOffY += ndc_dy;

            // (optional) clamp here for snappier feel; otherwise frame loop clamps:
            // float halfW = g_baseScaleX * g_targetZoom;
            // float halfH = g_baseScaleY * g_targetZoom;
            // float panLimitX = (halfW > 1.f) ? (halfW - 1.f) : 0.f;
            // float panLimitY = (halfH > 1.f) ? (halfH - 1.f) : 0.f;
            // g_targetOffX = std::clamp(g_targetOffX, -panLimitX, panLimitX);
            // g_targetOffY = std::clamp(g_targetOffY, -panLimitY, panLimitY);

            return 0;
        }
        break;
        }
    

    }
    return DefWindowProc(hWnd, msg, wP, lP);
}

// ------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {

    // Make the process DPI-aware BEFORE any windows/dialogs are created.
    EnablePerMonitorV2DpiAwarenessEarly();

    // Run windows file open dialog
    if (!OpenFileDialogAndLoad())
    return 0;   // no file → exit

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    g_screenW = screenW;
    g_screenH = screenH;

    // compute image vs screen aspect once
    {
        float imgAspect    = float(g_imgW) / float(g_imgH);
        float screenAspect = float(g_screenW) / float(g_screenH);
        g_baseScaleX = 1.0f;
        g_baseScaleY = 1.0f;
        if (imgAspect > screenAspect) {
            // image is wider → pillarbox vertically
            g_baseScaleY = screenAspect / imgAspect;
        } else {
            // image taller → letterbox horizontally
            g_baseScaleX = imgAspect / screenAspect;
        }
    }

    // 2) Win32 window setup
    WNDCLASS wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"HDRViewerClass";
    RegisterClass(&wc);


    // 2) Create a WS_POPUP window at full-screen size:
    HWND hwnd = CreateWindowEx(
        0,
        wc.lpszClassName,
        L"HDR Viewer",
        WS_POPUP | WS_VISIBLE,      // borderless & visible
        0, 0,                       // top-left
        screenW, screenH,           // full-screen dimensions
        nullptr, nullptr, hInst, nullptr
    );
    ShowWindow(hwnd, nCmdShow);

    // 3) DX12 device + swap chain
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                      IID_PPV_ARGS(&g_device));

    D3D12_COMMAND_QUEUE_DESC cqDesc{};
    g_device->CreateCommandQueue(
        &cqDesc, IID_PPV_ARGS(&g_cmdQueue));

    ThrowIfFailed(g_device->CreateFence(
    0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)
    ));
    g_fenceValue  = 0;
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent) {
        MessageBoxW(nullptr, L"Failed to create fence event", L"Error", MB_OK | MB_ICONERROR);
        return 0;
    }

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.BufferCount       = FrameCount;
    scd.Width             = screenW;
    scd.Height            = screenH;
    scd.Format            = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect        = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count  = 1;


    ComPtr<IDXGISwapChain1> sc1;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(
        g_cmdQueue.Get(), hwnd, &scd, nullptr, nullptr, &sc1
    ));
    ThrowIfFailed(sc1.As(&g_swapChain));

    // 5) Make sure to allow Alt+Enter (so the overlay can hook):
    factory->MakeWindowAssociation(hwnd, 0);

    // 6) Enter exclusive full-screen
    // ThrowIfFailed(g_swapChain->SetFullscreenState(TRUE, nullptr));

    // 7) **Force** the buffers to the exact full-screen size
    ThrowIfFailed(g_swapChain->ResizeBuffers(
        FrameCount,
        screenW,      // your GetSystemMetrics width
        screenH,      // your GetSystemMetrics height
        scd.Format,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
    ));

    // 8) Now recreate your RTV heap & views
    g_rtvHeap.Reset();
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FrameCount;
    rtvHeapDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(g_device->CreateDescriptorHeap(
        &rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap)
    ));
    g_rtvDescSize = g_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV
    );

    auto rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FrameCount; ++i) {
        ComPtr<ID3D12Resource> backBuffer;
        ThrowIfFailed(g_swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));
        g_renderTargets[i] = backBuffer;
        g_device->CreateRenderTargetView(
            backBuffer.Get(), nullptr, rtvHandle
        );
        rtvHandle.ptr += g_rtvDescSize;
    }


    // 9) Build SRV heap for later
    {
        D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
        srvDesc.NumDescriptors = 1;
        srvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        g_device->CreateDescriptorHeap(
            &srvDesc, IID_PPV_ARGS(&g_srvHeap));
    }

    // MessageBoxW(nullptr, L"SRV heap created", L"Debug", MB_OK);


    // 10) Create root signature
    {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors                    = 1;
        range.BaseShaderRegister                = 0;
        range.RegisterSpace                     = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        // 1) SRV parameter (t0)
        D3D12_ROOT_PARAMETER srvParam{};
        srvParam.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        srvParam.DescriptorTable.NumDescriptorRanges = 1;
        srvParam.DescriptorTable.pDescriptorRanges   = &range;
        srvParam.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        // 2) 32‐bit constants for scaleX/scaleY (b0)
        D3D12_ROOT_PARAMETER scaleParam{};
        scaleParam.ParameterType                    = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        scaleParam.Constants.Num32BitValues         = 4;  // scaleX, scaleY, offX, offY
        scaleParam.Constants.ShaderRegister         = 0; // b0
        scaleParam.Constants.RegisterSpace          = 0;
        scaleParam.ShaderVisibility                 = D3D12_SHADER_VISIBILITY_VERTEX;

        // 3) Static sampler as before
        D3D12_STATIC_SAMPLER_DESC sampDesc{};
        sampDesc.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampDesc.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampDesc.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampDesc.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampDesc.ShaderRegister = 0;
        sampDesc.RegisterSpace  = 0;
        sampDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // 4) Pack into array
        D3D12_ROOT_PARAMETER params[2] = { srvParam, scaleParam };

        // 5) Build & serialize
        D3D12_ROOT_SIGNATURE_DESC rsDesc{};
        rsDesc.NumParameters     = 2;
        rsDesc.pParameters       = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers   = &sampDesc;
        rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> rsBlob, errBlob;
        D3D12SerializeRootSignature(
          &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob);
        g_device->CreateRootSignature(
          0, rsBlob->GetBufferPointer(),
          rsBlob->GetBufferSize(),
          IID_PPV_ARGS(&g_rootSig));
    }

    // MessageBoxW(nullptr, L"Root signature created", L"Debug", MB_OK);


    // 12) Compile & create PSO (with explicit states + error check)
    {
        ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
        // Compile VS
        HRESULT hr = D3DCompile(g_VS, strlen(g_VS), nullptr, nullptr, nullptr,
                                "VSMain", "vs_5_0", 0, 0, &vsBlob, &errBlob);
        if (FAILED(hr)) {
            MessageBoxW(nullptr, L"Vertex shader compile failed", L"PSO Error", MB_OK | MB_ICONERROR);
            return 0;
        }
        // Compile PS with detailed error output
        ComPtr<ID3DBlob> psErr;
        hr = D3DCompile(
            g_PS, strlen(g_PS),
            nullptr, nullptr, nullptr,
            "PSMain", "ps_5_0",
            0, 0,
            &psBlob,
            &psErr
        );
        if (FAILED(hr)) {
            // Extract error message
            const char* errMsg = psErr 
                ? reinterpret_cast<const char*>(psErr->GetBufferPointer()) 
                : "Unknown PS compile error";
            MessageBoxA(nullptr, errMsg, "Pixel Shader Compile Error", MB_OK | MB_ICONERROR);
            return 0;
        }


        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature        = g_rootSig.Get();
        psoDesc.VS                    = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        psoDesc.PS                    = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };

        // Explicit default rasterizer state
        D3D12_RASTERIZER_DESC rastDesc{};
        rastDesc.FillMode              = D3D12_FILL_MODE_SOLID;
        rastDesc.CullMode              = D3D12_CULL_MODE_NONE;
        rastDesc.FrontCounterClockwise = FALSE;
        rastDesc.DepthClipEnable       = TRUE;
        psoDesc.RasterizerState        = rastDesc;

        // Explicit default blend state
        D3D12_RENDER_TARGET_BLEND_DESC rtbd{};
        rtbd.BlendEnable           = FALSE;
        rtbd.LogicOpEnable         = FALSE;
        rtbd.SrcBlend              = D3D12_BLEND_ONE;
        rtbd.DestBlend             = D3D12_BLEND_ZERO;
        rtbd.BlendOp               = D3D12_BLEND_OP_ADD;
        rtbd.SrcBlendAlpha         = D3D12_BLEND_ONE;
        rtbd.DestBlendAlpha        = D3D12_BLEND_ZERO;
        rtbd.BlendOpAlpha          = D3D12_BLEND_OP_ADD;
        rtbd.LogicOp               = D3D12_LOGIC_OP_NOOP;
        rtbd.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        D3D12_BLEND_DESC blendDesc{};
        blendDesc.AlphaToCoverageEnable   = FALSE;
        blendDesc.IndependentBlendEnable  = FALSE;
        blendDesc.RenderTarget[0]         = rtbd;
        psoDesc.BlendState                = blendDesc;

        psoDesc.DepthStencilState.DepthEnable   = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.SampleMask             = UINT_MAX;
        psoDesc.PrimitiveTopologyType  = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets       = 1;
        psoDesc.RTVFormats[0]          = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count       = 1;
        psoDesc.InputLayout            = { nullptr, 0 };

        // Create PSO and check errors
        hr = g_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&g_pipelineState));
        if (FAILED(hr)) {
            wchar_t buf[128];
            swprintf_s(buf, L"CreateGraphicsPipelineState failed: 0x%08X", hr);
            MessageBoxW(nullptr, buf, L"PSO Error", MB_OK | MB_ICONERROR);
            return 0;
        }
    }

    CreateTextPipeline();

    // ——— 13) Create and upload the texture (with debug) ———
    if (!g_pixels.empty()) {
        CreateTextureFromPixels();
    }

    g_lastMouseMove = std::chrono::steady_clock::now();

    // 14) Main loop: clear & present
    MSG msg{};
    while (msg.message != WM_QUIT) {
        {
            using clock = std::chrono::steady_clock;
            auto now   = clock::now();
            auto idle  = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastMouseMove).count();
            if (idle > 2000 && !g_cursorHidden) {
                ShowCursor(FALSE);
                g_cursorHidden = true;
            }
        }

        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
             // 1) Create per‐frame allocator & command list
            ComPtr<ID3D12CommandAllocator> allocator;
            ThrowIfFailed(g_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)
            ));
            ComPtr<ID3D12GraphicsCommandList> cl;
            ThrowIfFailed(g_device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                allocator.Get(), nullptr,
                IID_PPV_ARGS(&cl)
            ));

            // 2) Determine clear color
            FLOAT clearCol[4];
            if (g_pixels.empty()) {
                // default “no image” color—keep as is
                clearCol[0] = 0.0f;
                clearCol[1] = 0.2f;
                clearCol[2] = 0.4f;
                clearCol[3] = 1.0f;
            } else {
                // new dark slate tone
                clearCol[0] = 0.05f;  // almost black red
                clearCol[1] = 0.07f;  // very dark green
                clearCol[2] = 0.10f;  // hint of blue
                clearCol[3] = 1.0f;
            }
            // cl->ClearRenderTargetView(rtvHandle, clearCol, 0, nullptr);

            
            // 3) Transition back‐buffer into RENDER_TARGET
            cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
                g_renderTargets[g_frameIndex].Get(),
                D3D12_RESOURCE_STATE_PRESENT,
                D3D12_RESOURCE_STATE_RENDER_TARGET
            ));

            // 4) Bind the render target
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle =
                g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtvHandle.ptr += g_frameIndex * g_rtvDescSize;
            cl->OMSetRenderTargets(
                1,            // one RTV
                &rtvHandle,   // pointer to RTV handle
                FALSE,        // not a descriptor table
                nullptr       // no DSV
            );

            // 5) Clear it
            cl->ClearRenderTargetView(rtvHandle, clearCol, 0, nullptr);

            // 6) (Optional) Set viewport & scissor, if not done elsewhere
            int screenW = GetSystemMetrics(SM_CXSCREEN);
            int screenH = GetSystemMetrics(SM_CYSCREEN);
    
            D3D12_VIEWPORT vp = { 0.0f, 0.0f,
                      float(screenW), float(screenH),
                                0.0f, 1.0f };
            cl->RSSetViewports(1, &vp);
            D3D12_RECT sc = { 0, 0, screenW, screenH };
            cl->RSSetScissorRects(1, &sc);


            // 7) Bind your texture and draw
            ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
            cl->SetDescriptorHeaps(_countof(heaps), heaps);
            cl->SetGraphicsRootSignature(g_rootSig.Get());
            cl->SetPipelineState       (g_pipelineState.Get());

            // slot 0 → texture SRV
            cl->SetGraphicsRootDescriptorTable(
                0,
                g_srvHeap->GetGPUDescriptorHandleForHeapStart()
            );

            const float zoomLerp = 0.1f, panLerp = 0.1f;
            g_zoom += (g_targetZoom - g_zoom) * zoomLerp;
            g_offX += (g_targetOffX - g_offX) * panLerp;
            g_offY += (g_targetOffY - g_offY) * panLerp;

            // then clamp exactly as before
            // float halfW = g_baseScaleX * g_zoom;
            // float halfH = g_baseScaleY * g_zoom;
            // float panLimitX = (halfW > 1) ? (halfW - 1) : 0;
            // float panLimitY = (halfH > 1) ? (halfH - 1) : 0;
            // g_offX = std::clamp(g_offX, -panLimitX, panLimitX);
            // g_offY = std::clamp(g_offY, -panLimitY, panLimitY);

            // 4) push the four transform constants:
            float t[4] = { g_baseScaleX * g_zoom,
                        g_baseScaleY * g_zoom,
                        g_offX,
                        g_offY };

            cl->SetGraphicsRoot32BitConstants(1, 4, t, 0);


            // draw full-screen triangle
            cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            cl->DrawInstanced(4, 1, 0, 0);

            // Build the info line for current file and draw it
            if (!g_fileList.empty() && g_drawText) {
                std::string info = BuildInfoLine(g_fileList[g_currentFileIndex]);

                // Offsets for a crude 1-pixel border (in screen-space pixels)
                const float scale   = 3.0f;
                const float cx      = 0.5f * g_screenW;
                const float cy      = 0.025f * g_screenH;


                // Finally the main text in green on top
                DrawOverlayText(cl.Get(), info.c_str(), scale, cx, cy, 0,1,0,1.0f);
            }


            // 8) Transition back into PRESENT
            cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
                g_renderTargets[g_frameIndex].Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PRESENT
            ));

            // close + submit
            cl->Close();
            ID3D12CommandList* lists[] = { cl.Get() };
            g_cmdQueue->ExecuteCommandLists(_countof(lists), lists);

            // present immediately, no v-sync
            g_swapChain->Present(1, 0);

            // frame-timing
            using clock = std::chrono::high_resolution_clock;
            static auto lastFrame = clock::now();
            auto now     = clock::now();
            auto elapsed = std::chrono::duration<float, std::milli>(now - lastFrame).count();
            auto target  = 1000.0f / 60.0f;
            lastFrame = clock::now();

            // swap buffer index
            g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
        }
    }

    return 0;
}

