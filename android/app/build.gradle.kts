plugins {
    id("com.android.application")
}

val repoRootDir = rootProject.projectDir.parentFile
val hostBuildDir = File(repoRootDir, "build3")
val shaderSourceDir = File(repoRootDir, "assets/shaders")
val externalAssetsDir = File(repoRootDir, "assets")
val generatedAssetsDir = layout.buildDirectory.dir("generated/qymAssets/main").get().asFile
val isWindowsHost = System.getProperty("os.name").lowercase().contains("windows")
val shaderCompilerExe = if (isWindowsHost) {
    File(hostBuildDir, "tools/shader_compiler/Debug/ShaderCompiler.exe")
} else {
    File(hostBuildDir, "tools/shader_compiler/ShaderCompiler")
}

val shaderSourceFiles = fileTree(shaderSourceDir) {
    include("**/*.slang")
}

// Shader 编译：假设 ShaderCompiler 已由 PC cmake 构建好
// 如果不存在则跳过（开发者需先在 PC 上编译引擎）
val compileShaderBundles by tasks.registering {
    group = "build"
    description = "编译 shader bundle（需要先在 PC 上编译 ShaderCompiler）"
    inputs.files(shaderSourceFiles)
    doLast {
        if (!shaderCompilerExe.exists()) {
            logger.warn("[QymEngine] ShaderCompiler not found: ${shaderCompilerExe.absolutePath}")
            logger.warn("[QymEngine] 请先在 PC 上执行 cmake --build build3 编译引擎")
            logger.warn("[QymEngine] 跳过 shader 编译，使用已有的 .shaderbundle 文件")
            return@doLast
        }
        exec {
            commandLine(
                shaderCompilerExe.absolutePath,
                "--no-msl",
                shaderSourceDir.absolutePath,
                shaderSourceDir.absolutePath
            )
        }
    }
}

// Assets 同步：独立于 shader 编译，始终执行
val syncExternalAssets by tasks.registering(Sync::class) {
    group = "build"
    description = "同步仓库根目录的 assets 到 Android 构建目录"
    from(externalAssetsDir)
    into(generatedAssetsDir)
}

android {
    namespace = "com.qymengine.app"
    compileSdk = 35

    defaultConfig {
        applicationId = "com.qymengine.app"
        minSdk = 28
        targetSdk = 35
        versionCode = 1
        versionName = "0.1"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared"
                )
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
            assets.srcDirs(generatedAssetsDir)
        }
    }
}

tasks.named("preBuild") {
    dependsOn(syncExternalAssets)
}
