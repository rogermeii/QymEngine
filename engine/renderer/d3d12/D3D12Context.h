#pragma once

#ifdef _WIN32
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

struct SDL_Window;

namespace QymEngine {

using Microsoft::WRL::ComPtr;
struct UniformBufferObject;
class Scene;
class Camera;
enum class MeshType;

class D3D12Context {
public:
    static constexpr uint32_t FRAME_COUNT = 2;

    void init(SDL_Window* window);
    void shutdown();
    void renderFrame(Scene* scene = nullptr, Camera* camera = nullptr);
    bool saveScreenshot(const std::string& path);

    ID3D12Device* getDevice() const { return m_device.Get(); }
    uint32_t getWidth() const { return m_width; }
    uint32_t getHeight() const { return m_height; }

private:
    // Initialization
    void createDevice();
    void createCommandQueue();
    void createSwapChain(SDL_Window* window);
    void createRtvHeap();
    void createDepthBuffer();
    void createCommandAllocatorsAndList();
    void createFence();
    void waitForGpu();

    // Pipeline: loads DXIL from ShaderBundle
    void createRootSignature();
    void createPipelineState();
    void createConstantBuffer();
    void createMeshes();

    // Core objects
    ComPtr<IDXGIFactory4>           m_factory;
    ComPtr<ID3D12Device>            m_device;
    ComPtr<ID3D12CommandQueue>      m_commandQueue;
    ComPtr<IDXGISwapChain3>         m_swapChain;

    // Render targets
    ComPtr<ID3D12DescriptorHeap>    m_rtvHeap;
    uint32_t                        m_rtvDescriptorSize = 0;
    ComPtr<ID3D12Resource>          m_renderTargets[FRAME_COUNT];

    // Depth buffer
    ComPtr<ID3D12DescriptorHeap>    m_dsvHeap;
    ComPtr<ID3D12Resource>          m_depthBuffer;

    // Command recording
    ComPtr<ID3D12CommandAllocator>  m_commandAllocators[FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> m_commandList;

    // Pipeline
    ComPtr<ID3D12RootSignature>     m_rootSignature;
    ComPtr<ID3D12PipelineState>     m_pipelineState;

    // CBV/SRV resources
    ComPtr<ID3D12DescriptorHeap>    m_cbvSrvHeap;
    ComPtr<ID3D12Resource>          m_constantBuffer;   // FrameData (b0)
    ComPtr<ID3D12Resource>          m_materialBuffer;   // MaterialParams (b1)
    ComPtr<ID3D12Resource>          m_dummyTexture;     // 1x1 white placeholder
    void*                           m_cbvMapped = nullptr;

    // Geometry (per mesh type)
    struct D3D12Mesh {
        ComPtr<ID3D12Resource> vertexBuffer;
        ComPtr<ID3D12Resource> indexBuffer;
        D3D12_VERTEX_BUFFER_VIEW vbView = {};
        D3D12_INDEX_BUFFER_VIEW ibView = {};
        uint32_t indexCount = 0;
    };
    std::unordered_map<int, D3D12Mesh> m_meshes; // key = (int)MeshType

    // DXIL shader bytecode (loaded from ShaderBundle)
    std::vector<char>               m_vertexShaderDxil;
    std::vector<char>               m_pixelShaderDxil;

    // Sync
    ComPtr<ID3D12Fence>             m_fence;
    uint64_t                        m_fenceValues[FRAME_COUNT] = {};
    HANDLE                          m_fenceEvent = nullptr;

    uint32_t m_frameIndex = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};

} // namespace QymEngine

#endif // _WIN32
