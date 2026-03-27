# Android 构建与调试

## 项目结构

- `android/` — Gradle 工程（`app/build.gradle.kts`）
- `android-cmake/` — NDK 原生入口（`main.cpp`、`CMakeLists.txt`）
- 包名: `com.qymengine.app`
- Activity: `com.qymengine.app.QymActivity`

## 构建

```bash
# 完整 APK 构建
cd android && ./gradlew.bat assembleDebug

# 仅编译原生库（增量，更快）
"C:\Users\meiyuanqiao\AppData\Local\Android\Sdk\cmake\3.22.1\bin\ninja.exe" \
  -C "E:\MYQ\QymEngine\android\app\.cxx\Debug\4u1b1157\arm64-v8a" \
  SDL2 qymengine_android

# 安装到设备
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
```

## 启动与后端选择

```bash
# 默认 Vulkan 后端
adb shell am start -n com.qymengine.app/.QymActivity

# GLES 后端（通过 intent extra "args" 传参数）
adb shell am start -n com.qymengine.app/.QymActivity --es args "--gles"

# 强制停止
adb shell am force-stop com.qymengine.app
```

参数通过 `QymActivity.getArguments()` 读取 intent extra `"args"` 并按空格拆分传给 `SDL_main` 的 `argv`。

## 日志查看

```bash
# 查看引擎日志（SDL_Log 输出到 tag "SDL/APP"）
adb logcat -s "SDL/APP"

# 确认后端
adb logcat -d | grep "Using"

# 查看 GL/Vulkan 相关错误
adb logcat -d | grep -iE "error|fatal|crash" | grep -i "qym\|SDL\|vulkan\|GL"
```

## 平台注意事项

- Android assets 打包在 APK 内，不在真实文件系统上，必须通过 `SDL_RWFromFile` 或 `AAssetManager` 读取
- `std::filesystem` 在 Android 上不能访问 APK assets
- `glTextureSubImage3D` 等 Desktop GL DSA 函数在 NDK 头文件中不存在，需要 `#ifdef __ANDROID__` 保护
- GLES 后端无 `glTextureView` 支持，per-mip 采样通过 `GL_TEXTURE_BASE_LEVEL/MAX_LEVEL` 实现
