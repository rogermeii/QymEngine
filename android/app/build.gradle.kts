plugins {
    id("com.android.application")
}

val repoRootDir = rootProject.projectDir.parentFile
val hostBuildDir = File(repoRootDir, "build3")
val shaderSourceDir = File(repoRootDir, "assets/shaders")
val isWindowsHost = System.getProperty("os.name").lowercase().contains("windows")
val shaderCompilerExe = if (isWindowsHost) {
    File(hostBuildDir, "tools/shader_compiler/Debug/ShaderCompiler.exe")
} else {
    File(hostBuildDir, "tools/shader_compiler/ShaderCompiler")
}

val shaderSourceFiles = fileTree(shaderSourceDir) {
    include("*.slang")
}
val shaderCompilerSourceFiles = fileTree(File(repoRootDir, "tools/shader_compiler")) {
    include("**/*.cpp", "**/*.c", "**/*.h", "**/*.hpp", "CMakeLists.txt")
}
val shaderBundleOutputs = provider {
    shaderSourceFiles.files.map { shaderFile ->
        File(shaderSourceDir, "${shaderFile.nameWithoutExtension}.shaderbundle")
    }
}

val configureHostTools by tasks.registering(Exec::class) {
    group = "build"
    description = "配置桌面 ShaderCompiler 构建目录"
    inputs.files(
        File(repoRootDir, "CMakeLists.txt"),
        File(repoRootDir, "tools/shader_compiler/CMakeLists.txt")
    )
    outputs.file(File(hostBuildDir, "CMakeCache.txt"))
    commandLine(
        "cmake",
        "-S", repoRootDir.absolutePath,
        "-B", hostBuildDir.absolutePath
    )
}

val buildHostShaderCompiler by tasks.registering(Exec::class) {
    group = "build"
    description = "编译桌面 ShaderCompiler 工具"
    dependsOn(configureHostTools)
    inputs.files(shaderCompilerSourceFiles)
    outputs.file(shaderCompilerExe)
    commandLine(
        "cmake",
        "--build", hostBuildDir.absolutePath,
        "--config", "Debug",
        "--target", "ShaderCompiler",
        "--", "/m:4"
    )
}

val compileShaderBundles by tasks.registering(Exec::class) {
    group = "build"
    description = "编译 Android 打包所需的 shader bundle"
    dependsOn(buildHostShaderCompiler)
    inputs.files(shaderSourceFiles, shaderCompilerSourceFiles)
    outputs.files(shaderBundleOutputs)
    commandLine(
        shaderCompilerExe.absolutePath,
        shaderSourceDir.absolutePath,
        shaderSourceDir.absolutePath
    )
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
            assets.srcDirs("../../assets")
        }
    }
}

tasks.named("preBuild") {
    dependsOn(compileShaderBundles)
}
