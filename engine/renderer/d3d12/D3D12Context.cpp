#ifdef _WIN32

#include "renderer/d3d12/D3D12Context.h"
#include "renderer/Buffer.h"
#include "renderer/MeshData.h"
#include "renderer/MeshLibrary.h"
#include "asset/ShaderBundle.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "scene/Node.h"
#include <SDL.h>
#include <SDL_syswm.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <stb_image_write.h>
#include <stdexcept>
#include <iostream>
#include <cstring>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace QymEngine {

void D3D12Context::init(SDL_Window* window)
{
    SDL_GetWindowSize(window, (int*)&m_width, (int*)&m_height);
    createDevice();
    createCommandQueue();
    createSwapChain(window);
    createRtvHeap();
    createDepthBuffer();
    createCommandAllocatorsAndList();
    createFence();
    createRootSignature();
    createPipelineState();
    createConstantBuffer();
    createMeshes();
    std::cout << "[D3D12] Initialized (" << m_width << "x" << m_height << ")" << std::endl;
}

void D3D12Context::shutdown()
{
    waitForGpu();
    if (m_cbvMapped) { m_constantBuffer->Unmap(0, nullptr); m_cbvMapped = nullptr; }
    if (m_fenceEvent) CloseHandle(m_fenceEvent);
    std::cout << "[D3D12] Shutdown" << std::endl;
}

// ========================================================================
// Device / SwapChain / RTV / DSV / CommandList / Fence
// ========================================================================

void D3D12Context::createDevice()
{
    UINT dxgiFlags = 0;
#ifdef _DEBUG
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
            std::cout << "[D3D12] Debug layer enabled" << std::endl;
        }
    }
#endif
    if (FAILED(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&m_factory))))
        throw std::runtime_error("[D3D12] CreateDXGIFactory2 failed");

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; m_factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)))) {
            char name[256];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, 256, nullptr, nullptr);
            std::cout << "[D3D12] Adapter: " << name << std::endl;
            return;
        }
    }
    throw std::runtime_error("[D3D12] No suitable GPU");
}

void D3D12Context::createCommandQueue()
{
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    m_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_commandQueue));
}

void D3D12Context::createSwapChain(SDL_Window* window)
{
    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    SDL_GetWindowWMInfo(window, &wm);

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = m_width; desc.Height = m_height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = FRAME_COUNT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> sc1;
    m_factory->CreateSwapChainForHwnd(m_commandQueue.Get(), wm.info.win.window,
                                      &desc, nullptr, nullptr, &sc1);
    m_factory->MakeWindowAssociation(wm.info.win.window, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&m_swapChain);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
}

void D3D12Context::createRtvHeap()
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = FRAME_COUNT;
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_rtvHeap));
    m_rtvDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    auto handle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FRAME_COUNT; i++) {
        m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_renderTargets[i]));
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, handle);
        handle.ptr += m_rtvDescriptorSize;
    }
}

void D3D12Context::createDepthBuffer()
{
    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc = {};
    dsvDesc.NumDescriptors = 1;
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    m_device->CreateDescriptorHeap(&dsvDesc, IID_PPV_ARGS(&m_dsvHeap));

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = m_width; rd.Height = m_height;
    rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_D32_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&m_depthBuffer));
    m_device->CreateDepthStencilView(m_depthBuffer.Get(), nullptr,
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
}

void D3D12Context::createCommandAllocatorsAndList()
{
    for (UINT i = 0; i < FRAME_COUNT; i++)
        m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&m_commandAllocators[i]));
    m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocators[0].Get(), nullptr, IID_PPV_ARGS(&m_commandList));
    m_commandList->Close();
}

void D3D12Context::createFence()
{
    m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    m_fenceValues[m_frameIndex] = 1;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

void D3D12Context::waitForGpu()
{
    uint64_t v = m_fenceValues[m_frameIndex];
    m_commandQueue->Signal(m_fence.Get(), v);
    if (m_fence->GetCompletedValue() < v) {
        m_fence->SetEventOnCompletion(v, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    m_fenceValues[m_frameIndex]++;
}

// ========================================================================
// Pipeline: Root Signature + PSO from DXIL ShaderBundle
// ========================================================================

void D3D12Context::createRootSignature()
{
    // Slang DXIL register assignments (from slangc -target hlsl):
    //   b0: FrameData (CBV)
    //   b1: materialParams (CBV)
    //   b2: PushConstants (CBV, Slang maps push_constant to CBV)
    //   t0/s0: shadowMap
    //   t1/s1: albedoMap
    //   t2/s2: normalMap

    // Descriptor table for SRVs (t0-t2)
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 3;    // t0, t1, t2
    srvRange.BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER rootParams[4] = {};

    // [0] CBV: FrameData (b0) - root CBV
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [1] CBV: materialParams (b1) - root CBV
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [2] Root constants: PushConstants (b2, 20 x 32-bit = 80 bytes)
    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rootParams[2].Constants.ShaderRegister = 2;
    rootParams[2].Constants.Num32BitValues = sizeof(PushConstantData) / 4;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [3] Descriptor table: SRVs (t0-t2)
    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[3].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static samplers (s0, s1, s2)
    D3D12_STATIC_SAMPLER_DESC samplers[3] = {};
    for (int i = 0; i < 3; i++) {
        samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[i].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    }

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = _countof(rootParams);
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 3;
    rsDesc.pStaticSamplers = samplers;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> sig, err;
    if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
        if (err) std::cerr << "[D3D12] RootSig: " << (char*)err->GetBufferPointer() << std::endl;
        throw std::runtime_error("[D3D12] Failed to serialize root signature");
    }
    m_device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                   IID_PPV_ARGS(&m_rootSignature));
    std::cout << "[D3D12] Root signature created" << std::endl;
}

void D3D12Context::createPipelineState()
{
    // Load DXIL from Triangle.shaderbundle
    ShaderBundle bundle;
    std::string bundlePath = std::string(ASSETS_DIR) + "/shaders/Triangle.shaderbundle";
    if (!bundle.load(bundlePath) || !bundle.hasVariant("default_dxil"))
        throw std::runtime_error("[D3D12] Failed to load Triangle.shaderbundle default_dxil");

    m_vertexShaderDxil = bundle.getVertSpv("default_dxil");
    m_pixelShaderDxil = bundle.getFragSpv("default_dxil");
    std::cout << "[D3D12] Loaded DXIL: VS=" << m_vertexShaderDxil.size()
              << "B PS=" << m_pixelShaderDxil.size() << "B" << std::endl;

    // Input layout matching engine's Vertex struct (pos + color + texCoord + normal)
    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION",  0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD",  0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",    0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_rootSignature.Get();
    pso.VS = {m_vertexShaderDxil.data(), m_vertexShaderDxil.size()};
    pso.PS = {m_pixelShaderDxil.data(), m_pixelShaderDxil.size()};
    pso.InputLayout = {inputLayout, _countof(inputLayout)};
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pso.SampleDesc.Count = 1;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthEnable = TRUE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;

    HRESULT hr = m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pipelineState));
    if (FAILED(hr))
        throw std::runtime_error("[D3D12] Failed to create PSO (HRESULT=" + std::to_string(hr) + ")");
    std::cout << "[D3D12] PSO created from DXIL" << std::endl;
}

void D3D12Context::createConstantBuffer()
{
    // CBV/SRV heap: 3 SRVs (shadowMap t0, albedoMap t1, normalMap t2)
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 3;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_cbvSrvHeap));

    auto createUploadBuffer = [&](UINT size, ComPtr<ID3D12Resource>& resource) {
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (size + 255) & ~255; rd.Height = 1; rd.DepthOrArraySize = 1;
        rd.MipLevels = 1; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&resource));
    };

    // FrameData CBV (b0)
    createUploadBuffer(sizeof(UniformBufferObject), m_constantBuffer);
    m_constantBuffer->Map(0, nullptr, &m_cbvMapped);

    // MaterialParams CBV (b1) - default white material
    createUploadBuffer(256, m_materialBuffer);
    {
        struct MatParams { float baseColor[4]; float metallic; float roughness; };
        MatParams mat = {{1, 1, 1, 1}, 0.0f, 0.5f};
        void* mapped = nullptr;
        m_materialBuffer->Map(0, nullptr, &mapped);
        memcpy(mapped, &mat, sizeof(mat));
        m_materialBuffer->Unmap(0, nullptr);
    }

    // Dummy 1x1 white texture for SRV slots (t0, t1, t2)
    {
        D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 1; td.Height = 1; td.DepthOrArraySize = 1;
        td.MipLevels = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_dummyTexture));

        // Upload white pixel (0xFFFFFFFF)
        {
            uint32_t whitePixel = 0xFFFFFFFF;
            D3D12_HEAP_PROPERTIES uhp = {}; uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC ubd = {};
            ubd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            ubd.Width = 256; // min texture pitch
            ubd.Height = 1; ubd.DepthOrArraySize = 1; ubd.MipLevels = 1;
            ubd.SampleDesc.Count = 1; ubd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> uploadBuf;
            m_device->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &ubd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuf));
            void* mapped; uploadBuf->Map(0, nullptr, &mapped);
            memcpy(mapped, &whitePixel, 4);
            uploadBuf->Unmap(0, nullptr);

            // Copy using a temporary command list
            m_commandAllocators[0]->Reset();
            m_commandList->Reset(m_commandAllocators[0].Get(), nullptr);
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = m_dummyTexture.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = uploadBuf.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            src.PlacedFootprint.Footprint.Width = 1;
            src.PlacedFootprint.Footprint.Height = 1;
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = 256;
            m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = m_dummyTexture.Get();
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            m_commandList->ResourceBarrier(1, &b);
            m_commandList->Close();

            ID3D12CommandList* lists[] = {m_commandList.Get()};
            m_commandQueue->ExecuteCommandLists(1, lists);
            waitForGpu();
        }

        // Create SRV for each slot
        UINT srvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto handle = m_cbvSrvHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        for (int i = 0; i < 3; i++) {
            m_device->CreateShaderResourceView(m_dummyTexture.Get(), &srvDesc, handle);
            handle.ptr += srvSize;
        }
    }
}

void D3D12Context::createMeshes()
{
    auto uploadMesh = [&](MeshType type,
                          const std::vector<Vertex>& verts,
                          const std::vector<uint32_t>& indices) {
        D3D12Mesh mesh;
        mesh.indexCount = static_cast<uint32_t>(indices.size());

        auto upload = [&](const void* data, UINT size, ComPtr<ID3D12Resource>& res) {
            D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1;
            rd.MipLevels = 1; rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res));
            void* mapped; res->Map(0, nullptr, &mapped);
            memcpy(mapped, data, size);
            res->Unmap(0, nullptr);
        };

        UINT vbSize = static_cast<UINT>(verts.size() * sizeof(Vertex));
        UINT ibSize = static_cast<UINT>(indices.size() * sizeof(uint32_t));
        upload(verts.data(), vbSize, mesh.vertexBuffer);
        upload(indices.data(), ibSize, mesh.indexBuffer);

        mesh.vbView.BufferLocation = mesh.vertexBuffer->GetGPUVirtualAddress();
        mesh.vbView.StrideInBytes = sizeof(Vertex);
        mesh.vbView.SizeInBytes = vbSize;
        mesh.ibView.BufferLocation = mesh.indexBuffer->GetGPUVirtualAddress();
        mesh.ibView.Format = DXGI_FORMAT_R32_UINT;
        mesh.ibView.SizeInBytes = ibSize;

        m_meshes[static_cast<int>(type)] = std::move(mesh);
    };

    auto gen = [&](MeshType type, auto genFn) {
        std::vector<Vertex> v; std::vector<uint32_t> i;
        genFn(v, i);
        uploadMesh(type, v, i);
    };

    gen(MeshType::Quad,   generateQuad);
    gen(MeshType::Cube,   generateCube);
    gen(MeshType::Plane,  generatePlane);
    gen(MeshType::Sphere, generateSphere);
}

// ========================================================================
// Render Frame
// ========================================================================

void D3D12Context::renderFrame(Scene* scene, Camera* camera)
{
    float aspect = m_width / (float)m_height;
    UniformBufferObject ubo = {};

    // Camera VP (D3D12 不需要 Vulkan 的 Y-flip)
    if (camera) {
        ubo.view = glm::transpose(camera->getViewMatrix());
        // 手动构建投影矩阵，不用 Camera::getProjMatrix 的 Y-flip
        glm::mat4 proj = glm::perspective(glm::radians(camera->fov), aspect,
                                           camera->nearPlane, camera->farPlane);
        ubo.proj = glm::transpose(proj);
        ubo.cameraPos = camera->getPosition();
    } else {
        glm::vec3 eye(2.0f, 2.0f, 3.0f);
        ubo.view = glm::transpose(glm::lookAt(eye, glm::vec3(0), glm::vec3(0,1,0)));
        ubo.proj = glm::transpose(glm::perspective(glm::radians(45.0f), aspect, 0.1f, 100.0f));
        ubo.cameraPos = eye;
    }
    ubo.ambientColor = glm::vec3(0.15f);

    // Collect lights from scene
    int lightCount = 0;
    if (scene) {
        scene->traverseNodes([&](Node* node) {
            if (lightCount >= MAX_LIGHTS || !node->isLight()) return;
            LightData& ld = ubo.lights[lightCount];
            glm::vec3 pos = glm::vec3(node->getWorldMatrix()[3]);
            glm::vec3 dir = node->getLightDirection();
            glm::vec3 color = node->lightColor * node->lightIntensity;
            if (node->nodeType == NodeType::DirectionalLight) {
                ld.positionAndType = glm::vec4(0, 0, 0, float(LIGHT_TYPE_DIRECTIONAL));
                ld.directionAndRange = glm::vec4(dir, 0);
            } else if (node->nodeType == NodeType::PointLight) {
                ld.positionAndType = glm::vec4(pos, float(LIGHT_TYPE_POINT));
                ld.directionAndRange = glm::vec4(0, 0, 0, node->lightRange);
            } else if (node->nodeType == NodeType::SpotLight) {
                ld.positionAndType = glm::vec4(pos, float(LIGHT_TYPE_SPOT));
                ld.directionAndRange = glm::vec4(dir, node->lightRange);
                ld.spotParams = glm::vec4(glm::cos(glm::radians(node->spotInnerAngle)),
                                          glm::cos(glm::radians(node->spotOuterAngle)), 0, 0);
            }
            ld.colorAndIntensity = glm::vec4(color, node->lightIntensity);
            lightCount++;
        });
    }
    if (lightCount == 0) {
        ubo.lights[0].positionAndType = glm::vec4(0, 0, 0, 0);
        ubo.lights[0].directionAndRange = glm::vec4(glm::normalize(glm::vec3(-0.5f, -1.0f, -0.3f)), 0);
        ubo.lights[0].colorAndIntensity = glm::vec4(1, 1, 1, 1);
        lightCount = 1;
    }
    ubo.lightCountPad = glm::ivec4(lightCount, 0, 0, 0);
    memcpy(m_cbvMapped, &ubo, sizeof(ubo));

    auto* cmd = m_commandList.Get();
    m_commandAllocators[m_frameIndex]->Reset();
    cmd->Reset(m_commandAllocators[m_frameIndex].Get(), m_pipelineState.Get());

    // Transition: PRESENT -> RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_renderTargets[m_frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);

    auto rtvHandle = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += m_frameIndex * m_rtvDescriptorSize;
    auto dsvHandle = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    cmd->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    float clearColor[4] = {0.4f, 0.45f, 0.5f, 1.0f};
    cmd->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    cmd->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    cmd->SetGraphicsRootSignature(m_rootSignature.Get());
    cmd->SetGraphicsRootConstantBufferView(0, m_constantBuffer->GetGPUVirtualAddress());
    cmd->SetGraphicsRootConstantBufferView(1, m_materialBuffer->GetGPUVirtualAddress());
    ID3D12DescriptorHeap* heaps[] = {m_cbvSrvHeap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootDescriptorTable(3, m_cbvSrvHeap->GetGPUDescriptorHandleForHeapStart());

    D3D12_VIEWPORT vp = {0, 0, (float)m_width, (float)m_height, 0, 1};
    D3D12_RECT scissor = {0, 0, (LONG)m_width, (LONG)m_height};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &scissor);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Draw scene nodes
    auto drawNode = [&](Node* node) {
        if (node->isLight()) return;
        if (node->meshType == MeshType::None && node->meshPath.empty()) return;
        // Only built-in meshes for now (no .obj loading in D3D12 yet)
        if (node->meshType == MeshType::None) return;

        auto it = m_meshes.find(static_cast<int>(node->meshType));
        if (it == m_meshes.end()) return;
        auto& mesh = it->second;

        PushConstantData pc = {};
        pc.model = glm::transpose(node->getWorldMatrix());
        pc.highlighted = 0;
        cmd->SetGraphicsRoot32BitConstants(2, sizeof(PushConstantData) / 4, &pc, 0);

        cmd->IASetVertexBuffers(0, 1, &mesh.vbView);
        cmd->IASetIndexBuffer(&mesh.ibView);
        cmd->DrawIndexedInstanced(mesh.indexCount, 1, 0, 0, 0);
    };

    if (scene) {
        scene->traverseNodes(drawNode);
    } else {
        // Fallback: draw a single cube
        auto it = m_meshes.find(static_cast<int>(MeshType::Cube));
        if (it != m_meshes.end()) {
            PushConstantData pc = {};
            pc.model = glm::transpose(glm::mat4(1.0f));
            cmd->SetGraphicsRoot32BitConstants(2, sizeof(PushConstantData) / 4, &pc, 0);
            cmd->IASetVertexBuffers(0, 1, &it->second.vbView);
            cmd->IASetIndexBuffer(&it->second.ibView);
            cmd->DrawIndexedInstanced(it->second.indexCount, 1, 0, 0, 0);
        }
    }

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmd->ResourceBarrier(1, &barrier);
    cmd->Close();

    ID3D12CommandList* lists[] = {cmd};
    m_commandQueue->ExecuteCommandLists(1, lists);
    m_swapChain->Present(1, 0);

    uint64_t cv = m_fenceValues[m_frameIndex];
    m_commandQueue->Signal(m_fence.Get(), cv);
    m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();
    if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex]) {
        m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
    m_fenceValues[m_frameIndex] = cv + 1;
}

bool D3D12Context::saveScreenshot(const std::string& path)
{
    waitForGpu();

    auto* rt = m_renderTargets[m_frameIndex].Get();

    // Readback buffer
    UINT64 rowPitch = (m_width * 4 + 255) & ~255u; // D3D12 要求 256 对齐
    UINT64 bufSize = rowPitch * m_height;

    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bufSize; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> readback;
    m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));

    // Copy render target → readback
    m_commandAllocators[m_frameIndex]->Reset();
    m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rt;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_commandList->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = m_width;
    dst.PlacedFootprint.Footprint.Height = m_height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = rt;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

    m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_commandList->ResourceBarrier(1, &barrier);
    m_commandList->Close();

    ID3D12CommandList* lists[] = {m_commandList.Get()};
    m_commandQueue->ExecuteCommandLists(1, lists);
    waitForGpu();

    // Read pixels and write PNG
    void* mapped = nullptr;
    readback->Map(0, nullptr, &mapped);

    // stbi_write_png (stb already linked)
    // Convert from row-pitch-aligned to tight RGBA
    std::vector<uint8_t> pixels(m_width * m_height * 4);
    for (uint32_t y = 0; y < m_height; y++) {
        memcpy(pixels.data() + y * m_width * 4,
               (uint8_t*)mapped + y * rowPitch, m_width * 4);
    }
    readback->Unmap(0, nullptr);

    int ok = stbi_write_png(path.c_str(), m_width, m_height, 4, pixels.data(), m_width * 4);
    if (ok) std::cout << "[D3D12] Screenshot: " << path << std::endl;
    return ok != 0;
}

} // namespace QymEngine

#endif // _WIN32
