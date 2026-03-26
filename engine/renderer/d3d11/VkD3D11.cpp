#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "renderer/d3d11/VkD3D11Handles.h"
#include "renderer/VkDispatch.h"
#include <d3dcompiler.h>  // D3DCompile (ImGui shader runtime compilation)
#include <iostream>
#include <cstring>
#include <algorithm>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// Handle cast macros: VkXxx (opaque pointer) <-> D11_Xxx (actual struct)
#define AS_D11(Type, handle) reinterpret_cast<D11_##Type*>(handle)
#define TO_VK(VkType, ptr) reinterpret_cast<VkType>(ptr)

// 帧级诊断日志 (仅前几帧输出)
static uint32_t s_frameCount = 0;
static constexpr uint32_t TRACE_FRAMES = 0; // 设为 >0 开启帧级诊断日志
#define D3D11_TRACE(fmt, ...) \
    do { if (s_frameCount < TRACE_FRAMES) fprintf(stderr, "[D3D11 F%u] " fmt "\n", s_frameCount, ##__VA_ARGS__); } while(0)

// ============================================================================
// 辅助工具
// ============================================================================

#define D3D11_STUB(fn) \
    do { std::cerr << "[D3D11 Stub] " << #fn << " not implemented" << std::endl; } while(0)

static void d3d11_log(const char* msg) {
    FILE* f = fopen("E:/MYQ/QymEngine/captures/d3d11_trace.log", "a");
    if (f) { fprintf(f, "%s\n", msg); fclose(f); }
}

static void d3d11_heap_check(const char* label) {
    HANDLE heap = GetProcessHeap();
    BOOL ok = HeapValidate(heap, 0, nullptr);
    FILE* f = fopen("E:/MYQ/QymEngine/captures/d3d11_trace.log", "a");
    if (f) { fprintf(f, "HEAP %s: %s\n", label, ok ? "OK" : "CORRUPTED!"); fclose(f); }
}

// ImGui HLSL 着色器源码 (运行时通过 D3DCompile 编译为 SM5.0 DXBC)
static const char* s_imguiVS_hlsl = R"(
cbuffer Constants : register(b0)
{
    float2 uScale;
    float2 uTranslate;
};

struct VSInput
{
    float2 pos   : TEXCOORD0;
    float2 uv    : TEXCOORD1;
    float4 color : TEXCOORD2;
};

struct VSOutput
{
    float4 pos   : SV_Position;
    float4 color : COLOR;
    float2 uv    : TEXCOORD;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.pos = float4(input.pos * uScale + uTranslate, 0.0, 1.0);
    output.pos.y = -output.pos.y;
    output.color = input.color;
    output.uv = input.uv;
    return output;
}
)";

static const char* s_imguiPS_hlsl = R"(
Texture2D tex : register(t0);
SamplerState samp : register(s0);

struct PSInput
{
    float4 pos   : SV_Position;
    float4 color : COLOR;
    float2 uv    : TEXCOORD;
};

float4 main(PSInput input) : SV_Target
{
    return input.color * tex.Sample(samp, input.uv);
}
)";

// 缓存编译后的 ImGui DXBC
static std::vector<char> s_imguiVS_dxbc;
static std::vector<char> s_imguiPS_dxbc;

static bool compileImGuiShaders()
{
    if (!s_imguiVS_dxbc.empty() && !s_imguiPS_dxbc.empty())
        return true;

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;

    // 编译 VS
    HRESULT hr = D3DCompile(s_imguiVS_hlsl, strlen(s_imguiVS_hlsl), "imgui_vs",
                             nullptr, nullptr, "main", "vs_5_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) std::cerr << "[VkD3D11] ImGui VS compile error: "
                               << (char*)errBlob->GetBufferPointer() << std::endl;
        return false;
    }

    // 编译 PS
    hr = D3DCompile(s_imguiPS_hlsl, strlen(s_imguiPS_hlsl), "imgui_ps",
                    nullptr, nullptr, "main", "ps_5_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        if (errBlob) std::cerr << "[VkD3D11] ImGui PS compile error: "
                               << (char*)errBlob->GetBufferPointer() << std::endl;
        return false;
    }

    s_imguiVS_dxbc.assign(static_cast<const char*>(vsBlob->GetBufferPointer()),
                           static_cast<const char*>(vsBlob->GetBufferPointer()) + vsBlob->GetBufferSize());
    s_imguiPS_dxbc.assign(static_cast<const char*>(psBlob->GetBufferPointer()),
                           static_cast<const char*>(psBlob->GetBufferPointer()) + psBlob->GetBufferSize());

    std::cout << "[VkD3D11] ImGui shaders compiled (VS=" << s_imguiVS_dxbc.size()
              << "B, PS=" << s_imguiPS_dxbc.size() << "B)" << std::endl;
    return true;
}

// 刷新 D3D11 InfoQueue 中的 validation 消息
static void flushD3D11Messages(VkDevice device)
{
    auto* dev = AS_D11(Device, device);
    if (!dev || !dev->infoQueue) return;
    UINT64 count = dev->infoQueue->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; i++) {
        SIZE_T msgLen = 0;
        dev->infoQueue->GetMessage(i, nullptr, &msgLen);
        if (msgLen == 0) continue;
        auto* msg = (D3D11_MESSAGE*)malloc(msgLen);
        if (!msg) continue;
        dev->infoQueue->GetMessage(i, msg, &msgLen);
        const char* severity = "INFO";
        if (msg->Severity == D3D11_MESSAGE_SEVERITY_ERROR)          severity = "ERROR";
        else if (msg->Severity == D3D11_MESSAGE_SEVERITY_WARNING)   severity = "WARNING";
        else if (msg->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION) severity = "CORRUPTION";
        std::cerr << "[D3D11 " << severity << "] " << msg->pDescription << std::endl;
        free(msg);
    }
    dev->infoQueue->ClearStoredMessages();
}

// VkFormat -> DXGI_FORMAT 转换 (常用格式)
static DXGI_FORMAT vkFormatToDxgi(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8_UNORM:             return DXGI_FORMAT_R8_UNORM;
    case VK_FORMAT_R8G8_UNORM:           return DXGI_FORMAT_R8G8_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM:      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:       return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM:      return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:       return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case VK_FORMAT_R32G32B32A32_SFLOAT:  return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case VK_FORMAT_R32G32B32_SFLOAT:     return DXGI_FORMAT_R32G32B32_FLOAT;
    case VK_FORMAT_R32G32_SFLOAT:        return DXGI_FORMAT_R32G32_FLOAT;
    case VK_FORMAT_R32_SFLOAT:           return DXGI_FORMAT_R32_FLOAT;
    case VK_FORMAT_R16G16B16A16_SFLOAT:  return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case VK_FORMAT_R16G16_SFLOAT:        return DXGI_FORMAT_R16G16_FLOAT;
    case VK_FORMAT_D32_SFLOAT:           return DXGI_FORMAT_D32_FLOAT;
    case VK_FORMAT_D24_UNORM_S8_UINT:    return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case VK_FORMAT_D16_UNORM:            return DXGI_FORMAT_D16_UNORM;
    default:                             return DXGI_FORMAT_UNKNOWN;
    }
}

// 每像素字节数
static uint32_t texelSizeFromDxgi(DXGI_FORMAT fmt)
{
    switch (fmt) {
    case DXGI_FORMAT_R8_UNORM:              return 1;
    case DXGI_FORMAT_R8G8_UNORM:            return 2;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:     return 4;
    case DXGI_FORMAT_R16G16_FLOAT:          return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:    return 8;
    case DXGI_FORMAT_R32G32_FLOAT:          return 8;
    case DXGI_FORMAT_R32G32B32_FLOAT:       return 12;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:    return 16;
    default:                                return 4;
    }
}

// 解析 DXBC RDEF 块，提取 SRV 和 Sampler 的实际 register 索引
// Slang DXBC 编译器可能跳过未使用的资源，导致 register 不连续
static void parseDxbcRDEF(const void* dxbc, size_t size,
                           std::vector<uint32_t>& srvRegs,
                           std::vector<uint32_t>& samplerRegs)
{
    if (!dxbc || size < 32) return;
    auto* data = static_cast<const uint8_t*>(dxbc);

    // 验证 DXBC magic
    if (memcmp(data, "DXBC", 4) != 0) return;

    uint32_t chunkCount;
    memcpy(&chunkCount, data + 28, 4);

    for (uint32_t c = 0; c < chunkCount; c++) {
        uint32_t chunkOff;
        memcpy(&chunkOff, data + 32 + c * 4, 4);
        if (chunkOff + 8 > size) continue;

        // 检查 RDEF FourCC
        if (memcmp(data + chunkOff, "RDEF", 4) != 0) continue;

        uint32_t chunkSize;
        memcpy(&chunkSize, data + chunkOff + 4, 4);
        auto* rd = data + chunkOff + 8;
        if (chunkOff + 8 + chunkSize > size) continue;

        uint32_t boundResCount, boundResOff;
        memcpy(&boundResCount, rd + 8, 4);
        memcpy(&boundResOff, rd + 12, 4);

        // D3D11_SHADER_INPUT_BIND_DESC: 每项 32 字节
        // [0..4] nameOffset, [4..8] type, [20..24] bindPoint
        for (uint32_t r = 0; r < boundResCount; r++) {
            uint32_t entryOff = boundResOff + r * 32;
            if (entryOff + 32 > chunkSize) continue;

            uint32_t resType, bindPoint;
            memcpy(&resType, rd + entryOff + 4, 4);
            memcpy(&bindPoint, rd + entryOff + 20, 4);

            if (resType == 2) // D3D_SIT_TEXTURE
                srvRegs.push_back(bindPoint);
            else if (resType == 3) // D3D_SIT_SAMPLER
                samplerRegs.push_back(bindPoint);
        }
        break; // 只需要第一个 RDEF 块
    }

    // 按 register 索引排序
    std::sort(srvRegs.begin(), srvRegs.end());
    std::sort(samplerRegs.begin(), samplerRegs.end());
}

// D3D11 内存类型映射 (与 D3D12 一致):
//   0 = DEVICE_LOCAL  → D3D11_USAGE_DEFAULT
//   1 = HOST_VISIBLE | HOST_COHERENT  → D3D11_USAGE_DYNAMIC (或 STAGING)
//   2 = HOST_VISIBLE | HOST_COHERENT | HOST_CACHED → D3D11_USAGE_STAGING (readback)
static D3D11_USAGE memoryTypeToD3D11Usage(uint32_t memoryTypeIndex)
{
    switch (memoryTypeIndex) {
    case 0:  return D3D11_USAGE_DEFAULT;
    case 1:  return D3D11_USAGE_DYNAMIC;
    case 2:  return D3D11_USAGE_STAGING;
    default: return D3D11_USAGE_DEFAULT;
    }
}

// ============================================================================
// Instance
// ============================================================================

static VkResult VKAPI_CALL d3d11_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance)
{
    (void)pCreateInfo; (void)pAllocator;
    d3d11_log("vkCreateInstance");

    auto* inst = new D11_Instance();

    UINT dxgiFlags = 0;
#ifdef _DEBUG
    inst->debugEnabled = true;
    dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
    std::cout << "[VkD3D11] Debug mode active" << std::endl;
#endif

    if (FAILED(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&inst->factory)))) {
        delete inst;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    *pInstance = TO_VK(VkInstance, inst);
    std::cout << "[VkD3D11] Instance created" << std::endl;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator)
{
    (void)pAllocator;
    if (!instance) return;
    auto* inst = AS_D11(Instance, instance);

    for (auto pd : inst->physicalDevices) {
        delete AS_D11(PhysicalDevice, pd);
    }
    inst->physicalDevices.clear();

    delete inst;
    std::cout << "[VkD3D11] Instance destroyed" << std::endl;
}

// ============================================================================
// Physical Device
// ============================================================================

static VkResult VKAPI_CALL d3d11_vkEnumeratePhysicalDevices(
    VkInstance instance,
    uint32_t* pPhysicalDeviceCount,
    VkPhysicalDevice* pPhysicalDevices)
{
    if (!instance) return VK_ERROR_INITIALIZATION_FAILED;
    auto* inst = AS_D11(Instance, instance);

    // 首次调用时枚举 DXGI 适配器
    if (inst->physicalDevices.empty()) {
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; inst->factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

            // 非软件适配器直接加入列表（不调用 D3D11CreateDevice 探测，避免 RenderDoc hook 冲突）
            auto* pd = new D11_PhysicalDevice();
            pd->adapter = adapter;
            pd->adapterDesc = desc;
            pd->instance = instance;
            inst->physicalDevices.push_back(TO_VK(VkPhysicalDevice, pd));
            adapter.Reset();
        }
    }

    uint32_t count = static_cast<uint32_t>(inst->physicalDevices.size());
    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = count;
        return VK_SUCCESS;
    }

    uint32_t toWrite = std::min(*pPhysicalDeviceCount, count);
    for (uint32_t i = 0; i < toWrite; i++) {
        pPhysicalDevices[i] = inst->physicalDevices[i];
    }
    *pPhysicalDeviceCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* pProperties)
{
    if (!physicalDevice || !pProperties) return;
    auto* pd = AS_D11(PhysicalDevice, physicalDevice);
    memset(pProperties, 0, sizeof(*pProperties));

    pProperties->apiVersion = VK_API_VERSION_1_0;
    pProperties->driverVersion = 1;
    pProperties->vendorID = pd->adapterDesc.VendorId;
    pProperties->deviceID = pd->adapterDesc.DeviceId;
    pProperties->deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

    // 适配器名称 (UTF-16 -> UTF-8)
    WideCharToMultiByte(CP_UTF8, 0, pd->adapterDesc.Description, -1,
                        pProperties->deviceName, VK_MAX_PHYSICAL_DEVICE_NAME_SIZE,
                        nullptr, nullptr);

    // limits
    pProperties->limits.maxImageDimension2D = 16384;
    pProperties->limits.maxPushConstantsSize = 256;
    pProperties->limits.maxBoundDescriptorSets = 4;
    pProperties->limits.maxPerStageDescriptorSampledImages = 128;
    pProperties->limits.maxPerStageDescriptorSamplers = 16;
    pProperties->limits.minUniformBufferOffsetAlignment = 256;
    pProperties->limits.nonCoherentAtomSize = 64;
}

static void VKAPI_CALL d3d11_vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures)
{
    (void)physicalDevice;
    if (!pFeatures) return;
    memset(pFeatures, 0, sizeof(*pFeatures));

    pFeatures->samplerAnisotropy = VK_TRUE;
    pFeatures->fillModeNonSolid = VK_TRUE;
    pFeatures->wideLines = VK_FALSE;
    pFeatures->multiDrawIndirect = VK_FALSE;
}

static void VKAPI_CALL d3d11_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures)
{
    if (!pFeatures) return;
    d3d11_vkGetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);

    // 遍历 pNext 链
    VkBaseOutStructure* next = reinterpret_cast<VkBaseOutStructure*>(pFeatures->pNext);
    while (next) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
            auto* indexing = reinterpret_cast<VkPhysicalDeviceDescriptorIndexingFeatures*>(next);
            // D3D11 不支持 bindless，但报告基本支持以避免引擎崩溃
            indexing->shaderSampledImageArrayNonUniformIndexing = VK_FALSE;
            indexing->runtimeDescriptorArray = VK_FALSE;
            indexing->descriptorBindingPartiallyBound = VK_FALSE;
            indexing->descriptorBindingVariableDescriptorCount = VK_FALSE;
            indexing->descriptorBindingSampledImageUpdateAfterBind = VK_FALSE;
            indexing->descriptorBindingStorageBufferUpdateAfterBind = VK_FALSE;
        }
        next = next->pNext;
    }
}

static void VKAPI_CALL d3d11_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties* pMemoryProperties)
{
    (void)physicalDevice;
    if (!pMemoryProperties) return;
    memset(pMemoryProperties, 0, sizeof(*pMemoryProperties));

    // 1 个 memory heap
    pMemoryProperties->memoryHeapCount = 1;
    pMemoryProperties->memoryHeaps[0].size = 4ULL * 1024 * 1024 * 1024; // 4GB
    pMemoryProperties->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

    // 3 种 memory type (与 D3D12 后端一致)
    pMemoryProperties->memoryTypeCount = 3;

    // Type 0: DEVICE_LOCAL (D3D11_USAGE_DEFAULT)
    pMemoryProperties->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    pMemoryProperties->memoryTypes[0].heapIndex = 0;

    // Type 1: HOST_VISIBLE | HOST_COHERENT (D3D11_USAGE_DYNAMIC / STAGING upload)
    pMemoryProperties->memoryTypes[1].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    pMemoryProperties->memoryTypes[1].heapIndex = 0;

    // Type 2: HOST_VISIBLE | HOST_COHERENT | HOST_CACHED (STAGING readback)
    pMemoryProperties->memoryTypes[2].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    pMemoryProperties->memoryTypes[2].heapIndex = 0;
}

static void VKAPI_CALL d3d11_vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice physicalDevice,
    uint32_t* pQueueFamilyPropertyCount,
    VkQueueFamilyProperties* pQueueFamilyProperties)
{
    (void)physicalDevice;
    if (!pQueueFamilyProperties) {
        *pQueueFamilyPropertyCount = 1;
        return;
    }

    uint32_t toWrite = std::min(*pQueueFamilyPropertyCount, 1u);
    if (toWrite > 0) {
        pQueueFamilyProperties[0] = {};
        pQueueFamilyProperties[0].queueFlags =
            VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
        pQueueFamilyProperties[0].queueCount = 1;
        pQueueFamilyProperties[0].timestampValidBits = 64;
        pQueueFamilyProperties[0].minImageTransferGranularity = {1, 1, 1};
    }
    *pQueueFamilyPropertyCount = toWrite;
}

static VkResult VKAPI_CALL d3d11_vkEnumerateInstanceExtensionProperties(
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties)
{
    (void)pLayerName; (void)pProperties;
    const VkExtensionProperties exts[] = {
        {"VK_KHR_surface", 1},
        {"VK_KHR_win32_surface", 1},
    };
    uint32_t count = 2;

    if (!pProperties) {
        *pPropertyCount = count;
        return VK_SUCCESS;
    }
    uint32_t toWrite = std::min(*pPropertyCount, count);
    memcpy(pProperties, exts, toWrite * sizeof(VkExtensionProperties));
    *pPropertyCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d11_vkEnumerateInstanceLayerProperties(
    uint32_t* pPropertyCount,
    VkLayerProperties* pProperties)
{
    (void)pProperties;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d11_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties)
{
    (void)physicalDevice; (void)pLayerName;
    const VkExtensionProperties exts[] = {
        {"VK_KHR_swapchain", 1},
    };
    uint32_t count = 1;

    if (!pProperties) {
        *pPropertyCount = count;
        return VK_SUCCESS;
    }
    uint32_t toWrite = std::min(*pPropertyCount, count);
    memcpy(pProperties, exts, toWrite * sizeof(VkExtensionProperties));
    *pPropertyCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

// ============================================================================
// Device & Queue
// ============================================================================

static VkResult VKAPI_CALL d3d11_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice)
{
    (void)pCreateInfo; (void)pAllocator;
    if (!physicalDevice) return VK_ERROR_INITIALIZATION_FAILED;
    auto* pd = AS_D11(PhysicalDevice, physicalDevice);

    d3d11_log("vkCreateDevice ENTER");
    d3d11_heap_check("before D3D11CreateDevice");
    auto* dev = new D11_Device();
    dev->physicalDevice = physicalDevice;

    fprintf(stderr, "[VkD3D11] vkCreateDevice calling D3D11CreateDevice...\n"); fflush(stderr);
    UINT createFlags = 0;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    HRESULT hr = D3D11CreateDevice(
        pd->adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        createFlags,
        featureLevels, _countof(featureLevels),
        D3D11_SDK_VERSION,
        &dev->device,
        &dev->featureLevel,
        &dev->immediateContext);
    d3d11_heap_check("after D3D11CreateDevice");

    if (FAILED(hr)) {
        delete dev;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // 配置 D3D11 InfoQueue
    ComPtr<ID3D11Debug> debug;
    if (SUCCEEDED(dev->device.As(&debug))) {
        if (SUCCEEDED(dev->device.As(&dev->infoQueue))) {
            std::cout << "[VkD3D11] InfoQueue configured (validation enabled)" << std::endl;
        }
    }

    // 预编译 ImGui 着色器
    compileImGuiShaders();

    *pDevice = TO_VK(VkDevice, dev);

    char name[256];
    WideCharToMultiByte(CP_UTF8, 0, pd->adapterDesc.Description, -1,
                        name, 256, nullptr, nullptr);
    std::cout << "[VkD3D11] Device created on: " << name
              << " (FL=" << std::hex << dev->featureLevel << std::dec << ")" << std::endl;
    std::cerr << "[VkD3D11] vkCreateDevice returning OK" << std::endl;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator)
{
    (void)pAllocator;
    if (!device) return;
    delete AS_D11(Device, device);
    std::cout << "[VkD3D11] Device destroyed" << std::endl;
}

static void VKAPI_CALL d3d11_vkGetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue)
{
    fprintf(stderr, "[VkD3D11] GetDeviceQueue family=%u idx=%u\n", queueFamilyIndex, queueIndex);
    if (!device || !pQueue) return;

    auto* q = new D11_Queue();
    q->device = device;
    q->familyIndex = queueFamilyIndex;
    q->queueIndex = queueIndex;
    *pQueue = TO_VK(VkQueue, q);
}

static VkResult VKAPI_CALL d3d11_vkDeviceWaitIdle(VkDevice device)
{
    if (!device) return VK_ERROR_DEVICE_LOST;
    auto* dev = AS_D11(Device, device);
    // D3D11 immediate context: Flush 确保所有已提交的命令完成
    if (dev->immediateContext)
        dev->immediateContext->Flush();
    return VK_SUCCESS;
}

// ============================================================================
// Surface (Win32)
// ============================================================================

#ifdef VK_USE_PLATFORM_WIN32_KHR
static VkResult VKAPI_CALL d3d11_vkCreateWin32SurfaceKHR(
    VkInstance instance,
    const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface)
{
    (void)instance; (void)pAllocator;
    auto* surface = new D11_Surface();
    surface->hwnd = pCreateInfo->hwnd;
    surface->hinstance = pCreateInfo->hinstance;
    *pSurface = TO_VK(VkSurfaceKHR, surface);
    return VK_SUCCESS;
}
#endif

static void VKAPI_CALL d3d11_vkDestroySurfaceKHR(
    VkInstance instance,
    VkSurfaceKHR surface,
    const VkAllocationCallbacks* pAllocator)
{
    (void)instance; (void)pAllocator;
    delete AS_D11(Surface, surface);
}

static VkResult VKAPI_CALL d3d11_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* pSurfaceCapabilities)
{
    (void)physicalDevice;
    d3d11_log("GetSurfaceCapabilities");
    d3d11_heap_check("before GetSurfaceCaps");
    if (!surface || !pSurfaceCapabilities) return VK_ERROR_SURFACE_LOST_KHR;
    auto* surf = AS_D11(Surface, surface);

    RECT rect;
    GetClientRect(surf->hwnd, &rect);
    uint32_t w = rect.right - rect.left;
    uint32_t h = rect.bottom - rect.top;

    pSurfaceCapabilities->minImageCount = 2;
    pSurfaceCapabilities->maxImageCount = 3;
    pSurfaceCapabilities->currentExtent = {w, h};
    pSurfaceCapabilities->minImageExtent = {w, h};
    pSurfaceCapabilities->maxImageExtent = {w, h};
    pSurfaceCapabilities->maxImageArrayLayers = 1;
    pSurfaceCapabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pSurfaceCapabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    pSurfaceCapabilities->supportedUsageFlags =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d11_vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormatKHR* pSurfaceFormats)
{
    (void)physicalDevice; (void)surface;
    const VkSurfaceFormatKHR formats[] = {
        {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };
    uint32_t count = 3;
    if (!pSurfaceFormats) { *pSurfaceFormatCount = count; return VK_SUCCESS; }
    uint32_t toWrite = std::min(*pSurfaceFormatCount, count);
    memcpy(pSurfaceFormats, formats, toWrite * sizeof(VkSurfaceFormatKHR));
    *pSurfaceFormatCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d11_vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pPresentModeCount,
    VkPresentModeKHR* pPresentModes)
{
    (void)physicalDevice; (void)surface;
    const VkPresentModeKHR modes[] = {
        VK_PRESENT_MODE_FIFO_KHR,
        VK_PRESENT_MODE_IMMEDIATE_KHR,
    };
    uint32_t count = 2;
    if (!pPresentModes) { *pPresentModeCount = count; return VK_SUCCESS; }
    uint32_t toWrite = std::min(*pPresentModeCount, count);
    memcpy(pPresentModes, modes, toWrite * sizeof(VkPresentModeKHR));
    *pPresentModeCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d11_vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physicalDevice,
    uint32_t queueFamilyIndex,
    VkSurfaceKHR surface,
    VkBool32* pSupported)
{
    (void)physicalDevice; (void)queueFamilyIndex; (void)surface;
    *pSupported = VK_TRUE;
    return VK_SUCCESS;
}

// ============================================================================
// Memory
// ============================================================================

static VkResult VKAPI_CALL d3d11_vkAllocateMemory(
    VkDevice device,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDeviceMemory* pMemory)
{
    (void)pAllocator;
    if (!device || !pAllocateInfo) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    auto* mem = new D11_Memory();
    mem->d3dUsage = memoryTypeToD3D11Usage(pAllocateInfo->memoryTypeIndex);
    mem->size = pAllocateInfo->allocationSize;
    mem->memoryTypeIndex = pAllocateInfo->memoryTypeIndex;
    // D3D11: 实际资源在 vkBindBufferMemory / vkBindImageMemory 时创建
    *pMemory = TO_VK(VkDeviceMemory, mem);
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkFreeMemory(
    VkDevice device,
    VkDeviceMemory memory,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!memory) return;
    auto* mem = AS_D11(Memory, memory);
    // 释放 shadow buffer
    if (mem->mapped) {
        free(mem->mapped);
        mem->mapped = nullptr;
    }
    delete mem;
}

static VkResult VKAPI_CALL d3d11_vkMapMemory(
    VkDevice device,
    VkDeviceMemory memory,
    VkDeviceSize offset,
    VkDeviceSize size,
    VkMemoryMapFlags flags,
    void** ppData)
{
    (void)device; (void)size; (void)flags;
    if (!memory) return VK_ERROR_MEMORY_MAP_FAILED;
    auto* mem = AS_D11(Memory, memory);

    // 使用 shadow buffer 策略:
    // vkMapMemory 返回 shadow buffer 指针，引擎随意写入。
    // 实际 D3D11 buffer 更新在 draw 前通过 UpdateSubresource 完成。
    if (!mem->mapped) {
        mem->mapped = calloc(1, (size_t)mem->size);
        if (!mem->mapped) return VK_ERROR_MEMORY_MAP_FAILED;
    }

    *ppData = static_cast<uint8_t*>(mem->mapped) + offset;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkUnmapMemory(
    VkDevice device,
    VkDeviceMemory memory)
{
    (void)device;
    if (!memory) return;
    // 保持 shadow buffer 有效 (支持 persistent mapping 模式)
    // 不释放 memory->mapped
}

static VkResult VKAPI_CALL d3d11_vkFlushMappedMemoryRanges(
    VkDevice device,
    uint32_t memoryRangeCount,
    const VkMappedMemoryRange* pMemoryRanges)
{
    (void)device; (void)memoryRangeCount; (void)pMemoryRanges;
    // D3D11 shadow buffer 不需要显式 flush，在 draw 前自动同步
    return VK_SUCCESS;
}

// ============================================================================
// Buffer
// ============================================================================

static VkResult VKAPI_CALL d3d11_vkCreateBuffer(
    VkDevice device,
    const VkBufferCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkBuffer* pBuffer)
{
    (void)pAllocator;
    if (!device || !pCreateInfo) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    auto* buf = new D11_Buffer();
    buf->size = pCreateInfo->size;
    buf->usage = pCreateInfo->usage;
    *pBuffer = TO_VK(VkBuffer, buf);
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroyBuffer(
    VkDevice device,
    VkBuffer buffer,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!buffer) return;
    delete AS_D11(Buffer, buffer);
}

static void VKAPI_CALL d3d11_vkGetBufferMemoryRequirements(
    VkDevice device,
    VkBuffer buffer,
    VkMemoryRequirements* pMemoryRequirements)
{
    (void)device;
    if (!buffer || !pMemoryRequirements) return;
    auto* buf = AS_D11(Buffer, buffer);

    // D3D11 constant buffer 要求 16 字节对齐
    pMemoryRequirements->size = (buf->size + 255) & ~255ULL;
    pMemoryRequirements->alignment = 256;
    pMemoryRequirements->memoryTypeBits = 0x7; // type 0, 1, 2
}

static VkResult VKAPI_CALL d3d11_vkBindBufferMemory(
    VkDevice device,
    VkBuffer buffer,
    VkDeviceMemory memory,
    VkDeviceSize memoryOffset)
{
    (void)memoryOffset;
    if (!device || !buffer || !memory) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto* dev = AS_D11(Device, device);
    auto* buf = AS_D11(Buffer, buffer);
    auto* mem = AS_D11(Memory, memory);

    buf->boundMemory = memory;

    // 根据 memory type + buffer usage 决定 D3D11_USAGE 和 bind flags
    D3D11_BUFFER_DESC desc = {};
    VkDeviceSize bufSize = (buf->size > 0) ? buf->size : 256;
    desc.ByteWidth = static_cast<UINT>((bufSize + 15) & ~15ULL); // 16 对齐

    if (mem->memoryTypeIndex == 1) {
        // HOST_VISIBLE (upload)
        if (buf->usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) {
            // UBO → DYNAMIC + CPU_ACCESS_WRITE
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            buf->d3dUsage = D3D11_USAGE_DYNAMIC;
        } else if (buf->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) {
            // Staging upload → STAGING + READ/WRITE
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
            desc.BindFlags = 0; // staging 不能绑定
            buf->d3dUsage = D3D11_USAGE_STAGING;
        } else if (buf->usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) {
            // 动态 VB
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            buf->d3dUsage = D3D11_USAGE_DYNAMIC;
        } else if (buf->usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) {
            // 动态 IB
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            buf->d3dUsage = D3D11_USAGE_DYNAMIC;
        } else {
            // 其他 host-visible → staging
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ;
            desc.BindFlags = 0;
            buf->d3dUsage = D3D11_USAGE_STAGING;
        }
    } else if (mem->memoryTypeIndex == 2) {
        // READBACK → staging + read
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.BindFlags = 0;
        buf->d3dUsage = D3D11_USAGE_STAGING;
    } else {
        // DEVICE_LOCAL
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.CPUAccessFlags = 0;
        buf->d3dUsage = D3D11_USAGE_DEFAULT;

        // 设置 bind flags
        if (buf->usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
            desc.BindFlags |= D3D11_BIND_VERTEX_BUFFER;
        if (buf->usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
            desc.BindFlags |= D3D11_BIND_INDEX_BUFFER;
        if (buf->usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
            desc.BindFlags |= D3D11_BIND_CONSTANT_BUFFER;

        // 纯 transfer dst (readback) → staging
        if (buf->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT &&
            !(buf->usage & (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))) {
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.BindFlags = 0;
            buf->d3dUsage = D3D11_USAGE_STAGING;
        }

        // 如果没有 bind flags 且 default，至少设置一个
        if (desc.BindFlags == 0 && desc.Usage == D3D11_USAGE_DEFAULT) {
            // 通用 buffer: 不需要 bind flag 时用 staging
            desc.Usage = D3D11_USAGE_DEFAULT;
            // D3D11 DEFAULT 需要至少一个 bind flag，改为 staging
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            buf->d3dUsage = D3D11_USAGE_STAGING;
        }
    }

    HRESULT hr = dev->device->CreateBuffer(&desc, nullptr, &buf->buffer);
    if (FAILED(hr)) {
        fprintf(stderr, "[VkD3D11] BindBufferMemory: CreateBuffer FAILED hr=0x%lx size=%u usage=%d bindFlags=0x%x\n",
                hr, desc.ByteWidth, (int)desc.Usage, desc.BindFlags);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    return VK_SUCCESS;
}

// ============================================================================
// Image
// ============================================================================

static VkResult VKAPI_CALL d3d11_vkCreateImage(
    VkDevice device,
    const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImage* pImage)
{
    (void)pAllocator;
    if (!device || !pCreateInfo) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    auto* img = new D11_Image();
    img->format = pCreateInfo->format;
    img->width = pCreateInfo->extent.width;
    img->height = pCreateInfo->extent.height;
    img->mipLevels = pCreateInfo->mipLevels;
    img->arrayLayers = pCreateInfo->arrayLayers;
    img->usage = pCreateInfo->usage;
    img->currentLayout = pCreateInfo->initialLayout;

    // texture 在 vkBindImageMemory 时创建
    *pImage = TO_VK(VkImage, img);
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroyImage(
    VkDevice device,
    VkImage image,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!image) return;
    auto* img = AS_D11(Image, image);
    if (img->ownsResource) {
        delete img;
    }
    // swapchain image 不 delete (由 swapchain 管理)
}

static void VKAPI_CALL d3d11_vkGetImageMemoryRequirements(
    VkDevice device,
    VkImage image,
    VkMemoryRequirements* pMemoryRequirements)
{
    (void)device;
    if (!image || !pMemoryRequirements) return;
    auto* img = AS_D11(Image, image);

    DXGI_FORMAT fmt = vkFormatToDxgi(img->format);
    uint32_t texelSize = texelSizeFromDxgi(fmt);
    VkDeviceSize size = (VkDeviceSize)img->width * img->height * texelSize * img->arrayLayers;
    // mip chain 约 1.33x
    if (img->mipLevels > 1) size = (size * 4) / 3;
    size = (size + 4095) & ~4095ULL;

    pMemoryRequirements->size = size;
    pMemoryRequirements->alignment = 256;
    pMemoryRequirements->memoryTypeBits = 0x1; // 只支持 DEVICE_LOCAL
}

static VkResult VKAPI_CALL d3d11_vkBindImageMemory(
    VkDevice device,
    VkImage image,
    VkDeviceMemory memory,
    VkDeviceSize memoryOffset)
{
    (void)memoryOffset;
    if (!device || !image || !memory) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto* dev = AS_D11(Device, device);
    auto* img = AS_D11(Image, image);

    img->boundMemory = memory;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = img->width;
    desc.Height = img->height;
    desc.MipLevels = img->mipLevels;
    desc.ArraySize = img->arrayLayers;
    desc.Format = vkFormatToDxgi(img->format);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;

    // Depth format 需要 typeless 格式创建 (以便同时绑定 DSV + SRV)
    bool isDepth = (desc.Format == DXGI_FORMAT_D32_FLOAT ||
                    desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
                    desc.Format == DXGI_FORMAT_D16_UNORM);

    if (img->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
        desc.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
        // 如果同时需要 SRV (shadow map)，用 typeless 格式
        if (img->usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
            if (desc.Format == DXGI_FORMAT_D32_FLOAT)
                desc.Format = DXGI_FORMAT_R32_TYPELESS;
            else if (desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT)
                desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        }
    }
    if (img->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        desc.BindFlags |= D3D11_BIND_RENDER_TARGET;
    if (img->usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
        desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;

    // 至少要有一个 bind flag
    if (desc.BindFlags == 0) {
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    }

    // Cubemap (arrayLayers==6) 需要 MiscFlags 标记
    if (img->arrayLayers == 6) {
        desc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;
    }

    HRESULT hr = dev->device->CreateTexture2D(&desc, nullptr, &img->texture);
    if (FAILED(hr)) {
        fprintf(stderr, "[VkD3D11] BindImageMemory: CreateTexture2D FAILED hr=0x%lx fmt=%d %ux%u bind=0x%x\n",
                hr, (int)desc.Format, desc.Width, desc.Height, desc.BindFlags);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d11_vkCreateImageView(
    VkDevice device,
    const VkImageViewCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImageView* pView)
{
    fprintf(stderr, "[VkD3D11] CreateImageView\n");
    (void)pAllocator;
    if (!device || !pCreateInfo) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto* dev = AS_D11(Device, device);

    auto* view = new D11_ImageView();
    view->image = pCreateInfo->image;
    view->format = pCreateInfo->format;
    view->viewType = pCreateInfo->viewType;
    view->subresourceRange = pCreateInfo->subresourceRange;

    auto* img = AS_D11(Image, pCreateInfo->image);
    if (img && img->texture) {
        DXGI_FORMAT dxgiFormat = vkFormatToDxgi(pCreateInfo->format);

        // Depth format 判断
        bool isDepth = (pCreateInfo->subresourceRange.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) ||
                       (dxgiFormat == DXGI_FORMAT_D32_FLOAT || dxgiFormat == DXGI_FORMAT_D24_UNORM_S8_UINT ||
                        dxgiFormat == DXGI_FORMAT_D16_UNORM);

        // DSV
        if (isDepth && (img->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
            dsvDesc.Format = dxgiFormat;  // D32_FLOAT 等
            dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            dsvDesc.Texture2D.MipSlice = 0;

            HRESULT hr = dev->device->CreateDepthStencilView(
                img->texture.Get(), &dsvDesc, &view->dsv);
            if (SUCCEEDED(hr)) view->hasDsv = true;
        }

        // RTV
        if (!isDepth && (img->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
            DXGI_FORMAT rtvFormat = dxgiFormat;
            if (rtvFormat == DXGI_FORMAT_UNKNOWN) {
                // fallback 到 image 自身格式
                D3D11_TEXTURE2D_DESC texDesc;
                img->texture->GetDesc(&texDesc);
                rtvFormat = texDesc.Format;
            }

            D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
            rtvDesc.Format = rtvFormat;
            // Cubemap 面视图 (arrayLayers>1 的 image 上的单层子视图) 需要 TEXTURE2DARRAY
            if (img->arrayLayers > 1) {
                rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                rtvDesc.Texture2DArray.MipSlice = view->subresourceRange.baseMipLevel;
                rtvDesc.Texture2DArray.FirstArraySlice = view->subresourceRange.baseArrayLayer;
                rtvDesc.Texture2DArray.ArraySize = view->subresourceRange.layerCount;
            } else {
                rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                rtvDesc.Texture2D.MipSlice = 0;
            }

            HRESULT hr = dev->device->CreateRenderTargetView(
                img->texture.Get(), &rtvDesc, &view->rtv);
            if (SUCCEEDED(hr)) view->hasRtv = true;
        }

        // SRV
        if (img->usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            // Depth SRV 需要 float 格式
            if (isDepth && dxgiFormat == DXGI_FORMAT_D32_FLOAT)
                srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            else if (isDepth && dxgiFormat == DXGI_FORMAT_D24_UNORM_S8_UINT)
                srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            else
                srvDesc.Format = dxgiFormat;

            // Cubemap (arrayLayers==6 且 viewType==CUBE) 需要 TEXTURECUBE dimension
            if (view->viewType == VK_IMAGE_VIEW_TYPE_CUBE) {
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
                srvDesc.TextureCube.MipLevels = (img->mipLevels > 0) ? img->mipLevels : 1;
                srvDesc.TextureCube.MostDetailedMip = 0;
            } else {
                srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = (img->mipLevels > 0) ? img->mipLevels : 1;
                srvDesc.Texture2D.MostDetailedMip = 0;
            }

            HRESULT hr = dev->device->CreateShaderResourceView(
                img->texture.Get(), &srvDesc, &view->srv);
            if (SUCCEEDED(hr)) view->hasSrv = true;
        }
    }

    *pView = TO_VK(VkImageView, view);
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroyImageView(
    VkDevice device,
    VkImageView imageView,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_D11(ImageView, imageView);
}

// ============================================================================
// Sampler
// ============================================================================

static VkResult VKAPI_CALL d3d11_vkCreateSampler(
    VkDevice device,
    const VkSamplerCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSampler* pSampler)
{
    (void)pAllocator;
    if (!device || !pCreateInfo) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto* dev = AS_D11(Device, device);

    auto* sampler = new D11_Sampler();

    D3D11_SAMPLER_DESC desc = {};
    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    desc.MaxLOD = D3D11_FLOAT32_MAX;
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D11_COMPARISON_NEVER;

    // Vulkan filter 转换
    if (pCreateInfo->magFilter == VK_FILTER_NEAREST &&
        pCreateInfo->minFilter == VK_FILTER_NEAREST) {
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    }

    // Vulkan address mode 转换
    auto vkAddrToD3D = [](VkSamplerAddressMode mode) -> D3D11_TEXTURE_ADDRESS_MODE {
        switch (mode) {
        case VK_SAMPLER_ADDRESS_MODE_REPEAT:           return D3D11_TEXTURE_ADDRESS_WRAP;
        case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:  return D3D11_TEXTURE_ADDRESS_MIRROR;
        case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:    return D3D11_TEXTURE_ADDRESS_CLAMP;
        case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:  return D3D11_TEXTURE_ADDRESS_BORDER;
        default: return D3D11_TEXTURE_ADDRESS_CLAMP;
        }
    };
    desc.AddressU = vkAddrToD3D(pCreateInfo->addressModeU);
    desc.AddressV = vkAddrToD3D(pCreateInfo->addressModeV);
    desc.AddressW = vkAddrToD3D(pCreateInfo->addressModeW);

    if (pCreateInfo->anisotropyEnable && pCreateInfo->maxAnisotropy > 1.0f) {
        desc.Filter = D3D11_FILTER_ANISOTROPIC;
        desc.MaxAnisotropy = static_cast<UINT>(pCreateInfo->maxAnisotropy);
    }

    HRESULT hr = dev->device->CreateSamplerState(&desc, &sampler->sampler);
    if (FAILED(hr)) {
        delete sampler;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    *pSampler = TO_VK(VkSampler, sampler);
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroySampler(
    VkDevice device,
    VkSampler sampler,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_D11(Sampler, sampler);
}

// ============================================================================
// Command Pool & Buffer
// ============================================================================

static VkResult VKAPI_CALL d3d11_vkCreateCommandPool(
    VkDevice device,
    const VkCommandPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkCommandPool* pCommandPool)
{
    (void)pCreateInfo; (void)pAllocator;
    if (!device) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    auto* pool = new D11_CommandPool();
    pool->device = device;
    *pCommandPool = TO_VK(VkCommandPool, pool);
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroyCommandPool(
    VkDevice device,
    VkCommandPool commandPool,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_D11(CommandPool, commandPool);
}

static VkResult VKAPI_CALL d3d11_vkResetCommandPool(
    VkDevice device,
    VkCommandPool commandPool,
    VkCommandPoolResetFlags flags)
{
    (void)device; (void)commandPool; (void)flags;
    // D3D11: deferred context 在 BeginCommandBuffer 时重建
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d11_vkAllocateCommandBuffers(
    VkDevice device,
    const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer* pCommandBuffers)
{
    if (!device || !pAllocateInfo) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto* dev = AS_D11(Device, device);

    for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++) {
        auto* cmd = new D11_CommandBuffer();
        cmd->device = device;

        // 创建 push constant buffer (128 bytes, DYNAMIC)
        D3D11_BUFFER_DESC pcDesc = {};
        pcDesc.ByteWidth = 128;  // push constants 最大 128 bytes (与 D3D12 一致)
        pcDesc.Usage = D3D11_USAGE_DYNAMIC;
        pcDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        pcDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->device->CreateBuffer(&pcDesc, nullptr, &cmd->pushConstantBuffer);

        pCommandBuffers[i] = TO_VK(VkCommandBuffer, cmd);
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkFreeCommandBuffers(
    VkDevice device,
    VkCommandPool commandPool,
    uint32_t commandBufferCount,
    const VkCommandBuffer* pCommandBuffers)
{
    (void)device; (void)commandPool;
    for (uint32_t i = 0; i < commandBufferCount; i++) {
        delete AS_D11(CommandBuffer, pCommandBuffers[i]);
    }
}

static VkResult VKAPI_CALL d3d11_vkBeginCommandBuffer(
    VkCommandBuffer commandBuffer,
    const VkCommandBufferBeginInfo* pBeginInfo)
{
    (void)pBeginInfo;
    if (!commandBuffer) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);

    // 创建新的 deferred context
    cmd->deferredContext.Reset();
    cmd->commandList.Reset();

    auto* dev = AS_D11(Device, cmd->device);
    if (dev && dev->device) {
        HRESULT hr = dev->device->CreateDeferredContext(
            0, &cmd->deferredContext);
        if (FAILED(hr)) {
            fprintf(stderr, "[VkD3D11] BeginCommandBuffer: CreateDeferredContext FAILED hr=0x%lx\n", hr);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
    }

    cmd->isRecording = true;
    // 重置延迟绑定状态
    cmd->currentPipeline = VK_NULL_HANDLE;
    cmd->currentRenderPass = VK_NULL_HANDLE;
    cmd->currentFramebuffer = VK_NULL_HANDLE;
    memset(cmd->boundSets, 0, sizeof(cmd->boundSets));
    cmd->stateDirty = false;
    cmd->pushConstantSize = 0;
    cmd->pendingReadbacks.clear();
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d11_vkEndCommandBuffer(VkCommandBuffer commandBuffer)
{
    if (!commandBuffer) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);

    // FinishCommandList 生成 command list
    if (cmd->deferredContext) {
        HRESULT hr = cmd->deferredContext->FinishCommandList(
            FALSE, &cmd->commandList);
        if (FAILED(hr)) {
            fprintf(stderr, "[VkD3D11] EndCommandBuffer: FinishCommandList FAILED hr=0x%lx\n", hr);
        }
    }

    cmd->isRecording = false;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d11_vkResetCommandBuffer(
    VkCommandBuffer commandBuffer,
    VkCommandBufferResetFlags flags)
{
    (void)flags;
    if (!commandBuffer) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);
    // Reset 在 BeginCommandBuffer 中处理
    cmd->deferredContext.Reset();
    cmd->commandList.Reset();
    return VK_SUCCESS;
}

// ============================================================================
// Synchronization
// ============================================================================

static VkResult VKAPI_CALL d3d11_vkCreateFence(
    VkDevice device,
    const VkFenceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkFence* pFence)
{
    (void)device; (void)pAllocator;
    auto* f = new D11_Fence();
    f->signaled = (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
    *pFence = TO_VK(VkFence, f);
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroyFence(
    VkDevice device,
    VkFence fence,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_D11(Fence, fence);
}

static VkResult VKAPI_CALL d3d11_vkWaitForFences(
    VkDevice device,
    uint32_t fenceCount,
    const VkFence* pFences,
    VkBool32 waitAll,
    uint64_t timeout)
{
    (void)device; (void)waitAll; (void)timeout;
    // D3D11 immediate mode: ExecuteCommandList 是同步的
    for (uint32_t i = 0; i < fenceCount; i++) {
        if (pFences[i]) AS_D11(Fence, pFences[i])->signaled = true;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d11_vkResetFences(
    VkDevice device,
    uint32_t fenceCount,
    const VkFence* pFences)
{
    (void)device;
    for (uint32_t i = 0; i < fenceCount; i++) {
        if (pFences[i]) AS_D11(Fence, pFences[i])->signaled = false;
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d11_vkQueueSubmit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence)
{
    auto* q = AS_D11(Queue, queue);
    if (!q || !q->device) return VK_ERROR_DEVICE_LOST;
    auto* dev = AS_D11(Device, q->device);

    auto* immediateCtx = dev->immediateContext.Get();
    if (!immediateCtx) return VK_ERROR_DEVICE_LOST;

    for (uint32_t i = 0; i < submitCount; i++) {
        for (uint32_t j = 0; j < pSubmits[i].commandBufferCount; j++) {
            auto* cmd = AS_D11(CommandBuffer, pSubmits[i].pCommandBuffers[j]);
            D3D11_TRACE("QueueSubmit: cmd=%p cmdList=%p", (void*)pSubmits[i].pCommandBuffers[j],
                cmd ? cmd->commandList.Get() : nullptr);
            if (cmd && cmd->commandList) {
                immediateCtx->ExecuteCommandList(cmd->commandList.Get(), FALSE);
            }

            // 处理延迟回读 (CopyImageToBuffer 的 staging texture → buffer shadow memory)
            if (cmd && !cmd->pendingReadbacks.empty()) {
                for (auto& rb : cmd->pendingReadbacks) {
                    if (!rb.stagingTex) continue;
                    auto* dstBuf = AS_D11(Buffer, rb.dstBuffer);
                    auto* dstMem = dstBuf ? AS_D11(Memory, dstBuf->boundMemory) : nullptr;
                    if (!dstMem) continue;

                    // 确保 shadow buffer 存在
                    if (!dstMem->mapped) {
                        dstMem->mapped = calloc(1, (size_t)dstMem->size);
                    }
                    if (!dstMem->mapped) continue;

                    // 在 immediate context 上 Map staging texture
                    D3D11_MAPPED_SUBRESOURCE mapped;
                    HRESULT hr = immediateCtx->Map(rb.stagingTex.Get(), 0,
                        D3D11_MAP_READ, 0, &mapped);
                    if (SUCCEEDED(hr)) {
                        uint32_t dstRowPitch = rb.width * rb.texelSize;
                        uint8_t* dst = static_cast<uint8_t*>(dstMem->mapped) + rb.bufferOffset;
                        uint8_t* src = static_cast<uint8_t*>(mapped.pData);
                        // 逐行复制 (staging texture 的 RowPitch 可能大于实际宽度)
                        for (uint32_t row = 0; row < rb.height; row++) {
                            memcpy(dst + row * dstRowPitch, src + row * mapped.RowPitch, dstRowPitch);
                        }
                        immediateCtx->Unmap(rb.stagingTex.Get(), 0);
                    }
                }
                cmd->pendingReadbacks.clear();
            }
        }
    }

    if (fence) AS_D11(Fence, fence)->signaled = true;

    // 刷新 validation 消息
    flushD3D11Messages(q->device);

    s_frameCount++;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d11_vkQueueWaitIdle(VkQueue queue)
{
    auto* q = AS_D11(Queue, queue);
    if (!q || !q->device) return VK_ERROR_DEVICE_LOST;
    auto* dev = AS_D11(Device, q->device);
    if (dev->immediateContext)
        dev->immediateContext->Flush();
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d11_vkCreateSemaphore(
    VkDevice device,
    const VkSemaphoreCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSemaphore* pSemaphore)
{
    (void)device; (void)pCreateInfo; (void)pAllocator;
    auto* sem = new D11_Semaphore();
    *pSemaphore = TO_VK(VkSemaphore, sem);
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroySemaphore(
    VkDevice device,
    VkSemaphore semaphore,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_D11(Semaphore, semaphore);
}

// ============================================================================
// Render Pass / Framebuffer (存储元数据, D3D11 无等价概念)
// ============================================================================

static VkResult VKAPI_CALL d3d11_vkCreateRenderPass(
    VkDevice device,
    const VkRenderPassCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkRenderPass* pRenderPass)
{
    fprintf(stderr, "[VkD3D11] CreateRenderPass\n");
    (void)device; (void)pAllocator;

    auto* rp = new D11_RenderPass();
    rp->attachments.assign(pCreateInfo->pAttachments,
                           pCreateInfo->pAttachments + pCreateInfo->attachmentCount);

    rp->colorAttachmentCount = 0;
    rp->hasDepth = false;
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; i++) {
        auto fmt = pCreateInfo->pAttachments[i].format;
        if (fmt == VK_FORMAT_D32_SFLOAT || fmt == VK_FORMAT_D24_UNORM_S8_UINT
            || fmt == VK_FORMAT_D16_UNORM) {
            rp->hasDepth = true;
            rp->depthFormat = fmt;
        } else {
            rp->colorAttachmentCount++;
        }
    }

    *pRenderPass = TO_VK(VkRenderPass, rp);
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroyRenderPass(
    VkDevice device,
    VkRenderPass renderPass,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_D11(RenderPass, renderPass);
}

static VkResult VKAPI_CALL d3d11_vkCreateFramebuffer(
    VkDevice device,
    const VkFramebufferCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkFramebuffer* pFramebuffer)
{
    (void)device; (void)pAllocator;

    auto* fb = new D11_Framebuffer();
    fb->width = pCreateInfo->width;
    fb->height = pCreateInfo->height;
    fb->renderPass = pCreateInfo->renderPass;
    fb->attachments.assign(pCreateInfo->pAttachments,
                           pCreateInfo->pAttachments + pCreateInfo->attachmentCount);
    *pFramebuffer = TO_VK(VkFramebuffer, fb);
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroyFramebuffer(
    VkDevice device,
    VkFramebuffer framebuffer,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_D11(Framebuffer, framebuffer);
}

// ============================================================================
// Pipeline
// ============================================================================

static VkResult VKAPI_CALL d3d11_vkCreateShaderModule(
    VkDevice device,
    const VkShaderModuleCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkShaderModule* pShaderModule)
{
    fprintf(stderr, "[VkD3D11] CreateShaderModule size=%zu\n", pCreateInfo->codeSize);
    (void)device; (void)pAllocator;
    auto* sm = new D11_ShaderModule();
    sm->bytecode.assign(reinterpret_cast<const char*>(pCreateInfo->pCode),
                        reinterpret_cast<const char*>(pCreateInfo->pCode) + pCreateInfo->codeSize);

    // 检测 SPIR-V magic → 替换为预编译的 ImGui DXBC
    if (pCreateInfo->codeSize >= 4 && pCreateInfo->pCode[0] == 0x07230203) {
        sm->isImguiReplacement = true;
        if (!s_imguiVS_dxbc.empty() && !s_imguiPS_dxbc.empty()) {
            if (pCreateInfo->codeSize > 1000) {
                // 较大 → VS
                sm->bytecode = s_imguiVS_dxbc;
            } else {
                // 较小 → PS
                sm->bytecode = s_imguiPS_dxbc;
            }
            std::cout << "[VkD3D11] Replaced SPIR-V with ImGui DXBC (" << sm->bytecode.size() << "B)" << std::endl;
        } else {
            std::cerr << "[VkD3D11] Warning: ImGui DXBC not compiled, SPIR-V replacement failed" << std::endl;
        }
    }

    *pShaderModule = TO_VK(VkShaderModule, sm);
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroyShaderModule(
    VkDevice device,
    VkShaderModule shaderModule,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_D11(ShaderModule, shaderModule);
}

static VkResult VKAPI_CALL d3d11_vkCreatePipelineLayout(
    VkDevice device,
    const VkPipelineLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkPipelineLayout* pPipelineLayout)
{
    (void)device; (void)pAllocator;
    auto* layout = new D11_PipelineLayout();
    if (pCreateInfo->setLayoutCount > 0) {
        layout->setLayouts.assign(pCreateInfo->pSetLayouts,
                                  pCreateInfo->pSetLayouts + pCreateInfo->setLayoutCount);
    }
    if (pCreateInfo->pushConstantRangeCount > 0) {
        layout->pushConstantRanges.assign(pCreateInfo->pPushConstantRanges,
                                          pCreateInfo->pPushConstantRanges + pCreateInfo->pushConstantRangeCount);
    }
    // 计算 push constant 总大小
    for (auto& pc : layout->pushConstantRanges)
        layout->pushConstSize = std::max(layout->pushConstSize, pc.offset + pc.size);
    *pPipelineLayout = TO_VK(VkPipelineLayout, layout);
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroyPipelineLayout(
    VkDevice device,
    VkPipelineLayout pipelineLayout,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_D11(PipelineLayout, pipelineLayout);
}

// VkCompareOp -> D3D11_COMPARISON_FUNC
static D3D11_COMPARISON_FUNC vkCompareOpToD3D11(VkCompareOp op) {
    switch (op) {
    case VK_COMPARE_OP_NEVER:            return D3D11_COMPARISON_NEVER;
    case VK_COMPARE_OP_LESS:             return D3D11_COMPARISON_LESS;
    case VK_COMPARE_OP_EQUAL:            return D3D11_COMPARISON_EQUAL;
    case VK_COMPARE_OP_LESS_OR_EQUAL:    return D3D11_COMPARISON_LESS_EQUAL;
    case VK_COMPARE_OP_GREATER:          return D3D11_COMPARISON_GREATER;
    case VK_COMPARE_OP_NOT_EQUAL:        return D3D11_COMPARISON_NOT_EQUAL;
    case VK_COMPARE_OP_GREATER_OR_EQUAL: return D3D11_COMPARISON_GREATER_EQUAL;
    case VK_COMPARE_OP_ALWAYS:           return D3D11_COMPARISON_ALWAYS;
    default:                             return D3D11_COMPARISON_LESS;
    }
}

static VkResult VKAPI_CALL d3d11_vkCreateGraphicsPipelines(
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines)
{
    fprintf(stderr, "[VkD3D11] CreateGraphicsPipelines\n");
    (void)pipelineCache; (void)pAllocator;
    auto* dev = AS_D11(Device, device);

    for (uint32_t i = 0; i < createInfoCount; i++) {
        auto& ci = pCreateInfos[i];
        auto* pipeline = new D11_Pipeline();
        pipeline->layout = ci.layout;

        // 检测是否有 ImGui 替换 shader
        bool isImguiPipeline = false;
        for (uint32_t s = 0; s < ci.stageCount; s++) {
            auto* sm = AS_D11(ShaderModule, ci.pStages[s].module);
            if (sm && sm->isImguiReplacement)
                isImguiPipeline = true;
        }

        // === 提取 VS / PS 字节码 ===
        const void* vsData = nullptr; size_t vsSize = 0;
        const void* psData = nullptr; size_t psSize = 0;
        for (uint32_t s = 0; s < ci.stageCount; s++) {
            auto& stage = ci.pStages[s];
            auto* sm = AS_D11(ShaderModule, stage.module);
            if (!sm) continue;
            if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT) {
                vsData = sm->bytecode.data();
                vsSize = sm->bytecode.size();
                pipeline->vsBytecode = sm->bytecode;
            } else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
                psData = sm->bytecode.data();
                psSize = sm->bytecode.size();
            }
        }

        // === 创建 VS ===
        if (vsData && vsSize > 0) {
            HRESULT hr = dev->device->CreateVertexShader(vsData, vsSize, nullptr, &pipeline->vertexShader);
            if (FAILED(hr)) {
                fprintf(stderr, "[VkD3D11] CreateVertexShader FAILED hr=0x%lx size=%zu imgui=%d\n",
                        hr, vsSize, isImguiPipeline ? 1 : 0);
            }
        }

        // === 创建 PS ===
        if (psData && psSize > 0) {
            HRESULT hr = dev->device->CreatePixelShader(psData, psSize, nullptr, &pipeline->pixelShader);
            if (FAILED(hr)) {
                fprintf(stderr, "[VkD3D11] CreatePixelShader FAILED hr=0x%lx size=%zu imgui=%d\n",
                        hr, psSize, isImguiPipeline ? 1 : 0);
            }
        }

        // === Vertex Input → InputLayout ===
        if (ci.pVertexInputState && vsData && vsSize > 0) {
            // 存储 binding stride
            for (uint32_t b = 0; b < ci.pVertexInputState->vertexBindingDescriptionCount; b++) {
                auto& binding = ci.pVertexInputState->pVertexBindingDescriptions[b];
                if (binding.binding < 8) {
                    pipeline->vertexStrides[binding.binding] = binding.stride;
                    if (binding.binding + 1 > pipeline->vertexBindingCount)
                        pipeline->vertexBindingCount = binding.binding + 1;
                }
            }

            // 语义名约定:
            // - ImGui DXBC: TEXCOORD{N} (location 0,1,2 -> TEXCOORD0,1,2)
            // - 引擎 Slang DXBC: POSITION/COLOR/TEXCOORD/NORMAL
            static const char* engineSemantics[] = {
                "POSITION", "COLOR", "TEXCOORD", "NORMAL",
                "TANGENT", "BINORMAL", "BLENDWEIGHT", "BLENDINDICES"
            };

            std::vector<D3D11_INPUT_ELEMENT_DESC> inputLayout;
            for (uint32_t a = 0; a < ci.pVertexInputState->vertexAttributeDescriptionCount; a++) {
                auto& attr = ci.pVertexInputState->pVertexAttributeDescriptions[a];
                D3D11_INPUT_ELEMENT_DESC elem = {};
                if (isImguiPipeline) {
                    elem.SemanticName = "TEXCOORD";
                    elem.SemanticIndex = attr.location;
                } else {
                    elem.SemanticName = (attr.location < 8) ? engineSemantics[attr.location] : "TEXCOORD";
                    elem.SemanticIndex = (attr.location >= 4) ? attr.location - 4 : 0;
                }
                elem.Format = vkFormatToDxgi(attr.format);
                elem.InputSlot = attr.binding;
                elem.AlignedByteOffset = attr.offset;
                elem.InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
                inputLayout.push_back(elem);
            }

            if (!inputLayout.empty()) {
                HRESULT hr = dev->device->CreateInputLayout(
                    inputLayout.data(), static_cast<UINT>(inputLayout.size()),
                    vsData, vsSize, &pipeline->inputLayout);
                if (FAILED(hr)) {
                    fprintf(stderr, "[VkD3D11] CreateInputLayout FAILED hr=0x%lx elems=%zu imgui=%d\n",
                            hr, inputLayout.size(), isImguiPipeline ? 1 : 0);
                }
            }
        }

        // === Rasterizer State ===
        {
            D3D11_RASTERIZER_DESC raster = {};
            raster.FillMode = D3D11_FILL_SOLID;
            raster.CullMode = D3D11_CULL_NONE;
            raster.DepthClipEnable = TRUE;
            raster.FrontCounterClockwise = FALSE;
            raster.ScissorEnable = TRUE; // 启用 scissor test

            if (ci.pRasterizationState) {
                raster.FillMode = (ci.pRasterizationState->polygonMode == VK_POLYGON_MODE_LINE)
                    ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
                if (ci.pRasterizationState->cullMode == VK_CULL_MODE_BACK_BIT)
                    raster.CullMode = D3D11_CULL_BACK;
                else if (ci.pRasterizationState->cullMode == VK_CULL_MODE_FRONT_BIT)
                    raster.CullMode = D3D11_CULL_FRONT;
                else
                    raster.CullMode = D3D11_CULL_NONE;
                raster.FrontCounterClockwise = (ci.pRasterizationState->frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE);
                if (ci.pRasterizationState->depthBiasEnable) {
                    raster.DepthBias = static_cast<INT>(ci.pRasterizationState->depthBiasConstantFactor);
                    raster.SlopeScaledDepthBias = ci.pRasterizationState->depthBiasSlopeFactor;
                    raster.DepthBiasClamp = ci.pRasterizationState->depthBiasClamp;
                }
            }

            dev->device->CreateRasterizerState(&raster, &pipeline->rasterizerState);
        }

        // === Depth Stencil State ===
        {
            D3D11_DEPTH_STENCIL_DESC depthStencil = {};
            if (ci.pDepthStencilState) {
                depthStencil.DepthEnable = ci.pDepthStencilState->depthTestEnable;
                depthStencil.DepthWriteMask = ci.pDepthStencilState->depthWriteEnable
                    ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
                depthStencil.DepthFunc = vkCompareOpToD3D11(ci.pDepthStencilState->depthCompareOp);
                depthStencil.StencilEnable = ci.pDepthStencilState->stencilTestEnable;
            }

            dev->device->CreateDepthStencilState(&depthStencil, &pipeline->depthStencilState);
        }

        // === Blend State ===
        {
            D3D11_BLEND_DESC blend = {};
            blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
            blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;

            if (ci.pColorBlendState && ci.pColorBlendState->attachmentCount > 0) {
                auto& att = ci.pColorBlendState->pAttachments[0];
                blend.RenderTarget[0].BlendEnable = att.blendEnable;
                blend.RenderTarget[0].RenderTargetWriteMask = att.colorWriteMask & 0xF;
                if (att.blendEnable) {
                    auto vkBlendToD3D = [](VkBlendFactor f) -> D3D11_BLEND {
                        switch (f) {
                        case VK_BLEND_FACTOR_ZERO:                return D3D11_BLEND_ZERO;
                        case VK_BLEND_FACTOR_ONE:                 return D3D11_BLEND_ONE;
                        case VK_BLEND_FACTOR_SRC_ALPHA:           return D3D11_BLEND_SRC_ALPHA;
                        case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return D3D11_BLEND_INV_SRC_ALPHA;
                        case VK_BLEND_FACTOR_DST_ALPHA:           return D3D11_BLEND_DEST_ALPHA;
                        case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
                        case VK_BLEND_FACTOR_SRC_COLOR:           return D3D11_BLEND_SRC_COLOR;
                        case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return D3D11_BLEND_INV_SRC_COLOR;
                        case VK_BLEND_FACTOR_DST_COLOR:           return D3D11_BLEND_DEST_COLOR;
                        case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return D3D11_BLEND_INV_DEST_COLOR;
                        default: return D3D11_BLEND_ONE;
                        }
                    };
                    auto vkBlendOpToD3D = [](VkBlendOp op) -> D3D11_BLEND_OP {
                        switch (op) {
                        case VK_BLEND_OP_ADD:              return D3D11_BLEND_OP_ADD;
                        case VK_BLEND_OP_SUBTRACT:         return D3D11_BLEND_OP_SUBTRACT;
                        case VK_BLEND_OP_REVERSE_SUBTRACT: return D3D11_BLEND_OP_REV_SUBTRACT;
                        case VK_BLEND_OP_MIN:              return D3D11_BLEND_OP_MIN;
                        case VK_BLEND_OP_MAX:              return D3D11_BLEND_OP_MAX;
                        default: return D3D11_BLEND_OP_ADD;
                        }
                    };
                    blend.RenderTarget[0].SrcBlend = vkBlendToD3D(att.srcColorBlendFactor);
                    blend.RenderTarget[0].DestBlend = vkBlendToD3D(att.dstColorBlendFactor);
                    blend.RenderTarget[0].BlendOp = vkBlendOpToD3D(att.colorBlendOp);
                    blend.RenderTarget[0].SrcBlendAlpha = vkBlendToD3D(att.srcAlphaBlendFactor);
                    blend.RenderTarget[0].DestBlendAlpha = vkBlendToD3D(att.dstAlphaBlendFactor);
                    blend.RenderTarget[0].BlendOpAlpha = vkBlendOpToD3D(att.alphaBlendOp);
                }
                // 存储 blend constants
                if (ci.pColorBlendState->blendConstants) {
                    memcpy(pipeline->blendFactor, ci.pColorBlendState->blendConstants, sizeof(float) * 4);
                }
            }

            dev->device->CreateBlendState(&blend, &pipeline->blendState);
        }

        // === Topology ===
        pipeline->topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        if (ci.pInputAssemblyState) {
            switch (ci.pInputAssemblyState->topology) {
            case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
                pipeline->topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
                break;
            case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
                pipeline->topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
                break;
            case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
                pipeline->topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
                break;
            default:
                break;
            }
        }

        // 解析 PS DXBC RDEF，提取实际的 SRV/Sampler register 索引
        if (psData && psSize > 0 && !isImguiPipeline) {
            parseDxbcRDEF(psData, psSize, pipeline->srvRegisters, pipeline->samplerRegisters);
        }

        fprintf(stderr, "[VkD3D11] Pipeline: VS=%p PS=%p IL=%p RS=%p DS=%p BS=%p imgui=%d\n",
            pipeline->vertexShader.Get(), pipeline->pixelShader.Get(),
            pipeline->inputLayout.Get(), pipeline->rasterizerState.Get(),
            pipeline->depthStencilState.Get(), pipeline->blendState.Get(),
            isImguiPipeline ? 1 : 0);
        pPipelines[i] = TO_VK(VkPipeline, pipeline);
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroyPipeline(
    VkDevice device,
    VkPipeline pipeline,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_D11(Pipeline, pipeline);
}

// ============================================================================
// Descriptor
// ============================================================================

static VkResult VKAPI_CALL d3d11_vkCreateDescriptorPool(
    VkDevice device,
    const VkDescriptorPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorPool* pDescriptorPool)
{
    (void)pCreateInfo; (void)pAllocator;
    auto* pool = new D11_DescriptorPool();
    pool->device = device;
    *pDescriptorPool = TO_VK(VkDescriptorPool, pool);
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroyDescriptorPool(
    VkDevice device,
    VkDescriptorPool descriptorPool,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_D11(DescriptorPool, descriptorPool);
}

static VkResult VKAPI_CALL d3d11_vkCreateDescriptorSetLayout(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorSetLayout* pSetLayout)
{
    (void)device; (void)pAllocator;
    auto* layout = new D11_DescriptorSetLayout();
    layout->bindings.assign(pCreateInfo->pBindings,
                            pCreateInfo->pBindings + pCreateInfo->bindingCount);
    *pSetLayout = TO_VK(VkDescriptorSetLayout, layout);
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroyDescriptorSetLayout(
    VkDevice device,
    VkDescriptorSetLayout descriptorSetLayout,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_D11(DescriptorSetLayout, descriptorSetLayout);
}

static VkResult VKAPI_CALL d3d11_vkAllocateDescriptorSets(
    VkDevice device,
    const VkDescriptorSetAllocateInfo* pAllocateInfo,
    VkDescriptorSet* pDescriptorSets)
{
    (void)device;
    for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; i++) {
        auto* set = new D11_DescriptorSet();
        set->layout = pAllocateInfo->pSetLayouts[i];
        // D3D11: 不需要 heap 分配，直接持有 COM 指针
        pDescriptorSets[i] = TO_VK(VkDescriptorSet, set);
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d11_vkFreeDescriptorSets(
    VkDevice device,
    VkDescriptorPool descriptorPool,
    uint32_t descriptorSetCount,
    const VkDescriptorSet* pDescriptorSets)
{
    (void)device; (void)descriptorPool;
    for (uint32_t i = 0; i < descriptorSetCount; i++) {
        delete AS_D11(DescriptorSet, pDescriptorSets[i]);
    }
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkUpdateDescriptorSets(
    VkDevice device,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites,
    uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet* pDescriptorCopies)
{
    (void)device; (void)descriptorCopyCount; (void)pDescriptorCopies;

    for (uint32_t w = 0; w < descriptorWriteCount; w++) {
        auto& write = pDescriptorWrites[w];
        auto* set = AS_D11(DescriptorSet, write.dstSet);
        if (!set) continue;

        for (uint32_t d = 0; d < write.descriptorCount; d++) {
            uint32_t bindingIdx = write.dstBinding + d;

            if (write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                // UBO → 记录 buffer 指针 + VkBuffer (用于 shadow buffer 同步)
                if (write.pBufferInfo) {
                    auto& bufInfo = write.pBufferInfo[d];
                    auto* buf = AS_D11(Buffer, bufInfo.buffer);
                    if (buf && buf->buffer) {
                        if (bindingIdx < 8) {
                            set->cbvBuffers[bindingIdx] = buf->buffer.Get();
                            set->cbvVkBuffers[bindingIdx] = bufInfo.buffer;
                            if (bindingIdx + 1 > set->cbvCount) set->cbvCount = bindingIdx + 1;
                        }
                    }
                }
            } else if (write.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                       write.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                // Texture → SRV + Sampler
                if (write.pImageInfo) {
                    auto& imgInfo = write.pImageInfo[d];
                    auto* iv = AS_D11(ImageView, imgInfo.imageView);
                    if (iv && iv->hasSrv) {
                        if (bindingIdx < 8) {
                            set->srvs[bindingIdx] = iv->srv.Get();
                            if (bindingIdx + 1 > set->srvCount) set->srvCount = bindingIdx + 1;
                        }
                    }
                    // 如果是 combined image sampler 且有 sampler
                    auto* samp = AS_D11(Sampler, imgInfo.sampler);
                    if (samp && samp->sampler) {
                        if (bindingIdx < 8) {
                            set->samplers[bindingIdx] = samp->sampler.Get();
                        }
                    }
                }
            }
        }
    }
}

// ============================================================================
// Swapchain
// ============================================================================

static VkResult VKAPI_CALL d3d11_vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain)
{
    (void)pAllocator;
    d3d11_log("CreateSwapchain ENTER");
    d3d11_heap_check("before CreateSwapchain");
    if (!device || !pCreateInfo || !pCreateInfo->surface) return VK_ERROR_INITIALIZATION_FAILED;
    auto* dev = AS_D11(Device, device);
    auto* pd = AS_D11(PhysicalDevice, dev->physicalDevice);
    auto* inst = AS_D11(Instance, pd->instance);
    auto* surface = AS_D11(Surface, pCreateInfo->surface);

    auto* sc = new D11_Swapchain();
    uint32_t bufferCount = std::max(pCreateInfo->minImageCount, 2u);

    sc->width = pCreateInfo->imageExtent.width;
    sc->height = pCreateInfo->imageExtent.height;
    sc->format = vkFormatToDxgi(pCreateInfo->imageFormat);
    sc->imageCount = bufferCount;

    // DXGI flip-model 只支持 UNORM，SRGB 通过 RTV view 实现
    DXGI_FORMAT swapChainFormat = sc->format;
    if (swapChainFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
        swapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    else if (swapChainFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
        swapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    // 创建 DXGI SwapChain
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = sc->width;
    desc.Height = sc->height;
    desc.Format = swapChainFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = bufferCount;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    desc.BufferCount = 1;  // DISCARD 模式只需 1 个 back buffer

    IDXGIFactory4* factory = inst->factory.Get();
    d3d11_heap_check("before CreateSwapChainForHwnd");

    // D3D11: 用 device 而非 command queue 创建 swap chain
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = factory->CreateSwapChainForHwnd(
        dev->device.Get(), surface->hwnd,
        &desc, nullptr, nullptr, &sc1);
    fprintf(stderr, "[VkD3D11] CreateSwapChainForHwnd hr=0x%lx\n", hr);
    if (FAILED(hr)) {
        fprintf(stderr, "[VkD3D11] CreateSwapChainForHwnd FAILED hr=0x%lx\n", hr);
        delete sc;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    factory->MakeWindowAssociation(surface->hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc->swapChain = sc1;
    d3d11_heap_check("after CreateSwapChainForHwnd");

    // 获取 back buffer 并创建 RTV + VkImage 句柄
    d3d11_log("resize images");
    // D3D11 DISCARD: 只有 1 个 back buffer, 所有 image handle 指向同一个
    sc->images.resize(1);
    sc->rtvs.resize(1);
    sc->imageHandles.resize(bufferCount);  // 引擎需要按 imageCount 索引

    hr = sc->swapChain->GetBuffer(0, IID_PPV_ARGS(&sc->images[0]));
    if (FAILED(hr)) {
        fprintf(stderr, "[VkD3D11] GetBuffer(0) FAILED hr=0x%lx\n", hr);
        delete sc;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // 创建 RTV
    if (sc->format != swapChainFormat) {
        D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = sc->format;
        rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        dev->device->CreateRenderTargetView(sc->images[0].Get(), &rtvDesc, &sc->rtvs[0]);
    } else {
        dev->device->CreateRenderTargetView(sc->images[0].Get(), nullptr, &sc->rtvs[0]);
    }

    // 所有 image handle 指向同一个 buffer
    for (uint32_t i = 0; i < bufferCount; i++) {
        auto* img = new D11_Image();
        img->texture = sc->images[0];
        img->format = pCreateInfo->imageFormat;
        img->width = sc->width;
        img->height = sc->height;
        img->mipLevels = 1;
        img->usage = pCreateInfo->imageUsage;
        img->ownsResource = false;
        img->currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        sc->imageHandles[i] = TO_VK(VkImage, img);
    }

    sc->currentIndex = 0;
    *pSwapchain = TO_VK(VkSwapchainKHR, sc);
    std::cout << "[VkD3D11] SwapChain created (" << sc->width << "x" << sc->height
              << ", " << bufferCount << " buffers)" << std::endl;
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!swapchain) return;
    auto* sc = AS_D11(Swapchain, swapchain);
    for (auto imgHandle : sc->imageHandles) delete AS_D11(Image, imgHandle);
    delete sc;
}

static VkResult VKAPI_CALL d3d11_vkGetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages)
{
    (void)device;
    auto* sc = AS_D11(Swapchain, swapchain);
    if (!pSwapchainImages) {
        *pSwapchainImageCount = sc->imageCount;
        return VK_SUCCESS;
    }
    uint32_t toWrite = std::min(*pSwapchainImageCount, sc->imageCount);
    for (uint32_t i = 0; i < toWrite; i++) {
        pSwapchainImages[i] = sc->imageHandles[i];
    }
    *pSwapchainImageCount = toWrite;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d11_vkAcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* pImageIndex)
{
    (void)device; (void)timeout; (void)semaphore;
    auto* sc = AS_D11(Swapchain, swapchain);
    if (!sc || !sc->swapChain) return VK_ERROR_SURFACE_LOST_KHR;

    // D3D11 flip-model: GetCurrentBackBufferIndex 不存在于 IDXGISwapChain1
    // 使用循环索引
    *pImageIndex = sc->currentIndex;

    // 信号 fence (D3D11 中 acquire 是立即的)
    if (fence) AS_D11(Fence, fence)->signaled = true;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL d3d11_vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo)
{
    (void)queue;
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        auto* sc = AS_D11(Swapchain, pPresentInfo->pSwapchains[i]);
        if (sc && sc->swapChain) {
            HRESULT hr = sc->swapChain->Present(1, 0);
            if (FAILED(hr)) {
                fprintf(stderr, "[VkD3D11] Present FAILED hr=0x%lx\n", hr);
            }
            // 更新 currentIndex 循环
            sc->currentIndex = (sc->currentIndex + 1) % sc->imageCount;
        }
    }
    return VK_SUCCESS;
}

// ============================================================================
// Command Recording
// ============================================================================

static void VKAPI_CALL d3d11_vkCmdBeginRenderPass(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo* pRenderPassBegin,
    VkSubpassContents contents)
{
    (void)contents;
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);
    if (!cmd || !cmd->deferredContext) return;

    auto* rp = AS_D11(RenderPass, pRenderPassBegin->renderPass);
    auto* fb = AS_D11(Framebuffer, pRenderPassBegin->framebuffer);
    cmd->currentRenderPass = pRenderPassBegin->renderPass;
    cmd->currentFramebuffer = pRenderPassBegin->framebuffer;
    if (!rp || !fb) return;

    auto* ctx = cmd->deferredContext.Get();

    // 收集 RTV 和 DSV
    ID3D11RenderTargetView* rtvs[8] = {};
    uint32_t rtvCount = 0;
    ID3D11DepthStencilView* dsv = nullptr;

    for (size_t i = 0; i < fb->attachments.size(); i++) {
        auto* view = AS_D11(ImageView, fb->attachments[i]);
        if (!view) continue;
        if (view->hasRtv && view->rtv) {
            rtvs[rtvCount++] = view->rtv.Get();
        } else if (view->hasDsv && view->dsv) {
            dsv = view->dsv.Get();
        }
    }

    // 设置 render targets
    ctx->OMSetRenderTargets(rtvCount, rtvCount > 0 ? rtvs : nullptr, dsv);

    D3D11_TRACE("BeginRenderPass: rtvCount=%u dsv=%p fb=%ux%u", rtvCount, dsv, fb->width, fb->height);
    for (uint32_t ri = 0; ri < rtvCount; ri++)
        D3D11_TRACE("  RTV[%u]=%p", ri, rtvs[ri]);

    // 根据 render pass 的 loadOp 执行 clear
    // DONT_CARE 也需要 clear 以防止跨帧数据累积 (bloom 等后处理)
    uint32_t clearIdx = 0;
    for (size_t i = 0; i < fb->attachments.size() && i < rp->attachments.size(); i++) {
        auto* view = AS_D11(ImageView, fb->attachments[i]);
        if (!view) continue;
        auto& att = rp->attachments[i];

        if (view->hasRtv && view->rtv && att.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            if (clearIdx < pRenderPassBegin->clearValueCount) {
                auto& cv = pRenderPassBegin->pClearValues[clearIdx];
                ctx->ClearRenderTargetView(view->rtv.Get(), cv.color.float32);
            }
        } else if (view->hasRtv && view->rtv && att.loadOp == VK_ATTACHMENT_LOAD_OP_DONT_CARE) {
            // DONT_CARE: Vulkan 保证丢弃旧内容，D3D11 需要显式 clear 为黑色
            const float black[4] = {0, 0, 0, 0};
            ctx->ClearRenderTargetView(view->rtv.Get(), black);
        }
        if (view->hasDsv && view->dsv && att.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
            if (clearIdx < pRenderPassBegin->clearValueCount) {
                auto& cv = pRenderPassBegin->pClearValues[clearIdx];
                ctx->ClearDepthStencilView(view->dsv.Get(), D3D11_CLEAR_DEPTH,
                    cv.depthStencil.depth, cv.depthStencil.stencil);
            }
        } else if (view->hasDsv && view->dsv && att.loadOp == VK_ATTACHMENT_LOAD_OP_DONT_CARE) {
            ctx->ClearDepthStencilView(view->dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        }
        clearIdx++;
    }

    // 设置 viewport 和 scissor
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(fb->width);
    vp.Height = static_cast<float>(fb->height);
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    D3D11_RECT scissor = {0, 0, (LONG)fb->width, (LONG)fb->height};
    ctx->RSSetScissorRects(1, &scissor);
}

static void VKAPI_CALL d3d11_vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
{
    if (!commandBuffer) return;
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);

    // D3D11 不需要 layout barrier，只记录 layout 变化
    auto* rp = AS_D11(RenderPass, cmd->currentRenderPass);
    auto* fb = AS_D11(Framebuffer, cmd->currentFramebuffer);
    if (rp && fb) {
        for (size_t i = 0; i < fb->attachments.size() && i < rp->attachments.size(); i++) {
            auto* view = AS_D11(ImageView, fb->attachments[i]);
            if (!view || !view->image) continue;
            AS_D11(Image, view->image)->currentLayout = rp->attachments[i].finalLayout;
        }
    }

    cmd->currentRenderPass = VK_NULL_HANDLE;
    cmd->currentFramebuffer = VK_NULL_HANDLE;

    // 解除 render target 绑定 (防止 resource hazard)
    if (cmd->deferredContext) {
        ID3D11RenderTargetView* nullRtvs[8] = {};
        cmd->deferredContext->OMSetRenderTargets(8, nullRtvs, nullptr);
    }
}

// ---------------------------------------------------------------------------
// 延迟绑定: BindPipeline / BindDescriptorSets 只记录状态,
// flushGraphicsState() 在 Draw 前统一提交到 D3D11
// ---------------------------------------------------------------------------

static void VKAPI_CALL d3d11_vkCmdBindPipeline(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipeline pipeline)
{
    (void)pipelineBindPoint;
    if (!commandBuffer || !pipeline) return;
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);
    cmd->currentPipeline = pipeline;
    cmd->stateDirty = true;
}

static void VKAPI_CALL d3d11_vkCmdBindDescriptorSets(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout,
    uint32_t firstSet,
    uint32_t descriptorSetCount,
    const VkDescriptorSet* pDescriptorSets,
    uint32_t dynamicOffsetCount,
    const uint32_t* pDynamicOffsets)
{
    (void)pipelineBindPoint; (void)layout; (void)dynamicOffsetCount; (void)pDynamicOffsets;
    if (!commandBuffer) return;
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);

    for (uint32_t i = 0; i < descriptorSetCount; i++) {
        uint32_t setIdx = firstSet + i;
        if (setIdx < 4) {
            cmd->boundSets[setIdx] = pDescriptorSets[i];
        }
    }
    cmd->stateDirty = true;
}

// 在 Draw 前提交所有 D3D11 状态
static void flushGraphicsState(D11_CommandBuffer* cmd)
{
    if (!cmd->stateDirty) return;
    cmd->stateDirty = false;

    auto* ctx = cmd->deferredContext.Get();
    auto* pipeline = AS_D11(Pipeline, cmd->currentPipeline);
    if (!ctx || !pipeline) {
        D3D11_TRACE("flushGraphicsState: SKIP ctx=%p pipeline=%p", ctx, (void*)cmd->currentPipeline);
        return;
    }

    D3D11_TRACE("flushGraphicsState: VS=%p PS=%p IL=%p",
        pipeline->vertexShader.Get(), pipeline->pixelShader.Get(), pipeline->inputLayout.Get());

    // 1. 设置 shader 状态对象
    ctx->VSSetShader(pipeline->vertexShader.Get(), nullptr, 0);
    ctx->PSSetShader(pipeline->pixelShader.Get(), nullptr, 0);
    ctx->IASetInputLayout(pipeline->inputLayout.Get());
    ctx->IASetPrimitiveTopology(pipeline->topology);
    ctx->RSSetState(pipeline->rasterizerState.Get());
    ctx->OMSetDepthStencilState(pipeline->depthStencilState.Get(), pipeline->stencilRef);
    ctx->OMSetBlendState(pipeline->blendState.Get(), pipeline->blendFactor, 0xFFFFFFFF);

    auto* pl = AS_D11(PipelineLayout, pipeline->layout);
    if (!pl) return;

    // 2. 绑定 descriptor set 中的资源
    // 使用 binding index 直接作为 register slot，
    // 因为 Slang DXBC 编译器按 (set, binding) 顺序分配 register，
    // CBV/SRV/Sampler 各自独立编号，跨 set 累加。
    uint32_t cbvSlot = 0;
    uint32_t srvSlot = 0;
    uint32_t samplerSlot = 0;

    for (uint32_t s = 0; s < pl->setLayouts.size() && s < 4; s++) {
        auto* setLayout = AS_D11(DescriptorSetLayout, pl->setLayouts[s]);
        if (!setLayout) continue;
        auto* set = AS_D11(DescriptorSet, cmd->boundSets[s]);

        // 先统计本 set 中每种资源的最大 slot 偏移
        // Slang DXBC 的 register 分配规则:
        //   对每种资源类型 (CBV, SRV, Sampler) 在所有 set 中顺序编号
        //   set 内按 binding 顺序
        for (auto& binding : setLayout->bindings) {
            if (binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                // UBO → 绑定到 cbvSlot
                if (set && binding.binding < 8 && set->cbvBuffers[binding.binding]) {
                    ID3D11Buffer* buf = set->cbvBuffers[binding.binding];
                    ctx->VSSetConstantBuffers(cbvSlot, 1, &buf);
                    ctx->PSSetConstantBuffers(cbvSlot, 1, &buf);
                    D3D11_TRACE("  CBV b%u = %p (set%u bind%u)", cbvSlot, buf, s, binding.binding);
                }
                cbvSlot++;
            } else if (binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                       binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                // SRV + Sampler: 使用 RDEF 解析出的实际 register 索引
                for (uint32_t d = 0; d < binding.descriptorCount; d++) {
                    uint32_t bIdx = binding.binding + d;
                    // RDEF 中第 srvSlot 个 SRV 的实际 register
                    uint32_t actualSrv = (srvSlot < pipeline->srvRegisters.size())
                        ? pipeline->srvRegisters[srvSlot] : srvSlot;
                    uint32_t actualSamp = (samplerSlot < pipeline->samplerRegisters.size())
                        ? pipeline->samplerRegisters[samplerSlot] : samplerSlot;

                    if (set && bIdx < 8) {
                        if (set->srvs[bIdx]) {
                            ctx->PSSetShaderResources(actualSrv, 1, &set->srvs[bIdx]);
                            D3D11_TRACE("  SRV t%u = %p (set%u bind%u)", actualSrv, set->srvs[bIdx], s, bIdx);
                        }
                        if (set->samplers[bIdx]) {
                            ctx->PSSetSamplers(actualSamp, 1, &set->samplers[bIdx]);
                        }
                    }
                    srvSlot++;
                    samplerSlot++;
                }
            } else if (binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) {
                for (uint32_t d = 0; d < binding.descriptorCount; d++) {
                    uint32_t bIdx = binding.binding + d;
                    uint32_t actualSamp = (samplerSlot < pipeline->samplerRegisters.size())
                        ? pipeline->samplerRegisters[samplerSlot] : samplerSlot;
                    if (set && bIdx < 8 && set->samplers[bIdx]) {
                        ctx->PSSetSamplers(actualSamp, 1, &set->samplers[bIdx]);
                    }
                    samplerSlot++;
                }
            }
        }
    }

    // 3. Push constants → 专用 constant buffer (紧跟 UBO 后的 register)
    if (cmd->pushConstantSize > 0 && cmd->pushConstantBuffer) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = ctx->Map(cmd->pushConstantBuffer.Get(), 0,
                               D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped.pData, cmd->pushConstantData, cmd->pushConstantSize);
            ctx->Unmap(cmd->pushConstantBuffer.Get(), 0);
        }

        ID3D11Buffer* pcBuf = cmd->pushConstantBuffer.Get();
        ctx->VSSetConstantBuffers(cbvSlot, 1, &pcBuf);
        ctx->PSSetConstantBuffers(cbvSlot, 1, &pcBuf);
        D3D11_TRACE("  PushConst b%u = %p (%uB)", cbvSlot, pcBuf, cmd->pushConstantSize);
    }
}

// 在 draw 前同步所有绑定的 UBO shadow buffer 到 D3D11 buffer
// 通过 VkDescriptorSet_T 中的 cbvVkBuffers 追踪原始 VkBuffer,
// 从而访问 boundMemory->mapped (shadow buffer)
static void syncUBOShadowBuffers(D11_CommandBuffer* cmd)
{
    if (!cmd || !cmd->deferredContext) return;
    auto* ctx = cmd->deferredContext.Get();
    auto* pipeline = AS_D11(Pipeline, cmd->currentPipeline);
    if (!pipeline || !pipeline->layout) return;

    auto* pl = AS_D11(PipelineLayout, pipeline->layout);
    for (uint32_t s = 0; s < pl->setLayouts.size() && s < 4; s++) {
        auto* setLayout = AS_D11(DescriptorSetLayout, pl->setLayouts[s]);
        if (!setLayout) continue;
        auto* set = AS_D11(DescriptorSet, cmd->boundSets[s]);
        if (!set) continue;

        for (auto& binding : setLayout->bindings) {
            if (binding.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) continue;
            if (binding.binding >= 8) continue;

            ID3D11Buffer* d3dBuf = set->cbvBuffers[binding.binding];
            VkBuffer vkBuf = set->cbvVkBuffers[binding.binding];
            if (!d3dBuf || !vkBuf) continue;
            auto* buf = AS_D11(Buffer, vkBuf);
            auto* mem = AS_D11(Memory, buf->boundMemory);

            // 通过 VkBuffer 找到 shadow buffer
            if (mem && mem->mapped &&
                buf->d3dUsage == D3D11_USAGE_DYNAMIC) {
                // Map/WRITE_DISCARD + memcpy 从 shadow buffer
                D3D11_MAPPED_SUBRESOURCE mapped;
                HRESULT hr = ctx->Map(d3dBuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
                if (SUCCEEDED(hr)) {
                    memcpy(mapped.pData, mem->mapped, (size_t)buf->size);
                    ctx->Unmap(d3dBuf, 0);
                }
            }
        }
    }
}

static void VKAPI_CALL d3d11_vkCmdBindVertexBuffers(
    VkCommandBuffer commandBuffer,
    uint32_t firstBinding,
    uint32_t bindingCount,
    const VkBuffer* pBuffers,
    const VkDeviceSize* pOffsets)
{
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);
    if (!cmd || !cmd->deferredContext) return;
    auto* ctx = cmd->deferredContext.Get();

    for (uint32_t i = 0; i < bindingCount; i++) {
        uint32_t slot = firstBinding + i;
        auto* vb = AS_D11(Buffer, pBuffers[i]);
        if (vb && vb->buffer) {
            ID3D11Buffer* buf = vb->buffer.Get();
            UINT stride = 0;
            auto* pip = AS_D11(Pipeline, cmd->currentPipeline);
            if (pip && slot < 8)
                stride = pip->vertexStrides[slot];
            UINT offset = static_cast<UINT>(pOffsets[i]);

            // 如果是 DYNAMIC buffer 且有 shadow buffer，先同步数据
            auto* mem = AS_D11(Memory, vb->boundMemory);
            if (vb->d3dUsage == D3D11_USAGE_DYNAMIC && mem && mem->mapped) {
                D3D11_MAPPED_SUBRESOURCE mapped;
                HRESULT hr = ctx->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
                if (SUCCEEDED(hr)) {
                    memcpy(mapped.pData, mem->mapped, (size_t)vb->size);
                    ctx->Unmap(buf, 0);
                }
            }

            ctx->IASetVertexBuffers(slot, 1, &buf, &stride, &offset);
        }
    }
}

static void VKAPI_CALL d3d11_vkCmdBindIndexBuffer(
    VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkIndexType indexType)
{
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);
    auto* buf = AS_D11(Buffer, buffer);
    if (!cmd || !cmd->deferredContext || !buf || !buf->buffer) return;

    // 如果是 DYNAMIC buffer 且有 shadow buffer，先同步数据
    auto* mem = AS_D11(Memory, buf->boundMemory);
    if (buf->d3dUsage == D3D11_USAGE_DYNAMIC && mem && mem->mapped) {
        auto* ctx = cmd->deferredContext.Get();
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = ctx->Map(buf->buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped.pData, mem->mapped, (size_t)buf->size);
            ctx->Unmap(buf->buffer.Get(), 0);
        }
    }

    DXGI_FORMAT fmt = (indexType == VK_INDEX_TYPE_UINT32)
        ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    cmd->deferredContext->IASetIndexBuffer(
        buf->buffer.Get(), fmt, static_cast<UINT>(offset));
}

static void VKAPI_CALL d3d11_vkCmdDraw(
    VkCommandBuffer commandBuffer,
    uint32_t vertexCount,
    uint32_t instanceCount,
    uint32_t firstVertex,
    uint32_t firstInstance)
{
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);
    if (!cmd || !cmd->deferredContext) return;
    if (!cmd->currentPipeline) return;

    // 同步 UBO shadow buffers
    syncUBOShadowBuffers(cmd);
    flushGraphicsState(cmd);

    D3D11_TRACE("Draw: verts=%u inst=%u firstV=%u firstI=%u",
        vertexCount, instanceCount, firstVertex, firstInstance);

    if (instanceCount > 1 || firstInstance > 0) {
        cmd->deferredContext->DrawInstanced(vertexCount, instanceCount,
                                                       firstVertex, firstInstance);
    } else {
        cmd->deferredContext->Draw(vertexCount, firstVertex);
    }
}

static void VKAPI_CALL d3d11_vkCmdDrawIndexed(
    VkCommandBuffer commandBuffer,
    uint32_t indexCount,
    uint32_t instanceCount,
    uint32_t firstIndex,
    int32_t vertexOffset,
    uint32_t firstInstance)
{
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);
    if (!cmd || !cmd->deferredContext) return;
    if (!cmd->currentPipeline) return;

    syncUBOShadowBuffers(cmd);
    flushGraphicsState(cmd);

    D3D11_TRACE("DrawIndexed: idx=%u inst=%u firstI=%u vOff=%d",
        indexCount, instanceCount, firstIndex, vertexOffset);

    if (instanceCount > 1 || firstInstance > 0) {
        cmd->deferredContext->DrawIndexedInstanced(indexCount, instanceCount,
                                                              firstIndex, vertexOffset, firstInstance);
    } else {
        cmd->deferredContext->DrawIndexed(indexCount, firstIndex, vertexOffset);
    }
}

static void VKAPI_CALL d3d11_vkCmdSetViewport(
    VkCommandBuffer commandBuffer,
    uint32_t firstViewport,
    uint32_t viewportCount,
    const VkViewport* pViewports)
{
    (void)firstViewport;
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);
    if (!cmd || !cmd->deferredContext) return;

    std::vector<D3D11_VIEWPORT> d3dViewports(viewportCount);
    for (uint32_t i = 0; i < viewportCount; i++) {
        d3dViewports[i].TopLeftX = pViewports[i].x;
        d3dViewports[i].TopLeftY = pViewports[i].y;
        d3dViewports[i].Width = pViewports[i].width;
        d3dViewports[i].Height = pViewports[i].height;
        d3dViewports[i].MinDepth = pViewports[i].minDepth;
        d3dViewports[i].MaxDepth = pViewports[i].maxDepth;
    }
    cmd->deferredContext->RSSetViewports(viewportCount, d3dViewports.data());
}

static void VKAPI_CALL d3d11_vkCmdSetScissor(
    VkCommandBuffer commandBuffer,
    uint32_t firstScissor,
    uint32_t scissorCount,
    const VkRect2D* pScissors)
{
    (void)firstScissor;
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);
    if (!cmd || !cmd->deferredContext) return;

    std::vector<D3D11_RECT> d3dRects(scissorCount);
    for (uint32_t i = 0; i < scissorCount; i++) {
        d3dRects[i].left = pScissors[i].offset.x;
        d3dRects[i].top = pScissors[i].offset.y;
        d3dRects[i].right = pScissors[i].offset.x + pScissors[i].extent.width;
        d3dRects[i].bottom = pScissors[i].offset.y + pScissors[i].extent.height;
    }
    cmd->deferredContext->RSSetScissorRects(scissorCount, d3dRects.data());
}

static void VKAPI_CALL d3d11_vkCmdPushConstants(
    VkCommandBuffer commandBuffer,
    VkPipelineLayout layout,
    VkShaderStageFlags stageFlags,
    uint32_t offset,
    uint32_t size,
    const void* pValues)
{
    (void)layout; (void)stageFlags;
    if (!commandBuffer || !pValues) return;
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);

    // 记录 push constant 数据，在 flushGraphicsState 时上传
    if (offset + size <= sizeof(cmd->pushConstantData)) {
        memcpy(cmd->pushConstantData + offset, pValues, size);
        cmd->pushConstantSize = std::max(cmd->pushConstantSize, offset + size);
        cmd->stateDirty = true;
    }
}

static void VKAPI_CALL d3d11_vkCmdCopyBuffer(
    VkCommandBuffer commandBuffer,
    VkBuffer srcBuffer,
    VkBuffer dstBuffer,
    uint32_t regionCount,
    const VkBufferCopy* pRegions)
{
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);
    auto* src = AS_D11(Buffer, srcBuffer);
    auto* dst = AS_D11(Buffer, dstBuffer);
    if (!cmd || !src || !dst) return;
    if (!cmd->deferredContext) return;

    auto* ctx = cmd->deferredContext.Get();
    auto* srcMem = AS_D11(Memory, src->boundMemory);
    auto* dstMem = AS_D11(Memory, dst->boundMemory);

    for (uint32_t i = 0; i < regionCount; i++) {
        auto& region = pRegions[i];

        bool srcStaging = (src->d3dUsage == D3D11_USAGE_STAGING);
        bool dstStaging = (dst->d3dUsage == D3D11_USAGE_STAGING);
        bool srcDynamic = (src->d3dUsage == D3D11_USAGE_DYNAMIC);
        bool dstDefault = (dst->d3dUsage == D3D11_USAGE_DEFAULT);
        bool srcHasShadow = (srcMem && srcMem->mapped);

        if (srcStaging && dstDefault && src->buffer && dst->buffer) {
            // Staging → Default: Map staging, UpdateSubresource
            if (srcHasShadow) {
                // 有 shadow buffer, 用 shadow buffer 的数据 UpdateSubresource
                D3D11_BOX box = {};
                box.left = 0;
                box.right = static_cast<UINT>(region.size);
                box.top = 0; box.bottom = 1;
                box.front = 0; box.back = 1;
                ctx->UpdateSubresource(dst->buffer.Get(), 0, &box,
                    static_cast<uint8_t*>(srcMem->mapped) + region.srcOffset,
                    static_cast<UINT>(region.size), 0);
            } else {
                // 没有 shadow: 使用 CopySubresourceRegion
                D3D11_BOX box = {};
                box.left = static_cast<UINT>(region.srcOffset);
                box.right = static_cast<UINT>(region.srcOffset + region.size);
                box.top = 0; box.bottom = 1;
                box.front = 0; box.back = 1;
                ctx->CopySubresourceRegion(dst->buffer.Get(), 0,
                    static_cast<UINT>(region.dstOffset), 0, 0,
                    src->buffer.Get(), 0, &box);
            }
        } else if (srcDynamic && dstDefault && src->buffer && dst->buffer) {
            // Dynamic → Default: 同 staging
            if (srcHasShadow) {
                D3D11_BOX box = {};
                box.left = 0;
                box.right = static_cast<UINT>(region.size);
                box.top = 0; box.bottom = 1;
                box.front = 0; box.back = 1;
                ctx->UpdateSubresource(dst->buffer.Get(), 0, &box,
                    static_cast<uint8_t*>(srcMem->mapped) + region.srcOffset,
                    static_cast<UINT>(region.size), 0);
            } else {
                D3D11_BOX box = {};
                box.left = static_cast<UINT>(region.srcOffset);
                box.right = static_cast<UINT>(region.srcOffset + region.size);
                box.top = 0; box.bottom = 1;
                box.front = 0; box.back = 1;
                ctx->CopySubresourceRegion(dst->buffer.Get(), 0,
                    static_cast<UINT>(region.dstOffset), 0, 0,
                    src->buffer.Get(), 0, &box);
            }
        } else if (srcStaging && dstStaging) {
            // Staging → Staging: CPU memcpy (两边都可 Map)
            if (srcMem && srcMem->mapped && dstMem && dstMem->mapped) {
                memcpy(static_cast<uint8_t*>(dstMem->mapped) + region.dstOffset,
                       static_cast<uint8_t*>(srcMem->mapped) + region.srcOffset,
                       (size_t)region.size);
            }
        } else if (src->buffer && dst->buffer) {
            // 通用: CopySubresourceRegion
            D3D11_BOX box = {};
            box.left = static_cast<UINT>(region.srcOffset);
            box.right = static_cast<UINT>(region.srcOffset + region.size);
            box.top = 0; box.bottom = 1;
            box.front = 0; box.back = 1;
            ctx->CopySubresourceRegion(dst->buffer.Get(), 0,
                static_cast<UINT>(region.dstOffset), 0, 0,
                src->buffer.Get(), 0, &box);
        }
    }
}

static void VKAPI_CALL d3d11_vkCmdCopyBufferToImage(
    VkCommandBuffer commandBuffer,
    VkBuffer srcBuffer,
    VkImage dstImage,
    VkImageLayout dstImageLayout,
    uint32_t regionCount,
    const VkBufferImageCopy* pRegions)
{
    (void)dstImageLayout;
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);
    auto* src = AS_D11(Buffer, srcBuffer);
    auto* dstImg = AS_D11(Image, dstImage);
    if (!cmd || !src || !dstImg) return;
    if (!dstImg->texture) return;
    if (!cmd->deferredContext) return;

    auto* ctx = cmd->deferredContext.Get();
    auto* srcMem = AS_D11(Memory, src->boundMemory);

    for (uint32_t i = 0; i < regionCount; i++) {
        auto& region = pRegions[i];
        DXGI_FORMAT fmt = vkFormatToDxgi(dstImg->format);
        uint32_t texelSize = texelSizeFromDxgi(fmt);

        uint32_t width = region.imageExtent.width;
        uint32_t height = region.imageExtent.height;
        uint32_t srcRowPitch = (region.bufferRowLength > 0)
            ? region.bufferRowLength * texelSize : width * texelSize;

        // 获取源数据
        void* srcData = nullptr;
        if (srcMem && srcMem->mapped) {
            srcData = static_cast<uint8_t*>(srcMem->mapped) + region.bufferOffset;
        }

        if (srcData) {
            // 使用 UpdateSubresource 上传纹理数据
            D3D11_BOX box = {};
            box.left = region.imageOffset.x;
            box.top = region.imageOffset.y;
            box.front = 0;
            box.right = region.imageOffset.x + width;
            box.bottom = region.imageOffset.y + height;
            box.back = 1;

            ctx->UpdateSubresource(
                dstImg->texture.Get(),
                region.imageSubresource.mipLevel,
                &box,
                srcData,
                srcRowPitch,
                srcRowPitch * height);
        } else if (src->buffer) {
            // 如果没有 shadow buffer，需要 Map staging buffer
            D3D11_MAPPED_SUBRESOURCE mapped;
            HRESULT hr = ctx->Map(src->buffer.Get(), 0, D3D11_MAP_READ, 0, &mapped);
            if (SUCCEEDED(hr)) {
                void* data = static_cast<uint8_t*>(mapped.pData) + region.bufferOffset;
                D3D11_BOX box = {};
                box.left = region.imageOffset.x;
                box.top = region.imageOffset.y;
                box.front = 0;
                box.right = region.imageOffset.x + width;
                box.bottom = region.imageOffset.y + height;
                box.back = 1;

                ctx->UpdateSubresource(
                    dstImg->texture.Get(),
                    region.imageSubresource.mipLevel,
                    &box,
                    data,
                    srcRowPitch,
                    srcRowPitch * height);
                ctx->Unmap(src->buffer.Get(), 0);
            }
        }
    }
}

static void VKAPI_CALL d3d11_vkCmdCopyImageToBuffer(
    VkCommandBuffer commandBuffer,
    VkImage srcImage,
    VkImageLayout srcImageLayout,
    VkBuffer dstBuffer,
    uint32_t regionCount,
    const VkBufferImageCopy* pRegions)
{
    (void)srcImageLayout;
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);
    auto* srcImg = AS_D11(Image, srcImage);
    auto* dstBuf = AS_D11(Buffer, dstBuffer);
    if (!cmd || !srcImg || !dstBuf) return;
    if (!srcImg->texture) return;
    if (!cmd->deferredContext || !cmd->device) return;

    auto* ctx = cmd->deferredContext.Get();
    auto* dev = AS_D11(Device, cmd->device);

    for (uint32_t i = 0; i < regionCount; i++) {
        auto& region = pRegions[i];
        DXGI_FORMAT fmt = vkFormatToDxgi(srcImg->format);
        uint32_t texelSize = texelSizeFromDxgi(fmt);

        uint32_t width = region.imageExtent.width;
        uint32_t height = region.imageExtent.height;
        uint32_t rowPitch = width * texelSize;

        // 创建 staging texture 用于回读
        D3D11_TEXTURE2D_DESC stagingDesc = {};
        stagingDesc.Width = width;
        stagingDesc.Height = height;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = fmt;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        ComPtr<ID3D11Texture2D> stagingTex;
        HRESULT hr = dev->device->CreateTexture2D(&stagingDesc, nullptr, &stagingTex);
        if (FAILED(hr)) continue;

        // 复制源 texture → staging texture
        D3D11_BOX srcBox = {};
        srcBox.left = region.imageOffset.x;
        srcBox.top = region.imageOffset.y;
        srcBox.front = 0;
        srcBox.right = region.imageOffset.x + width;
        srcBox.bottom = region.imageOffset.y + height;
        srcBox.back = 1;

        ctx->CopySubresourceRegion(stagingTex.Get(), 0, 0, 0, 0,
            srcImg->texture.Get(), region.imageSubresource.mipLevel, &srcBox);

        // 记录延迟回读: deferred context 无法 Map staging texture，
        // 需要在 QueueSubmit ExecuteCommandList 之后通过 immediate context 完成
        D11_PendingReadback rb;
        rb.stagingTex = stagingTex;
        rb.dstBuffer = dstBuffer;
        rb.width = width;
        rb.height = height;
        rb.texelSize = texelSize;
        rb.bufferOffset = region.bufferOffset;
        cmd->pendingReadbacks.push_back(std::move(rb));
    }
}

static void VKAPI_CALL d3d11_vkCmdPipelineBarrier(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlags srcStageMask,
    VkPipelineStageFlags dstStageMask,
    VkDependencyFlags dependencyFlags,
    uint32_t memoryBarrierCount,
    const VkMemoryBarrier* pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier* pBufferMemoryBarriers,
    uint32_t imageMemoryBarrierCount,
    const VkImageMemoryBarrier* pImageMemoryBarriers)
{
    (void)srcStageMask; (void)dstStageMask; (void)dependencyFlags;
    (void)memoryBarrierCount; (void)pMemoryBarriers;
    (void)bufferMemoryBarrierCount; (void)pBufferMemoryBarriers;

    // D3D11 不需要显式 barrier，只记录 layout 变化
    for (uint32_t i = 0; i < imageMemoryBarrierCount; i++) {
        auto& ib = pImageMemoryBarriers[i];
        if (ib.image) {
            AS_D11(Image, ib.image)->currentLayout = ib.newLayout;
        }
    }
}

static void VKAPI_CALL d3d11_vkCmdBlitImage(
    VkCommandBuffer commandBuffer,
    VkImage srcImage, VkImageLayout srcImageLayout,
    VkImage dstImage, VkImageLayout dstImageLayout,
    uint32_t regionCount, const VkImageBlit* pRegions,
    VkFilter filter)
{
    (void)srcImageLayout; (void)dstImageLayout; (void)filter;
    auto* cmd = AS_D11(CommandBuffer, commandBuffer);
    auto* srcImg = AS_D11(Image, srcImage);
    auto* dstImg = AS_D11(Image, dstImage);
    if (!cmd || !srcImg || !dstImg) return;
    if (!srcImg->texture || !dstImg->texture) return;
    if (!cmd->deferredContext) return;

    auto* ctx = cmd->deferredContext.Get();

    for (uint32_t i = 0; i < regionCount; i++) {
        auto& region = pRegions[i];
        uint32_t srcW = region.srcOffsets[1].x - region.srcOffsets[0].x;
        uint32_t srcH = region.srcOffsets[1].y - region.srcOffsets[0].y;
        uint32_t dstW = region.dstOffsets[1].x - region.dstOffsets[0].x;
        uint32_t dstH = region.dstOffsets[1].y - region.dstOffsets[0].y;

        if (srcW == dstW && srcH == dstH &&
            region.srcOffsets[0].x == 0 && region.srcOffsets[0].y == 0 &&
            region.dstOffsets[0].x == 0 && region.dstOffsets[0].y == 0 &&
            srcW == srcImg->width && srcH == srcImg->height) {
            // 全图同尺寸 → CopyResource
            ctx->CopyResource(dstImg->texture.Get(), srcImg->texture.Get());
        } else {
            // 区域复制 (D3D11 不支持缩放 blit，取 min 尺寸)
            uint32_t copyW = std::min(srcW, dstW);
            uint32_t copyH = std::min(srcH, dstH);

            D3D11_BOX srcBox = {};
            srcBox.left = region.srcOffsets[0].x;
            srcBox.top = region.srcOffsets[0].y;
            srcBox.front = 0;
            srcBox.right = srcBox.left + copyW;
            srcBox.bottom = srcBox.top + copyH;
            srcBox.back = 1;

            ctx->CopySubresourceRegion(
                dstImg->texture.Get(), region.dstSubresource.mipLevel,
                region.dstOffsets[0].x, region.dstOffsets[0].y, 0,
                srcImg->texture.Get(), region.srcSubresource.mipLevel,
                &srcBox);
        }
    }
}

// ============================================================================
// Debug
// ============================================================================

static VkResult VKAPI_CALL d3d11_vkCreateDebugUtilsMessengerEXT(
    VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDebugUtilsMessengerEXT* pMessenger)
{
    (void)instance; (void)pAllocator;
    auto* msg = new D11_DebugMessenger();
    msg->callback = pCreateInfo->pfnUserCallback;
    msg->userData = pCreateInfo->pUserData;
    msg->severityFilter = pCreateInfo->messageSeverity;
    msg->typeFilter = pCreateInfo->messageType;
    *pMessenger = TO_VK(VkDebugUtilsMessengerEXT, msg);
    return VK_SUCCESS;
}

static void VKAPI_CALL d3d11_vkDestroyDebugUtilsMessengerEXT(
    VkInstance instance,
    VkDebugUtilsMessengerEXT messenger,
    const VkAllocationCallbacks* pAllocator)
{
    (void)instance; (void)pAllocator;
    delete AS_D11(DebugMessenger, messenger);
}

// ============================================================================
// vkGetInstanceProcAddr (用于 ImGui 等第三方库加载函数)
// ============================================================================

static PFN_vkVoidFunction VKAPI_CALL d3d11_vkGetInstanceProcAddr(
    VkInstance instance,
    const char* pName)
{
    (void)instance;
    #define CHECK_FUNC(fn) if (strcmp(pName, #fn) == 0) return reinterpret_cast<PFN_vkVoidFunction>(d3d11_##fn)

    CHECK_FUNC(vkCreateInstance);
    CHECK_FUNC(vkDestroyInstance);
    CHECK_FUNC(vkEnumeratePhysicalDevices);
    CHECK_FUNC(vkGetPhysicalDeviceProperties);
    CHECK_FUNC(vkGetPhysicalDeviceFeatures);
    CHECK_FUNC(vkGetPhysicalDeviceFeatures2);
    CHECK_FUNC(vkGetPhysicalDeviceMemoryProperties);
    CHECK_FUNC(vkGetPhysicalDeviceQueueFamilyProperties);
    CHECK_FUNC(vkEnumerateInstanceExtensionProperties);
    CHECK_FUNC(vkEnumerateInstanceLayerProperties);
    CHECK_FUNC(vkEnumerateDeviceExtensionProperties);
    CHECK_FUNC(vkCreateDevice);
    CHECK_FUNC(vkDestroyDevice);
    CHECK_FUNC(vkGetDeviceQueue);
    CHECK_FUNC(vkDeviceWaitIdle);
    CHECK_FUNC(vkDestroySurfaceKHR);
    CHECK_FUNC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    CHECK_FUNC(vkGetPhysicalDeviceSurfaceFormatsKHR);
    CHECK_FUNC(vkGetPhysicalDeviceSurfacePresentModesKHR);
    CHECK_FUNC(vkGetPhysicalDeviceSurfaceSupportKHR);
    CHECK_FUNC(vkCreateSwapchainKHR);
    CHECK_FUNC(vkDestroySwapchainKHR);
    CHECK_FUNC(vkGetSwapchainImagesKHR);
    CHECK_FUNC(vkAcquireNextImageKHR);
    CHECK_FUNC(vkQueuePresentKHR);
    CHECK_FUNC(vkCreateBuffer);
    CHECK_FUNC(vkDestroyBuffer);
    CHECK_FUNC(vkGetBufferMemoryRequirements);
    CHECK_FUNC(vkAllocateMemory);
    CHECK_FUNC(vkFreeMemory);
    CHECK_FUNC(vkBindBufferMemory);
    CHECK_FUNC(vkMapMemory);
    CHECK_FUNC(vkUnmapMemory);
    CHECK_FUNC(vkFlushMappedMemoryRanges);
    CHECK_FUNC(vkCreateImage);
    CHECK_FUNC(vkDestroyImage);
    CHECK_FUNC(vkGetImageMemoryRequirements);
    CHECK_FUNC(vkBindImageMemory);
    CHECK_FUNC(vkCreateImageView);
    CHECK_FUNC(vkDestroyImageView);
    CHECK_FUNC(vkCreateSampler);
    CHECK_FUNC(vkDestroySampler);
    CHECK_FUNC(vkCreateCommandPool);
    CHECK_FUNC(vkDestroyCommandPool);
    CHECK_FUNC(vkResetCommandPool);
    CHECK_FUNC(vkAllocateCommandBuffers);
    CHECK_FUNC(vkFreeCommandBuffers);
    CHECK_FUNC(vkBeginCommandBuffer);
    CHECK_FUNC(vkEndCommandBuffer);
    CHECK_FUNC(vkResetCommandBuffer);
    CHECK_FUNC(vkCmdBeginRenderPass);
    CHECK_FUNC(vkCmdEndRenderPass);
    CHECK_FUNC(vkCmdBindPipeline);
    CHECK_FUNC(vkCmdBindDescriptorSets);
    CHECK_FUNC(vkCmdBindVertexBuffers);
    CHECK_FUNC(vkCmdBindIndexBuffer);
    CHECK_FUNC(vkCmdDraw);
    CHECK_FUNC(vkCmdDrawIndexed);
    CHECK_FUNC(vkCmdSetViewport);
    CHECK_FUNC(vkCmdSetScissor);
    CHECK_FUNC(vkCmdPushConstants);
    CHECK_FUNC(vkCmdCopyBuffer);
    CHECK_FUNC(vkCmdCopyBufferToImage);
    CHECK_FUNC(vkCmdCopyImageToBuffer);
    CHECK_FUNC(vkCmdPipelineBarrier);
    CHECK_FUNC(vkCmdBlitImage);
    CHECK_FUNC(vkWaitForFences);
    CHECK_FUNC(vkResetFences);
    CHECK_FUNC(vkQueueSubmit);
    CHECK_FUNC(vkQueueWaitIdle);
    CHECK_FUNC(vkCreatePipelineLayout);
    CHECK_FUNC(vkDestroyPipelineLayout);
    CHECK_FUNC(vkCreateShaderModule);
    CHECK_FUNC(vkDestroyShaderModule);
    CHECK_FUNC(vkCreateGraphicsPipelines);
    CHECK_FUNC(vkDestroyPipeline);
    CHECK_FUNC(vkCreateRenderPass);
    CHECK_FUNC(vkDestroyRenderPass);
    CHECK_FUNC(vkCreateFramebuffer);
    CHECK_FUNC(vkDestroyFramebuffer);
    CHECK_FUNC(vkCreateDescriptorPool);
    CHECK_FUNC(vkDestroyDescriptorPool);
    CHECK_FUNC(vkCreateDescriptorSetLayout);
    CHECK_FUNC(vkDestroyDescriptorSetLayout);
    CHECK_FUNC(vkAllocateDescriptorSets);
    CHECK_FUNC(vkFreeDescriptorSets);
    CHECK_FUNC(vkUpdateDescriptorSets);
    CHECK_FUNC(vkCreateSemaphore);
    CHECK_FUNC(vkDestroySemaphore);
    CHECK_FUNC(vkCreateFence);
    CHECK_FUNC(vkDestroyFence);
    CHECK_FUNC(vkCreateDebugUtilsMessengerEXT);
    CHECK_FUNC(vkDestroyDebugUtilsMessengerEXT);
    CHECK_FUNC(vkGetInstanceProcAddr);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    CHECK_FUNC(vkCreateWin32SurfaceKHR);
#endif

    #undef CHECK_FUNC
    return nullptr;
}

// ============================================================================
// 分发表加载入口
// ============================================================================

void vkLoadD3D11Dispatch()
{
    #define VK_D3D11(fn) fn = d3d11_##fn

    // Instance
    VK_D3D11(vkCreateInstance);
    VK_D3D11(vkDestroyInstance);
    VK_D3D11(vkEnumerateInstanceExtensionProperties);
    VK_D3D11(vkEnumerateInstanceLayerProperties);
    VK_D3D11(vkGetInstanceProcAddr);

    // Physical Device
    VK_D3D11(vkEnumeratePhysicalDevices);
    VK_D3D11(vkGetPhysicalDeviceProperties);
    VK_D3D11(vkGetPhysicalDeviceFeatures);
    VK_D3D11(vkGetPhysicalDeviceFeatures2);
    VK_D3D11(vkGetPhysicalDeviceMemoryProperties);
    VK_D3D11(vkGetPhysicalDeviceQueueFamilyProperties);
    VK_D3D11(vkEnumerateDeviceExtensionProperties);

    // Device & Queue
    VK_D3D11(vkCreateDevice);
    VK_D3D11(vkDestroyDevice);
    VK_D3D11(vkGetDeviceQueue);
    VK_D3D11(vkDeviceWaitIdle);

    // Surface
    VK_D3D11(vkDestroySurfaceKHR);
    VK_D3D11(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    VK_D3D11(vkGetPhysicalDeviceSurfaceFormatsKHR);
    VK_D3D11(vkGetPhysicalDeviceSurfacePresentModesKHR);
    VK_D3D11(vkGetPhysicalDeviceSurfaceSupportKHR);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    VK_D3D11(vkCreateWin32SurfaceKHR);
#endif

    // Swapchain
    VK_D3D11(vkCreateSwapchainKHR);
    VK_D3D11(vkDestroySwapchainKHR);
    VK_D3D11(vkGetSwapchainImagesKHR);
    VK_D3D11(vkAcquireNextImageKHR);
    VK_D3D11(vkQueuePresentKHR);

    // Buffer
    VK_D3D11(vkCreateBuffer);
    VK_D3D11(vkDestroyBuffer);
    VK_D3D11(vkGetBufferMemoryRequirements);

    // Memory
    VK_D3D11(vkAllocateMemory);
    VK_D3D11(vkFreeMemory);
    VK_D3D11(vkBindBufferMemory);
    VK_D3D11(vkMapMemory);
    VK_D3D11(vkUnmapMemory);
    VK_D3D11(vkFlushMappedMemoryRanges);

    // Image
    VK_D3D11(vkCreateImage);
    VK_D3D11(vkDestroyImage);
    VK_D3D11(vkGetImageMemoryRequirements);
    VK_D3D11(vkBindImageMemory);
    VK_D3D11(vkCreateImageView);
    VK_D3D11(vkDestroyImageView);

    // Sampler
    VK_D3D11(vkCreateSampler);
    VK_D3D11(vkDestroySampler);

    // Command Pool & Buffer
    VK_D3D11(vkCreateCommandPool);
    VK_D3D11(vkDestroyCommandPool);
    VK_D3D11(vkResetCommandPool);
    VK_D3D11(vkAllocateCommandBuffers);
    VK_D3D11(vkFreeCommandBuffers);
    VK_D3D11(vkBeginCommandBuffer);
    VK_D3D11(vkEndCommandBuffer);
    VK_D3D11(vkResetCommandBuffer);

    // Command Recording
    VK_D3D11(vkCmdBeginRenderPass);
    VK_D3D11(vkCmdEndRenderPass);
    VK_D3D11(vkCmdBindPipeline);
    VK_D3D11(vkCmdBindDescriptorSets);
    VK_D3D11(vkCmdBindVertexBuffers);
    VK_D3D11(vkCmdBindIndexBuffer);
    VK_D3D11(vkCmdDraw);
    VK_D3D11(vkCmdDrawIndexed);
    VK_D3D11(vkCmdSetViewport);
    VK_D3D11(vkCmdSetScissor);
    VK_D3D11(vkCmdPushConstants);
    VK_D3D11(vkCmdCopyBuffer);
    VK_D3D11(vkCmdCopyBufferToImage);
    VK_D3D11(vkCmdCopyImageToBuffer);
    VK_D3D11(vkCmdPipelineBarrier);
    VK_D3D11(vkCmdBlitImage);

    // Synchronization
    VK_D3D11(vkWaitForFences);
    VK_D3D11(vkResetFences);
    VK_D3D11(vkQueueSubmit);
    VK_D3D11(vkQueueWaitIdle);

    // Pipeline
    VK_D3D11(vkCreatePipelineLayout);
    VK_D3D11(vkDestroyPipelineLayout);
    VK_D3D11(vkCreateShaderModule);
    VK_D3D11(vkDestroyShaderModule);
    VK_D3D11(vkCreateGraphicsPipelines);
    VK_D3D11(vkDestroyPipeline);

    // Render Pass & Framebuffer
    VK_D3D11(vkCreateRenderPass);
    VK_D3D11(vkDestroyRenderPass);
    VK_D3D11(vkCreateFramebuffer);
    VK_D3D11(vkDestroyFramebuffer);

    // Descriptor
    VK_D3D11(vkCreateDescriptorPool);
    VK_D3D11(vkDestroyDescriptorPool);
    VK_D3D11(vkCreateDescriptorSetLayout);
    VK_D3D11(vkDestroyDescriptorSetLayout);
    VK_D3D11(vkAllocateDescriptorSets);
    VK_D3D11(vkFreeDescriptorSets);
    VK_D3D11(vkUpdateDescriptorSets);

    // Sync Objects
    VK_D3D11(vkCreateSemaphore);
    VK_D3D11(vkDestroySemaphore);
    VK_D3D11(vkCreateFence);
    VK_D3D11(vkDestroyFence);

    // Debug
    VK_D3D11(vkCreateDebugUtilsMessengerEXT);
    VK_D3D11(vkDestroyDebugUtilsMessengerEXT);

    #undef VK_D3D11
}

#endif // _WIN32
