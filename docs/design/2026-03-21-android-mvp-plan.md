# Android 引擎渲染 MVP 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 Android 真机上运行一个硬编码旋转立方体，验证 Vulkan 渲染管线。

**Architecture:** 新建 `android/` 目录作为 Android Studio 项目，通过 CMake 交叉编译引擎核心 + SDL2 为 native library。Android 入口通过 SDL2 的 SDLActivity 启动，native 层硬编码场景和渲染循环。需要适配着色器加载（APK assets）和 Vulkan 设备扩展（移动 GPU 兼容）。

**Tech Stack:** SDL2, Vulkan 1.1+, Android NDK 29, Gradle, CMake 3.20+, C++17

---

## 环境信息

- Android SDK: `C:\Users\meiyuanqiao\AppData\Local\Android\Sdk`
- NDK: `C:\Users\meiyuanqiao\Perforce\meiyuanqiao_Engine_Release_CBT3\External\Android\NonRedistributable\sdk\builds\ndk\29.0.14206865`
- Java: JDK 17 (`C:\Program Files\Java\jdk-17`)
- 项目根目录: `E:\MYQ\QymEngine`

---

### Task 1: 引擎代码平台适配 — Vulkan 扩展和着色器加载

**Files:**
- Modify: `engine/renderer/VulkanContext.cpp` — 设备扩展改为可选
- Modify: `engine/renderer/Pipeline.cpp` — 着色器加载支持 SDL_RWops
- Modify: `engine/renderer/Renderer.cpp` — grid 着色器加载同样适配
- Modify: `engine/renderer/Texture.cpp` — 纹理加载适配（如有 ASSETS_DIR 依赖）
- Modify: `engine/CMakeLists.txt` — 平台条件宏

- [ ] **Step 1: 修改 VulkanContext.cpp — 设备扩展改为可选**

当前 `s_deviceExtensions` 包含 `VK_KHR_PRESENT_ID` 和 `VK_KHR_PRESENT_WAIT`，移动 GPU 可能不支持。将这两个改为可选扩展：

```cpp
// 必需扩展
const std::vector<const char*> VulkanContext::s_deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

// 可选扩展 — 仅在支持时启用
static const std::vector<const char*> s_optionalDeviceExtensions = {
    VK_KHR_PRESENT_ID_EXTENSION_NAME,
    VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
};
```

在 `createLogicalDevice()` 中，检查可选扩展是否可用，动态构建最终扩展列表：

```cpp
std::vector<const char*> enabledExtensions(s_deviceExtensions.begin(), s_deviceExtensions.end());
uint32_t extensionCount;
vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extensionCount, nullptr);
std::vector<VkExtensionProperties> availableExtensions(extensionCount);
vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &extensionCount, availableExtensions.data());
for (const char* opt : s_optionalDeviceExtensions) {
    for (const auto& ext : availableExtensions) {
        if (strcmp(ext.extensionName, opt) == 0) {
            enabledExtensions.push_back(opt);
            break;
        }
    }
}
createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
createInfo.ppEnabledExtensionNames = enabledExtensions.data();
```

同时修改 `checkDeviceExtensionSupport()` 只检查必需扩展。

- [ ] **Step 2: 修改 Pipeline.cpp — readFile 使用 SDL_RWops**

替换 `std::ifstream` 为 `SDL_RWops`，这样在 Android 上自动从 APK assets 读取：

```cpp
#include <SDL.h>

std::vector<char> Pipeline::readFile(const std::string& filename)
{
    SDL_RWops* rw = SDL_RWFromFile(filename.c_str(), "rb");
    if (!rw)
        throw std::runtime_error("failed to open file: " + filename + " (" + SDL_GetError() + ")");

    Sint64 fileSize = SDL_RWsize(rw);
    if (fileSize < 0) {
        SDL_RWclose(rw);
        throw std::runtime_error("failed to get file size: " + filename);
    }

    std::vector<char> buffer(static_cast<size_t>(fileSize));
    SDL_RWread(rw, buffer.data(), 1, static_cast<size_t>(fileSize));
    SDL_RWclose(rw);

    return buffer;
}
```

同时修改 `Pipeline::create()` 中的路径构建。在 Android 上 `ASSETS_DIR` 不需要前缀（SDL_RWFromFile 直接从 assets 目录读），在 Windows 上保持现有行为：

```cpp
#ifdef __ANDROID__
    std::string vertFile = vertPath.empty() ? "shaders/vert.spv" : vertPath;
    std::string fragFile = fragPath.empty() ? "shaders/frag.spv" : fragPath;
#else
    std::string vertFile = vertPath.empty()
        ? std::string(ASSETS_DIR) + "/shaders/vert.spv"
        : std::string(ASSETS_DIR) + "/" + vertPath;
    std::string fragFile = fragPath.empty()
        ? std::string(ASSETS_DIR) + "/shaders/frag.spv"
        : std::string(ASSETS_DIR) + "/" + fragPath;
#endif
```

- [ ] **Step 3: 修改 Renderer.cpp — grid 着色器和 assetManager 路径适配**

`Renderer::init()` 中 grid 着色器加载和 `scanAssets` 路径也需要 Android 条件编译：

```cpp
// grid 着色器路径
#ifdef __ANDROID__
    auto gridVertCode = readFile("shaders/grid_vert.spv");
    auto gridFragCode = readFile("shaders/grid_frag.spv");
#else
    auto gridVertCode = readFile(std::string(ASSETS_DIR) + "/shaders/grid_vert.spv");
    auto gridFragCode = readFile(std::string(ASSETS_DIR) + "/shaders/grid_frag.spv");
#endif

// AssetManager — Android MVP 不需要扫描资产
#ifndef __ANDROID__
    m_assetManager.scanAssets(std::string(ASSETS_DIR));
#endif
```

- [ ] **Step 4: 修改 Texture.cpp — 纹理路径适配**

```cpp
#ifdef __ANDROID__
    stbi_uc* pixels = stbi_load("textures/texture.jpg", ...);
#else
    stbi_uc* pixels = stbi_load((std::string(ASSETS_DIR) + "/textures/texture.jpg").c_str(), ...);
#endif
```

注意：`stbi_load` 不支持 SDL_RWops，Android 上可能需要先用 SDL_RWops 读到内存再用 `stbi_load_from_memory`。如果 MVP 不使用纹理（硬编码立方体用纯色），可以跳过，通过条件编译跳过纹理加载。

- [ ] **Step 5: 修改 engine/CMakeLists.txt — 平台条件宏**

```cmake
if(ANDROID)
    target_compile_definitions(QymEngineLib PUBLIC VK_USE_PLATFORM_ANDROID_KHR)
else()
    target_compile_definitions(QymEngineLib PUBLIC VK_USE_PLATFORM_WIN32_KHR)
endif()
```

- [ ] **Step 6: 在 Windows 上编译验证不 break 现有功能**

```bash
cmake -S . -B build3 -G "Visual Studio 17 2022" -A x64
cmake --build build3 --config Debug
```

---

### Task 2: Android 项目骨架

**Files:**
- Create: `android/app/build.gradle.kts`
- Create: `android/app/src/main/AndroidManifest.xml`
- Create: `android/build.gradle.kts`
- Create: `android/settings.gradle.kts`
- Create: `android/gradle.properties`
- Create: `android/local.properties`
- Create: `android/app/src/main/java/com/qymengine/app/QymActivity.java`

- [ ] **Step 1: 创建 android/settings.gradle.kts**

```kotlin
rootProject.name = "QymEngine"
include(":app")
```

- [ ] **Step 2: 创建 android/build.gradle.kts**

```kotlin
plugins {
    id("com.android.application") version "8.7.0" apply false
}
```

- [ ] **Step 3: 创建 android/gradle.properties**

```properties
android.useAndroidX=true
org.gradle.jvmargs=-Xmx2048m
```

- [ ] **Step 4: 创建 android/local.properties**

```properties
sdk.dir=C\:\\Users\\meiyuanqiao\\AppData\\Local\\Android\\Sdk
ndk.dir=C\:\\Users\\meiyuanqiao\\Perforce\\meiyuanqiao_Engine_Release_CBT3\\External\\Android\\NonRedistributable\\sdk\\builds\\ndk\\29.0.14206865
```

- [ ] **Step 5: 创建 android/app/build.gradle.kts**

```kotlin
plugins {
    id("com.android.application")
}

android {
    namespace = "com.qymengine.app"
    compileSdk = 35
    ndkVersion = "29.0.14206865"

    defaultConfig {
        applicationId = "com.qymengine.app"
        minSdk = 28
        targetSdk = 35
        versionCode = 1
        versionName = "0.1"

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DCMAKE_CXX_STANDARD=17"
                )
                abiFilters += listOf("arm64-v8a")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("../../android-cmake/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }

    sourceSets {
        getByName("main") {
            assets.srcDirs("../../assets")
        }
    }
}
```

注意: `assets.srcDirs("../../assets")` 让 Gradle 将项目根的 `assets/` 目录打包进 APK。

- [ ] **Step 6: 创建 AndroidManifest.xml**

```xml
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android">

    <uses-feature android:name="android.hardware.vulkan.level" android:version="1" android:required="true" />
    <uses-feature android:name="android.hardware.vulkan.version" android:version="0x00401000" android:required="true" />

    <application
        android:label="QymEngine"
        android:hasCode="true"
        android:allowBackup="false">

        <activity
            android:name=".QymActivity"
            android:configChanges="orientation|screenSize|keyboardHidden"
            android:exported="true">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
```

- [ ] **Step 7: 创建 QymActivity.java**

```java
package com.qymengine.app;

import org.libsdl.app.SDLActivity;

public class QymActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {
            "SDL2",
            "qymengine_android"
        };
    }
}
```

---

### Task 3: Android CMake 构建和入口代码

**Files:**
- Create: `android-cmake/CMakeLists.txt` — Android native 构建
- Create: `android-cmake/main.cpp` — Android 入口 (硬编码旋转立方体)

- [ ] **Step 1: 创建 android-cmake/CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.20)
project(QymEngineAndroid LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Assets path not used on Android (SDL_RWFromFile reads from APK assets)
add_compile_definitions(ASSETS_DIR="")

# Vulkan
find_package(Vulkan REQUIRED)

# GLM
add_library(glm INTERFACE)
target_include_directories(glm INTERFACE "${CMAKE_SOURCE_DIR}/../3rd-party/glm-1.0.1-light")

# STB
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE "${CMAKE_SOURCE_DIR}/../3rd-party/stb-master")

# nlohmann/json
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE "${CMAKE_SOURCE_DIR}/../3rd-party/nlohmann")

# tinyobjloader
add_library(tinyobjloader INTERFACE)
target_include_directories(tinyobjloader INTERFACE "${CMAKE_SOURCE_DIR}/../3rd-party/tinyobjloader")

# SDL2 (from source)
add_subdirectory("${CMAKE_SOURCE_DIR}/../build3/_deps/sdl2-src" "${CMAKE_BINARY_DIR}/sdl2-build")

# Engine library
file(GLOB_RECURSE ENGINE_SOURCES
    "${CMAKE_SOURCE_DIR}/../engine/core/*.cpp"
    "${CMAKE_SOURCE_DIR}/../engine/renderer/*.cpp"
    "${CMAKE_SOURCE_DIR}/../engine/scene/*.cpp"
    "${CMAKE_SOURCE_DIR}/../engine/asset/*.cpp"
)

add_library(QymEngineLib STATIC ${ENGINE_SOURCES})
target_include_directories(QymEngineLib PUBLIC "${CMAKE_SOURCE_DIR}/../engine")
target_link_libraries(QymEngineLib PUBLIC
    Vulkan::Vulkan
    SDL2::SDL2
    glm stb nlohmann_json tinyobjloader
)
target_compile_definitions(QymEngineLib PUBLIC
    VK_USE_PLATFORM_ANDROID_KHR
    SDL_MAIN_HANDLED
    GLM_FORCE_RADIANS
    GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)

# Android app shared library
add_library(qymengine_android SHARED
    "${CMAKE_SOURCE_DIR}/main.cpp"
)
target_link_libraries(qymengine_android PRIVATE
    QymEngineLib
    android
    log
)
```

注意：Android 上 SDL2 需要编译为 shared library（`.so`），因此用 `SDL2::SDL2` 而非 `SDL2::SDL2-static`。SDL2 的 Java 层会 `System.loadLibrary("SDL2")`。

- [ ] **Step 2: 创建 android-cmake/main.cpp**

```cpp
#include "core/Application.h"
#include "renderer/Renderer.h"
#include "scene/Scene.h"
#include "scene/Camera.h"

#include <SDL.h>

class AndroidApp : public QymEngine::Application {
public:
    AndroidApp() : Application({"QymEngine", 0, 0, true}) {} // fullscreen

protected:
    void onInit() override {
        m_renderer.init(getWindow());

        // Hardcoded cube scene
        auto* cube = m_scene.createNode("Cube");
        cube->meshType = QymEngine::MeshType::Cube;

        // Fixed camera
        m_camera.target = {0.0f, 0.0f, 0.0f};
        m_camera.distance = 4.0f;
        m_camera.yaw = -45.0f;
        m_camera.pitch = 30.0f;
        m_renderer.setCamera(&m_camera);

        // Offscreen setup (render to swapchain directly via drawScene)
    }

    void onUpdate() override {
        // Rotate cube each frame
        auto* root = m_scene.getRoot();
        if (!root->getChildren().empty()) {
            auto* cube = root->getChildren()[0].get();
            cube->transform.rotation.y += 0.5f;
        }

        if (m_renderer.beginFrame()) {
            m_renderer.drawScene(m_scene);
            m_renderer.endFrame();
        }
    }

    void onShutdown() override {
        m_renderer.shutdown();
    }

private:
    QymEngine::Renderer m_renderer;
    QymEngine::Scene    m_scene;
    QymEngine::Camera   m_camera;
};

int main(int argc, char* argv[]) {
    AndroidApp app;
    try {
        app.run();
    } catch (...) {
        SDL_Log("QymEngine crashed");
        return 1;
    }
    return 0;
}
```

- [ ] **Step 3: 拷贝 SDL2 Java 源码到 Android 项目**

SDL2 需要 Java 层（SDLActivity 等）。将 SDL2 源码中的 Java 文件拷贝到 Android 项目：

```bash
cp -r build3/_deps/sdl2-src/android-project/app/src/main/java/org \
      android/app/src/main/java/
```

---

### Task 4: 构建 APK 并部署到真机

- [ ] **Step 1: 设置 Gradle wrapper**

```bash
cd android
gradle wrapper --gradle-version 8.11.1
```

如果系统没有 gradle 命令，从 Android Studio 拷贝或手动下载 gradle-wrapper.jar。

- [ ] **Step 2: 构建 Debug APK**

```bash
cd android
./gradlew assembleDebug
```

预期输出: `android/app/build/outputs/apk/debug/app-debug.apk`

如果遇到编译错误，根据错误信息修复。常见问题：
- NDK 路径不对 → 修改 `local.properties`
- SDL2 Java 文件缺失 → 确认 Step 3 拷贝正确
- Vulkan header 找不到 → NDK 版本需要 >= 26

- [ ] **Step 3: 安装到真机并运行**

```bash
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.qymengine.app/.QymActivity
```

用 `adb logcat -s SDL QymEngine` 查看日志。

- [ ] **Step 4: 验证**

真机上应该看到一个旋转的立方体。如果黑屏或闪退，用 `adb logcat` 排查。

- [ ] **Step 5: 提交**

```bash
git add android/ android-cmake/
git add engine/renderer/VulkanContext.cpp engine/renderer/Pipeline.cpp
git add engine/renderer/Renderer.cpp engine/renderer/Texture.cpp
git add engine/CMakeLists.txt
git commit -m "feat: add Android MVP - rotating cube on Vulkan"
```
