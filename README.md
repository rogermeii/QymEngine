# QymEngine

基于 Vulkan/D3D11/D3D12/OpenGL/GLES 的图形渲染引擎与编辑器原型。

## 当前状态

- Windows 后端：`Vulkan / D3D12 / D3D11 / OpenGL / GLES`
- Android 后端：`Vulkan / GLES`
- Shader 产物：构建时自动生成 `.shaderbundle`
- 回归验证：本地脚本可覆盖 Windows 5 后端与 Android 2 后端
- CI：已接入 Windows 五后端 smoke 和 Android Debug APK 构建

## 桌面构建

首次配置：

```bat
cmake -S . -B build3 -G "Visual Studio 17 2022" -A x64
```

编译编辑器：

```bat
cmake --build build3 --config Debug --target QymEditor -- /m:4
```

完整桌面构建：

```bat
cmake --build build3 --config Debug --target ALL_BUILD -- /m:4
```

## Android 构建

```bat
cd android
gradlew.bat :app:assembleDebug
```

Android 打包前会自动：

1. 配置桌面 `ShaderCompiler`
2. 编译主机侧 `ShaderCompiler`
3. 检查并更新 `assets/shaders/*.shaderbundle`

## Shader Bundle

桌面 CMake 构建会自动触发 `CompileShaders` 目标。

相关位置：

- 构建逻辑：[CMakeLists.txt](CMakeLists.txt)
- Android 入口：[android/app/build.gradle.kts](android/app/build.gradle.kts)
- 编译器：[tools/shader_compiler/main.cpp](tools/shader_compiler/main.cpp)

手动重编全部 shader：

```bat
build3\tools\shader_compiler\Debug\ShaderCompiler.exe assets\shaders assets\shaders
```

## 本地回归脚本

Windows + Android 全量回归：

```bat
python scripts\verify_backends.py
```

只跑 Windows 五后端：

```bat
python scripts\verify_backends.py --windows-only
```

只跑 Android 两后端：

```bat
python scripts\verify_backends.py --android-only
```

Android 真机一键入口：

```bat
python scripts\verify_android_device.py
```

或：

```bat
scripts\verify_android_device.bat
```

常用参数：

- `--skip-build`：跳过 `assembleDebug`
- `--skip-install`：跳过 `adb install -r`
- `--output-dir <dir>`：指定结果输出目录

## 回归结果输出

默认输出目录：

- Windows/全量回归：`captures/verify/backend_regression`
- Android 真机一键入口：`captures/verify/android_device`

关键文件：

- `results.json`
- `summary.txt`
- `*.png`
- `*.log`

## CI 工作流

Windows 五后端 smoke：

- [Windows Backend Regression](.github/workflows/windows-backend-regression.yml)

Android Debug APK 构建：

- [Android Debug Build](.github/workflows/android-debug-build.yml)

Windows workflow 会上传：

- `results.json`
- `summary.txt`
- 关键截图
- 日志

Android workflow 会上传：

- Debug APK
- 构建日志

## 说明

- 当前工作区默认忽略 `build/`、`build2/`、`build3/`、`captures/` 等高频产物。
- 仓库内仍保留 `.shaderbundle`，方便别的机器拉代码后直接运行。
