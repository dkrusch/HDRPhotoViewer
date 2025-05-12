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


#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

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
ComPtr<ID3D12Resource>       g_scaleBuffer;  
ComPtr<ID3D12DescriptorHeap> g_cbvSrvUavHeap;
ComPtr<ID3D12Resource>       g_texture;
ComPtr<ID3D12Fence>          g_fence;
UINT                         g_rtvDescSize;
UINT                         g_frameIndex = 0;
UINT64                       g_fenceValue = 0;
HANDLE                       g_fenceEvent = nullptr;



// List of full paths to each image in the chosen folder
static std::vector<std::wstring> g_fileList;

// Index into g_fileList for the currently displayed image
// size_t is a good choice since it matches vector::size_type
static size_t g_currentIndex = 0;

// Image data globals
std::vector<uint8_t>         g_pixels;
int                          g_imgW = 0, g_imgH = 0;
// Track zoom interval and mouse position
float g_zoom       = 1.0f;    // current, used for rendering
float g_targetZoom = 1.0f;    // goal, set by wheel
float g_offX = 0.0f;    // offset in clip space (-1…1)
float g_offY = 0.0f;
float g_targetOffX = 0.0f;
float g_targetOffY = 0.0f;

int g_screenW = 0;
int g_screenH = 0;

// Letterbox scales (computed once at load)
float  g_baseScaleX = 1.0f;
float  g_baseScaleY = 1.0f;

// Full‐screen triangle shaders (will draw your image later)
static const char* g_VS = R"(
cbuffer TransformCB : register(b0)
{
    float g_baseScaleX;
    float g_baseScaleY;
    float g_offX;
    float g_offY;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    float2 quadPos[4] = {
        float2(-g_baseScaleX, -g_baseScaleY),
        float2( g_baseScaleX, -g_baseScaleY),
        float2(-g_baseScaleX,  g_baseScaleY),
        float2( g_baseScaleX,  g_baseScaleY)
    };
    float2 quadUV[4] = {
        float2(0, 1),
        float2(1, 1),
        float2(0, 0),
        float2(1, 0)
    };

    VSOut o;
    // apply zoom‐center translation
    o.pos = float4(quadPos[vid] + float2(g_offX, g_offY), 0, 1);
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


// Simple HRESULT checker (used by UpdateSubresources block)
inline void ThrowIfFailed(HRESULT hr) {
    if (FAILED(hr)) {
        wchar_t buf[64];
        swprintf_s(buf, L"HRESULT failed: 0x%08X", hr);
        MessageBoxW(nullptr, buf, L"Error", MB_OK | MB_ICONERROR);
        exit((int)hr);
    }
}

void CreateGpuTexture()
{
    OutputDebugStringW(L"CT: Enter CreateGpuTexture\n");

    // 1) Describe the texture
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = UINT64(g_imgW);
    texDesc.Height           = UINT(g_imgH);
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    OutputDebugStringW(L"CT: texDesc filled\n");

    // 2) Create default-heap texture
    D3D12_HEAP_PROPERTIES defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> tex;
    HRESULT hr = g_device->CreateCommittedResource(
        &defaultHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&tex)
    );
    OutputDebugStringW(L"CT: After CreateCommittedResource(default)\n");
    if (FAILED(hr)) {
        wchar_t buf[64]; swprintf_s(buf, L"CT ERROR default heap: 0x%08X\n", hr);
        OutputDebugStringW(buf);
        return;
    }

    // 3) Create upload-heap
    UINT64 uploadSize = GetRequiredIntermediateSize(tex.Get(), 0, 1);
    D3D12_HEAP_PROPERTIES uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);
    ComPtr<ID3D12Resource> uploadHeap;
    hr = g_device->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadHeap)
    );
    OutputDebugStringW(L"CT: After CreateCommittedResource(upload)\n");
    if (FAILED(hr)) {
        wchar_t buf[64]; swprintf_s(buf, L"CT ERROR upload heap: 0x%08X\n", hr);
        OutputDebugStringW(buf);
        return;
    }

    // 4) Prepare subresource
    D3D12_SUBRESOURCE_DATA subData = {};
    subData.pData      = g_pixels.data();
    subData.RowPitch   = SIZE_T(g_imgW) * 4;
    subData.SlicePitch = subData.RowPitch * g_imgH;
    OutputDebugStringW(L"CT: Prepared subData\n");

    // 5) Record copy & barrier
    ComPtr<ID3D12CommandAllocator> tmpAlloc;
    ThrowIfFailed(g_device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&tmpAlloc)
    ));
    OutputDebugStringW(L"CT: Created tmp allocator\n");

    ComPtr<ID3D12GraphicsCommandList> tmpList;
    ThrowIfFailed(g_device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, tmpAlloc.Get(), nullptr,
        IID_PPV_ARGS(&tmpList)
    ));
    OutputDebugStringW(L"CT: Created tmp command list\n");

    UpdateSubresources(tmpList.Get(), tex.Get(), uploadHeap.Get(), 0, 0, 1, &subData);
    OutputDebugStringW(L"CT: Called UpdateSubresources\n");

    tmpList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
        tex.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    ));
    OutputDebugStringW(L"CT: Added ResourceBarrier\n");

    ThrowIfFailed(tmpList->Close());
    OutputDebugStringW(L"CT: Closed command list\n");

    // 6) Execute & fence-sync
    g_cmdQueue->ExecuteCommandLists(1, reinterpret_cast<ID3D12CommandList* const*>(tmpList.GetAddressOf()));
    OutputDebugStringW(L"CT: Executed command list\n");

    g_fenceValue++;
    ThrowIfFailed(g_cmdQueue->Signal(g_fence.Get(), g_fenceValue));
    ThrowIfFailed(g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent));
    WaitForSingleObject(g_fenceEvent, INFINITE);
    OutputDebugStringW(L"CT: Fence sync done\n");

    // 7) Create the SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format                  = texDesc.Format;
    srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels     = 1;

    g_device->CreateShaderResourceView(
        tex.Get(), &srvDesc,
        g_srvHeap->GetCPUDescriptorHandleForHeapStart()
    );
    OutputDebugStringW(L"CT: Created SRV\n");

    // 8) Store globally
    g_texture = tex;
    OutputDebugStringW(L"CT: Finished CreateGpuTexture\n");
}


// ------------------------------------------------
// Load an image from disk into g_pixels, g_imgW, g_imgH
bool LoadPhoto(const std::wstring& path) {
    // 1) Convert UTF-16 to UTF-8
    int len = WideCharToMultiByte(CP_UTF8, 0,
        path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        MessageBoxW(nullptr,
            L"Failed to convert file path to UTF-8",
            L"LoadImage Error",
            MB_OK | MB_ICONERROR);
        return false;
    }
    std::string u8(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0,
        path.c_str(), -1,
        &u8[0], len,
        nullptr, nullptr);

    // 2) Load with stb_image
    int channels = 0;
    unsigned char* data = stbi_load(
        u8.c_str(), &g_imgW, &g_imgH, &channels, 4);

    // Debug A2
    if (data) {
    } else {
        return false;
    }

    if (!data) {
        const char* err = stbi_failure_reason();
        if (!err) err = "Unknown error";
        // Convert err (UTF-8) to UTF-16 for MessageBoxW
        int wlen = MultiByteToWideChar(
            CP_UTF8, 0, err, -1, nullptr, 0
        );
        std::wstring werr(wlen, L'\0');
        MultiByteToWideChar(
            CP_UTF8, 0, err, -1,
            &werr[0], wlen
        );
        MessageBoxW(nullptr,
            werr.c_str(),
            L"LoadImage Error",
            MB_OK | MB_ICONERROR);
        return false;
    }


    // 3) Copy into the global buffer
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

void LoadCurrentImage()
{

    const auto& path = g_fileList[g_currentIndex];

    // load into CPU memory
    if (LoadPhoto(g_fileList[g_currentIndex])) {
        // upload to GPU & create SRV
        // Debug B1: LoadImage returned true

        // Call GPU upload
        CreateGpuTexture();

        // Debug B2: returned from CreateGpuTexture

        // reset pan/zoom so each image starts centered
        g_zoom       = g_targetZoom = 1.0f;
        g_offX       = g_targetOffX = 0.0f;
        g_offY       = g_targetOffY = 0.0f;
    }
    else {
        MessageBoxW(nullptr, L"Failed to load image", L"Error", MB_OK|MB_ICONERROR);
    }

    // Debug B1: LoadImage returned true

    // Call GPU upload
    CreateGpuTexture();

    // Debug B2: returned from CreateGpuTexture

    // 4) reset zoom/pan
    g_zoom       = g_targetZoom = 1.0f;
    g_offX       = g_targetOffX = 0.0f;
    g_offY       = g_targetOffY = 0.0f;
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
        // 1) current cursor in client
        POINT pt; GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);
        float ndcX = (pt.x/float(g_screenW))*2 - 1;
        float ndcY = 1 - (pt.y/float(g_screenH))*2;

        // 2) update g_targetZoom as before
        int notches = GET_WHEEL_DELTA_WPARAM(wP) / WHEEL_DELTA;
        const float zoomStep = 0.025f;
        float factorZ = powf(1.0f + zoomStep, float(notches));
        g_targetZoom = std::clamp(g_targetZoom * factorZ, 0.1f, 10.0f);

        // 3) perfect cursor‐centric pivot
        float oldZ = g_zoom;
        float newZ = g_targetZoom;
        float f    = newZ / oldZ;
        g_targetOffX = g_targetOffX * f + ndcX * (1.0f - f);
        g_targetOffY = g_targetOffY * f + ndcY * (1.0f - f);

        return 0;
    }

    case WM_KEYDOWN:
    {
        if (wP == VK_ESCAPE) {
            // Cleanly close the window / exit message loop
            PostQuitMessage(0);
            return 0;
        }
        if (wP == VK_LEFT || wP == VK_RIGHT) {
            // 3a) advance or rewind index
            if (wP == VK_LEFT && g_currentIndex > 0) {
                --g_currentIndex;
            } else if (wP == VK_RIGHT && g_currentIndex + 1 < (int)g_fileList.size()) {
                ++g_currentIndex;
            } else break;

            // 3b) load the newly selected image
            LoadCurrentImage();
            return 0;
        }
        break;
    }
    

    }
    return DefWindowProc(hWnd, msg, wP, lP);
}


// ------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
     #if defined(_DEBUG)
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
        }
    #endif

    // ─── A) Prompt & load on CPU ───
    OPENFILENAMEW ofn{};
    wchar_t szFile[MAX_PATH]{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Images\0*.jpg;*.png\0All Files\0*.*\0";
    ofn.lpstrFile   = szFile;
    ofn.nMaxFile    = MAX_PATH;
    if (GetOpenFileNameW(&ofn)) {
        if (!LoadPhoto(szFile)) {
            return 0;
        }
    } else {
        return 0;
    }

    // ─── B) Create your fullscreen window ───
    WNDCLASS wc{}; 
      wc.lpfnWndProc   = WndProc; 
      wc.hInstance     = hInst; 
      wc.lpszClassName = L"HDRViewerClass";
    RegisterClass(&wc);
    int screenW = GetSystemMetrics(SM_CXSCREEN),
        screenH = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowEx(
        WS_EX_APPWINDOW, L"HDRViewerClass", L"HDR Viewer",
        WS_POPUP, 0, 0, screenW, screenH,
        nullptr, nullptr, hInst, nullptr
    );
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // ─── C) D3D12 init ───
    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));

    ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device)));

    D3D12_COMMAND_QUEUE_DESC cqDesc{};
    ThrowIfFailed(g_device->CreateCommandQueue(&cqDesc, IID_PPV_ARGS(&g_cmdQueue)));

    // ─── D) Fence & event ───
    ThrowIfFailed(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)));
    g_fenceValue = 0;
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent) {
        return 0;
    }

    // ─── E) Swap chain & RTV heap ───
    DXGI_SWAP_CHAIN_DESC1 scd{};
      scd.BufferCount      = FrameCount;
      scd.Width            = screenW;
      scd.Height           = screenH;
      scd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
      scd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      scd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      scd.SampleDesc.Count = 1;
    ComPtr<IDXGISwapChain1> sc1;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(
        g_cmdQueue.Get(), hwnd, &scd, nullptr, nullptr, &sc1
    ));
    ThrowIfFailed(sc1.As(&g_swapChain));
    factory->MakeWindowAssociation(hwnd, 0);
    ThrowIfFailed(g_swapChain->SetFullscreenState(TRUE, nullptr));
    ThrowIfFailed(g_swapChain->ResizeBuffers(
        FrameCount, screenW, screenH, scd.Format,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH
    ));
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();

    {
      D3D12_DESCRIPTOR_HEAP_DESC hd{};
      hd.NumDescriptors = FrameCount;
      hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
      ThrowIfFailed(g_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rtvHeap)));
      g_rtvDescSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

      auto handle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
      for (UINT i = 0; i < FrameCount; ++i) {
          ComPtr<ID3D12Resource> buf;
          ThrowIfFailed(g_swapChain->GetBuffer(i, IID_PPV_ARGS(&buf)));
          g_renderTargets[i] = buf;
          g_device->CreateRenderTargetView(buf.Get(), nullptr, handle);
          handle.ptr += g_rtvDescSize;
      }
    }

    // ─── F) SRV heap ───
    {
        D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
        srvDesc.NumDescriptors = 1;
        srvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap)));
    }
    

    // ─── 9b) Create CBV/SRV/UAV heap + upload-heap constant buffer for scaleX/Y ───
    // 9b.1) CBV/SRV/UAV heap
    D3D12_DESCRIPTOR_HEAP_DESC cbvSrvDesc{};
    cbvSrvDesc.NumDescriptors = 1;  // one CBV
    cbvSrvDesc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbvSrvDesc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(g_device->CreateDescriptorHeap(&cbvSrvDesc, IID_PPV_ARGS(&g_cbvSrvUavHeap)));

    // 9b.2) Upload-heap buffer (must be 256-byte aligned)
    D3D12_HEAP_PROPERTIES heapProps    = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC    bufDesc      = CD3DX12_RESOURCE_DESC::Buffer((sizeof(float)*2 + 255) & ~255);
    ThrowIfFailed(g_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&g_scaleBuffer)
    ));

    // 9b.3) Fill it with your computed g_scaleX/g_scaleY
    float* mapped = nullptr;
    ThrowIfFailed(g_scaleBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    mapped[0] = g_baseScaleX;
    mapped[1] = g_baseScaleY;
    g_scaleBuffer->Unmap(0, nullptr);

    // 9b.4) Create the CBV in descriptor-heap slot 0
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
    cbvDesc.BufferLocation = g_scaleBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes    = (sizeof(float)*2 + 255) & ~255;  // 256-byte aligned
    CD3DX12_CPU_DESCRIPTOR_HANDLE cbvHandle(
        g_cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart()
    );
    g_device->CreateConstantBufferView(&cbvDesc, cbvHandle);

    // ─── G) CreateGpuTexture ───
    CreateGpuTexture();

    // ——— X) Create root signature with SRV(t0) + CBV(b0) ———
    {
        // 1) Descriptor Table for t0 (texture SRV)
        D3D12_DESCRIPTOR_RANGE srvRange = {};
        srvRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors                    = 1;
        srvRange.BaseShaderRegister                = 0;
        srvRange.RegisterSpace                     = 0;
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER srvParam = {};
        srvParam.ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        srvParam.DescriptorTable.NumDescriptorRanges = 1;
        srvParam.DescriptorTable.pDescriptorRanges   = &srvRange;
        srvParam.ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        // 2) CBV for your scale constants (register b0)
        D3D12_ROOT_PARAMETER cbvParam = {};
        cbvParam.ParameterType                 = D3D12_ROOT_PARAMETER_TYPE_CBV;
        cbvParam.Descriptor.ShaderRegister     = 0;  // b0
        cbvParam.Descriptor.RegisterSpace      = 0;
        cbvParam.ShaderVisibility              = D3D12_SHADER_VISIBILITY_VERTEX;

        // 3) A static sampler for the pixel shader
        D3D12_STATIC_SAMPLER_DESC sampDesc{};
        sampDesc.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampDesc.AddressU       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampDesc.AddressV       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampDesc.AddressW       = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampDesc.ShaderRegister = 0;
        sampDesc.RegisterSpace  = 0;
        sampDesc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // 4) Pack into array
        D3D12_ROOT_PARAMETER params[2] = { srvParam, cbvParam };

        // 5) Build & serialize
        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters     = 2;
        rsDesc.pParameters       = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers   = &sampDesc;
        rsDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        // Then serialize & create as before:
        ComPtr<ID3DBlob> rsBlob, errBlob;
        ThrowIfFailed(D3D12SerializeRootSignature(
            &rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &errBlob));
        ThrowIfFailed(g_device->CreateRootSignature(
            0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
            IID_PPV_ARGS(&g_rootSig)));
    }

    // ——— Compile & create PSO ———
    {
        ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;

        // 1) Compile the vertex shader
        ThrowIfFailed(D3DCompile(
            g_VS, strlen(g_VS),
            nullptr, nullptr, nullptr,
            "VSMain", "vs_5_0",
            0, 0,
            &vsBlob, &errBlob
        ));

        // 2) Compile the pixel shader
        ThrowIfFailed(D3DCompile(
            g_PS, strlen(g_PS),
            nullptr, nullptr, nullptr,
            "PSMain", "ps_5_0",
            0, 0,
            &psBlob, &errBlob
        ));

        // 3) Describe the pipeline
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature        = g_rootSig.Get();
        psoDesc.VS                    = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        psoDesc.PS                    = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };

        // default blend & rasterizer from the helper
        psoDesc.BlendState            = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.RasterizerState       = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);

        // use a *valid* default depth/stencil description…
        psoDesc.DepthStencilState     = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        // …then just turn off both depth & stencil
        psoDesc.DepthStencilState.DepthEnable   = FALSE;
        psoDesc.DepthStencilState.StencilEnable = FALSE;

        psoDesc.SampleMask            = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets      = 1;
        psoDesc.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.SampleDesc.Count      = 1;

        // no vertex buffers (we use SV_VertexID)
        psoDesc.InputLayout           = { nullptr, 0 };

        // ——— H) Create + fill upload‐heap CBV for scaleX/scaleY ———
        {
            // 1) Describe & allocate the buffer (256-byte aligned)
            D3D12_HEAP_PROPERTIES heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            auto cbSize = (sizeof(float)*2 + 255) & ~255;  
            D3D12_RESOURCE_DESC bufDesc = CD3DX12_RESOURCE_DESC::Buffer(cbSize);
            ThrowIfFailed(g_device->CreateCommittedResource(
                &heapProps,
                D3D12_HEAP_FLAG_NONE,
                &bufDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                IID_PPV_ARGS(&g_scaleBuffer)
            ));

            // 2) Map & write your scales (you should have computed g_baseScaleX/Y after LoadImage)
            float* mapped = nullptr;
            ThrowIfFailed(g_scaleBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
            mapped[0] = g_baseScaleX;
            mapped[1] = g_baseScaleY;
            g_scaleBuffer->Unmap(0, nullptr);

            // 3) Create the CBV descriptor in slot 0 of the CBV/SRV/UAV heap
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc{};
            cbvDesc.BufferLocation = g_scaleBuffer->GetGPUVirtualAddress();
            cbvDesc.SizeInBytes    = cbSize;
            CD3DX12_CPU_DESCRIPTOR_HANDLE handle(
                g_cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart()
            );
            g_device->CreateConstantBufferView(&cbvDesc, handle);
        }

        // 4) Create it
        HRESULT hr = g_device->CreateGraphicsPipelineState(
            &psoDesc,
            IID_PPV_ARGS(&g_pipelineState)
        );
        if (FAILED(hr)) {
            wchar_t buf[128];
            swprintf_s(buf, L"PSO creation failed: 0x%08X", hr);
            MessageBoxW(nullptr, buf, L"PSO Error", MB_OK|MB_ICONERROR);
            return 0;
        }
    }


        // 9) Main loop: clear & present, instrumented
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            // 1) Allocate a command list for this frame
            ComPtr<ID3D12CommandAllocator> alloc;
            ThrowIfFailed(g_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(&alloc)
            ));
            ComPtr<ID3D12GraphicsCommandList> cl;
            ThrowIfFailed(g_device->CreateCommandList(
                0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                alloc.Get(), nullptr,
                IID_PPV_ARGS(&cl)
            ));

            // 2) Transition the back-buffer INTO RENDER_TARGET
            cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
                g_renderTargets[g_frameIndex].Get(),
                D3D12_RESOURCE_STATE_PRESENT,
                D3D12_RESOURCE_STATE_RENDER_TARGET
            ));

            // 3) Bind & clear the RTV
            CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
                g_rtvHeap->GetCPUDescriptorHandleForHeapStart(),
                g_frameIndex, g_rtvDescSize
            );
            const FLOAT clearCol[4] = { 0.05f, 0.1f, 0.15f, 1.0f };
            cl->ClearRenderTargetView(rtv, clearCol, 0, nullptr);

            // 4) Viewport + scissor
            D3D12_VIEWPORT vp{ 0, 0, float(g_screenW), float(g_screenH), 0, 1 };
            D3D12_RECT   sc{ 0, 0, g_screenW, g_screenH };
            cl->RSSetViewports(1, &vp);
            cl->RSSetScissorRects(1, &sc);

            // 5) Bind root signature, PSO, descriptor heap, draw
            cl->SetGraphicsRootSignature(g_rootSig.Get());
            cl->SetPipelineState(g_pipelineState.Get());
            ID3D12DescriptorHeap* dh[] = { g_srvHeap.Get() };
            cl->SetDescriptorHeaps(1, dh);
            cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            // slot 0 = your texture SRV:
            cl->SetGraphicsRootDescriptorTable(
                0,
                g_srvHeap->GetGPUDescriptorHandleForHeapStart()
            );
            // slot 1 = your CBV (if you have one; otherwise skip this):
            // cl->SetGraphicsRootConstantBufferView(1, g_yourCB->GetGPUVirtualAddress());

            cl->DrawInstanced(3, 1, 0, 0);

            // 6) Transition back INTO PRESENT
            cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
                g_renderTargets[g_frameIndex].Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PRESENT
            ));

            // 7) Execute & present
            ThrowIfFailed(cl->Close());
            ID3D12CommandList* lists[] = { cl.Get() };
            g_cmdQueue->ExecuteCommandLists(1, lists);
            g_swapChain->Present(1, 0);

            // 8) Advance to next buffer
            g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
        }
    }

    MessageBoxW(nullptr, L"LR: Exiting", L"Debug", MB_OK | MB_SYSTEMMODAL); 

    return 0;
}




