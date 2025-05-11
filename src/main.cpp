// src/main.cpp
#include <windows.h>
#include <wrl.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <d3dx12.h>       // from third_party/d3dx12/d3dx12.h
#include <commdlg.h>      // GetOpenFileNameW
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>
#include <thread>


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
// Our loaded image
ComPtr<ID3D12Resource>       g_texture;
ComPtr<ID3D12Fence>          g_fence;
UINT                         g_rtvDescSize;
UINT                         g_frameIndex = 0;
UINT64                       g_fenceValue = 0;
HANDLE                       g_fenceEvent = nullptr;



// Image data globals
std::vector<uint8_t>         g_pixels;
int                          g_imgW = 0, g_imgH = 0;

// Full‐screen triangle shaders (will draw your image later)
static const char* g_VS = R"(
cbuffer ScaleCB : register(b0)
{
    float scaleX;
    float scaleY;
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    // 4 corners of a rectangle centered at (0,0) scaled by scaleX/scaleY
    float2 quadPos[4] = {
        float2(-scaleX, -scaleY),  // bottom-left
        float2( scaleX, -scaleY),  // bottom-right
        float2(-scaleX,  scaleY),  // top-left
        float2( scaleX,  scaleY)   // top-right
    };
    // Corresponding UVs
    float2 quadUV[4] = {
        float2(0, 1),
        float2(1, 1),
        float2(0, 0),
        float2(1, 0)
    };

    VSOut o;
    o.pos = float4(quadPos[vid], 0, 1);
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


// Forward‐declare Win32 window proc
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// ------------------------------------------------
// Load an image from disk into g_pixels, g_imgW, g_imgH
bool LoadImage(const std::wstring& path) {
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


// ------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    // 1) File-open dialog
    OPENFILENAMEW ofn{};
    wchar_t szFile[MAX_PATH]{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Images\0*.jpg;*.png\0All Files\0*.*\0";
    ofn.lpstrFile   = szFile;
    ofn.nMaxFile    = MAX_PATH;
    if (GetOpenFileNameW(&ofn)) {
        bool ok = LoadImage(szFile);
        MessageBoxW(nullptr,
            ok ? L"LoadImage returned true" : L"LoadImage returned false",
            L"Debug", MB_OK);
    }

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);

    // Compute letter-box scales:
    float imgAspect    = float(g_imgW) / float(g_imgH);
    float screenAspect = float(screenW) / float(screenH);
    float scaleX = 1, scaleY = 1;
    if (imgAspect > screenAspect) {
        // image is wider → pillar‐box
        scaleY = screenAspect / imgAspect;
    } else {
        // image is taller → letter‐box
        scaleX = imgAspect / screenAspect;
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
    ThrowIfFailed(g_swapChain->SetFullscreenState(TRUE, nullptr));

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
        scaleParam.Constants.Num32BitValues         = 2;
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


    // 11) Compile & create PSO (with explicit states + error check)
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




    // ——— 8) Create and upload the texture (with debug) ———
    if (!g_pixels.empty()) {

        // Describe the texture
        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width            = UINT64(g_imgW);
        texDesc.Height           = UINT(g_imgH);
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels        = 1;
        texDesc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;

        // 8.2: Create default‐heap texture
        ComPtr<ID3D12Resource> tex;
        ThrowIfFailed(g_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&tex)
        ));

        // 8.3: Create upload‐heap
        UINT64 uploadSize = GetRequiredIntermediateSize(tex.Get(), 0, 1);
        ComPtr<ID3D12Resource> uploadHeap;
        ThrowIfFailed(g_device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(uploadSize),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&uploadHeap)
        ));

        // 8.4: Prepare subresource data
        D3D12_SUBRESOURCE_DATA sub = {};
        sub.pData      = g_pixels.data();
        sub.RowPitch   = SIZE_T(g_imgW) * 4;
        sub.SlicePitch = sub.RowPitch * g_imgH;

        // 8.5: Record copy & barrier
        ComPtr<ID3D12CommandAllocator> tmpAlloc;
        ThrowIfFailed(g_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&tmpAlloc)
        ));
        ComPtr<ID3D12GraphicsCommandList> tmpList;
        ThrowIfFailed(g_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            tmpAlloc.Get(), nullptr,
            IID_PPV_ARGS(&tmpList)
        ));
        UpdateSubresources(tmpList.Get(), tex.Get(), uploadHeap.Get(),
                        0, 0, 1, &sub);

        tmpList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
            tex.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
        ));

        ThrowIfFailed(tmpList->Close());

        // 8.7: Execute & fence‐sync
        g_cmdQueue->ExecuteCommandLists(1,
            reinterpret_cast<ID3D12CommandList* const*>(tmpList.GetAddressOf())
        );

        // fence
        g_fenceValue++;
        ThrowIfFailed(g_cmdQueue->Signal(g_fence.Get(), g_fenceValue));
        ThrowIfFailed(g_fence->SetEventOnCompletion(g_fenceValue, g_fenceEvent));
        WaitForSingleObject(g_fenceEvent, INFINITE);

        // 8.9: Create the SRV
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format                  = texDesc.Format;
        srvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels     = 1;
        g_device->CreateShaderResourceView(
            tex.Get(), &srvDesc,
            g_srvHeap->GetCPUDescriptorHandleForHeapStart()
        );

        // 8.10: Store in global
        g_texture = tex;
    }




    // 9) Main loop: clear & present
    MSG msg{};
    while (msg.message != WM_QUIT) {
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

            // slot 1 → two 32-bit constants (scaleX, scaleY)
            float scaleVals[2] = { scaleX, scaleY };
            cl->SetGraphicsRoot32BitConstants(1, 2, scaleVals, 0);

            // draw full-screen triangle
            cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            cl->DrawInstanced(4, 1, 0, 0);

            // 8) Transition back into PRESENT
            cl->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(
                g_renderTargets[g_frameIndex].Get(),
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PRESENT
            ));

            // 9) Execute & present
            using clock = std::chrono::high_resolution_clock;
            static auto lastFrame = clock::now();

            // close + submit
            cl->Close();
            ID3D12CommandList* lists[] = { cl.Get() };
            g_cmdQueue->ExecuteCommandLists(_countof(lists), lists);

            // present (still v-sync once, so FrameTime ≥ display interval)
            g_swapChain->Present(1, 0);

            // frame-timing
            auto now      = clock::now();
            auto elapsed  = std::chrono::duration<float, std::milli>(now - lastFrame).count();
            auto desired  = 1000.0f / 60.0f;   // ~16.6667 ms
            if (elapsed < desired) {
                std::this_thread::sleep_for(std::chrono::milliseconds(long(desired - elapsed)));
            }
            lastFrame = clock::now();

            // advance to next buffer
            g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
        }
    }

    return 0;
}

// Window procedure
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg,
                         WPARAM wP, LPARAM lP)
{
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wP, lP);
}
