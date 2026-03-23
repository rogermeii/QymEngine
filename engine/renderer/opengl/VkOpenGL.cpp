// ============================================================================
// OpenGL 4.5 后端 - VkDispatch 分发层完整实现
//
// 每个 gl_vkXxx 函数对应一个 Vulkan API，通过 vkLoadOpenGLDispatch() 注册到
// 全局函数指针表。使用 OpenGL 4.5 DSA (Direct State Access) API。
// 所有 vkCmdXxx 命令为即时模式 (immediate mode)。
// ============================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif

#include "renderer/opengl/VkOpenGLHandles.h"
#include "renderer/VkDispatch.h"
#ifdef __ANDROID__
#include <GLES3/gl32.h>
#include <GLES3/gl3ext.h>
// Android GLES: Desktop GL 4.4+ 常量在 GLES 中不存在，但 compat 代码引用它们作为标志位
#ifndef GL_DYNAMIC_STORAGE_BIT
#define GL_DYNAMIC_STORAGE_BIT 0x0100
#endif
#ifndef GL_MAP_WRITE_BIT
#define GL_MAP_WRITE_BIT 0x0002
#endif
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#endif
#ifndef GL_CLIENT_STORAGE_BIT
#define GL_CLIENT_STORAGE_BIT 0x0200
#endif
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
#ifndef GL_LOWER_LEFT_EXT
#define GL_LOWER_LEFT_EXT 0x8CA1
#endif
#ifndef GL_ZERO_TO_ONE_EXT
#define GL_ZERO_TO_ONE_EXT 0x935F
#endif
#ifndef GL_LOWER_LEFT
#define GL_LOWER_LEFT GL_LOWER_LEFT_EXT
#endif
#ifndef GL_ZERO_TO_ONE
#define GL_ZERO_TO_ONE GL_ZERO_TO_ONE_EXT
#endif
#ifndef PFNGLCLIPCONTROLEXTPROC
typedef void (GL_APIENTRYP PFNGLCLIPCONTROLEXTPROC)(GLenum origin, GLenum depth);
#endif
// Android GLES: GL_DEBUG 常量来自 GL_KHR_debug 扩展
#ifndef GL_DEBUG_OUTPUT
#define GL_DEBUG_OUTPUT 0x92E0
#endif
#ifndef GL_DEBUG_OUTPUT_SYNCHRONOUS
#define GL_DEBUG_OUTPUT_SYNCHRONOUS 0x8242
#endif
#ifndef GL_DEBUG_SEVERITY_HIGH
#define GL_DEBUG_SEVERITY_HIGH 0x9146
#endif
#ifndef GL_DEBUG_SEVERITY_MEDIUM
#define GL_DEBUG_SEVERITY_MEDIUM 0x9147
#endif
#ifndef GL_DEBUG_SEVERITY_LOW
#define GL_DEBUG_SEVERITY_LOW 0x9148
#endif
#ifndef GL_DEBUG_SEVERITY_NOTIFICATION
#define GL_DEBUG_SEVERITY_NOTIFICATION 0x826B
#endif
#ifndef GL_DEBUG_TYPE_PERFORMANCE
#define GL_DEBUG_TYPE_PERFORMANCE 0x8250
#endif
#else
#include <glad/glad.h>
#endif
#include <SDL.h>
#ifndef __ANDROID__
#include <SDL_syswm.h>
#endif
#include <iostream>
#include <cstring>
#include <algorithm>
#include <vector>
#include <set>

#ifndef GL_LOWER_LEFT_EXT
#define GL_LOWER_LEFT_EXT 0x8CA1
#endif
#ifndef GL_ZERO_TO_ONE_EXT
#define GL_ZERO_TO_ONE_EXT 0x935F
#endif
#ifndef PFNGLCLIPCONTROLEXTPROC
#if defined(GL_APIENTRYP)
typedef void (GL_APIENTRYP PFNGLCLIPCONTROLEXTPROC)(GLenum origin, GLenum depth);
#else
typedef void (APIENTRYP PFNGLCLIPCONTROLEXTPROC)(GLenum origin, GLenum depth);
#endif
#endif

// Handle cast macros: VkXxx (opaque pointer) <-> GL_Xxx (actual struct)
#define AS_GL(Type, handle) reinterpret_cast<GL_##Type*>(handle)
#define TO_VK(VkType, ptr)  reinterpret_cast<VkType>(ptr)

// 前向声明 (在文件后半部分定义)
// (GL_ARB_gl_spirv 已移除 — 设计选择 GLSL 文本方案)

// 帧级诊断日志 (仅前几帧输出)
static uint32_t s_frameCount = 0;
static constexpr uint32_t TRACE_FRAMES = 0; // 设为 >0 开启帧级诊断日志
#define GL_TRACE(fmt, ...) \
    do { if (s_frameCount < TRACE_FRAMES) fprintf(stderr, "[OpenGL F%u] " fmt "\n", s_frameCount, ##__VA_ARGS__); } while(0)

// 全局 SDL_Window 指针 — 在 vkCreateWin32SurfaceKHR 中通过 HWND 获取
static SDL_Window* s_sdlWindow = nullptr;

// GLES 运行时标记 — 在 gl_vkCreateDevice 中根据 vkIsGLESBackend() 设置
static bool s_isGLES = false;
// 某些 GLES 平台（尤其 Android/EGL）默认帧缓冲本身就是 sRGB capable。
// 这类平台如果再手动做一次 gamma 编码，会导致画面发白/过曝。
static bool s_defaultFramebufferIsSRGB = false;
static PFNGLCLIPCONTROLEXTPROC s_glClipControlEXT = nullptr;

// SPIR-V magic number (仅用于检测 ImGui shader 并替换为 GLSL)
static constexpr uint32_t SPIRV_MAGIC = 0x07230203;

// ============================================================================
// GL/GLES 兼容层
// Desktop GL 4.5 用 DSA (Direct State Access)，GLES 3.0 用 bind-to-edit
// ============================================================================

#ifdef __ANDROID__
// Android 上 s_isGLES 永远为 true，Desktop GL 4.5 DSA 函数路径不会被执行
// 定义 stub 以避免编译错误（GLES3 头文件不包含这些函数声明）
#define glCreateBuffers(n, bufs) ((void)0)
#define glNamedBufferStorage(buf, sz, data, flags) ((void)0)
#define glNamedBufferSubData(buf, off, sz, data) ((void)0)
#define glCopyNamedBufferSubData(s, d, so, doff, sz) ((void)0)
#define glCreateTextures(tgt, n, tex) ((void)0)
#define glTextureStorage2D(tex, lv, fmt, w, h) ((void)0)
#define glTextureSubImage2D(tex, lv, x, y, w, h, f, t, d) ((void)0)
#define glGetTextureImage(tex, lv, f, t, sz, px) ((void)0)
#define glCreateSamplers(n, s) ((void)0)
#define glBindTextureUnit(unit, tex) ((void)0)
#define glCreateVertexArrays(n, a) ((void)0)
#define glCreateFramebuffers(n, f) ((void)0)
#define glNamedFramebufferTexture(fbo, att, tex, lv) ((void)0)
#define glNamedFramebufferDrawBuffers(fbo, n, bufs) ((void)0)
#define glCheckNamedFramebufferStatus(fbo, tgt) GL_FRAMEBUFFER_COMPLETE
#define glVertexArrayVertexBuffer(vao, slot, buf, off, stride) ((void)0)
#define glVertexArrayElementBuffer(vao, buf) ((void)0)
#define glVertexArrayAttribFormat(vao, idx, sz, tp, n, off) ((void)0)
#define glVertexArrayAttribBinding(vao, idx, bind) ((void)0)
#define glEnableVertexArrayAttrib(vao, idx) ((void)0)
#define glDrawElementsBaseVertex(mode, cnt, type, idx, bv) ((void)0)
#define glDrawElementsInstancedBaseVertexBaseInstance(m, c, t, i, ic, bv, bi) ((void)0)
#define glDrawArraysInstancedBaseInstance(m, f, c, ic, bi) ((void)0)
#define glPolygonMode(face, mode) ((void)0)
#define glClearDepth(d) glClearDepthf((float)(d))
// GL_FILL 常量 (GLES 不支持 glPolygonMode)
#ifndef GL_FILL
#define GL_FILL 0x1B02
#endif
#ifndef GL_LINE
#define GL_LINE 0x1B01
#endif
// Desktop GL 扩展标志
#define GLAD_GL_KHR_debug 0
#endif

static void compat_CreateBuffers(GLsizei n, GLuint* buffers) {
    if (s_isGLES) glGenBuffers(n, buffers);
    else glCreateBuffers(n, buffers);
}

static void compat_BufferStorage(GLuint buffer, GLsizeiptr size, const void* data, GLbitfield flags) {
    if (s_isGLES) {
        GLenum usage = GL_STATIC_DRAW;
        if (flags & GL_DYNAMIC_STORAGE_BIT) usage = GL_DYNAMIC_DRAW;
        else if (flags & GL_MAP_READ_BIT) usage = GL_STREAM_READ;
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glBufferData(GL_ARRAY_BUFFER, size, data, usage);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    } else {
        glNamedBufferStorage(buffer, size, data, flags);
    }
}

static void compat_BufferSubData(GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data) {
    if (s_isGLES) {
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glBufferSubData(GL_ARRAY_BUFFER, offset, size, data);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    } else {
        glNamedBufferSubData(buffer, offset, size, data);
    }
}

static void compat_CopyBufferSubData(GLuint src, GLuint dst, GLintptr srcOff, GLintptr dstOff, GLsizeiptr size) {
    if (s_isGLES) {
        glBindBuffer(GL_COPY_READ_BUFFER, src);
        glBindBuffer(GL_COPY_WRITE_BUFFER, dst);
        glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, srcOff, dstOff, size);
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
        glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    } else {
        glCopyNamedBufferSubData(src, dst, srcOff, dstOff, size);
    }
}

static void compat_CreateTextures(GLenum target, GLsizei n, GLuint* textures) {
    if (s_isGLES) {
        glGenTextures(n, textures);
        // 必须绑定一次以初始化纹理目标
        for (GLsizei i = 0; i < n; i++) {
            glBindTexture(target, textures[i]);
            glBindTexture(target, 0);
        }
    } else {
        glCreateTextures(target, n, textures);
    }
}

static void compat_TextureStorage2D(GLuint tex, GLsizei levels, GLenum internalFormat, GLsizei w, GLsizei h) {
    if (s_isGLES) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexStorage2D(GL_TEXTURE_2D, levels, internalFormat, w, h);
        glBindTexture(GL_TEXTURE_2D, 0);
    } else {
        glTextureStorage2D(tex, levels, internalFormat, w, h);
    }
}

static void compat_TextureSubImage2D(GLuint tex, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void* data) {
    if (s_isGLES) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexSubImage2D(GL_TEXTURE_2D, level, x, y, w, h, fmt, type, data);
        glBindTexture(GL_TEXTURE_2D, 0);
    } else {
        glTextureSubImage2D(tex, level, x, y, w, h, fmt, type, data);
    }
}

static void compat_GetTextureImage(GLuint tex, GLint level, GLenum fmt, GLenum type, GLsizei totalBytes, void* pixels, uint32_t width, uint32_t height) {
    if (s_isGLES) {
        // GLES 没有 glGetTexImage，用 FBO + glReadPixels 替代
        GLuint tmpFbo = 0;
        glGenFramebuffers(1, &tmpFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, tmpFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, level);
        glReadPixels(0, 0, width, height, fmt, type, pixels);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &tmpFbo);
    } else {
        glGetTextureImage(tex, level, fmt, type, totalBytes, pixels);
    }
}

static void compat_CreateSamplers(GLsizei n, GLuint* samplers) {
    if (s_isGLES) glGenSamplers(n, samplers);
    else glCreateSamplers(n, samplers);
}

static void compat_BindTextureUnit(GLuint unit, GLuint texture) {
    if (s_isGLES) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, texture);
    } else {
        glBindTextureUnit(unit, texture);
    }
}

static void compat_CreateVertexArrays(GLsizei n, GLuint* arrays) {
    if (s_isGLES) glGenVertexArrays(n, arrays);
    else glCreateVertexArrays(n, arrays);
}

static void compat_CreateFramebuffers(GLsizei n, GLuint* fbos) {
    if (s_isGLES) glGenFramebuffers(n, fbos);
    else glCreateFramebuffers(n, fbos);
}

static void compat_FramebufferTexture(GLuint fbo, GLenum attachment, GLuint texture, GLint level) {
    if (s_isGLES) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, texture, level);
    } else {
        glNamedFramebufferTexture(fbo, attachment, texture, level);
    }
}

static void compat_FramebufferDrawBuffers(GLuint fbo, GLsizei n, const GLenum* bufs) {
    if (s_isGLES) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glDrawBuffers(n, bufs);
    } else {
        glNamedFramebufferDrawBuffers(fbo, n, bufs);
    }
}

static GLenum compat_CheckFramebufferStatus(GLuint fbo) {
    if (s_isGLES) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        return glCheckFramebufferStatus(GL_FRAMEBUFFER);
    } else {
        return glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER);
    }
}

// 全局: 当前绑定的 pipeline (用于 GLES 模式下 VertexAttribPointer 设置)
static VkPipeline s_gles_currentPipeline = VK_NULL_HANDLE;

static void compat_VertexArrayVertexBuffer(GLuint vao, GLuint slot, GLuint buffer, GLintptr offset, GLsizei stride) {
    if (s_isGLES) {
        // GLES 3.0: 绑定 VAO + buffer，然后设置所有属于该 binding slot 的 attrib pointers
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        // 从当前 pipeline 获取属性信息并调用 glVertexAttribPointer
        if (s_gles_currentPipeline) {
            auto* pipeline = reinterpret_cast<GL_Pipeline*>(s_gles_currentPipeline);
            static int s_vapLog = 0;
            for (uint32_t a = 0; a < pipeline->attribCount; a++) {
                if (pipeline->attribBinding[a] == slot) {
                    glVertexAttribPointer(a,
                        pipeline->attribComponents[a],
                        pipeline->attribType[a],
                        pipeline->attribNormalized[a] ? GL_TRUE : GL_FALSE,
                        stride,
                        (const void*)(uintptr_t)(offset + pipeline->attribOffset[a]));
                    // 只记录高编号 VAO（场景 pipeline）
                    if (vao >= 5 && s_vapLog < 20) {
                        SDL_Log("[VkOpenGL] VAP: vao=%u attr=%u comp=%d stride=%d offset=%ld buf=%u prog=%u",
                                vao, a, pipeline->attribComponents[a], stride,
                                (long)(offset + pipeline->attribOffset[a]), buffer, pipeline->program);
                        s_vapLog++;
                    }
                }
            }
            if (vao >= 5 && s_vapLog < 20) {
                SDL_Log("[VkOpenGL] VAP summary: vao=%u slot=%u attribCount=%u prog=%u",
                        vao, slot, pipeline->attribCount, pipeline->program);
            }
        } else {
            static bool logged = false;
            if (!logged) { SDL_Log("[VkOpenGL] VAP: s_gles_currentPipeline is NULL!"); logged = true; }
        }
    } else {
        glVertexArrayVertexBuffer(vao, slot, buffer, offset, stride);
    }
}

static void compat_VertexArrayElementBuffer(GLuint vao, GLuint buffer) {
    if (s_isGLES) {
        glBindVertexArray(vao);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
    } else {
        glVertexArrayElementBuffer(vao, buffer);
    }
}

// ============================================================================
// ImGui GLSL 着色器 (嵌入)
// ============================================================================

// ============================================================================
// GLSL 后处理: 修复 Slang 生成的 Vulkan GLSL 使其兼容 OpenGL 4.5
// ============================================================================

// 移除 layout(..., set = N, ...) 中的 set = N, 并将 binding 重新编号
// 同时将 std430 替换为 std140 (uniform blocks 不支持 std430)
// outSamplerUnits: 输出 sampler 名称 → texture unit 映射 (GLES 用)
static void replaceAll(std::string& s, const std::string& from, const std::string& to);
static std::string fixupGLSL(const std::string& source,
                             std::vector<std::pair<std::string, int>>* outSamplerUnits = nullptr)
{
    // === 第一遍: 收集所有 sampler 的 (set, binding)，按 (set, binding) 排序后分配连续编号 ===
    // 这确保 layout(binding=N) 与 flushGraphicsState 的 walk 顺序一致
    struct SamplerInfo { int set; int binding; std::string name; int newBinding; };
    std::vector<SamplerInfo> samplers;
    {
        auto lines = source;
        size_t pos = 0;
        while (pos < lines.size()) {
            size_t eol = lines.find('\n', pos);
            if (eol == std::string::npos) eol = lines.size();
            std::string line = lines.substr(pos, eol - pos);
            pos = eol + 1;
            // 检查下一行是否是 sampler 声明
            if (line.find("layout(") != std::string::npos && pos < lines.size()) {
                size_t eol2 = lines.find('\n', pos);
                if (eol2 == std::string::npos) eol2 = lines.size();
                std::string nextLine = lines.substr(pos, eol2 - pos);
                if (nextLine.find("sampler") != std::string::npos && nextLine.find("uniform") != std::string::npos) {
                    int set = 0, binding = 0;
                    auto extractInt = [&](const std::string& s, const std::string& key) -> int {
                        auto p = s.find(key);
                        if (p == std::string::npos) return -1;
                        p += key.size();
                        while (p < s.size() && s[p] == ' ') p++;
                        if (p < s.size() && s[p] == '=') p++;
                        while (p < s.size() && s[p] == ' ') p++;
                        int val = 0;
                        while (p < s.size() && s[p] >= '0' && s[p] <= '9') { val = val*10 + (s[p]-'0'); p++; }
                        return val;
                    };
                    set = extractInt(line, "set");
                    if (set < 0) set = 0;
                    binding = extractInt(line, "binding");
                    if (binding < 0) binding = 0;
                    // 提取名称
                    auto namePos = nextLine.rfind(' ');
                    std::string name = (namePos != std::string::npos) ? nextLine.substr(namePos+1) : "";
                    if (!name.empty() && name.back() == ';') name.pop_back();
                    samplers.push_back({set, binding, name, 0});
                }
            }
        }
        // 按 (set, binding) 排序
        std::sort(samplers.begin(), samplers.end(), [](const SamplerInfo& a, const SamplerInfo& b) {
            return (a.set != b.set) ? (a.set < b.set) : (a.binding < b.binding);
        });
        for (size_t i = 0; i < samplers.size(); i++)
            samplers[i].newBinding = (int)i;
    }

    // === 第二遍: 正常处理 + 替换 sampler binding ===
    std::string result;
    result.reserve(source.size() + 512);

    // 在 #version 行后面插入需要的扩展声明
    bool extensionsInserted = false;

    size_t pos = 0;
    while (pos < source.size()) {
        size_t lineEnd = source.find('\n', pos);
        if (lineEnd == std::string::npos) lineEnd = source.size();
        std::string line = source.substr(pos, lineEnd - pos);
        pos = lineEnd + 1;

        // GLES: 替换 #version 450 为 #version 300 es
        if (s_isGLES && line.find("#version") != std::string::npos) {
            // 替换整行为 GLES 版本
            size_t vPos = line.find("#version");
            result += "#version 300 es";
            if (pos <= source.size()) result += '\n';
        } else {
            result += line;
            if (pos <= source.size()) result += '\n';
        }

        // 在 #version 行之后插入扩展 (仅 Desktop GL)
        if (!extensionsInserted && line.find("#version") != std::string::npos) {
            if (s_isGLES) {
                result += "precision highp float;\n";
            } else {
                result += "#extension GL_ARB_separate_shader_objects : enable\n";
                // 允许 std430 在 uniform blocks 中使用 (NVIDIA 扩展，绝大多数桌面驱动支持)
                result += "#extension GL_NV_uniform_buffer_std430_layout : enable\n";
            }
            extensionsInserted = true;
        }
    }

    // 移除 layout 中的 "set = N" — 在结果上做第二遍处理
    std::string pass2;
    pass2.reserve(result.size());
    pos = 0;
    while (pos < result.size()) {
        size_t lineEnd = result.find('\n', pos);
        if (lineEnd == std::string::npos) lineEnd = result.size();
        std::string line = result.substr(pos, lineEnd - pos);
        pos = lineEnd + 1;

        // 移除 layout 中的 "set = N"
        size_t layoutPos = line.find("layout(");
        if (layoutPos != std::string::npos) {
            size_t parenEnd = line.find(')', layoutPos);
            if (parenEnd != std::string::npos) {
                std::string layoutContent = line.substr(layoutPos + 7, parenEnd - layoutPos - 7);
                std::string cleaned;
                size_t i = 0;
                while (i < layoutContent.size()) {
                    while (i < layoutContent.size() && layoutContent[i] == ' ') i++;
                    if (i >= layoutContent.size()) break;
                    if (layoutContent.substr(i, 3) == "set") {
                        size_t j = i + 3;
                        while (j < layoutContent.size() && layoutContent[j] == ' ') j++;
                        if (j < layoutContent.size() && layoutContent[j] == '=') {
                            j++;
                            while (j < layoutContent.size() && (layoutContent[j] == ' ' || (layoutContent[j] >= '0' && layoutContent[j] <= '9'))) j++;
                            while (j < layoutContent.size() && (layoutContent[j] == ',' || layoutContent[j] == ' ')) j++;
                            i = j;
                            continue;
                        }
                    }
                    size_t commaPos = layoutContent.find(',', i);
                    if (commaPos == std::string::npos) commaPos = layoutContent.size();
                    std::string token = layoutContent.substr(i, commaPos - i);
                    while (!token.empty() && token.back() == ' ') token.pop_back();
                    if (!token.empty()) {
                        if (!cleaned.empty()) cleaned += ", ";
                        cleaned += token;
                    }
                    i = (commaPos < layoutContent.size()) ? commaPos + 1 : layoutContent.size();
                }
                line = line.substr(0, layoutPos + 7) + cleaned + line.substr(parenEnd);

                // 如果下一行是 sampler，替换 binding 值为按 (set,binding) 排序后的编号
                if (pos < result.size()) {
                    size_t peekEnd = result.find('\n', pos);
                    if (peekEnd == std::string::npos) peekEnd = result.size();
                    std::string nextLine = result.substr(pos, peekEnd - pos);
                    if (nextLine.find("sampler") != std::string::npos && nextLine.find("uniform") != std::string::npos) {
                        // 从 nextLine 提取 sampler 名称
                        auto sp = nextLine.rfind(' ');
                        std::string sName = (sp != std::string::npos) ? nextLine.substr(sp + 1) : "";
                        if (!sName.empty() && sName.back() == ';') sName.pop_back();
                        // 查找对应的 newBinding
                        for (auto& si : samplers) {
                            if (si.name == sName) {
                                // 替换 binding = N
                                size_t bPos = line.find("binding");
                                if (bPos != std::string::npos) {
                                    size_t eqPos = line.find('=', bPos);
                                    if (eqPos != std::string::npos) {
                                        size_t numStart = eqPos + 1;
                                        while (numStart < line.size() && line[numStart] == ' ') numStart++;
                                        size_t numEnd = numStart;
                                        while (numEnd < line.size() && line[numEnd] >= '0' && line[numEnd] <= '9') numEnd++;
                                        line = line.substr(0, numStart) + std::to_string(si.newBinding) + line.substr(numEnd);
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }

        pass2 += line;
        if (pos <= result.size()) pass2 += '\n';
    }

    // Vulkan GLSL 内建变量 → 标准 OpenGL GLSL
    // gl_VertexIndex → gl_VertexID, gl_InstanceIndex → gl_InstanceID
    {
        size_t p = 0;
        while ((p = pass2.find("gl_VertexIndex", p)) != std::string::npos) {
            pass2.replace(p, 14, "gl_VertexID   ");
            p += 14;
        }
        p = 0;
        while ((p = pass2.find("gl_InstanceIndex", p)) != std::string::npos) {
            pass2.replace(p, 16, "gl_InstanceID   ");
            p += 16;
        }
    }

    // 移除 layout(push_constant) — OpenGL 没有 push constant，用 UBO 替代
    {
        size_t p = 0;
        while ((p = pass2.find("layout(push_constant)", p)) != std::string::npos) {
            pass2.replace(p, 21, "/* push_constant */  ");
            p += 21;
        }
    }

    // GLES: std430 → std140 (GLES 3.0 不支持 uniform block 的 std430 布局)
    if (s_isGLES) {
        size_t p = 0;
        while ((p = pass2.find("std430", p)) != std::string::npos) {
            pass2.replace(p, 6, "std140");
            p += 6;
        }
    }

    // GLES: 移除残留的 #extension 行 (GLES 不支持 GL_ARB_separate_shader_objects 等)
    if (s_isGLES) {
        size_t p = 0;
        while ((p = pass2.find("#extension GL_ARB_separate_shader_objects", p)) != std::string::npos) {
            size_t eol = pass2.find('\n', p);
            if (eol != std::string::npos)
                pass2.erase(p, eol - p + 1);
            else
                pass2.erase(p);
        }
        p = 0;
        while ((p = pass2.find("#extension GL_NV_uniform_buffer_std430_layout", p)) != std::string::npos) {
            size_t eol = pass2.find('\n', p);
            if (eol != std::string::npos)
                pass2.erase(p, eol - p + 1);
            else
                pass2.erase(p);
        }
    }

    // GLES: C-style 初始化器 → 构造函数调用
    // 1) "Type var[N] = { ... }" → "Type var[N] = Type[N]( ... )"  (数组)
    // 2) "Type var = { ... }"    → "Type var = Type( ... )"          (struct)
    if (s_isGLES) {
        std::string pass_init;
        pass_init.reserve(pass2.size());
        size_t p = 0;
        while (p < pass2.size()) {
            size_t lineEnd = pass2.find('\n', p);
            if (lineEnd == std::string::npos) lineEnd = pass2.size();
            std::string line = pass2.substr(p, lineEnd - p);
            p = lineEnd + 1;

            // 查找 "= {" 模式
            size_t eqBrace = line.find("= {");
            if (eqBrace == std::string::npos) eqBrace = line.find("={");
            if (eqBrace != std::string::npos) {
                // 找匹配的 }（可能嵌套）
                int depth = 0;
                size_t closeBrace = std::string::npos;
                for (size_t k = eqBrace; k < line.size(); k++) {
                    if (line[k] == '{') depth++;
                    else if (line[k] == '}') { depth--; if (depth == 0) { closeBrace = k; break; } }
                }
                if (closeBrace != std::string::npos) {
                    std::string prefix = line.substr(0, eqBrace);
                    while (!prefix.empty() && prefix.back() == ' ') prefix.pop_back();

                    // 检查是否是数组类型: "Type varname[N]"
                    std::string arraySize;
                    if (prefix.size() >= 3 && prefix.back() == ']') {
                        size_t bracketOpen = prefix.rfind('[');
                        if (bracketOpen != std::string::npos) {
                            arraySize = prefix.substr(bracketOpen); // "[N]"
                            prefix = prefix.substr(0, bracketOpen);
                            while (!prefix.empty() && prefix.back() == ' ') prefix.pop_back();
                        }
                    }

                    // 提取变量名 (最后一个标识符)
                    size_t varEnd = prefix.size();
                    size_t varStart = varEnd;
                    while (varStart > 0 && (isalnum(prefix[varStart-1]) || prefix[varStart-1] == '_'))
                        varStart--;
                    // 跳过空格找类型名
                    size_t typeEnd = varStart;
                    while (typeEnd > 0 && prefix[typeEnd-1] == ' ') typeEnd--;
                    size_t typeStart = typeEnd;
                    while (typeStart > 0 && (isalnum(prefix[typeStart-1]) || prefix[typeStart-1] == '_'))
                        typeStart--;
                    std::string typeName = prefix.substr(typeStart, typeEnd - typeStart);

                    if (!typeName.empty()) {
                        size_t braceStart = line.find('{', eqBrace);
                        std::string values = line.substr(braceStart + 1, closeBrace - braceStart - 1);
                        // 数组: Type[N]( ... )  struct: Type( ... )
                        std::string ctor = typeName + arraySize + "(" + values + ")";
                        line = line.substr(0, eqBrace) + "= " + ctor + line.substr(closeBrace + 1);
                    }
                }
            }

            pass_init += line;
            if (p <= pass2.size()) pass_init += '\n';
        }
        pass2 = std::move(pass_init);
    }

    // GLES: 移除 layout(column_major) buffer; 语句 (GLES 3.0 没有 buffer 限定符)
    if (s_isGLES) {
        size_t p = 0;
        while ((p = pass2.find("layout(column_major) buffer;", p)) != std::string::npos) {
            size_t eol = pass2.find('\n', p);
            if (eol != std::string::npos)
                pass2.replace(p, eol - p, "/* layout(column_major) buffer; */");
            else
                pass2.replace(p, 28, "/* layout(column_major) buffer; */");
            p += 30;
        }
    }

    // GLES: 移除 layout(binding = N) 中的 binding 限定符
    // GLES 3.0 不支持 layout(binding=N)，UBO 绑定通过 glUniformBlockBinding 在运行时设置
    // sampler 绑定通过 glUniform1i 设置
    if (s_isGLES) {
        // 移除 layout 中的 "binding = N"
        std::string pass3;
        pass3.reserve(pass2.size());
        size_t p = 0;
        while (p < pass2.size()) {
            size_t lineEnd = pass2.find('\n', p);
            if (lineEnd == std::string::npos) lineEnd = pass2.size();
            std::string line = pass2.substr(p, lineEnd - p);
            p = lineEnd + 1;

            size_t layoutPos = line.find("layout(");
            if (layoutPos != std::string::npos) {
                size_t parenEnd = line.find(')', layoutPos);
                if (parenEnd != std::string::npos) {
                    std::string content = line.substr(layoutPos + 7, parenEnd - layoutPos - 7);
                    // 移除 binding = N
                    std::string cleaned;
                    size_t i = 0;
                    while (i < content.size()) {
                        while (i < content.size() && content[i] == ' ') i++;
                        if (i >= content.size()) break;
                        if (content.substr(i, 7) == "binding") {
                            size_t j = i + 7;
                            while (j < content.size() && content[j] == ' ') j++;
                            if (j < content.size() && content[j] == '=') {
                                j++;
                                while (j < content.size() && (content[j] == ' ' || (content[j] >= '0' && content[j] <= '9'))) j++;
                                while (j < content.size() && (content[j] == ',' || content[j] == ' ')) j++;
                                i = j;
                                continue;
                            }
                        }
                        size_t commaPos = content.find(',', i);
                        if (commaPos == std::string::npos) commaPos = content.size();
                        std::string token = content.substr(i, commaPos - i);
                        while (!token.empty() && token.back() == ' ') token.pop_back();
                        if (!token.empty()) {
                            if (!cleaned.empty()) cleaned += ", ";
                            cleaned += token;
                        }
                        i = (commaPos < content.size()) ? commaPos + 1 : content.size();
                    }
                    if (cleaned.empty()) {
                        // layout() 变空了，移除整个 layout()
                        line = line.substr(0, layoutPos) + line.substr(parenEnd + 1);
                        // 移除前导空白
                        while (!line.empty() && line[0] == ' ') line.erase(0, 1);
                    } else {
                        line = line.substr(0, layoutPos + 7) + cleaned + line.substr(parenEnd);
                    }
                }
            }

            pass3 += line;
            if (p <= pass2.size()) pass3 += '\n';
        }
        pass2 = std::move(pass3);
    }

    // 输出 sampler 名称 → texture unit 映射 (GLES 需要在 pipeline 创建时通过 glUniform1i 设置)
    if (outSamplerUnits) {
        outSamplerUnits->clear();
        for (auto& si : samplers) {
            outSamplerUnits->push_back({si.name, si.newBinding});
        }
    }

    // GLES: 去除重复 struct 定义 (Slang GLSL 输出可能在 VS/FS 拼接后有重复)
    // Adreno GLES 编译器对此严格报错
    if (s_isGLES) {
        std::set<std::string> seenStructs;
        std::string cleaned;
        cleaned.reserve(pass2.size());
        size_t p = 0;
        while (p < pass2.size()) {
            size_t lineEnd = pass2.find('\n', p);
            if (lineEnd == std::string::npos) lineEnd = pass2.size();
            std::string line = pass2.substr(p, lineEnd - p);
            p = lineEnd + 1;

            // 检测 "struct Name" 行
            size_t structPos = line.find("struct ");
            if (structPos != std::string::npos && line.find('{') == std::string::npos &&
                line.find(';') == std::string::npos && line.find('(') == std::string::npos) {
                // 提取 struct 名称 (可能带 #line 前缀)
                std::string afterStruct = line.substr(structPos + 7);
                while (!afterStruct.empty() && afterStruct.back() == ' ') afterStruct.pop_back();
                if (!afterStruct.empty()) {
                    if (!seenStructs.insert(afterStruct).second) {
                        // 重复 struct：跳过到对应的 "};"
                        int depth = 0;
                        bool foundOpen = false;
                        while (p < pass2.size()) {
                            size_t le = pass2.find('\n', p);
                            if (le == std::string::npos) le = pass2.size();
                            std::string l = pass2.substr(p, le - p);
                            p = le + 1;
                            for (char c : l) {
                                if (c == '{') { depth++; foundOpen = true; }
                                if (c == '}') depth--;
                            }
                            if (foundOpen && depth <= 0) break;
                        }
                        continue; // 跳过已添加的行
                    }
                }
            }
            cleaned += line;
            if (p <= pass2.size()) cleaned += '\n';
        }
        pass2 = std::move(cleaned);
    }

    // GLES 整数 varying 的 flat 修饰符处理放到后面统一处理（合并 layout 行后）

    // GLES: Adreno 编译器不接受 layout qualifier 和 storage qualifier 分开两行
    // 合并 "layout(...)\n[flat ]out/in ..." 为单行，同时移除 #line 指令
    if (s_isGLES) {
        // 先移除所有 #line 指令 (简化 Adreno 编译)
        {
            std::string noLine;
            noLine.reserve(pass2.size());
            size_t p = 0;
            while (p < pass2.size()) {
                size_t lineEnd = pass2.find('\n', p);
                if (lineEnd == std::string::npos) lineEnd = pass2.size();
                std::string line = pass2.substr(p, lineEnd - p);
                p = lineEnd + 1;
                // 跳过 #line 指令
                size_t firstNS = line.find_first_not_of(' ');
                if (firstNS != std::string::npos && line.substr(firstNS, 5) == "#line")
                    continue;
                noLine += line + "\n";
            }
            pass2 = std::move(noLine);
        }
        // 合并 layout() + 变量声明
        {
            std::string merged;
            merged.reserve(pass2.size());
            size_t p = 0;
            while (p < pass2.size()) {
                size_t lineEnd = pass2.find('\n', p);
                if (lineEnd == std::string::npos) lineEnd = pass2.size();
                std::string line = pass2.substr(p, lineEnd - p);
                p = lineEnd + 1;

                std::string trimmed = line;
                while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
                // layout(...) 独占一行且不是 uniform → 合并下一行
                if (trimmed.size() > 7 && trimmed.back() == ')' &&
                    trimmed.find("layout(") != std::string::npos &&
                    trimmed.find("uniform") == std::string::npos) {
                    if (p < pass2.size()) {
                        size_t nextEnd = pass2.find('\n', p);
                        if (nextEnd == std::string::npos) nextEnd = pass2.size();
                        std::string nextLine = pass2.substr(p, nextEnd - p);
                        size_t ns = nextLine.find_first_not_of(' ');
                        if (ns != std::string::npos) {
                            std::string start = nextLine.substr(ns, 5);
                            if (start.substr(0,4) == "out " || start.substr(0,3) == "in " ||
                                start.substr(0,5) == "flat ") {
                                merged += trimmed + " " + nextLine.substr(ns) + "\n";
                                p = nextEnd + 1;
                                continue;
                            }
                        }
                    }
                }
                merged += line + "\n";
            }
            pass2 = std::move(merged);
        }
    }

    // GLES: 整数 varying 必须有 flat，且 flat 必须在 layout 之前
    // 处理合并后的行: "layout(location=N) flat out int" → "flat layout(location=N) out int"
    // 和缺少 flat 的: "layout(location=N) out int" → "flat layout(location=N) out int"
    // 以及无 layout 的: "in int" → "flat in int"
    if (s_isGLES) {
        std::string fixFlat;
        fixFlat.reserve(pass2.size());
        size_t p = 0;
        while (p < pass2.size()) {
            size_t lineEnd = pass2.find('\n', p);
            if (lineEnd == std::string::npos) lineEnd = pass2.size();
            std::string line = pass2.substr(p, lineEnd - p);
            p = lineEnd + 1;

            // 检测整数 varying (out int/in int/out ivec/in ivec)
            bool hasInt = (line.find("out int ") != std::string::npos ||
                           line.find("in int ") != std::string::npos ||
                           line.find("out ivec") != std::string::npos ||
                           line.find("in ivec") != std::string::npos);
            // 排除 struct 成员 (缩进的行) 和 uniform block 成员
            if (hasInt && line.find_first_not_of(' ') > 0)
                hasInt = false;

            if (hasInt) {
                // 整数 varying 需要 flat 修饰符 (GLES 3.0 要求)
                // 注: Slang shader 中 highlighted 已改为 float 避免 Adreno 兼容问题
                // 此处处理其他可能的 int varying
                if (line.find("flat") == std::string::npos) {
                    size_t outPos = line.find("out ");
                    size_t inPos = line.find("in ");
                    size_t insertPos = (outPos != std::string::npos) ? outPos :
                                       (inPos != std::string::npos) ? inPos : std::string::npos;
                    if (insertPos != std::string::npos)
                        line.insert(insertPos, "flat ");
                }
            }

            fixFlat += line + "\n";
        }
        pass2 = std::move(fixFlat);
    }

    if (s_isGLES) {
        replaceAll(pass2, "layout(row_major) uniform;\n", "");
        replaceAll(pass2, "layout(row_major) buffer;\n", "");
        replaceAll(pass2,
            "struct _MatrixStorage_float4x4_ColMajorstd140_0\n{\n    vec4  data_0[4];\n};",
            "struct _MatrixStorage_float4x4_ColMajorstd140_0\n{\n    mat4 data_0;\n};");
        replaceAll(pass2,
            "struct _MatrixStorage_float4x4_ColMajorstd430_0\n{\n    vec4  data_0[4];\n};",
            "struct _MatrixStorage_float4x4_ColMajorstd430_0\n{\n    mat4 data_0;\n};");
        replaceAll(pass2,
            "struct _MatrixStorage_float4x4_ColMajorstd140_0\n{\n    mat4 data_0;\n};",
            "");
        replaceAll(pass2,
            "struct _MatrixStorage_float4x4_ColMajorstd430_0\n{\n    mat4 data_0;\n};",
            "");
        replaceAll(pass2, "_MatrixStorage_float4x4_ColMajorstd140_0", "mat4");
        replaceAll(pass2, "_MatrixStorage_float4x4_ColMajorstd430_0", "mat4");
        replaceAll(pass2,
            "return mat4x4(_S1.data_0[0][0], _S1.data_0[1][0], _S1.data_0[2][0], _S1.data_0[3][0], _S1.data_0[0][1], _S1.data_0[1][1], _S1.data_0[2][1], _S1.data_0[3][1], _S1.data_0[0][2], _S1.data_0[1][2], _S1.data_0[2][2], _S1.data_0[3][2], _S1.data_0[0][3], _S1.data_0[1][3], _S1.data_0[2][3], _S1.data_0[3][3]);",
            "return _S1.data_0;");
        replaceAll(pass2,
            "return mat4x4(_S2.data_0[0][0], _S2.data_0[1][0], _S2.data_0[2][0], _S2.data_0[3][0], _S2.data_0[0][1], _S2.data_0[1][1], _S2.data_0[2][1], _S2.data_0[3][1], _S2.data_0[0][2], _S2.data_0[1][2], _S2.data_0[2][2], _S2.data_0[3][2], _S2.data_0[0][3], _S2.data_0[1][3], _S2.data_0[2][3], _S2.data_0[3][3]);",
            "return _S2.data_0;");
        replaceAll(pass2, "return _S1.data_0;", "return _S1;");
        replaceAll(pass2, "return _S2.data_0;", "return _S2;");
        replaceAll(pass2,
            "vec4 worldPos_1 = (((vec4(input_position_0, 1.0)) * (unpackStorage_1(pc_0.model_0))));",
            "vec4 worldPos_1 = (unpackStorage_1(pc_0.model_0) * vec4(input_position_0, 1.0));");
        replaceAll(pass2,
            "vec4 worldPos_0 = (((vec4(input_position_0, 1.0)) * (unpackStorage_1(pc_0.model_0))));",
            "vec4 worldPos_0 = (unpackStorage_1(pc_0.model_0) * vec4(input_position_0, 1.0));");
        replaceAll(pass2,
            "vec4 _S3 = ((((((worldPos_1) * (unpackStorage_0(frame_0.view_0))))) * (unpackStorage_0(frame_0.proj_0))));",
            "vec4 _S3 = (transpose(unpackStorage_0(frame_0.proj_0)) * (transpose(unpackStorage_0(frame_0.view_0)) * worldPos_1));");
        replaceAll(pass2,
            "gl_Position = (((worldPos_0) * (unpackStorage_0(frame_0.lightVP_0))));",
            "gl_Position = (transpose(unpackStorage_0(frame_0.lightVP_0)) * worldPos_0);");
        replaceAll(pass2,
            "vec4 lightClip_0 = (((vec4(worldPos_0, 1.0)) * (unpackStorage_0(frame_0.lightVP_0))));",
            "vec4 lightClip_0 = (transpose(unpackStorage_0(frame_0.lightVP_0)) * vec4(worldPos_0, 1.0));");
        replaceAll(pass2,
            "mat4x4 viewProjInv_0 = mat4Inverse_0((((unpackStorage_0(frame_0.view_0)) * (unpackStorage_0(frame_0.proj_0)))));",
            "mat4x4 viewProjInv_0 = mat4Inverse_0((transpose(unpackStorage_0(frame_0.proj_0)) * transpose(unpackStorage_0(frame_0.view_0))));");
        replaceAll(pass2,
            "vec4 unprojected_0 = (((vec4(p_0, 1.0)) * (viewProjInv_0)));",
            "vec4 unprojected_0 = (viewProjInv_0 * vec4(p_0, 1.0));");
        replaceAll(pass2,
            "vec4 clipPos_0 = ((((((vec4(fragPos_1, 1.0)) * (unpackStorage_0(frame_0.view_0))))) * (unpackStorage_0(frame_0.proj_0))));",
            "vec4 clipPos_0 = (transpose(unpackStorage_0(frame_0.proj_0)) * (transpose(unpackStorage_0(frame_0.view_0)) * vec4(fragPos_1, 1.0)));");
        replaceAll(pass2,
            "vec3 _S7 = (((input_normal_0) * (_S6)));",
            "vec3 _S7 = (_S6 * input_normal_0);");
        replaceAll(pass2,
            "mat4x4 _S5 = unpackStorage_1(pc_0.model_0);",
            "mat4x4 _S5 = unpackStorage_1(pc_0.model_0);");
    }

    return pass2;
}

static std::vector<std::pair<std::string, int>> extractVertexInputs(const std::string& source)
{
    std::vector<std::pair<std::string, int>> inputs;
    size_t pos = 0;
    while (pos < source.size()) {
        size_t lineEnd = source.find('\n', pos);
        if (lineEnd == std::string::npos) lineEnd = source.size();
        std::string line = source.substr(pos, lineEnd - pos);
        pos = lineEnd + 1;

        size_t layoutPos = line.find("layout(location");
        size_t inPos = line.find(" in ");
        if (layoutPos == std::string::npos || inPos == std::string::npos)
            continue;

        size_t eqPos = line.find('=', layoutPos);
        if (eqPos == std::string::npos)
            continue;
        size_t numStart = eqPos + 1;
        while (numStart < line.size() && line[numStart] == ' ') numStart++;
        size_t numEnd = numStart;
        while (numEnd < line.size() && line[numEnd] >= '0' && line[numEnd] <= '9') numEnd++;
        if (numEnd == numStart)
            continue;
        int location = std::stoi(line.substr(numStart, numEnd - numStart));

        size_t nameEnd = line.find(';', inPos);
        if (nameEnd == std::string::npos)
            nameEnd = line.size();
        size_t lastSpace = line.rfind(' ', nameEnd);
        if (lastSpace == std::string::npos || lastSpace <= inPos)
            continue;
        std::string name = line.substr(lastSpace + 1, nameEnd - lastSpace - 1);
        if (!name.empty())
            inputs.push_back({name, location});
    }
    return inputs;
}

static void replaceAll(std::string& s, const std::string& from, const std::string& to)
{
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

static const char* s_imguiVS_glsl = R"(
#version 450
layout(std140, binding = 0) uniform Constants {
    vec2 uScale;
    vec2 uTranslate;
};
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;
out vec4 fragColor;
out vec2 fragUV;
void main() {
    gl_Position = vec4(pos * uScale + uTranslate, 0.0, 1.0);
    gl_Position.y = -gl_Position.y;  // OpenGL Y-up, ImGui/Vulkan Y-down
    fragColor = color;
    fragUV = uv;
}
)";

static const char* s_imguiPS_glsl = R"(
#version 450
layout(binding = 0) uniform sampler2D tex;
in vec4 fragColor;
in vec2 fragUV;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = fragColor * texture(tex, fragUV);
}
)";

// GLES 3.0 ImGui 顶点着色器
static const char* s_imguiVS_gles = R"(#version 300 es
precision highp float;
layout(std140) uniform Constants {
    vec2 uScale;
    vec2 uTranslate;
};
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;
out vec4 fragColor;
out vec2 fragUV;
void main() {
    gl_Position = vec4(pos * uScale + uTranslate, 0.0, 1.0);
    gl_Position.y = -gl_Position.y;
    fragColor = color;
    fragUV = uv;
})";

// GLES 3.0 ImGui 片段着色器
// 非 sRGB 默认帧缓冲: 需要手动 sRGB 编码
static const char* s_imguiPS_gles_manual_srgb = R"(#version 300 es
precision mediump float;
uniform sampler2D tex;
in vec4 fragColor;
in vec2 fragUV;
layout(location = 0) out vec4 outColor;
void main() {
    vec4 color = fragColor * texture(tex, fragUV);
    // 手动 sRGB 编码 (线性 → sRGB gamma)
    color.rgb = pow(color.rgb, vec3(1.0/2.2));
    outColor = color;
})";

// sRGB 默认帧缓冲: 驱动/系统会自动处理编码，不能重复 gamma
static const char* s_imguiPS_gles_auto_srgb = R"(#version 300 es
precision mediump float;
uniform sampler2D tex;
in vec4 fragColor;
in vec2 fragUV;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = fragColor * texture(tex, fragUV);
})";

// GLES 场景诊断 shader：
// 仅用于 4 路顶点输入的场景网格管线，绕过 Slang 生成的 _MatrixStorage/unpackStorage 路径，
// 直接验证 std140 mat4 + push-constant UBO 是否能正常完成位置变换。
static const char* s_glesSceneVS = R"(#version 300 es
precision highp float;
layout(std140) uniform block_FrameData_std140_0 {
    mat4 view_0;
    mat4 proj_0;
} frame_0;
layout(std140) uniform block_MaterialParams_std140_0 {
    vec4 baseColor_0;
    float metallic_0;
    float roughness_0;
    vec2 _pad1;
} materialParams_0;
layout(std140) uniform block_PushConstants_std140_0 {
    mat4 model_0;
    int highlighted_0;
    uint materialIndex_0;
    vec2 _pad0;
} pc_0;
layout(location = 0) in vec3 input_position_0;
layout(location = 1) in vec3 input_color_0;
layout(location = 2) in vec2 input_texCoord_0;
layout(location = 3) in vec3 input_normal_0;
out vec2 sceneTexCoord;
out vec4 sceneBaseColor;
out float sceneHighlighted;
void main() {
    vec4 worldPos = pc_0.model_0 * vec4(input_position_0, 1.0);
    gl_Position = transpose(frame_0.proj_0) * (transpose(frame_0.view_0) * worldPos);
    sceneTexCoord = input_texCoord_0;
    sceneBaseColor = materialParams_0.baseColor_0;
    sceneHighlighted = float(pc_0.highlighted_0);
})";

static const char* s_glesSceneFS = R"(#version 300 es
precision mediump float;
uniform sampler2D albedoMap_0;
in vec2 sceneTexCoord;
in vec4 sceneBaseColor;
in float sceneHighlighted;
layout(location = 0) out vec4 entryPointParam_fragmentMain_0;
void main() {
    if (sceneHighlighted > 0.5) {
        entryPointParam_fragmentMain_0 = sceneBaseColor;
        return;
    }
    entryPointParam_fragmentMain_0 = texture(albedoMap_0, sceneTexCoord) * sceneBaseColor;
})";


// ============================================================================
// 格式转换辅助函数
// ============================================================================

struct GLFormatInfo {
    GLenum internalFormat;
    GLenum format;
    GLenum type;
    uint32_t texelSize;
    bool isDepth;
};

static GLFormatInfo vkFormatToGL(VkFormat vkFmt)
{
    switch (vkFmt) {
    case VK_FORMAT_R8_UNORM:            return {GL_R8,      GL_RED,   GL_UNSIGNED_BYTE,  1, false};
    case VK_FORMAT_R8G8_UNORM:          return {GL_RG8,     GL_RG,    GL_UNSIGNED_BYTE,  2, false};
    case VK_FORMAT_R8G8B8A8_UNORM:      return {GL_RGBA8,   GL_RGBA,  GL_UNSIGNED_BYTE,  4, false};
    case VK_FORMAT_R8G8B8A8_SRGB:       return {GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false};
    // 注意: GLES 3.0 不支持 GL_BGRA 作为像素传输格式，回退到 GL_RGBA
    case VK_FORMAT_B8G8R8A8_UNORM:      return {GL_RGBA8,   (GLenum)(s_isGLES ? GL_RGBA : GL_BGRA),  GL_UNSIGNED_BYTE,  4, false};
    case VK_FORMAT_B8G8R8A8_SRGB:       return {GL_SRGB8_ALPHA8, (GLenum)(s_isGLES ? GL_RGBA : GL_BGRA), GL_UNSIGNED_BYTE, 4, false};
    case VK_FORMAT_R32G32B32A32_SFLOAT:  return {GL_RGBA32F, GL_RGBA,  GL_FLOAT,          16, false};
    case VK_FORMAT_R32G32B32_SFLOAT:     return {GL_RGB32F,  GL_RGB,   GL_FLOAT,          12, false};
    case VK_FORMAT_R32G32_SFLOAT:        return {GL_RG32F,   GL_RG,    GL_FLOAT,          8, false};
    case VK_FORMAT_R32_SFLOAT:           return {GL_R32F,    GL_RED,   GL_FLOAT,          4, false};
    case VK_FORMAT_R16G16B16A16_SFLOAT:  return {GL_RGBA16F, GL_RGBA,  GL_HALF_FLOAT,     8, false};
    case VK_FORMAT_D32_SFLOAT:           return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 4, true};
    case VK_FORMAT_D24_UNORM_S8_UINT:    return {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 4, true};
    case VK_FORMAT_D16_UNORM:            return {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 2, true};
    default:                             return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 4, false};
    }
}

static GLenum vkTopologyToGL(VkPrimitiveTopology topo)
{
    switch (topo) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:     return GL_POINTS;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:      return GL_LINES;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:     return GL_LINE_STRIP;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:  return GL_TRIANGLES;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:   return GL_TRIANGLE_FAN;
    default:                                   return GL_TRIANGLES;
    }
}

static GLenum vkCompareOpToGL(VkCompareOp op)
{
    switch (op) {
    case VK_COMPARE_OP_NEVER:            return GL_NEVER;
    case VK_COMPARE_OP_LESS:             return GL_LESS;
    case VK_COMPARE_OP_EQUAL:            return GL_EQUAL;
    case VK_COMPARE_OP_LESS_OR_EQUAL:    return GL_LEQUAL;
    case VK_COMPARE_OP_GREATER:          return GL_GREATER;
    case VK_COMPARE_OP_NOT_EQUAL:        return GL_NOTEQUAL;
    case VK_COMPARE_OP_GREATER_OR_EQUAL: return GL_GEQUAL;
    case VK_COMPARE_OP_ALWAYS:           return GL_ALWAYS;
    default:                             return GL_LESS;
    }
}

static GLenum vkBlendFactorToGL(VkBlendFactor factor)
{
    switch (factor) {
    case VK_BLEND_FACTOR_ZERO:                 return GL_ZERO;
    case VK_BLEND_FACTOR_ONE:                  return GL_ONE;
    case VK_BLEND_FACTOR_SRC_COLOR:            return GL_SRC_COLOR;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:  return GL_ONE_MINUS_SRC_COLOR;
    case VK_BLEND_FACTOR_DST_COLOR:            return GL_DST_COLOR;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:  return GL_ONE_MINUS_DST_COLOR;
    case VK_BLEND_FACTOR_SRC_ALPHA:            return GL_SRC_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:  return GL_ONE_MINUS_SRC_ALPHA;
    case VK_BLEND_FACTOR_DST_ALPHA:            return GL_DST_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:  return GL_ONE_MINUS_DST_ALPHA;
    case VK_BLEND_FACTOR_CONSTANT_COLOR:       return GL_CONSTANT_COLOR;
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return GL_ONE_MINUS_CONSTANT_COLOR;
    default:                                   return GL_ONE;
    }
}

static GLenum vkBlendOpToGL(VkBlendOp op)
{
    switch (op) {
    case VK_BLEND_OP_ADD:              return GL_FUNC_ADD;
    case VK_BLEND_OP_SUBTRACT:         return GL_FUNC_SUBTRACT;
    case VK_BLEND_OP_REVERSE_SUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
    case VK_BLEND_OP_MIN:              return GL_MIN;
    case VK_BLEND_OP_MAX:              return GL_MAX;
    default:                           return GL_FUNC_ADD;
    }
}

static bool detectDefaultFramebufferSRGB()
{
#ifdef GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING
#ifdef __ANDROID__
    {
#else
    if (glGetFramebufferAttachmentParameteriv) {
#endif
        GLint prevFbo = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        GLint encoding = GL_LINEAR;
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK,
            GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
        GLenum err = glGetError();

        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));

        if (err == GL_NO_ERROR) {
            bool isSRGB = (encoding == GL_SRGB);
            SDL_Log("[VkOpenGL] GL reports default framebuffer encoding = 0x%x", encoding);
            return isSRGB;
        }
    }
#endif

    int sdlSRGBCapable = 0;
    if (SDL_GL_GetAttribute(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, &sdlSRGBCapable) == 0 && sdlSRGBCapable != 0) {
        SDL_Log("[VkOpenGL] SDL reports default framebuffer is sRGB-capable");
        return true;
    }

    SDL_Log("[VkOpenGL] Default framebuffer sRGB capability unknown; assuming linear");
    return false;
}

static int vkFormatComponents(VkFormat fmt)
{
    switch (fmt) {
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R8_UNORM:
        return 1;
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R8G8_UNORM:
        return 2;
    case VK_FORMAT_R32G32B32_SFLOAT:
        return 3;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return 4;
    default:
        return 4;
    }
}

static GLenum vkFormatGLType(VkFormat fmt)
{
    switch (fmt) {
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32G32_SFLOAT:
    case VK_FORMAT_R32G32B32_SFLOAT:
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return GL_FLOAT;
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return GL_UNSIGNED_BYTE;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return GL_HALF_FLOAT;
    default:
        return GL_FLOAT;
    }
}

static bool vkFormatIsNormalized(VkFormat fmt)
{
    switch (fmt) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return true;
    default:
        return false;
    }
}

// ============================================================================
// OpenGL 调试回调
// ============================================================================

#ifndef APIENTRY
#define APIENTRY GL_APIENTRY
#endif
static void APIENTRY glDebugCallback(GLenum source, GLenum type, GLuint id,
    GLenum severity, GLsizei length, const GLchar* message, const void* userParam)
{
    (void)source; (void)id; (void)length; (void)userParam;
    // 过滤掉 NOTIFICATION 级别和性能警告
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    if (type == GL_DEBUG_TYPE_PERFORMANCE) return;
    const char* sevStr = "INFO";
    if (severity == GL_DEBUG_SEVERITY_HIGH)   sevStr = "ERROR";
    else if (severity == GL_DEBUG_SEVERITY_MEDIUM) sevStr = "WARNING";
    else if (severity == GL_DEBUG_SEVERITY_LOW)    sevStr = "LOW";
    SDL_Log("[OpenGL %s] %s", sevStr, message);
}

// ============================================================================
// Instance
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateInstance(
    const VkInstanceCreateInfo*  pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance*                  pInstance)
{
    (void)pCreateInfo; (void)pAllocator;
    auto* inst = new GL_Instance();
    inst->initialized = true;
    *pInstance = TO_VK(VkInstance, inst);
    std::cout << "[VkOpenGL] Instance created" << std::endl;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroyInstance(
    VkInstance                   instance,
    const VkAllocationCallbacks* pAllocator)
{
    (void)pAllocator;
    if (!instance) return;
    auto* inst = AS_GL(Instance, instance);
    for (auto pd : inst->physicalDevices)
        delete AS_GL(PhysicalDevice, pd);
    inst->physicalDevices.clear();
    delete inst;
    std::cout << "[VkOpenGL] Instance destroyed" << std::endl;
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkEnumerateInstanceExtensionProperties(
    const char*              pLayerName,
    uint32_t*                pPropertyCount,
    VkExtensionProperties*   pProperties)
{
    (void)pLayerName;
    const VkExtensionProperties exts[] = {
        {"VK_KHR_surface", 1},
        {"VK_KHR_win32_surface", 1},
    };
    uint32_t count = 2;
    if (!pProperties) { *pPropertyCount = count; return VK_SUCCESS; }
    uint32_t toWrite = std::min(*pPropertyCount, count);
    memcpy(pProperties, exts, toWrite * sizeof(VkExtensionProperties));
    *pPropertyCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkEnumerateInstanceLayerProperties(
    uint32_t*          pPropertyCount,
    VkLayerProperties* pProperties)
{
    (void)pProperties;
    *pPropertyCount = 0;
    return VK_SUCCESS;
}

// 前向声明 debug 函数
static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateDebugUtilsMessengerEXT(
    VkInstance, const VkDebugUtilsMessengerCreateInfoEXT*, const VkAllocationCallbacks*,
    VkDebugUtilsMessengerEXT*);
static VKAPI_ATTR void VKAPI_CALL gl_vkDestroyDebugUtilsMessengerEXT(
    VkInstance, VkDebugUtilsMessengerEXT, const VkAllocationCallbacks*);

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL gl_vkGetInstanceProcAddr(
    VkInstance  instance,
    const char* pName)
{
    (void)instance;
    if (!pName) return nullptr;

    // 返回全局注册的函数指针 — ImGui_ImplVulkan 通过此函数加载所有 VK 函数
    // vkLoadOpenGLDispatch() 已设置全局函数指针，直接从那里返回
    #define RET_IF(fn) if (strcmp(pName, #fn) == 0) return (PFN_vkVoidFunction)::fn

    // Instance
    RET_IF(vkCreateInstance);
    RET_IF(vkDestroyInstance);
    RET_IF(vkEnumerateInstanceExtensionProperties);
    RET_IF(vkEnumerateInstanceLayerProperties);
    RET_IF(vkGetInstanceProcAddr);

    // Physical Device
    RET_IF(vkEnumeratePhysicalDevices);
    RET_IF(vkGetPhysicalDeviceProperties);
    RET_IF(vkGetPhysicalDeviceFeatures);
    RET_IF(vkGetPhysicalDeviceFeatures2);
    RET_IF(vkGetPhysicalDeviceMemoryProperties);
    RET_IF(vkGetPhysicalDeviceQueueFamilyProperties);
    RET_IF(vkEnumerateDeviceExtensionProperties);

    // Device & Queue
    RET_IF(vkCreateDevice);
    RET_IF(vkDestroyDevice);
    RET_IF(vkGetDeviceQueue);
    RET_IF(vkDeviceWaitIdle);

    // Surface
    RET_IF(vkDestroySurfaceKHR);
    RET_IF(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    RET_IF(vkGetPhysicalDeviceSurfaceFormatsKHR);
    RET_IF(vkGetPhysicalDeviceSurfacePresentModesKHR);
    RET_IF(vkGetPhysicalDeviceSurfaceSupportKHR);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    RET_IF(vkCreateWin32SurfaceKHR);
#endif

    // Swapchain
    RET_IF(vkCreateSwapchainKHR);
    RET_IF(vkDestroySwapchainKHR);
    RET_IF(vkGetSwapchainImagesKHR);
    RET_IF(vkAcquireNextImageKHR);
    RET_IF(vkQueuePresentKHR);

    // Buffer
    RET_IF(vkCreateBuffer);
    RET_IF(vkDestroyBuffer);
    RET_IF(vkGetBufferMemoryRequirements);

    // Memory
    RET_IF(vkAllocateMemory);
    RET_IF(vkFreeMemory);
    RET_IF(vkBindBufferMemory);
    RET_IF(vkMapMemory);
    RET_IF(vkUnmapMemory);
    RET_IF(vkFlushMappedMemoryRanges);

    // Image
    RET_IF(vkCreateImage);
    RET_IF(vkDestroyImage);
    RET_IF(vkGetImageMemoryRequirements);
    RET_IF(vkBindImageMemory);
    RET_IF(vkCreateImageView);
    RET_IF(vkDestroyImageView);

    // Sampler
    RET_IF(vkCreateSampler);
    RET_IF(vkDestroySampler);

    // Command Pool & Buffer
    RET_IF(vkCreateCommandPool);
    RET_IF(vkDestroyCommandPool);
    RET_IF(vkResetCommandPool);
    RET_IF(vkAllocateCommandBuffers);
    RET_IF(vkFreeCommandBuffers);
    RET_IF(vkBeginCommandBuffer);
    RET_IF(vkEndCommandBuffer);
    RET_IF(vkResetCommandBuffer);

    // Command Recording
    RET_IF(vkCmdBeginRenderPass);
    RET_IF(vkCmdEndRenderPass);
    RET_IF(vkCmdBindPipeline);
    RET_IF(vkCmdBindDescriptorSets);
    RET_IF(vkCmdBindVertexBuffers);
    RET_IF(vkCmdBindIndexBuffer);
    RET_IF(vkCmdDraw);
    RET_IF(vkCmdDrawIndexed);
    RET_IF(vkCmdSetViewport);
    RET_IF(vkCmdSetScissor);
    RET_IF(vkCmdPushConstants);
    RET_IF(vkCmdCopyBuffer);
    RET_IF(vkCmdCopyBufferToImage);
    RET_IF(vkCmdCopyImageToBuffer);
    RET_IF(vkCmdPipelineBarrier);
    RET_IF(vkCmdBlitImage);

    // Synchronization
    RET_IF(vkWaitForFences);
    RET_IF(vkResetFences);
    RET_IF(vkQueueSubmit);
    RET_IF(vkQueueWaitIdle);

    // Pipeline
    RET_IF(vkCreatePipelineLayout);
    RET_IF(vkDestroyPipelineLayout);
    RET_IF(vkCreateShaderModule);
    RET_IF(vkDestroyShaderModule);
    RET_IF(vkCreateGraphicsPipelines);
    RET_IF(vkDestroyPipeline);

    // Render Pass & Framebuffer
    RET_IF(vkCreateRenderPass);
    RET_IF(vkDestroyRenderPass);
    RET_IF(vkCreateFramebuffer);
    RET_IF(vkDestroyFramebuffer);

    // Descriptor
    RET_IF(vkCreateDescriptorPool);
    RET_IF(vkDestroyDescriptorPool);
    RET_IF(vkCreateDescriptorSetLayout);
    RET_IF(vkDestroyDescriptorSetLayout);
    RET_IF(vkAllocateDescriptorSets);
    RET_IF(vkFreeDescriptorSets);
    RET_IF(vkUpdateDescriptorSets);

    // Sync Objects
    RET_IF(vkCreateSemaphore);
    RET_IF(vkDestroySemaphore);
    RET_IF(vkCreateFence);
    RET_IF(vkDestroyFence);

    // Debug
    RET_IF(vkCreateDebugUtilsMessengerEXT);
    RET_IF(vkDestroyDebugUtilsMessengerEXT);

    #undef RET_IF

    return nullptr;
}

// ============================================================================
// Physical Device
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkEnumeratePhysicalDevices(
    VkInstance       instance,
    uint32_t*        pPhysicalDeviceCount,
    VkPhysicalDevice* pPhysicalDevices)
{
    if (!instance) return VK_ERROR_INITIALIZATION_FAILED;
    auto* inst = AS_GL(Instance, instance);

    // 首次调用时创建 1 个物理设备
    if (inst->physicalDevices.empty()) {
        auto* pd = new GL_PhysicalDevice();
        pd->instance = instance;
        // GL_RENDERER/GL_VERSION 在 GL context 创建后才可用
        inst->physicalDevices.push_back(TO_VK(VkPhysicalDevice, pd));
    }

    uint32_t count = static_cast<uint32_t>(inst->physicalDevices.size());
    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = count;
        return VK_SUCCESS;
    }
    uint32_t toWrite = std::min(*pPhysicalDeviceCount, count);
    for (uint32_t i = 0; i < toWrite; i++)
        pPhysicalDevices[i] = inst->physicalDevices[i];
    *pPhysicalDeviceCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkGetPhysicalDeviceProperties(
    VkPhysicalDevice          physicalDevice,
    VkPhysicalDeviceProperties* pProperties)
{
    if (!physicalDevice || !pProperties) return;
    auto* pd = AS_GL(PhysicalDevice, physicalDevice);
    memset(pProperties, 0, sizeof(*pProperties));

    pProperties->apiVersion = VK_API_VERSION_1_0;
    pProperties->driverVersion = 1;
    pProperties->vendorID = 0;
    pProperties->deviceID = 0;
    pProperties->deviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;

    // 如果 GL context 已创建，填入真实信息
    if (!pd->rendererStr.empty()) {
        strncpy(pProperties->deviceName, pd->rendererStr.c_str(),
                VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
    } else {
        strncpy(pProperties->deviceName, "OpenGL 4.5 Device",
                VK_MAX_PHYSICAL_DEVICE_NAME_SIZE - 1);
    }

    pProperties->limits.maxImageDimension2D = 16384;
    pProperties->limits.maxPushConstantsSize = 256;
    pProperties->limits.maxBoundDescriptorSets = 4;
    pProperties->limits.maxPerStageDescriptorSampledImages = 128;
    pProperties->limits.maxPerStageDescriptorSamplers = 16;
    pProperties->limits.minUniformBufferOffsetAlignment = 256;
    pProperties->limits.nonCoherentAtomSize = 64;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice      physicalDevice,
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

static VKAPI_ATTR void VKAPI_CALL gl_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice       physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures)
{
    if (!pFeatures) return;
    gl_vkGetPhysicalDeviceFeatures(physicalDevice, &pFeatures->features);

    // 遍历 pNext 链
    VkBaseOutStructure* next = reinterpret_cast<VkBaseOutStructure*>(pFeatures->pNext);
    while (next) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
            auto* indexing = reinterpret_cast<VkPhysicalDeviceDescriptorIndexingFeatures*>(next);
            // OpenGL 不支持 bindless descriptor indexing
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

static VKAPI_ATTR void VKAPI_CALL gl_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice                  physicalDevice,
    VkPhysicalDeviceMemoryProperties* pMemoryProperties)
{
    (void)physicalDevice;
    if (!pMemoryProperties) return;
    memset(pMemoryProperties, 0, sizeof(*pMemoryProperties));

    // 1 个 memory heap
    pMemoryProperties->memoryHeapCount = 1;
    pMemoryProperties->memoryHeaps[0].size = 4ULL * 1024 * 1024 * 1024; // 4GB
    pMemoryProperties->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;

    // 3 种 memory type (与 D3D11/D3D12 后端一致)
    pMemoryProperties->memoryTypeCount = 3;

    // Type 0: DEVICE_LOCAL
    pMemoryProperties->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    pMemoryProperties->memoryTypes[0].heapIndex = 0;

    // Type 1: HOST_VISIBLE | HOST_COHERENT (upload)
    pMemoryProperties->memoryTypes[1].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    pMemoryProperties->memoryTypes[1].heapIndex = 0;

    // Type 2: HOST_VISIBLE | HOST_COHERENT | HOST_CACHED (readback)
    pMemoryProperties->memoryTypes[2].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    pMemoryProperties->memoryTypes[2].heapIndex = 0;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice          physicalDevice,
    uint32_t*                 pQueueFamilyPropertyCount,
    VkQueueFamilyProperties*  pQueueFamilyProperties)
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

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice     physicalDevice,
    const char*          pLayerName,
    uint32_t*            pPropertyCount,
    VkExtensionProperties* pProperties)
{
    (void)physicalDevice; (void)pLayerName;
    const VkExtensionProperties exts[] = {
        {"VK_KHR_swapchain", 1},
    };
    uint32_t count = 1;
    if (!pProperties) { *pPropertyCount = count; return VK_SUCCESS; }
    uint32_t toWrite = std::min(*pPropertyCount, count);
    memcpy(pProperties, exts, toWrite * sizeof(VkExtensionProperties));
    *pPropertyCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

// ============================================================================
// Device & Queue
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateDevice(
    VkPhysicalDevice             physicalDevice,
    const VkDeviceCreateInfo*    pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice*                    pDevice)
{
    (void)pCreateInfo; (void)pAllocator;
    if (!physicalDevice) return VK_ERROR_INITIALIZATION_FAILED;
    auto* pd = AS_GL(PhysicalDevice, physicalDevice);

    auto* dev = new GL_Device();
    dev->physicalDevice = physicalDevice;

    // 获取 SDL_Window — 在 vkCreateWin32SurfaceKHR 中已保存到全局
    SDL_Window* window = s_sdlWindow;
    if (!window) {
        // 尝试获取第一个 SDL 窗口
        window = SDL_GetWindowFromID(1);
    }
    if (!window) {
        std::cerr << "[VkOpenGL] ERROR: No SDL_Window available for GL context!" << std::endl;
        delete dev;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    dev->window = window;
    s_sdlWindow = window; // 保存全局引用用于 SDL_GL_SwapWindow

    // 创建 OpenGL context
    SDL_Log("[VkOpenGL] Creating GL context for window %p", (void*)window);
    dev->glContext = SDL_GL_CreateContext(window);
    if (!dev->glContext) {
        SDL_Log("[VkOpenGL] SDL_GL_CreateContext FAILED: %s", SDL_GetError());
        delete dev;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    SDL_Log("[VkOpenGL] GL context created OK");

    SDL_GL_MakeCurrent(window, dev->glContext);
    SDL_GL_SetSwapInterval(1); // VSync

#ifdef __ANDROID__
    // Android GLES: 函数由系统 libGLESv3.so 直接提供，无需 GLAD 加载
    s_isGLES = true;
    SDL_Log("[VkOpenGL] Android GLES: using system GLES3 functions");
#else
    // 加载 OpenGL 函数指针 (Desktop: GLAD)
    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        std::cerr << "[VkOpenGL] gladLoadGLLoader failed!" << std::endl;
        SDL_GL_DeleteContext(dev->glContext);
        delete dev;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // 设置 GLES 运行时标记
    s_isGLES = QymEngine::vkIsGLESBackend();

    // GLES: glad 按 Desktop GL 版本号加载函数，GLES context 的版本号不匹配
    // 需要手动加载 GLES 3.0 核心函数中 glad 未加载的那些
    if (s_isGLES) {
        auto load = (GLADloadproc)SDL_GL_GetProcAddress;
        if (!glTexStorage2D) glTexStorage2D = (PFNGLTEXSTORAGE2DPROC)load("glTexStorage2D");
        if (!glTexStorage3D) glTexStorage3D = (PFNGLTEXSTORAGE3DPROC)load("glTexStorage3D");
        if (!glDrawArraysInstanced) glDrawArraysInstanced = (PFNGLDRAWARRAYSINSTANCEDPROC)load("glDrawArraysInstanced");
        if (!glDrawElementsInstanced) glDrawElementsInstanced = (PFNGLDRAWELEMENTSINSTANCEDPROC)load("glDrawElementsInstanced");
        if (!glGenSamplers) glGenSamplers = (PFNGLGENSAMPLERSPROC)load("glGenSamplers");
        if (!glDeleteSamplers) glDeleteSamplers = (PFNGLDELETESAMPLERSPROC)load("glDeleteSamplers");
        if (!glBindSampler) glBindSampler = (PFNGLBINDSAMPLERPROC)load("glBindSampler");
        if (!glSamplerParameteri) glSamplerParameteri = (PFNGLSAMPLERPARAMETERIPROC)load("glSamplerParameteri");
        if (!glSamplerParameterf) glSamplerParameterf = (PFNGLSAMPLERPARAMETERFPROC)load("glSamplerParameterf");
        if (!glGetActiveUniformBlockName) glGetActiveUniformBlockName = (PFNGLGETACTIVEUNIFORMBLOCKNAMEPROC)load("glGetActiveUniformBlockName");
        if (!glUniformBlockBinding) glUniformBlockBinding = (PFNGLUNIFORMBLOCKBINDINGPROC)load("glUniformBlockBinding");
        if (!glGetProgramiv) glGetProgramiv = (PFNGLGETPROGRAMIVPROC)load("glGetProgramiv");
        if (!glCopyBufferSubData) glCopyBufferSubData = (PFNGLCOPYBUFFERSUBDATAPROC)load("glCopyBufferSubData");
        if (!glBlitFramebuffer) glBlitFramebuffer = (PFNGLBLITFRAMEBUFFERPROC)load("glBlitFramebuffer");
        if (!glGenVertexArrays) glGenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)load("glGenVertexArrays");
        if (!glBindVertexArray) glBindVertexArray = (PFNGLBINDVERTEXARRAYPROC)load("glBindVertexArray");
        if (!glDeleteVertexArrays) glDeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSPROC)load("glDeleteVertexArrays");
        if (!glVertexAttribPointer) glVertexAttribPointer = (PFNGLVERTEXATTRIBPOINTERPROC)load("glVertexAttribPointer");
        if (!glEnableVertexAttribArray) glEnableVertexAttribArray = (PFNGLENABLEVERTEXATTRIBARRAYPROC)load("glEnableVertexAttribArray");
        if (!glDrawBuffers) glDrawBuffers = (PFNGLDRAWBUFFERSPROC)load("glDrawBuffers");
        if (!glBindBufferBase) glBindBufferBase = (PFNGLBINDBUFFERBASEPROC)load("glBindBufferBase");
        if (!glDepthRangef) glDepthRangef = (PFNGLDEPTHRANGEFPROC)load("glDepthRangef");
        if (!glClearDepthf) glClearDepthf = (PFNGLCLEARDEPTHFPROC)load("glClearDepthf");
        if (!glReadPixels) glReadPixels = (PFNGLREADPIXELSPROC)load("glReadPixels");
        if (!s_glClipControlEXT &&
            (SDL_GL_ExtensionSupported("GL_EXT_clip_control") ||
             SDL_GL_ExtensionSupported("GL_ARB_clip_control"))) {
            s_glClipControlEXT = (PFNGLCLIPCONTROLEXTPROC)SDL_GL_GetProcAddress("glClipControlEXT");
            if (!s_glClipControlEXT)
                s_glClipControlEXT = (PFNGLCLIPCONTROLEXTPROC)SDL_GL_GetProcAddress("glClipControl");
        }
        std::cout << "[VkOpenGL] GLES function post-load completed" << std::endl;
    }
#endif

    // 填充 PhysicalDevice 信息
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    const char* version  = (const char*)glGetString(GL_VERSION);
    if (renderer) pd->rendererStr = renderer;
    if (version)  pd->versionStr = version;

    SDL_Log("[VkOpenGL] GL_RENDERER: %s", renderer ? renderer : "?");
    SDL_Log("[VkOpenGL] GL_VERSION:  %s", version ? version : "?");
    if (s_isGLES) SDL_Log("[VkOpenGL] Running in GLES 3.0 mode");
    if (s_isGLES) {
        s_defaultFramebufferIsSRGB = detectDefaultFramebufferSRGB();
        SDL_Log("[VkOpenGL] Default framebuffer sRGB = %s",
                s_defaultFramebufferIsSRGB ? "YES" : "NO");
        if (s_glClipControlEXT) {
            s_glClipControlEXT(GL_LOWER_LEFT_EXT, GL_ZERO_TO_ONE_EXT);
            QymEngine::vkSetClipControlSupport(true);
            SDL_Log("[VkOpenGL] GLES clip control enabled via extension");
        } else {
            QymEngine::vkSetClipControlSupport(false);
            SDL_Log("[VkOpenGL] GLES clip control unavailable; using manual depth fix");
        }
    } else {
        s_defaultFramebufferIsSRGB = false;
        QymEngine::vkSetClipControlSupport(false);
    }

    // 启用 debug 输出
#ifdef _DEBUG
#ifndef __ANDROID__
    // Android GLES: 跳过 GL debug output (某些 Adreno 驱动不稳定)
    if (GLAD_GL_KHR_debug || glDebugMessageCallback) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(glDebugCallback, nullptr);
        glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION,
                              0, nullptr, GL_FALSE);
        SDL_Log("[VkOpenGL] Debug output enabled");
    }
#endif
#endif

    // 启用 sRGB 帧缓冲
    // GLES: 默认帧缓冲 (FBO 0) 不是 sRGB，手动在 ImGui 着色器中做 sRGB 编码
    // Desktop GL: glEnable(GL_FRAMEBUFFER_SRGB) 自动处理
    if (!s_isGLES) {
        glEnable(GL_FRAMEBUFFER_SRGB);
    }

    // 关键: 引擎使用 GLM_FORCE_DEPTH_ZERO_TO_ONE (Vulkan/D3D 的 [0,1] depth range)
    // OpenGL 默认 [-1,1]，必须切换到 [0,1] 以匹配投影矩阵
    // GLES 3.0 没有 glClipControl，Y-flip 在 Renderer 中通过投影矩阵处理
    if (!s_isGLES) {
#ifndef __ANDROID__
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        QymEngine::vkSetClipControlSupport(true);
#endif
    }

    *pDevice = TO_VK(VkDevice, dev);
    SDL_Log("[VkOpenGL] Device created");
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroyDevice(
    VkDevice                     device,
    const VkAllocationCallbacks* pAllocator)
{
    (void)pAllocator;
    if (!device) return;
    auto* dev = AS_GL(Device, device);
    if (dev->glContext) {
        SDL_GL_DeleteContext(dev->glContext);
    }
    delete dev;
    std::cout << "[VkOpenGL] Device destroyed" << std::endl;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkGetDeviceQueue(
    VkDevice  device,
    uint32_t  queueFamilyIndex,
    uint32_t  queueIndex,
    VkQueue*  pQueue)
{
    (void)queueFamilyIndex; (void)queueIndex;
    if (!device || !pQueue) return;
    auto* q = new GL_Queue();
    q->device = device;
    *pQueue = TO_VK(VkQueue, q);
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkDeviceWaitIdle(VkDevice device)
{
    (void)device;
    glFinish();
    return VK_SUCCESS;
}

// ============================================================================
// Surface
// ============================================================================

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroySurfaceKHR(
    VkInstance                   instance,
    VkSurfaceKHR                 surface,
    const VkAllocationCallbacks* pAllocator)
{
    (void)instance; (void)pAllocator;
    delete AS_GL(Surface, surface);
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice          physicalDevice,
    VkSurfaceKHR              surface,
    VkSurfaceCapabilitiesKHR* pSurfaceCapabilities)
{
    (void)physicalDevice;
    if (!pSurfaceCapabilities) return VK_ERROR_SURFACE_LOST_KHR;
    auto* surf = surface ? AS_GL(Surface, surface) : nullptr;

    uint32_t w = 1280, h = 720;
    SDL_Window* win = (surf && surf->window) ? surf->window : s_sdlWindow;
    if (win) {
        int iw, ih;
        SDL_GL_GetDrawableSize(win, &iw, &ih);
        if (iw <= 0 || ih <= 0) SDL_GetWindowSize(win, &iw, &ih); // fallback
        w = static_cast<uint32_t>(iw > 0 ? iw : 1280);
        h = static_cast<uint32_t>(ih > 0 ? ih : 720);
    }

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

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice    physicalDevice,
    VkSurfaceKHR        surface,
    uint32_t*           pSurfaceFormatCount,
    VkSurfaceFormatKHR* pSurfaceFormats)
{
    (void)physicalDevice; (void)surface;
    // GLES: 使用 UNORM 格式，避免 Adreno 等移动 GPU 的 sRGB 帧缓冲 bug
    // (写入 SRGB8_ALPHA8 时的自动 linear→sRGB 转换不一致)
    // sRGB 编码由 ImGui 着色器手动处理 (pow(1/2.2))
    // Desktop GL: 使用 SRGB 格式，GL_FRAMEBUFFER_SRGB 自动处理转换
    const VkSurfaceFormatKHR formats_gles[] = {
        {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };
    const VkSurfaceFormatKHR formats_gl[] = {
        {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };
    const VkSurfaceFormatKHR* formats = s_isGLES ? formats_gles : formats_gl;
    uint32_t count = s_isGLES ? 2 : 3;
    if (!pSurfaceFormats) { *pSurfaceFormatCount = count; return VK_SUCCESS; }
    uint32_t toWrite = std::min(*pSurfaceFormatCount, count);
    memcpy(pSurfaceFormats, formats, toWrite * sizeof(VkSurfaceFormatKHR));
    *pSurfaceFormatCount = toWrite;
    return (toWrite < count) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice  physicalDevice,
    VkSurfaceKHR      surface,
    uint32_t*         pPresentModeCount,
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

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physicalDevice,
    uint32_t         queueFamilyIndex,
    VkSurfaceKHR     surface,
    VkBool32*        pSupported)
{
    (void)physicalDevice; (void)queueFamilyIndex; (void)surface;
    *pSupported = VK_TRUE;
    return VK_SUCCESS;
}

#ifdef VK_USE_PLATFORM_WIN32_KHR
static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateWin32SurfaceKHR(
    VkInstance                         instance,
    const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks*       pAllocator,
    VkSurfaceKHR*                      pSurface)
{
    (void)instance; (void)pAllocator;
    auto* surf = new GL_Surface();

    // 通过 HWND 找到对应的 SDL_Window
    // 使用 SDL_GetWindowFromID 遍历可能的 ID (通常第一个窗口是 ID 1)
    if (!s_sdlWindow) {
        for (uint32_t id = 1; id <= 10; id++) {
            SDL_Window* win = SDL_GetWindowFromID(id);
            if (win) {
                SDL_SysWMinfo wm;
                SDL_VERSION(&wm.version);
                if (SDL_GetWindowWMInfo(win, &wm)) {
                    if (wm.info.win.window == pCreateInfo->hwnd) {
                        s_sdlWindow = win;
                        break;
                    }
                }
            }
        }
    }

    surf->window = s_sdlWindow;
    *pSurface = TO_VK(VkSurfaceKHR, surf);
    return VK_SUCCESS;
}
#endif

// ============================================================================
// Swapchain
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateSwapchainKHR(
    VkDevice                        device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks*    pAllocator,
    VkSwapchainKHR*                 pSwapchain)
{
    (void)pAllocator;
    auto* sc = new GL_Swapchain();
    if (pCreateInfo) {
        sc->width = pCreateInfo->imageExtent.width;
        sc->height = pCreateInfo->imageExtent.height;
        sc->imageCount = std::max(pCreateInfo->minImageCount, 3u);
        sc->format = pCreateInfo->imageFormat;
    }

    // 为每个 swapchain image 创建一个 GL_Image 占位 (代表 default framebuffer)
    sc->imageHandles.resize(sc->imageCount);
    for (uint32_t i = 0; i < sc->imageCount; i++) {
        auto* img = new GL_Image();
        img->format = sc->format;
        img->width = sc->width;
        img->height = sc->height;
        img->ownsResource = false; // 标记为 swapchain image (不拥有资源)
        img->texture = 0; // FBO 0 的 default framebuffer
        sc->imageHandles[i] = TO_VK(VkImage, img);
    }

    *pSwapchain = TO_VK(VkSwapchainKHR, sc);
    SDL_Log("[VkOpenGL] Swapchain created %ux%u images=%u", sc->width, sc->height, sc->imageCount);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroySwapchainKHR(
    VkDevice                     device,
    VkSwapchainKHR               swapchain,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!swapchain) return;
    auto* sc = AS_GL(Swapchain, swapchain);
    for (auto& img : sc->imageHandles)
        delete AS_GL(Image, img);
    delete sc;
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkGetSwapchainImagesKHR(
    VkDevice       device,
    VkSwapchainKHR swapchain,
    uint32_t*      pSwapchainImageCount,
    VkImage*       pSwapchainImages)
{
    (void)device;
    if (!swapchain) return VK_ERROR_DEVICE_LOST;
    auto* sc = AS_GL(Swapchain, swapchain);

    if (!pSwapchainImages) {
        *pSwapchainImageCount = sc->imageCount;
        return VK_SUCCESS;
    }

    uint32_t toWrite = std::min(*pSwapchainImageCount, sc->imageCount);
    for (uint32_t i = 0; i < toWrite; i++)
        pSwapchainImages[i] = sc->imageHandles[i];
    *pSwapchainImageCount = toWrite;
    return (toWrite < sc->imageCount) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkAcquireNextImageKHR(
    VkDevice       device,
    VkSwapchainKHR swapchain,
    uint64_t       timeout,
    VkSemaphore    semaphore,
    VkFence        fence,
    uint32_t*      pImageIndex)
{
    (void)device; (void)timeout; (void)semaphore; (void)fence;
    if (!swapchain) return VK_ERROR_DEVICE_LOST;
    auto* sc = AS_GL(Swapchain, swapchain);

    *pImageIndex = sc->currentIndex;
    sc->currentIndex = (sc->currentIndex + 1) % sc->imageCount;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkQueuePresentKHR(
    VkQueue                 queue,
    const VkPresentInfoKHR* pPresentInfo)
{
    (void)queue; (void)pPresentInfo;
    if (s_sdlWindow) {
        SDL_GL_SwapWindow(s_sdlWindow);
    } else {
        if (s_frameCount < 3) SDL_Log("[VkOpenGL] WARNING: s_sdlWindow is null in Present!");
    }
    if (s_frameCount < 3) SDL_Log("[VkOpenGL] Present frame %u", s_frameCount);
    s_frameCount++;
    return VK_SUCCESS;
}

// ============================================================================
// Buffer
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateBuffer(
    VkDevice                     device,
    const VkBufferCreateInfo*    pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkBuffer*                    pBuffer)
{
    (void)device; (void)pAllocator;
    auto* buf = new GL_Buffer();
    if (pCreateInfo) {
        buf->size = pCreateInfo->size;
        buf->usage = pCreateInfo->usage;
    }
    *pBuffer = TO_VK(VkBuffer, buf);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroyBuffer(
    VkDevice                     device,
    VkBuffer                     buffer,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!buffer) return;
    auto* buf = AS_GL(Buffer, buffer);
    if (buf->buffer) {
        glDeleteBuffers(1, &buf->buffer);
    }
    delete buf;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkGetBufferMemoryRequirements(
    VkDevice                device,
    VkBuffer                buffer,
    VkMemoryRequirements*   pMemoryRequirements)
{
    (void)device;
    if (!buffer || !pMemoryRequirements) return;
    auto* buf = AS_GL(Buffer, buffer);
    pMemoryRequirements->size = (buf->size + 255) & ~255ULL;
    pMemoryRequirements->alignment = 256;
    pMemoryRequirements->memoryTypeBits = 0x7; // type 0, 1, 2
}

// ============================================================================
// Memory
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkAllocateMemory(
    VkDevice                    device,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDeviceMemory*             pMemory)
{
    (void)device; (void)pAllocator;
    if (!pAllocateInfo) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto* mem = new GL_Memory();
    mem->size = pAllocateInfo->allocationSize;
    mem->memoryTypeIndex = pAllocateInfo->memoryTypeIndex;
    *pMemory = TO_VK(VkDeviceMemory, mem);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkFreeMemory(
    VkDevice                     device,
    VkDeviceMemory               memory,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!memory) return;
    auto* mem = AS_GL(Memory, memory);
    if (mem->mapped) {
        free(mem->mapped);
        mem->mapped = nullptr;
    }
    delete mem;
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkBindBufferMemory(
    VkDevice       device,
    VkBuffer       buffer,
    VkDeviceMemory memory,
    VkDeviceSize   memoryOffset)
{
    (void)device; (void)memoryOffset;
    if (!buffer || !memory) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto* buf = AS_GL(Buffer, buffer);
    auto* mem = AS_GL(Memory, memory);
    buf->boundMemory = memory;

    // 创建 GL buffer
    compat_CreateBuffers(1, &buf->buffer);

    // 根据 memory type 设置 flags
    GLbitfield flags = 0;
    if (mem->memoryTypeIndex == 1) {
        // HOST_VISIBLE — 需要 DYNAMIC_STORAGE_BIT 以允许 glNamedBufferSubData
        flags = GL_DYNAMIC_STORAGE_BIT | GL_MAP_WRITE_BIT;
    } else if (mem->memoryTypeIndex == 2) {
        // HOST_CACHED (readback)
        flags = GL_MAP_READ_BIT | GL_CLIENT_STORAGE_BIT;
    } else {
        // DEVICE_LOCAL — immutable, but we still need DYNAMIC_STORAGE_BIT for UpdateSubData
        flags = GL_DYNAMIC_STORAGE_BIT;
    }

    VkDeviceSize bufSize = std::max(buf->size, (VkDeviceSize)16);
    compat_BufferStorage(buf->buffer, (GLsizeiptr)bufSize, nullptr, flags);

    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkMapMemory(
    VkDevice         device,
    VkDeviceMemory   memory,
    VkDeviceSize     offset,
    VkDeviceSize     size,
    VkMemoryMapFlags flags,
    void**           ppData)
{
    (void)device; (void)size; (void)flags;
    if (!memory) return VK_ERROR_MEMORY_MAP_FAILED;
    auto* mem = AS_GL(Memory, memory);

    // Shadow buffer 策略
    if (!mem->mapped) {
        mem->mapped = calloc(1, (size_t)mem->size);
        if (!mem->mapped) return VK_ERROR_MEMORY_MAP_FAILED;
    }

    *ppData = static_cast<uint8_t*>(mem->mapped) + offset;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkUnmapMemory(
    VkDevice       device,
    VkDeviceMemory memory)
{
    (void)device; (void)memory;
    // 保持 shadow buffer 有效 (persistent mapping)
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkFlushMappedMemoryRanges(
    VkDevice                   device,
    uint32_t                   memoryRangeCount,
    const VkMappedMemoryRange* pMemoryRanges)
{
    (void)device; (void)memoryRangeCount; (void)pMemoryRanges;
    // Shadow buffer 不需要显式 flush
    return VK_SUCCESS;
}

// ============================================================================
// Image
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateImage(
    VkDevice                     device,
    const VkImageCreateInfo*     pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImage*                     pImage)
{
    (void)device; (void)pAllocator;
    auto* img = new GL_Image();
    if (pCreateInfo) {
        img->format = pCreateInfo->format;
        img->width = pCreateInfo->extent.width;
        img->height = pCreateInfo->extent.height;
        img->mipLevels = pCreateInfo->mipLevels;
        img->usage = pCreateInfo->usage;
    }
    *pImage = TO_VK(VkImage, img);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroyImage(
    VkDevice                     device,
    VkImage                      image,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!image) return;
    auto* img = AS_GL(Image, image);
    if (img->ownsResource && img->texture) {
        glDeleteTextures(1, &img->texture);
    }
    delete img;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkGetImageMemoryRequirements(
    VkDevice               device,
    VkImage                image,
    VkMemoryRequirements*  pMemoryRequirements)
{
    (void)device;
    if (!image || !pMemoryRequirements) return;
    auto* img = AS_GL(Image, image);
    auto fi = vkFormatToGL(img->format);
    pMemoryRequirements->size = (VkDeviceSize)img->width * img->height * fi.texelSize * img->mipLevels;
    pMemoryRequirements->alignment = 256;
    pMemoryRequirements->memoryTypeBits = 0x7;
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkBindImageMemory(
    VkDevice       device,
    VkImage        image,
    VkDeviceMemory memory,
    VkDeviceSize   memoryOffset)
{
    (void)device; (void)memoryOffset;
    if (!image || !memory) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    auto* img = AS_GL(Image, image);
    img->boundMemory = memory;

    // 创建纹理
    compat_CreateTextures(GL_TEXTURE_2D, 1, &img->texture);
    auto fi = vkFormatToGL(img->format);
    compat_TextureStorage2D(img->texture, img->mipLevels, fi.internalFormat,
                            img->width, img->height);

    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateImageView(
    VkDevice                      device,
    const VkImageViewCreateInfo*  pCreateInfo,
    const VkAllocationCallbacks*  pAllocator,
    VkImageView*                  pView)
{
    (void)device; (void)pAllocator;
    auto* view = new GL_ImageView();
    if (pCreateInfo) {
        view->image = pCreateInfo->image;
        view->format = pCreateInfo->format;
        view->viewType = pCreateInfo->viewType;
        view->subresourceRange = pCreateInfo->subresourceRange;
    }
    *pView = TO_VK(VkImageView, view);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroyImageView(
    VkDevice                     device,
    VkImageView                  imageView,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_GL(ImageView, imageView);
}

// ============================================================================
// Sampler
// ============================================================================

static GLenum vkFilterToGL(VkFilter filter)
{
    return (filter == VK_FILTER_NEAREST) ? GL_NEAREST : GL_LINEAR;
}

static GLenum vkMipmapModeToGL(VkSamplerMipmapMode mode, VkFilter minFilter)
{
    if (minFilter == VK_FILTER_NEAREST) {
        return (mode == VK_SAMPLER_MIPMAP_MODE_NEAREST) ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_LINEAR;
    } else {
        return (mode == VK_SAMPLER_MIPMAP_MODE_NEAREST) ? GL_LINEAR_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR;
    }
}

static GLenum vkAddressModeToGL(VkSamplerAddressMode mode)
{
    switch (mode) {
    case VK_SAMPLER_ADDRESS_MODE_REPEAT:            return GL_REPEAT;
    case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:   return GL_MIRRORED_REPEAT;
    case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE:     return GL_CLAMP_TO_EDGE;
    case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER:   return GL_CLAMP_TO_BORDER;
    default:                                        return GL_REPEAT;
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateSampler(
    VkDevice                     device,
    const VkSamplerCreateInfo*   pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSampler*                   pSampler)
{
    (void)device; (void)pAllocator;
    auto* samp = new GL_Sampler();
    compat_CreateSamplers(1, &samp->sampler);

    if (pCreateInfo) {
        glSamplerParameteri(samp->sampler, GL_TEXTURE_MAG_FILTER,
                            vkFilterToGL(pCreateInfo->magFilter));
        glSamplerParameteri(samp->sampler, GL_TEXTURE_MIN_FILTER,
                            vkMipmapModeToGL(pCreateInfo->mipmapMode, pCreateInfo->minFilter));
        glSamplerParameteri(samp->sampler, GL_TEXTURE_WRAP_S,
                            vkAddressModeToGL(pCreateInfo->addressModeU));
        glSamplerParameteri(samp->sampler, GL_TEXTURE_WRAP_T,
                            vkAddressModeToGL(pCreateInfo->addressModeV));
        glSamplerParameteri(samp->sampler, GL_TEXTURE_WRAP_R,
                            vkAddressModeToGL(pCreateInfo->addressModeW));

        if (pCreateInfo->anisotropyEnable && pCreateInfo->maxAnisotropy > 1.0f) {
            // GL_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FE (from GL_EXT_texture_filter_anisotropic)
            glSamplerParameterf(samp->sampler, 0x84FE, pCreateInfo->maxAnisotropy);
        }

        if (pCreateInfo->compareEnable) {
            glSamplerParameteri(samp->sampler, GL_TEXTURE_COMPARE_MODE,
                                GL_COMPARE_REF_TO_TEXTURE);
            glSamplerParameteri(samp->sampler, GL_TEXTURE_COMPARE_FUNC,
                                vkCompareOpToGL(pCreateInfo->compareOp));
        }

        glSamplerParameterf(samp->sampler, GL_TEXTURE_MIN_LOD, pCreateInfo->minLod);
        glSamplerParameterf(samp->sampler, GL_TEXTURE_MAX_LOD, pCreateInfo->maxLod);
    }

    *pSampler = TO_VK(VkSampler, samp);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroySampler(
    VkDevice                     device,
    VkSampler                    sampler,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!sampler) return;
    auto* s = AS_GL(Sampler, sampler);
    if (s->sampler) glDeleteSamplers(1, &s->sampler);
    delete s;
}

// ============================================================================
// Command Pool & Buffer
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateCommandPool(
    VkDevice                       device,
    const VkCommandPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks*   pAllocator,
    VkCommandPool*                 pCommandPool)
{
    (void)pCreateInfo; (void)pAllocator;
    auto* pool = new GL_CommandPool();
    pool->device = device;
    *pCommandPool = TO_VK(VkCommandPool, pool);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroyCommandPool(
    VkDevice                     device,
    VkCommandPool                commandPool,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_GL(CommandPool, commandPool);
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkResetCommandPool(
    VkDevice                device,
    VkCommandPool           commandPool,
    VkCommandPoolResetFlags flags)
{
    (void)device; (void)commandPool; (void)flags;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkAllocateCommandBuffers(
    VkDevice                         device,
    const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer*                 pCommandBuffers)
{
    if (pAllocateInfo && pCommandBuffers) {
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
            auto* cb = new GL_CommandBuffer();
            cb->device = device;
            // 创建 push constant UBO
            compat_CreateBuffers(1, &cb->pushConstantUBO);
            compat_BufferStorage(cb->pushConstantUBO, 256, nullptr, GL_DYNAMIC_STORAGE_BIT);
            pCommandBuffers[i] = TO_VK(VkCommandBuffer, cb);
        }
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkFreeCommandBuffers(
    VkDevice               device,
    VkCommandPool          commandPool,
    uint32_t               commandBufferCount,
    const VkCommandBuffer* pCommandBuffers)
{
    (void)device; (void)commandPool;
    if (pCommandBuffers) {
        for (uint32_t i = 0; i < commandBufferCount; ++i) {
            auto* cb = AS_GL(CommandBuffer, pCommandBuffers[i]);
            if (cb) {
                if (cb->pushConstantUBO)
                    glDeleteBuffers(1, &cb->pushConstantUBO);
                delete cb;
            }
        }
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkBeginCommandBuffer(
    VkCommandBuffer                   commandBuffer,
    const VkCommandBufferBeginInfo*   pBeginInfo)
{
    (void)pBeginInfo;
    auto* cb = AS_GL(CommandBuffer, commandBuffer);
    if (cb) {
        cb->isRecording = true;
        cb->currentPipeline = VK_NULL_HANDLE;
        cb->currentRenderPass = VK_NULL_HANDLE;
        cb->currentFramebuffer = VK_NULL_HANDLE;
        memset(cb->boundSets, 0, sizeof(cb->boundSets));
        cb->stateDirty = false;
        cb->pushConstantSize = 0;
        cb->boundIndexBuffer = 0;
        cb->pendingReadbacks.clear();
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkEndCommandBuffer(VkCommandBuffer commandBuffer)
{
    auto* cb = AS_GL(CommandBuffer, commandBuffer);
    if (cb) {
        cb->isRecording = false;
        // OpenGL immediate mode: flush
        glFlush();
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkResetCommandBuffer(
    VkCommandBuffer          commandBuffer,
    VkCommandBufferResetFlags flags)
{
    (void)flags;
    auto* cb = AS_GL(CommandBuffer, commandBuffer);
    if (cb) {
        cb->isRecording = false;
        cb->currentRenderPass = VK_NULL_HANDLE;
        cb->currentFramebuffer = VK_NULL_HANDLE;
        cb->currentPipeline = VK_NULL_HANDLE;
        memset(cb->boundSets, 0, sizeof(cb->boundSets));
        cb->stateDirty = false;
        cb->pushConstantSize = 0;
        cb->pendingReadbacks.clear();
    }
    return VK_SUCCESS;
}

// ============================================================================
// Command Recording — 即时模式
// ============================================================================

static VKAPI_ATTR void VKAPI_CALL gl_vkCmdBeginRenderPass(
    VkCommandBuffer               commandBuffer,
    const VkRenderPassBeginInfo*  pRenderPassBegin,
    VkSubpassContents             contents)
{
    (void)contents;
    auto* cb = AS_GL(CommandBuffer, commandBuffer);
    if (!cb || !pRenderPassBegin) return;

    auto* rp = AS_GL(RenderPass, pRenderPassBegin->renderPass);
    auto* fb = AS_GL(Framebuffer, pRenderPassBegin->framebuffer);
    cb->currentRenderPass = pRenderPassBegin->renderPass;
    cb->currentFramebuffer = pRenderPassBegin->framebuffer;
    if (!rp || !fb) return;

    // 绑定 FBO
    glBindFramebuffer(GL_FRAMEBUFFER, fb->fbo);

    // 设置 viewport 和 scissor
    glViewport(0, 0, fb->width, fb->height);
    glScissor(0, 0, fb->width, fb->height);
    glEnable(GL_SCISSOR_TEST);

    // 根据 loadOp 执行 clear
    GLbitfield clearMask = 0;
    uint32_t clearIdx = 0;
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    for (size_t i = 0; i < fb->attachments.size() && i < rp->attachments.size(); i++) {
        auto& att = rp->attachments[i];
        auto fi = vkFormatToGL(att.format);

        if (att.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && clearIdx < pRenderPassBegin->clearValueCount) {
            auto& cv = pRenderPassBegin->pClearValues[clearIdx];
            if (fi.isDepth) {
                glDepthMask(GL_TRUE); // 确保可以 clear depth
                if (s_isGLES)
                    glClearDepthf(cv.depthStencil.depth);
                else
                    glClearDepth(cv.depthStencil.depth);
                clearMask |= GL_DEPTH_BUFFER_BIT;
                if (att.format == VK_FORMAT_D24_UNORM_S8_UINT) {
                    glClearStencil(cv.depthStencil.stencil);
                    clearMask |= GL_STENCIL_BUFFER_BIT;
                }
            } else {
                float r = cv.color.float32[0], g = cv.color.float32[1];
                float b = cv.color.float32[2], a = cv.color.float32[3];
                // GLES: FBO 0 (默认帧缓冲) 是 RGBA8 (非 sRGB)，需要手动 sRGB 编码 clear color
                // 离屏 SRGB8_ALPHA8 FBO 由驱动自动处理
                if (s_isGLES && fb->fbo == 0 && !s_defaultFramebufferIsSRGB) {
                    r = powf(r, 1.0f/2.2f);
                    g = powf(g, 1.0f/2.2f);
                    b = powf(b, 1.0f/2.2f);
                }
                glClearColor(r, g, b, a);
                clearMask |= GL_COLOR_BUFFER_BIT;
            }
        }
        clearIdx++;
    }
    if (clearMask) {
        // 临时关闭 scissor test 以清除全帧缓冲
        glDisable(GL_SCISSOR_TEST);
        glClear(clearMask);
        glEnable(GL_SCISSOR_TEST);
    }

    GL_TRACE("BeginRenderPass: fbo=%u %ux%u attachments=%zu",
             fb->fbo, fb->width, fb->height, fb->attachments.size());
    SDL_Log("[VkOpenGL] BeginRenderPass: fbo=%u %ux%u attachments=%zu",
            fb->fbo, fb->width, fb->height, fb->attachments.size());
}

static VKAPI_ATTR void VKAPI_CALL gl_vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
{
    auto* cb = AS_GL(CommandBuffer, commandBuffer);
    if (!cb) return;

    // 诊断: 读取 offscreen FBO 中心像素
    {
        static int s_endRPColorLog = 0;
        auto* fb_diag = AS_GL(Framebuffer, cb->currentFramebuffer);
        if (fb_diag && fb_diag->fbo != 0) {
            GLenum err = GL_NO_ERROR;
            if (fb_diag->fbo != 1 && s_endRPColorLog < 5) {
                uint8_t pixel[4] = {0};
                glReadPixels(fb_diag->width/2, fb_diag->height/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
                err = glGetError();
                SDL_Log("[VkOpenGL] EndRP fbo=%u center pixel: R=%u G=%u B=%u A=%u err=0x%x",
                        fb_diag->fbo, pixel[0], pixel[1], pixel[2], pixel[3], err);
                s_endRPColorLog++;
            }
        }
    }

    // 更新 image layout 追踪
    auto* rp = AS_GL(RenderPass, cb->currentRenderPass);
    auto* fb = AS_GL(Framebuffer, cb->currentFramebuffer);
    if (rp && fb) {
        for (size_t i = 0; i < fb->attachments.size() && i < rp->attachments.size(); i++) {
            auto* view = AS_GL(ImageView, fb->attachments[i]);
            if (!view || !view->image) continue;
            AS_GL(Image, view->image)->currentLayout = rp->attachments[i].finalLayout;
        }
    }

    cb->currentRenderPass = VK_NULL_HANDLE;
    cb->currentFramebuffer = VK_NULL_HANDLE;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkCmdBindPipeline(
    VkCommandBuffer     commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipeline          pipeline)
{
    (void)pipelineBindPoint;
    auto* cb = AS_GL(CommandBuffer, commandBuffer);
    if (cb) {
        cb->currentPipeline = pipeline;
        cb->stateDirty = true;
        if (s_isGLES) s_gles_currentPipeline = pipeline;
    }
}

static VKAPI_ATTR void VKAPI_CALL gl_vkCmdBindDescriptorSets(
    VkCommandBuffer        commandBuffer,
    VkPipelineBindPoint    pipelineBindPoint,
    VkPipelineLayout       layout,
    uint32_t               firstSet,
    uint32_t               descriptorSetCount,
    const VkDescriptorSet* pDescriptorSets,
    uint32_t               dynamicOffsetCount,
    const uint32_t*        pDynamicOffsets)
{
    (void)pipelineBindPoint; (void)layout; (void)dynamicOffsetCount; (void)pDynamicOffsets;
    auto* cb = AS_GL(CommandBuffer, commandBuffer);
    if (cb && pDescriptorSets) {
        for (uint32_t i = 0; i < descriptorSetCount && (firstSet + i) < 4; ++i) {
            cb->boundSets[firstSet + i] = pDescriptorSets[i];
        }
        cb->stateDirty = true;
    }
}

static VKAPI_ATTR void VKAPI_CALL gl_vkCmdPushConstants(
    VkCommandBuffer    commandBuffer,
    VkPipelineLayout   layout,
    VkShaderStageFlags stageFlags,
    uint32_t           offset,
    uint32_t           size,
    const void*        pValues)
{
    (void)layout; (void)stageFlags;
    auto* cb = AS_GL(CommandBuffer, commandBuffer);
    if (cb && pValues && (offset + size) <= 128) {
        memcpy(cb->pushConstantData + offset, pValues, size);
        if (offset + size > cb->pushConstantSize)
            cb->pushConstantSize = offset + size;
        cb->stateDirty = true;
    }
}

// --- 同步 UBO shadow buffer 数据到 GL buffer ---
static void syncUBOShadowBuffers(GL_CommandBuffer* cb)
{
    auto* pipeline = AS_GL(Pipeline, cb->currentPipeline);
    if (!pipeline || !pipeline->layout) return;
    auto* pl = AS_GL(PipelineLayout, pipeline->layout);
    if (!pl) return;

    for (uint32_t s = 0; s < pl->setLayouts.size() && s < 4; s++) {
        auto* setLayout = AS_GL(DescriptorSetLayout, pl->setLayouts[s]);
        if (!setLayout) continue;
        auto* set = AS_GL(DescriptorSet, cb->boundSets[s]);
        if (!set) continue;

        for (auto& binding : setLayout->bindings) {
            if (binding.descriptorType != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) continue;
            if (binding.binding >= 8) continue;

            GLuint glBuf = set->uboBuffers[binding.binding];
            VkBuffer vkBuf = set->uboVkBuffers[binding.binding];
            if (!glBuf || !vkBuf) continue;

            auto* buf = AS_GL(Buffer, vkBuf);
            if (!buf || !buf->boundMemory) continue;
            auto* mem = AS_GL(Memory, buf->boundMemory);
            if (!mem || !mem->mapped) continue;

            compat_BufferSubData(glBuf, 0, (GLsizeiptr)buf->size, mem->mapped);
        }
    }
}

// --- 刷新所有图形状态到 OpenGL ---
static void flushGraphicsState(GL_CommandBuffer* cb)
{
    if (!cb->stateDirty) return;
    cb->stateDirty = false;

    auto* pipeline = AS_GL(Pipeline, cb->currentPipeline);
    if (!pipeline) return;

    // 1. 着色器和 VAO
    glUseProgram(pipeline->program);
    glBindVertexArray(pipeline->vao);
    {
        static int s_flushLog2 = 0;
        auto* fb_diag = AS_GL(Framebuffer, cb->currentFramebuffer);
        // 只记录 offscreen FBO (fbo=2) 的 flush
        if (fb_diag && fb_diag->fbo == 2 && s_flushLog2 < 10) {
            // 检查关键 GL 状态
            GLint curProg = 0, curFBO = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &curProg);
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &curFBO);
            GLenum fbStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            SDL_Log("[VkOpenGL] flush-fbo2 #%d: prog=%u vao=%u curProg=%d curFBO=%d fbStatus=0x%x",
                    s_flushLog2, pipeline->program, pipeline->vao,
                    curProg, curFBO, fbStatus);
            s_flushLog2++;
        }
    }

    // 2. Rasterizer 状态
    if (pipeline->cullEnable) {
        glEnable(GL_CULL_FACE);
        glCullFace(pipeline->cullMode);
    } else {
        glDisable(GL_CULL_FACE);
    }
    glFrontFace(pipeline->frontFace);
    // GLES 3.0 不支持 glPolygonMode
    if (!s_isGLES) {
        glPolygonMode(GL_FRONT_AND_BACK, pipeline->polygonMode);
    }

    // Depth bias
    if (pipeline->depthBiasEnable) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(pipeline->depthBiasSlope, pipeline->depthBiasConstant);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    // 3. Depth 状态
    if (pipeline->depthTestEnable) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(pipeline->depthFunc);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(pipeline->depthWriteEnable ? GL_TRUE : GL_FALSE);

    // 4. Blend 状态
    if (pipeline->blendEnable) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(pipeline->srcBlend, pipeline->dstBlend,
                            pipeline->srcBlendAlpha, pipeline->dstBlendAlpha);
        glBlendEquationSeparate(pipeline->blendOp, pipeline->blendOpAlpha);
    } else {
        glDisable(GL_BLEND);
    }
    glColorMask(
        (pipeline->colorWriteMask & 0x1) ? GL_TRUE : GL_FALSE,
        (pipeline->colorWriteMask & 0x2) ? GL_TRUE : GL_FALSE,
        (pipeline->colorWriteMask & 0x4) ? GL_TRUE : GL_FALSE,
        (pipeline->colorWriteMask & 0x8) ? GL_TRUE : GL_FALSE);

    // 5. 绑定 descriptor set 资源
    auto* pl = AS_GL(PipelineLayout, pipeline->layout);
    if (!pl) return;
    // 诊断: 检查 descriptor set 绑定状态
    {
        static int s_descLog = 0;
        auto* fb_diag2 = AS_GL(Framebuffer, cb->currentFramebuffer);
        if (fb_diag2 && fb_diag2->fbo == 2 && s_descLog < 3) {
            for (uint32_t s = 0; s < 4; s++) {
                auto* dset = AS_GL(DescriptorSet, cb->boundSets[s]);
                if (dset) {
                    SDL_Log("[VkOpenGL] descDiag fbo=2: set[%u] bound, ubo0=%u ubo1=%u tex0=%u tex1=%u",
                            s, dset->uboBuffers[0], dset->uboBuffers[1],
                            dset->textures[0], dset->textures[1]);
                } else {
                    SDL_Log("[VkOpenGL] descDiag fbo=2: set[%u] = NULL", s);
                }
            }
            // 读取 FrameData UBO 前 16 字节 (view 矩阵的第一行)
            auto* set0 = AS_GL(DescriptorSet, cb->boundSets[0]);
            if (set0 && set0->uboBuffers[0]) {
                float data[4] = {0};
                glBindBuffer(GL_UNIFORM_BUFFER, set0->uboBuffers[0]);
                void* mapped = glMapBufferRange(GL_UNIFORM_BUFFER, 0, 16, GL_MAP_READ_BIT);
                if (mapped) {
                    memcpy(data, mapped, 16);
                    glUnmapBuffer(GL_UNIFORM_BUFFER);
                }
                SDL_Log("[VkOpenGL] FrameData UBO first 16B: %.3f %.3f %.3f %.3f", data[0], data[1], data[2], data[3]);
                int shadowParams[4] = {0};
                mapped = glMapBufferRange(GL_UNIFORM_BUFFER, 752, 16, GL_MAP_READ_BIT);
                if (mapped) {
                    memcpy(shadowParams, mapped, 16);
                    glUnmapBuffer(GL_UNIFORM_BUFFER);
                }
                SDL_Log("[VkOpenGL] FrameData shadowParams: %d %d %d %d",
                        shadowParams[0], shadowParams[1], shadowParams[2], shadowParams[3]);
            }
            s_descLog++;
        }
    }

    uint32_t uboSlot = 0;
    uint32_t texUnit = 0;

    for (uint32_t s = 0; s < pl->setLayouts.size() && s < 4; s++) {
        auto* setLayout = AS_GL(DescriptorSetLayout, pl->setLayouts[s]);
        if (!setLayout) continue;
        auto* set = AS_GL(DescriptorSet, cb->boundSets[s]);

        for (auto& binding : setLayout->bindings) {
            if (binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                if (set && binding.binding < 8 && set->uboBuffers[binding.binding]) {
                    glBindBufferBase(GL_UNIFORM_BUFFER, uboSlot, set->uboBuffers[binding.binding]);
                    GL_TRACE("  UBO slot=%u buf=%u (set%u bind%u)", uboSlot,
                             set->uboBuffers[binding.binding], s, binding.binding);
                }
                uboSlot++;
            } else if (binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                       binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                for (uint32_t d = 0; d < binding.descriptorCount; d++) {
                    uint32_t bIdx = binding.binding + d;
                    if (set && bIdx < 8) {
                        if (set->textures[bIdx])
                            compat_BindTextureUnit(texUnit, set->textures[bIdx]);
                        if (set->samplers[bIdx])
                            glBindSampler(texUnit, set->samplers[bIdx]);
                    }
                    texUnit++;
                }
            }
        }
    }

    // 6. Push constants UBO
    if (cb->pushConstantSize > 0) {
        compat_BufferSubData(cb->pushConstantUBO, 0, cb->pushConstantSize, cb->pushConstantData);
        glBindBufferBase(GL_UNIFORM_BUFFER, uboSlot, cb->pushConstantUBO);
        GL_TRACE("  PushConst slot=%u size=%u", uboSlot, cb->pushConstantSize);
    }
}

static VKAPI_ATTR void VKAPI_CALL gl_vkCmdBindVertexBuffers(
    VkCommandBuffer     commandBuffer,
    uint32_t            firstBinding,
    uint32_t            bindingCount,
    const VkBuffer*     pBuffers,
    const VkDeviceSize* pOffsets)
{
    auto* cb = AS_GL(CommandBuffer, commandBuffer);
    if (!cb) return;
    auto* pipeline = AS_GL(Pipeline, cb->currentPipeline);

    for (uint32_t i = 0; i < bindingCount; i++) {
        uint32_t slot = firstBinding + i;
        auto* buf = AS_GL(Buffer, pBuffers[i]);
        if (!buf || !buf->buffer) continue;

        // 同步 shadow buffer → GL buffer
        auto* mem = AS_GL(Memory, buf->boundMemory);
        if (mem && mem->mapped && mem->memoryTypeIndex == 1) {
            compat_BufferSubData(buf->buffer, 0, (GLsizeiptr)buf->size, mem->mapped);
        }

        GLuint vao = pipeline ? pipeline->vao : 0;
        uint32_t stride = (pipeline && slot < 8) ? pipeline->vertexStrides[slot] : 0;
        if (vao) {
            compat_VertexArrayVertexBuffer(vao, slot, buf->buffer,
                                           (GLintptr)pOffsets[i], stride);
        }

        // GLES: 保存缓冲区信息用于 DrawElementsBaseVertex 模拟
        if (s_isGLES && slot < 8) {
            cb->glesVertexBuffers[slot] = buf->buffer;
            cb->glesVertexOffsets[slot] = (GLintptr)pOffsets[i];
        }
    }
}

static VKAPI_ATTR void VKAPI_CALL gl_vkCmdBindIndexBuffer(
    VkCommandBuffer commandBuffer,
    VkBuffer        buffer,
    VkDeviceSize    offset,
    VkIndexType     indexType)
{
    auto* cb = AS_GL(CommandBuffer, commandBuffer);
    if (!cb) return;
    auto* buf = AS_GL(Buffer, buffer);
    if (!buf) return;

    // 同步 shadow buffer → GL buffer
    auto* mem = AS_GL(Memory, buf->boundMemory);
    if (mem && mem->mapped && mem->memoryTypeIndex == 1) {
        compat_BufferSubData(buf->buffer, 0, (GLsizeiptr)buf->size, mem->mapped);
    }

    cb->boundIndexBuffer = buf->buffer;
    cb->indexType = (indexType == VK_INDEX_TYPE_UINT16) ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    cb->indexBufferOffset = offset;

    // 绑定到当前 pipeline 的 VAO
    auto* pipeline = AS_GL(Pipeline, cb->currentPipeline);
    if (pipeline && pipeline->vao) {
        compat_VertexArrayElementBuffer(pipeline->vao, buf->buffer);
    }
}

static VKAPI_ATTR void VKAPI_CALL gl_vkCmdDraw(
    VkCommandBuffer commandBuffer,
    uint32_t        vertexCount,
    uint32_t        instanceCount,
    uint32_t        firstVertex,
    uint32_t        firstInstance)
{
    auto* cb = AS_GL(CommandBuffer, commandBuffer);
    if (!cb || !cb->currentPipeline) return;
    auto* pipeline = AS_GL(Pipeline, cb->currentPipeline);
    if (!pipeline) return;

    syncUBOShadowBuffers(cb);
    flushGraphicsState(cb);

    GL_TRACE("Draw: verts=%u inst=%u firstV=%u", vertexCount, instanceCount, firstVertex);
    {
        static int s_drawLogCount2 = 0;
        auto* fb = AS_GL(Framebuffer, cb->currentFramebuffer);
        if (s_drawLogCount2 < 10) {
            GLenum err = glGetError();
            SDL_Log("[VkOpenGL] Draw #%d: verts=%u prog=%u fbo=%u err=0x%x",
                    s_drawLogCount2, vertexCount, pipeline->program,
                    fb ? fb->fbo : 9999, err);
            s_drawLogCount2++;
        }
    }

    if (instanceCount > 1 || firstInstance > 0) {
        if (s_isGLES) {
            glDrawArraysInstanced(pipeline->topology, firstVertex, vertexCount, instanceCount);
        } else {
            glDrawArraysInstancedBaseInstance(pipeline->topology, firstVertex,
                                              vertexCount, instanceCount, firstInstance);
        }
    } else {
        glDrawArrays(pipeline->topology, firstVertex, vertexCount);
    }
    // 诊断: 在 offscreen FBO 上的 draw 后读取像素
    {
        static int s_postDrawRead = 0;
        auto* fb_d = AS_GL(Framebuffer, cb->currentFramebuffer);
        if (fb_d && fb_d->fbo != 0 && s_postDrawRead < 3) {
            glFinish();
            uint8_t px[4] = {0};
            glReadPixels(fb_d->width/2, fb_d->height/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            GLenum e2 = glGetError();
            SDL_Log("[VkOpenGL] PostDraw fbo=%u prog=%u pixel: R=%u G=%u B=%u A=%u depthTest=%d err=0x%x",
                    fb_d->fbo, pipeline->program, px[0], px[1], px[2], px[3],
                    pipeline->depthTestEnable, e2);
            s_postDrawRead++;
        }
    }
}

static VKAPI_ATTR void VKAPI_CALL gl_vkCmdDrawIndexed(
    VkCommandBuffer commandBuffer,
    uint32_t        indexCount,
    uint32_t        instanceCount,
    uint32_t        firstIndex,
    int32_t         vertexOffset,
    uint32_t        firstInstance)
{
    auto* cb = AS_GL(CommandBuffer, commandBuffer);
    if (!cb || !cb->currentPipeline) return;
    auto* pipeline = AS_GL(Pipeline, cb->currentPipeline);
    if (!pipeline) return;

    syncUBOShadowBuffers(cb);
    flushGraphicsState(cb);

    // 确保 index buffer 绑定到 VAO
    if (cb->boundIndexBuffer && pipeline->vao) {
        compat_VertexArrayElementBuffer(pipeline->vao, cb->boundIndexBuffer);
    }

    uint32_t indexSize = (cb->indexType == GL_UNSIGNED_SHORT) ? 2 : 4;
    const void* offset = (const void*)(uintptr_t)(firstIndex * indexSize + cb->indexBufferOffset);

    GL_TRACE("DrawIndexed: idx=%u inst=%u firstI=%u vOff=%d", indexCount, instanceCount, firstIndex, vertexOffset);
    {
        static int s_drawLogCount = 0;
        auto* fb = AS_GL(Framebuffer, cb->currentFramebuffer);
        // 诊断: 在 fbo=2 的第一次 DrawIndexed 前，查询 VAO 属性状态
        static int s_fbo2VaoCheck = 0;
        if (fb && fb->fbo == 2 && s_fbo2VaoCheck < 1) {
            s_fbo2VaoCheck++;
            GLint curVao = 0;
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &curVao);
            for (int ai = 0; ai < 4; ai++) {
                GLint enabled = 0, size = 0, stride = 0, type = 0, bufBinding = 0;
                void* pointer = nullptr;
                glGetVertexAttribiv(ai, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &enabled);
                glGetVertexAttribiv(ai, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
                glGetVertexAttribiv(ai, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &stride);
                glGetVertexAttribiv(ai, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
                glGetVertexAttribiv(ai, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &bufBinding);
                glGetVertexAttribPointerv(ai, GL_VERTEX_ATTRIB_ARRAY_POINTER, &pointer);
                SDL_Log("[VkOpenGL] fbo2 VAO=%d attr[%d]: en=%d sz=%d stride=%d buf=%d ptr=%ld",
                        curVao, ai, enabled, size, stride, bufBinding, (long)(uintptr_t)pointer);
            }
            // 查询 uniform block binding
            GLint numBlocks = 0;
            glGetProgramiv(pipeline->program, GL_ACTIVE_UNIFORM_BLOCKS, &numBlocks);
            for (int bi = 0; bi < numBlocks && bi < 4; bi++) {
                char bname[128] = {};
                GLint bsize = 0, bbind = 0;
                glGetActiveUniformBlockName(pipeline->program, bi, sizeof(bname), nullptr, bname);
                glGetActiveUniformBlockiv(pipeline->program, bi, GL_UNIFORM_BLOCK_DATA_SIZE, &bsize);
                glGetActiveUniformBlockiv(pipeline->program, bi, GL_UNIFORM_BLOCK_BINDING, &bbind);
                // 查询实际绑定的 buffer
                GLint boundBuf = 0;
                glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, bbind, &boundBuf);
                SDL_Log("[VkOpenGL] fbo2 block[%d]='%s' size=%d binding=%d boundBuf=%d",
                        bi, bname, bsize, bbind, boundBuf);
            }
            // 读取 push constant 数据 (model 矩阵前 16 字节)
            {
                GLint pcBuf = 0;
                glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING, numBlocks > 0 ? numBlocks - 1 : 2, &pcBuf);
                if (pcBuf > 0) {
                    glBindBuffer(GL_UNIFORM_BUFFER, pcBuf);
                    float pc[16] = {0};
                    void* m = glMapBufferRange(GL_UNIFORM_BUFFER, 0, 64, GL_MAP_READ_BIT);
                    if (m) { memcpy(pc, m, 64); glUnmapBuffer(GL_UNIFORM_BUFFER); }
                    SDL_Log("[VkOpenGL] PushConst model rows: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]",
                            pc[0], pc[1], pc[2], pc[3],
                            pc[4], pc[5], pc[6], pc[7],
                            pc[8], pc[9], pc[10], pc[11],
                            pc[12], pc[13], pc[14], pc[15]);
                }
            }
            // 读取顶点缓冲区前 44 字节 (第一个顶点)
            GLint vbuf = 0;
            glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &vbuf);
            if (vbuf > 0) {
                glBindBuffer(GL_ARRAY_BUFFER, vbuf);
                float vtx[11] = {0};  // 44 bytes = 11 floats
                void* mapped = glMapBufferRange(GL_ARRAY_BUFFER, 0, 44, GL_MAP_READ_BIT);
                if (mapped) {
                    memcpy(vtx, mapped, 44);
                    glUnmapBuffer(GL_ARRAY_BUFFER);
                    SDL_Log("[VkOpenGL] VTX0: pos(%.2f,%.2f,%.2f) col(%.2f,%.2f,%.2f) uv(%.2f,%.2f) n(%.2f,%.2f,%.2f)",
                            vtx[0], vtx[1], vtx[2], vtx[3], vtx[4], vtx[5],
                            vtx[6], vtx[7], vtx[8], vtx[9], vtx[10]);
                } else {
                    SDL_Log("[VkOpenGL] VTX: mapBuffer failed err=0x%x", glGetError());
                }
            }
        }
        if (s_drawLogCount < 30) {
            GLenum err = glGetError();
            SDL_Log("[VkOpenGL] DrawIndexed #%d: idx=%u prog=%u fbo=%u err=0x%x",
                    s_drawLogCount, indexCount, pipeline->program,
                    fb ? fb->fbo : 9999, err);
            s_drawLogCount++;
        }
    }

    if (instanceCount > 1 || firstInstance > 0) {
        if (s_isGLES) {
            // GLES 3.0 只有 glDrawElementsInstanced (无 BaseVertex/BaseInstance)
            glDrawElementsInstanced(pipeline->topology, indexCount, cb->indexType,
                                    offset, instanceCount);
        } else {
            glDrawElementsInstancedBaseVertexBaseInstance(
                pipeline->topology, indexCount, cb->indexType,
                offset, instanceCount, vertexOffset, firstInstance);
        }
    } else if (vertexOffset != 0) {
        if (s_isGLES) {
            // GLES 3.0 没有 glDrawElementsBaseVertex
            // 通过调整 glVertexAttribPointer 的偏移来模拟 vertexOffset
            glBindVertexArray(pipeline->vao);
            for (uint32_t a = 0; a < pipeline->attribCount; a++) {
                uint32_t binding = pipeline->attribBinding[a];
                uint32_t stride = (binding < 8) ? pipeline->vertexStrides[binding] : 0;
                GLuint buffer = (binding < 8) ? cb->glesVertexBuffers[binding] : 0;
                GLintptr baseOffset = (binding < 8) ? cb->glesVertexOffsets[binding] : 0;
                if (buffer) {
                    glBindBuffer(GL_ARRAY_BUFFER, buffer);
                    glVertexAttribPointer(a,
                        pipeline->attribComponents[a],
                        pipeline->attribType[a],
                        pipeline->attribNormalized[a] ? GL_TRUE : GL_FALSE,
                        stride,
                        (const void*)(uintptr_t)(baseOffset + pipeline->attribOffset[a] + vertexOffset * stride));
                }
            }
            glDrawElements(pipeline->topology, indexCount, cb->indexType, offset);
            // 恢复原始偏移
            for (uint32_t a = 0; a < pipeline->attribCount; a++) {
                uint32_t binding = pipeline->attribBinding[a];
                uint32_t stride = (binding < 8) ? pipeline->vertexStrides[binding] : 0;
                GLuint buffer = (binding < 8) ? cb->glesVertexBuffers[binding] : 0;
                GLintptr baseOffset = (binding < 8) ? cb->glesVertexOffsets[binding] : 0;
                if (buffer) {
                    glBindBuffer(GL_ARRAY_BUFFER, buffer);
                    glVertexAttribPointer(a,
                        pipeline->attribComponents[a],
                        pipeline->attribType[a],
                        pipeline->attribNormalized[a] ? GL_TRUE : GL_FALSE,
                        stride,
                        (const void*)(uintptr_t)(baseOffset + pipeline->attribOffset[a]));
                }
            }
        } else {
            glDrawElementsBaseVertex(pipeline->topology, indexCount, cb->indexType,
                                      (void*)offset, vertexOffset);
        }
    } else {
        glDrawElements(pipeline->topology, indexCount, cb->indexType, offset);
    }
    // 诊断: 在 offscreen FBO 上的 DrawIndexed 后读取像素
    {
        static int s_postDrawIdxRead = 0;
        auto* fb_d = AS_GL(Framebuffer, cb->currentFramebuffer);
        if (fb_d && fb_d->fbo != 0 && fb_d->fbo != 1 && s_postDrawIdxRead < 5) {
            glFinish();
            uint8_t px[4] = {0};
            glReadPixels(fb_d->width/2, fb_d->height/2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            SDL_Log("[VkOpenGL] PostDrawIdx fbo=%u prog=%u pixel: R=%u G=%u B=%u A=%u depthEn=%d depthFunc=0x%x",
                    fb_d->fbo, pipeline->program, px[0], px[1], px[2], px[3],
                    pipeline->depthTestEnable, pipeline->depthFunc);
            s_postDrawIdxRead++;
        }
    }
}

static VKAPI_ATTR void VKAPI_CALL gl_vkCmdSetViewport(
    VkCommandBuffer  commandBuffer,
    uint32_t         firstViewport,
    uint32_t         viewportCount,
    const VkViewport* pViewports)
{
    (void)commandBuffer; (void)firstViewport;
    if (!pViewports || viewportCount == 0) return;

    // OpenGL viewport: Y 从底部开始
    // Vulkan viewport 可能有负 height (Y-flip), OpenGL 不支持
    float x = pViewports[0].x;
    float y = pViewports[0].y;
    float w = pViewports[0].width;
    float h = pViewports[0].height;

    // 处理 Vulkan Y-flip (height < 0)
    if (h < 0) {
        y = y + h;
        h = -h;
    }

    glViewport((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h);
    glDepthRangef(pViewports[0].minDepth, pViewports[0].maxDepth);
}

static VKAPI_ATTR void VKAPI_CALL gl_vkCmdSetScissor(
    VkCommandBuffer commandBuffer,
    uint32_t        firstScissor,
    uint32_t        scissorCount,
    const VkRect2D* pScissors)
{
    (void)firstScissor;
    if (!pScissors || scissorCount == 0) return;

    // OpenGL scissor Y 轴原点在左下，Vulkan 在左上，需要翻转
    auto* cmd = AS_GL(CommandBuffer, commandBuffer);
    int32_t x = pScissors[0].offset.x;
    int32_t y = pScissors[0].offset.y;
    int32_t w = pScissors[0].extent.width;
    int32_t h = pScissors[0].extent.height;

    // 获取当前 framebuffer 高度用于 Y 翻转
    uint32_t fbHeight = 0;
    if (cmd && cmd->currentFramebuffer) {
        auto* fb = AS_GL(Framebuffer, cmd->currentFramebuffer);
        if (fb) fbHeight = fb->height;
    }
    if (fbHeight > 0) {
        y = fbHeight - y - h;
    }

    glScissor(x, y, w, h);
}

static VKAPI_ATTR void VKAPI_CALL gl_vkCmdCopyBuffer(
    VkCommandBuffer      commandBuffer,
    VkBuffer             srcBuffer,
    VkBuffer             dstBuffer,
    uint32_t             regionCount,
    const VkBufferCopy*  pRegions)
{
    (void)commandBuffer;
    auto* src = AS_GL(Buffer, srcBuffer);
    auto* dst = AS_GL(Buffer, dstBuffer);
    if (!src || !dst || !pRegions) return;

    // 如果 src 有 shadow buffer，先同步
    auto* srcMem = AS_GL(Memory, src->boundMemory);
    if (srcMem && srcMem->mapped && src->buffer) {
        compat_BufferSubData(src->buffer, 0, (GLsizeiptr)src->size, srcMem->mapped);
    }

    for (uint32_t i = 0; i < regionCount; i++) {
        if (src->buffer && dst->buffer) {
            compat_CopyBufferSubData(src->buffer, dst->buffer,
                (GLintptr)pRegions[i].srcOffset,
                (GLintptr)pRegions[i].dstOffset,
                (GLsizeiptr)pRegions[i].size);
        } else if (srcMem && srcMem->mapped) {
            // Fallback: shadow buffer memcpy
            auto* dstMem = AS_GL(Memory, dst->boundMemory);
            if (dstMem && dstMem->mapped) {
                memcpy(static_cast<uint8_t*>(dstMem->mapped) + pRegions[i].dstOffset,
                       static_cast<uint8_t*>(srcMem->mapped) + pRegions[i].srcOffset,
                       (size_t)pRegions[i].size);
            }
        }
    }
}

static VKAPI_ATTR void VKAPI_CALL gl_vkCmdCopyBufferToImage(
    VkCommandBuffer          commandBuffer,
    VkBuffer                 srcBuffer,
    VkImage                  dstImage,
    VkImageLayout            dstImageLayout,
    uint32_t                 regionCount,
    const VkBufferImageCopy* pRegions)
{
    (void)commandBuffer; (void)dstImageLayout;
    auto* src = AS_GL(Buffer, srcBuffer);
    auto* dst = AS_GL(Image, dstImage);
    if (!src || !dst || !pRegions) return;

    auto* srcMem = AS_GL(Memory, src->boundMemory);
    if (!srcMem || !srcMem->mapped) return;

    auto fi = vkFormatToGL(dst->format);

    for (uint32_t i = 0; i < regionCount; i++) {
        auto& r = pRegions[i];
        uint32_t w = r.imageExtent.width;
        uint32_t h = r.imageExtent.height;
        uint32_t mip = r.imageSubresource.mipLevel;
        const void* data = static_cast<const uint8_t*>(srcMem->mapped) + r.bufferOffset;

        glPixelStorei(GL_UNPACK_ROW_LENGTH, r.bufferRowLength > 0 ? r.bufferRowLength : 0);
        compat_TextureSubImage2D(dst->texture, mip,
            r.imageOffset.x, r.imageOffset.y, w, h,
            fi.format, fi.type, data);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
}

static VKAPI_ATTR void VKAPI_CALL gl_vkCmdCopyImageToBuffer(
    VkCommandBuffer          commandBuffer,
    VkImage                  srcImage,
    VkImageLayout            srcImageLayout,
    VkBuffer                 dstBuffer,
    uint32_t                 regionCount,
    const VkBufferImageCopy* pRegions)
{
    (void)srcImageLayout;
    auto* cb = AS_GL(CommandBuffer, commandBuffer);
    auto* src = AS_GL(Image, srcImage);
    auto* dst = AS_GL(Buffer, dstBuffer);
    if (!cb || !src || !dst || !pRegions) return;

    auto fi = vkFormatToGL(src->format);

    for (uint32_t i = 0; i < regionCount; i++) {
        auto& r = pRegions[i];

        // 统一使用延迟回读 — 确保 QueueSubmit 时 shadow buffer 已分配
        GL_PendingReadback rb;
        rb.srcTexture = (src->ownsResource && src->texture) ? src->texture : 0;
        rb.dstBuffer = dstBuffer;
        rb.width = r.imageExtent.width;
        rb.height = r.imageExtent.height;
        rb.texelSize = fi.texelSize;
        rb.bufferOffset = r.bufferOffset;
        rb.glFormat = fi.format;
        rb.glType = fi.type;
        rb.mipLevel = r.imageSubresource.mipLevel;
        cb->pendingReadbacks.push_back(rb);
    }
}

static VKAPI_ATTR void VKAPI_CALL gl_vkCmdPipelineBarrier(
    VkCommandBuffer             commandBuffer,
    VkPipelineStageFlags        srcStageMask,
    VkPipelineStageFlags        dstStageMask,
    VkDependencyFlags           dependencyFlags,
    uint32_t                    memoryBarrierCount,
    const VkMemoryBarrier*      pMemoryBarriers,
    uint32_t                    bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier* pBufferMemoryBarriers,
    uint32_t                    imageMemoryBarrierCount,
    const VkImageMemoryBarrier* pImageMemoryBarriers)
{
    (void)commandBuffer; (void)srcStageMask; (void)dstStageMask; (void)dependencyFlags;
    (void)memoryBarrierCount; (void)pMemoryBarriers;
    (void)bufferMemoryBarrierCount; (void)pBufferMemoryBarriers;

    // 更新 image layout 追踪
    if (pImageMemoryBarriers) {
        for (uint32_t i = 0; i < imageMemoryBarrierCount; ++i) {
            auto* img = AS_GL(Image, pImageMemoryBarriers[i].image);
            if (img) img->currentLayout = pImageMemoryBarriers[i].newLayout;
        }
    }

    // OpenGL memory barrier (GLES 3.0 没有 glMemoryBarrier，GLES 3.1 才有)
    if (!s_isGLES) {
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
    } else {
        glFlush(); // GLES 3.0 fallback
    }
}

static VKAPI_ATTR void VKAPI_CALL gl_vkCmdBlitImage(
    VkCommandBuffer   commandBuffer,
    VkImage           srcImage,
    VkImageLayout     srcImageLayout,
    VkImage           dstImage,
    VkImageLayout     dstImageLayout,
    uint32_t          regionCount,
    const VkImageBlit* pRegions,
    VkFilter          filter)
{
    (void)commandBuffer; (void)srcImageLayout; (void)dstImageLayout; (void)filter;
    auto* src = AS_GL(Image, srcImage);
    auto* dst = AS_GL(Image, dstImage);
    if (!src || !dst || !pRegions || regionCount == 0) return;

    for (uint32_t i = 0; i < regionCount; i++) {
        auto& r = pRegions[i];
        // 使用 glCopyImageSubData (不支持缩放, 只做 1:1 copy)
        uint32_t srcW = std::abs(r.srcOffsets[1].x - r.srcOffsets[0].x);
        uint32_t srcH = std::abs(r.srcOffsets[1].y - r.srcOffsets[0].y);
        if (src->texture && dst->texture) {
            if (s_isGLES) {
                // GLES 3.0 没有 glCopyImageSubData，用 FBO + glBlitFramebuffer 替代
                GLuint srcFbo = 0, dstFbo = 0;
                glGenFramebuffers(1, &srcFbo);
                glGenFramebuffers(1, &dstFbo);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
                glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                    GL_TEXTURE_2D, src->texture, r.srcSubresource.mipLevel);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                    GL_TEXTURE_2D, dst->texture, r.dstSubresource.mipLevel);
                glBlitFramebuffer(
                    r.srcOffsets[0].x, r.srcOffsets[0].y, r.srcOffsets[0].x + srcW, r.srcOffsets[0].y + srcH,
                    r.dstOffsets[0].x, r.dstOffsets[0].y, r.dstOffsets[0].x + srcW, r.dstOffsets[0].y + srcH,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &srcFbo);
                glDeleteFramebuffers(1, &dstFbo);
            } else {
                glCopyImageSubData(
                    src->texture, GL_TEXTURE_2D, r.srcSubresource.mipLevel,
                    r.srcOffsets[0].x, r.srcOffsets[0].y, 0,
                    dst->texture, GL_TEXTURE_2D, r.dstSubresource.mipLevel,
                    r.dstOffsets[0].x, r.dstOffsets[0].y, 0,
                    srcW, srcH, 1);
            }
        }
    }
}

// ============================================================================
// Synchronization
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkWaitForFences(
    VkDevice       device,
    uint32_t       fenceCount,
    const VkFence* pFences,
    VkBool32       waitAll,
    uint64_t       timeout)
{
    (void)device; (void)waitAll; (void)timeout;
    // OpenGL 单线程: fence 总是已完成
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkResetFences(
    VkDevice       device,
    uint32_t       fenceCount,
    const VkFence* pFences)
{
    (void)device;
    if (pFences) {
        for (uint32_t i = 0; i < fenceCount; ++i) {
            auto* f = AS_GL(Fence, pFences[i]);
            if (f) f->signaled = false;
        }
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkQueueSubmit(
    VkQueue             queue,
    uint32_t            submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence             fence)
{
    (void)queue; (void)submitCount; (void)pSubmits;

    // 处理 pending readbacks
    if (pSubmits) {
        for (uint32_t s = 0; s < submitCount; s++) {
            for (uint32_t c = 0; c < pSubmits[s].commandBufferCount; c++) {
                auto* cb = AS_GL(CommandBuffer, pSubmits[s].pCommandBuffers[c]);
                if (!cb) continue;
                for (auto& rb : cb->pendingReadbacks) {
                    auto* dstBuf = AS_GL(Buffer, rb.dstBuffer);
                    auto* dstMem = dstBuf ? AS_GL(Memory, dstBuf->boundMemory) : nullptr;
                    if (!dstMem) continue;

                    // 确保 shadow buffer 已分配 (vkMapMemory 可能尚未调用)
                    if (!dstMem->mapped) {
                        dstMem->mapped = calloc(1, (size_t)dstMem->size);
                        if (!dstMem->mapped) continue;
                    }

                    uint32_t rowBytes = rb.width * rb.texelSize;
                    uint32_t totalBytes = rowBytes * rb.height;
                    void* dstPtr = static_cast<uint8_t*>(dstMem->mapped) + rb.bufferOffset;

                    if (rb.srcTexture != 0) {
                        // 纹理回读
                        compat_GetTextureImage(rb.srcTexture, rb.mipLevel,
                                               rb.glFormat, rb.glType,
                                               totalBytes, dstPtr, rb.width, rb.height);
                    } else {
                        // Swapchain / FBO 0 回读
                        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                        glReadPixels(0, 0, rb.width, rb.height,
                                     rb.glFormat, rb.glType, dstPtr);
                    }

                    // OpenGL 返回像素从底部到顶部，需要垂直翻转
                    std::vector<uint8_t> rowTmp(rowBytes);
                    uint8_t* pixels = static_cast<uint8_t*>(dstPtr);
                    for (uint32_t y = 0; y < rb.height / 2; y++) {
                        uint8_t* top = pixels + y * rowBytes;
                        uint8_t* bot = pixels + (rb.height - 1 - y) * rowBytes;
                        memcpy(rowTmp.data(), top, rowBytes);
                        memcpy(top, bot, rowBytes);
                        memcpy(bot, rowTmp.data(), rowBytes);
                    }
                }
                cb->pendingReadbacks.clear();
            }
        }
    }

    glFlush();

    if (fence != VK_NULL_HANDLE) {
        auto* f = AS_GL(Fence, fence);
        if (f) f->signaled = true;
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkQueueWaitIdle(VkQueue queue)
{
    (void)queue;
    glFinish();
    return VK_SUCCESS;
}

// ============================================================================
// Pipeline
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreatePipelineLayout(
    VkDevice                          device,
    const VkPipelineLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks*      pAllocator,
    VkPipelineLayout*                 pPipelineLayout)
{
    (void)device; (void)pAllocator;
    auto* layout = new GL_PipelineLayout();
    if (pCreateInfo) {
        for (uint32_t i = 0; i < pCreateInfo->setLayoutCount; ++i)
            layout->setLayouts.push_back(pCreateInfo->pSetLayouts[i]);
        for (uint32_t i = 0; i < pCreateInfo->pushConstantRangeCount; ++i) {
            layout->pushConstantRanges.push_back(pCreateInfo->pPushConstantRanges[i]);
            layout->pushConstSize += pCreateInfo->pPushConstantRanges[i].size;
        }
    }
    *pPipelineLayout = TO_VK(VkPipelineLayout, layout);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroyPipelineLayout(
    VkDevice                     device,
    VkPipelineLayout             pipelineLayout,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_GL(PipelineLayout, pipelineLayout);
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateShaderModule(
    VkDevice                        device,
    const VkShaderModuleCreateInfo* pCreateInfo,
    const VkAllocationCallbacks*    pAllocator,
    VkShaderModule*                 pShaderModule)
{
    (void)device; (void)pAllocator;
    auto* mod = new GL_ShaderModule();

    if (pCreateInfo && pCreateInfo->pCode && pCreateInfo->codeSize > 0) {
        // 检测 SPIR-V magic number
        const uint32_t* words = pCreateInfo->pCode;
        bool isSpirvData = (pCreateInfo->codeSize >= 4 && words[0] == SPIRV_MAGIC);

        if (isSpirvData) {
            // ImGui SPIR-V (来自 imgui_impl_vulkan) 通常 < 5KB，引擎 shader > 10KB
            // ImGui SPIR-V 含 Vulkan 特有的 set 装饰和 Y-down 坐标，必须替换为自定义 GLSL
            bool isSmallSpirv = (pCreateInfo->codeSize < 8000);
            // 所有 SPIR-V 都是 ImGui shader（引擎 shader 应该是 GLSL 文本变体）
            // ImGui SPIR-V 来自 imgui_impl_vulkan，必须替换为自定义 GLSL
            mod->isImguiReplacement = true;
            if (pCreateInfo->codeSize > 8000) {
                // 意外的大 SPIR-V — 可能是引擎 shader 走了错误路径
                fprintf(stderr, "[VkOpenGL] WARNING: large SPIR-V (%zu bytes) — expected GLSL text for OpenGL backend\n",
                    pCreateInfo->codeSize);
            }
            } else {
                // 已经是 GLSL 文本 — 应用后处理修复
                std::string raw(reinterpret_cast<const char*>(pCreateInfo->pCode),
                                pCreateInfo->codeSize);
                while (!raw.empty() && raw.back() == '\0')
                    raw.pop_back();
                mod->glslSource = fixupGLSL(raw, s_isGLES ? &mod->samplerUnits : nullptr);
                // 诊断: 输出 GLSL 的前几个 "in " 和 "layout" 行
                {
                static int s_shaderDumpCount = 0;
                if (s_isGLES && s_shaderDumpCount < 1) {
                    // 输出所有 struct 定义和 UBO 声明
                    size_t pp = 0;
                    int found = 0;
                    bool inStruct = false;
                    while (pp < mod->glslSource.size() && found < 60) {
                        size_t eol = mod->glslSource.find('\n', pp);
                        if (eol == std::string::npos) eol = mod->glslSource.size();
                        std::string ln = mod->glslSource.substr(pp, eol - pp);
                        pp = eol + 1;
                        if (ln.find("struct ") != std::string::npos || ln.find("uniform block_") != std::string::npos)
                            inStruct = true;
                        if (inStruct) {
                            SDL_Log("[SRC#%d] %s", s_shaderDumpCount, ln.c_str());
                            found++;
                            if (ln.find("};") != std::string::npos) inStruct = false;
                        }
                    }
                    s_shaderDumpCount++;
                }
            }
        }
    }

    *pShaderModule = TO_VK(VkShaderModule, mod);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroyShaderModule(
    VkDevice                     device,
    VkShaderModule               shaderModule,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_GL(ShaderModule, shaderModule);
}

static GLuint compileGLShader(GLenum type, const char* source, const char* label)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        GLint logLen = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
        std::string infoLog(logLen + 1, '\0');
        glGetShaderInfoLog(shader, logLen, nullptr, infoLog.data());
        SDL_Log("[VkOpenGL] %s compile FAILED: %s", label, infoLog.c_str());
        // 输出前 130 行到 logcat (找 line 81 附近)
        {
            static int dumpCount = 0;
            if (dumpCount++ < 2) {
                const char* s = source;
                for (int line = 1; *s && line <= 130; line++) {
                    const char* le = s;
                    while (*le && *le != '\n') le++;
                    if (line >= 100 && line <= 115) // 输出 line 100-115
                        SDL_Log("[%s L%03d] %.*s", label, line, (int)(le - s), s);
                    s = (*le == '\n') ? le + 1 : le;
                }
            }
        }
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

// (GL_ARB_gl_spirv 代码已移除 — 按设计文档使用 GLSL 文本方案)

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateGraphicsPipelines(
    VkDevice                            device,
    VkPipelineCache                     pipelineCache,
    uint32_t                            createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks*        pAllocator,
    VkPipeline*                         pPipelines)
{
    (void)device; (void)pipelineCache; (void)pAllocator;

    for (uint32_t i = 0; i < createInfoCount; i++) {
        auto& ci = pCreateInfos[i];
        auto* pipe = new GL_Pipeline();
        pipe->layout = ci.layout;
        std::vector<std::pair<std::string, int>> vertexInputs;
        bool useGlesSceneOverride = false;

        if (s_isGLES) {
            for (uint32_t s = 0; s < ci.stageCount; s++) {
                auto& stage = ci.pStages[s];
                if (stage.stage != VK_SHADER_STAGE_VERTEX_BIT)
                    continue;
                auto* sm = AS_GL(ShaderModule, stage.module);
                if (!sm || sm->isImguiReplacement || sm->glslSource.empty())
                    continue;
                vertexInputs = extractVertexInputs(sm->glslSource);
                // 默认先走正式的 Slang 生成 GLSL 路径。
                // 仅在后续需要保底时再启用自定义 scene override。
                if (vertexInputs.size() == 4)
                    useGlesSceneOverride = false;
                break;
            }
        }

        // === 编译 shader 并链接 program ===
        GLuint vs = 0, fs = 0;
        for (uint32_t s = 0; s < ci.stageCount; s++) {
            auto& stage = ci.pStages[s];
            auto* sm = AS_GL(ShaderModule, stage.module);
            if (!sm) continue;

            GLenum glStage = (stage.stage == VK_SHADER_STAGE_VERTEX_BIT) ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
            const char* label = (stage.stage == VK_SHADER_STAGE_VERTEX_BIT) ? "VS" : "PS";
            GLuint shader = 0;

            if (useGlesSceneOverride) {
                const char* glsl = (stage.stage == VK_SHADER_STAGE_VERTEX_BIT) ? s_glesSceneVS : s_glesSceneFS;
                shader = compileGLShader(glStage, glsl, label);
            } else if (sm->isImguiReplacement) {
                // ImGui shader: 必须用自定义 GLSL (含 Y-flip)，不能用原始 SPIR-V
                const char* glsl;
                if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT) {
                    glsl = s_isGLES ? s_imguiVS_gles : s_imguiVS_glsl;
                } else {
                    if (s_isGLES)
                        glsl = s_defaultFramebufferIsSRGB ? s_imguiPS_gles_auto_srgb
                                                          : s_imguiPS_gles_manual_srgb;
                    else
                        glsl = s_imguiPS_glsl;
                }
                shader = compileGLShader(glStage, glsl, label);
            } else if (!sm->glslSource.empty()) {
                if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT && vertexInputs.empty())
                    vertexInputs = extractVertexInputs(sm->glslSource);
                shader = compileGLShader(glStage, sm->glslSource.c_str(), label);
            }

            if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT) vs = shader;
            else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) fs = shader;
        }

        if (vs && fs) {
            pipe->program = glCreateProgram();
            glAttachShader(pipe->program, vs);
            glAttachShader(pipe->program, fs);
            for (auto& [name, location] : vertexInputs)
                glBindAttribLocation(pipe->program, static_cast<GLuint>(location), name.c_str());
            glLinkProgram(pipe->program);

            GLint linkStatus = 0;
            glGetProgramiv(pipe->program, GL_LINK_STATUS, &linkStatus);
            if (!linkStatus) {
                GLint logLen = 0;
                glGetProgramiv(pipe->program, GL_INFO_LOG_LENGTH, &logLen);
                std::string infoLog(logLen + 1, '\0');
                glGetProgramInfoLog(pipe->program, logLen, nullptr, infoLog.data());
                SDL_Log("[VkOpenGL] Program link FAILED:\n%s", infoLog.c_str());
                glDeleteProgram(pipe->program);
                pipe->program = 0;
            }
        }
        SDL_Log("[VkOpenGL] Pipeline: vs=%u fs=%u prog=%u", vs, fs, pipe->program);
        if (pipe->program && useGlesSceneOverride) {
            glUseProgram(pipe->program);
            GLint albedoLoc = glGetUniformLocation(pipe->program, "albedoMap_0");
            if (albedoLoc >= 0)
                glUniform1i(albedoLoc, 1);
            glUseProgram(0);
        }
        // 链接后设置所有 uniform block 的 binding point
        // Slang GLSL 输出的 uniform block 都没有 layout(binding=N)，全部默认 binding=0
        // 按 block 出现顺序分配递增 binding：
        //   FrameData → 0, MaterialParams → 1, PushConstants → 2
        // 这与 flushGraphicsState 中 UBO 的 slot 分配顺序一致
        if (pipe->program) {
            GLint numBlocks = 0;
            glGetProgramiv(pipe->program, GL_ACTIVE_UNIFORM_BLOCKS, &numBlocks);

            // 先收集所有 block 名称，按 "FrameData 优先, PushConstants 最后" 排序
            struct BlockInfo { GLuint index; std::string name; };
            std::vector<BlockInfo> blocks;
            for (GLint b = 0; b < numBlocks; b++) {
                char blockName[256] = {};
                GLsizei nameLen = 0;
                glGetActiveUniformBlockName(pipe->program, b, sizeof(blockName), &nameLen, blockName);
                blocks.push_back({(GLuint)b, std::string(blockName)});
            }

            // 排序: FrameData → 0, 其他 UBO → 1..N-1, PushConstants → N
            auto sortKey = [](const std::string& name) -> int {
                if (name.find("FrameData") != std::string::npos) return 0;
                if (name.find("PushConstants") != std::string::npos) return 1000;
                return 1;
            };
            std::sort(blocks.begin(), blocks.end(),
                [&](const BlockInfo& a, const BlockInfo& b) { return sortKey(a.name) < sortKey(b.name); });

            for (uint32_t i = 0; i < blocks.size(); i++) {
                glUniformBlockBinding(pipe->program, blocks[i].index, i);
            }

            // GLES: sampler uniform 没有 layout(binding=N)，需要手动设置 texture unit
            // 使用 fixupGLSL 生成的 (name → newBinding) 映射
            if (s_isGLES) {
                // 收集所有 stage 的 sampler 映射
                std::vector<std::pair<std::string, int>> allSamplerUnits;
                for (uint32_t s = 0; s < ci.stageCount; s++) {
                    auto* sm = AS_GL(ShaderModule, ci.pStages[s].module);
                    if (sm) {
                        for (auto& su : sm->samplerUnits) {
                            // 避免重复（VS 和 PS 可能共享 sampler）
                            bool found = false;
                            for (auto& existing : allSamplerUnits) {
                                if (existing.first == su.first) { found = true; break; }
                            }
                            if (!found) allSamplerUnits.push_back(su);
                        }
                    }
                }
                if (!allSamplerUnits.empty()) {
                    glUseProgram(pipe->program);
                    for (auto& su : allSamplerUnits) {
                        GLint loc = glGetUniformLocation(pipe->program, su.first.c_str());
                        if (loc >= 0) {
                            glUniform1i(loc, su.second);
                        }
                    }
                    glUseProgram(0);
                }
            }
        }

        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);

        // === 创建 VAO 并设置顶点属性 ===
        compat_CreateVertexArrays(1, &pipe->vao);

        if (ci.pVertexInputState) {
            // 保存 binding stride
            for (uint32_t b = 0; b < ci.pVertexInputState->vertexBindingDescriptionCount; b++) {
                auto& binding = ci.pVertexInputState->pVertexBindingDescriptions[b];
                if (binding.binding < 8) {
                    pipe->vertexStrides[binding.binding] = binding.stride;
                    if (binding.binding + 1 > pipe->vertexBindingCount)
                        pipe->vertexBindingCount = binding.binding + 1;
                }
            }

            // 设置顶点属性
            if (s_isGLES) {
                // GLES 3.0: 使用 glVertexAttribPointer (bind-to-edit 模式)
                // 注意: 真正的 glVertexAttribPointer 调用在 CmdBindVertexBuffers 中进行
                // 这里只保存属性信息并启用
                glBindVertexArray(pipe->vao);
                for (uint32_t a = 0; a < ci.pVertexInputState->vertexAttributeDescriptionCount; a++) {
                    auto& attr = ci.pVertexInputState->pVertexAttributeDescriptions[a];
                    glEnableVertexAttribArray(attr.location);
                    // 保存属性信息到 pipeline 结构以备后续使用
                    if (attr.location < 16) {
                        pipe->attribComponents[attr.location] = vkFormatComponents(attr.format);
                        pipe->attribType[attr.location] = vkFormatGLType(attr.format);
                        pipe->attribNormalized[attr.location] = vkFormatIsNormalized(attr.format);
                        pipe->attribOffset[attr.location] = attr.offset;
                        pipe->attribBinding[attr.location] = attr.binding;
                        if (attr.location >= pipe->attribCount)
                            pipe->attribCount = attr.location + 1;
                    }
                }
                glBindVertexArray(0);
            } else {
                // Desktop GL 4.5: 使用 DSA
                for (uint32_t a = 0; a < ci.pVertexInputState->vertexAttributeDescriptionCount; a++) {
                    auto& attr = ci.pVertexInputState->pVertexAttributeDescriptions[a];
                    int components = vkFormatComponents(attr.format);
                    GLenum type = vkFormatGLType(attr.format);
                    bool normalized = vkFormatIsNormalized(attr.format);

                    glEnableVertexArrayAttrib(pipe->vao, attr.location);
                    glVertexArrayAttribFormat(pipe->vao, attr.location,
                        components, type, normalized ? GL_TRUE : GL_FALSE, attr.offset);
                    glVertexArrayAttribBinding(pipe->vao, attr.location, attr.binding);
                }
            }
        }

        // === Topology ===
        if (ci.pInputAssemblyState) {
            pipe->topology = vkTopologyToGL(ci.pInputAssemblyState->topology);
        }

        // === Rasterizer State ===
        if (ci.pRasterizationState) {
            auto* rs = ci.pRasterizationState;
            pipe->polygonMode = (rs->polygonMode == VK_POLYGON_MODE_LINE) ? GL_LINE : GL_FILL;

            pipe->cullEnable = (rs->cullMode != VK_CULL_MODE_NONE);
            if (rs->cullMode == VK_CULL_MODE_BACK_BIT)
                pipe->cullMode = GL_BACK;
            else if (rs->cullMode == VK_CULL_MODE_FRONT_BIT)
                pipe->cullMode = GL_FRONT;
            else if (rs->cullMode == VK_CULL_MODE_FRONT_AND_BACK)
                pipe->cullMode = GL_FRONT_AND_BACK;

            // Vulkan: CCW = front face. OpenGL: same convention
            pipe->frontFace = (rs->frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE) ? GL_CCW : GL_CW;

            pipe->depthBiasEnable = rs->depthBiasEnable;
            pipe->depthBiasConstant = rs->depthBiasConstantFactor;
            pipe->depthBiasSlope = rs->depthBiasSlopeFactor;
            pipe->depthBiasClamp = rs->depthBiasClamp;
        }

        // === Depth State ===
        if (ci.pDepthStencilState) {
            auto* ds = ci.pDepthStencilState;
            pipe->depthTestEnable = ds->depthTestEnable;
            pipe->depthWriteEnable = ds->depthWriteEnable;
            pipe->depthFunc = vkCompareOpToGL(ds->depthCompareOp);
        }

        // === Blend State ===
        if (ci.pColorBlendState && ci.pColorBlendState->attachmentCount > 0) {
            auto& att = ci.pColorBlendState->pAttachments[0];
            pipe->blendEnable = att.blendEnable;
            pipe->colorWriteMask = att.colorWriteMask & 0xF;
            if (att.blendEnable) {
                pipe->srcBlend = vkBlendFactorToGL(att.srcColorBlendFactor);
                pipe->dstBlend = vkBlendFactorToGL(att.dstColorBlendFactor);
                pipe->blendOp = vkBlendOpToGL(att.colorBlendOp);
                pipe->srcBlendAlpha = vkBlendFactorToGL(att.srcAlphaBlendFactor);
                pipe->dstBlendAlpha = vkBlendFactorToGL(att.dstAlphaBlendFactor);
                pipe->blendOpAlpha = vkBlendOpToGL(att.alphaBlendOp);
            }
            if (ci.pColorBlendState->blendConstants) {
                memcpy(pipe->blendConstants, ci.pColorBlendState->blendConstants, 4 * sizeof(float));
            }
        }

        pPipelines[i] = TO_VK(VkPipeline, pipe);
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroyPipeline(
    VkDevice                     device,
    VkPipeline                   pipeline,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!pipeline) return;
    auto* pipe = AS_GL(Pipeline, pipeline);
    if (pipe->program) glDeleteProgram(pipe->program);
    if (pipe->vao) glDeleteVertexArrays(1, &pipe->vao);
    delete pipe;
}

// ============================================================================
// Render Pass & Framebuffer
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateRenderPass(
    VkDevice                       device,
    const VkRenderPassCreateInfo*  pCreateInfo,
    const VkAllocationCallbacks*   pAllocator,
    VkRenderPass*                  pRenderPass)
{
    (void)device; (void)pAllocator;
    auto* rp = new GL_RenderPass();
    if (pCreateInfo) {
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i)
            rp->attachments.push_back(pCreateInfo->pAttachments[i]);
        rp->colorAttachmentCount = 0;
        rp->hasDepth = false;
        if (pCreateInfo->subpassCount > 0 && pCreateInfo->pSubpasses) {
            rp->colorAttachmentCount = pCreateInfo->pSubpasses[0].colorAttachmentCount;
            rp->hasDepth = (pCreateInfo->pSubpasses[0].pDepthStencilAttachment != nullptr);
        }
    }
    *pRenderPass = TO_VK(VkRenderPass, rp);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroyRenderPass(
    VkDevice                     device,
    VkRenderPass                 renderPass,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_GL(RenderPass, renderPass);
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateFramebuffer(
    VkDevice                       device,
    const VkFramebufferCreateInfo* pCreateInfo,
    const VkAllocationCallbacks*   pAllocator,
    VkFramebuffer*                 pFramebuffer)
{
    (void)device; (void)pAllocator;
    auto* fb = new GL_Framebuffer();
    if (pCreateInfo) {
        fb->width = pCreateInfo->width;
        fb->height = pCreateInfo->height;
        fb->renderPass = pCreateInfo->renderPass;
        for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i)
            fb->attachments.push_back(pCreateInfo->pAttachments[i]);
    }

    // 检查是否所有 attachment 都是 swapchain image (ownsResource=false)
    bool isSwapchainFB = false;
    if (fb->attachments.size() > 0) {
        auto* firstView = AS_GL(ImageView, fb->attachments[0]);
        if (firstView) {
            auto* firstImg = AS_GL(Image, firstView->image);
            if (firstImg && !firstImg->ownsResource) {
                isSwapchainFB = true;
            }
        }
    }

    if (isSwapchainFB) {
        // Swapchain framebuffer → FBO 0 (default framebuffer)
        fb->fbo = 0;
    } else {
        // 创建新 FBO
        compat_CreateFramebuffers(1, &fb->fbo);

        uint32_t colorIdx = 0;
        for (uint32_t i = 0; i < fb->attachments.size(); i++) {
            auto* view = AS_GL(ImageView, fb->attachments[i]);
            if (!view) continue;
            auto* img = AS_GL(Image, view->image);
            if (!img || !img->texture) continue;

            auto fi = vkFormatToGL(view->format);
            if (fi.isDepth) {
                GLenum depthAttach = (view->format == VK_FORMAT_D24_UNORM_S8_UINT)
                    ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
                compat_FramebufferTexture(fb->fbo, depthAttach, img->texture, 0);
            } else {
                compat_FramebufferTexture(fb->fbo, GL_COLOR_ATTACHMENT0 + colorIdx, img->texture, 0);
                colorIdx++;
            }
        }

        // 设置 draw buffers
        if (colorIdx > 0) {
            GLenum drawBuffers[8];
            for (uint32_t c = 0; c < colorIdx; c++)
                drawBuffers[c] = GL_COLOR_ATTACHMENT0 + c;
            compat_FramebufferDrawBuffers(fb->fbo, colorIdx, drawBuffers);
        }

        // 验证 FBO 完整性
        GLenum status = compat_CheckFramebufferStatus(fb->fbo);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "[VkOpenGL] FBO %u incomplete: 0x%x\n", fb->fbo, status);
        }
    }

    *pFramebuffer = TO_VK(VkFramebuffer, fb);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroyFramebuffer(
    VkDevice                     device,
    VkFramebuffer                framebuffer,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    if (!framebuffer) return;
    auto* fb = AS_GL(Framebuffer, framebuffer);
    if (fb->fbo != 0) {
        glDeleteFramebuffers(1, &fb->fbo);
    }
    delete fb;
}

// ============================================================================
// Descriptor
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateDescriptorPool(
    VkDevice                          device,
    const VkDescriptorPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks*      pAllocator,
    VkDescriptorPool*                 pDescriptorPool)
{
    (void)pCreateInfo; (void)pAllocator;
    auto* pool = new GL_DescriptorPool();
    pool->device = device;
    *pDescriptorPool = TO_VK(VkDescriptorPool, pool);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroyDescriptorPool(
    VkDevice                     device,
    VkDescriptorPool             descriptorPool,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_GL(DescriptorPool, descriptorPool);
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateDescriptorSetLayout(
    VkDevice                                device,
    const VkDescriptorSetLayoutCreateInfo*  pCreateInfo,
    const VkAllocationCallbacks*            pAllocator,
    VkDescriptorSetLayout*                  pSetLayout)
{
    (void)device; (void)pAllocator;
    auto* layout = new GL_DescriptorSetLayout();
    if (pCreateInfo) {
        for (uint32_t i = 0; i < pCreateInfo->bindingCount; ++i)
            layout->bindings.push_back(pCreateInfo->pBindings[i]);
    }
    *pSetLayout = TO_VK(VkDescriptorSetLayout, layout);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroyDescriptorSetLayout(
    VkDevice                     device,
    VkDescriptorSetLayout        descriptorSetLayout,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_GL(DescriptorSetLayout, descriptorSetLayout);
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkAllocateDescriptorSets(
    VkDevice                           device,
    const VkDescriptorSetAllocateInfo* pAllocateInfo,
    VkDescriptorSet*                   pDescriptorSets)
{
    (void)device;
    if (pAllocateInfo && pDescriptorSets) {
        for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; ++i) {
            auto* ds = new GL_DescriptorSet();
            ds->layout = pAllocateInfo->pSetLayouts ? pAllocateInfo->pSetLayouts[i] : VK_NULL_HANDLE;
            pDescriptorSets[i] = TO_VK(VkDescriptorSet, ds);
        }
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkFreeDescriptorSets(
    VkDevice               device,
    VkDescriptorPool       descriptorPool,
    uint32_t               descriptorSetCount,
    const VkDescriptorSet* pDescriptorSets)
{
    (void)device; (void)descriptorPool;
    if (pDescriptorSets) {
        for (uint32_t i = 0; i < descriptorSetCount; ++i) {
            delete AS_GL(DescriptorSet, pDescriptorSets[i]);
        }
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkUpdateDescriptorSets(
    VkDevice                    device,
    uint32_t                    descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites,
    uint32_t                    descriptorCopyCount,
    const VkCopyDescriptorSet*  pDescriptorCopies)
{
    (void)device; (void)descriptorCopyCount; (void)pDescriptorCopies;
    if (!pDescriptorWrites) return;

    for (uint32_t i = 0; i < descriptorWriteCount; ++i) {
        const auto& write = pDescriptorWrites[i];
        auto* ds = AS_GL(DescriptorSet, write.dstSet);
        if (!ds) continue;

        if (write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            write.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
            if (write.pBufferInfo) {
                uint32_t slot = write.dstBinding;
                if (slot < 8) {
                    auto* buf = AS_GL(Buffer, write.pBufferInfo->buffer);
                    ds->uboBuffers[slot] = buf ? buf->buffer : 0;
                    ds->uboVkBuffers[slot] = write.pBufferInfo->buffer;
                    if (slot >= ds->uboCount)
                        ds->uboCount = slot + 1;
                }
            }
        } else if (write.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                   write.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
            if (write.pImageInfo) {
                for (uint32_t d = 0; d < write.descriptorCount; d++) {
                    uint32_t slot = write.dstBinding + d;
                    if (slot >= 8) break;
                    auto& imgInfo = write.pImageInfo[d];
                    auto* view = AS_GL(ImageView, imgInfo.imageView);
                    auto* img = view ? AS_GL(Image, view->image) : nullptr;
                    auto* samp = AS_GL(Sampler, imgInfo.sampler);

                    ds->textures[slot] = img ? img->texture : 0;
                    ds->samplers[slot] = samp ? samp->sampler : 0;
                    if (slot >= ds->textureCount)
                        ds->textureCount = slot + 1;
                }
            }
        }
    }
}

// ============================================================================
// Sync Objects
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateSemaphore(
    VkDevice                     device,
    const VkSemaphoreCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSemaphore*                 pSemaphore)
{
    (void)device; (void)pCreateInfo; (void)pAllocator;
    auto* sem = new GL_Semaphore();
    *pSemaphore = TO_VK(VkSemaphore, sem);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroySemaphore(
    VkDevice                     device,
    VkSemaphore                  semaphore,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_GL(Semaphore, semaphore);
}

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateFence(
    VkDevice                 device,
    const VkFenceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkFence*                 pFence)
{
    (void)device; (void)pAllocator;
    auto* fence = new GL_Fence();
    if (pCreateInfo && (pCreateInfo->flags & VK_FENCE_CREATE_SIGNALED_BIT))
        fence->signaled = true;
    *pFence = TO_VK(VkFence, fence);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroyFence(
    VkDevice                     device,
    VkFence                      fence,
    const VkAllocationCallbacks* pAllocator)
{
    (void)device; (void)pAllocator;
    delete AS_GL(Fence, fence);
}

// ============================================================================
// Debug
// ============================================================================

static VKAPI_ATTR VkResult VKAPI_CALL gl_vkCreateDebugUtilsMessengerEXT(
    VkInstance                                  instance,
    const VkDebugUtilsMessengerCreateInfoEXT*   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDebugUtilsMessengerEXT*                   pMessenger)
{
    (void)instance; (void)pAllocator;
    auto* dbg = new GL_DebugMessenger();
    if (pCreateInfo) {
        dbg->callback = pCreateInfo->pfnUserCallback;
        dbg->userData = pCreateInfo->pUserData;
    }
    *pMessenger = TO_VK(VkDebugUtilsMessengerEXT, dbg);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL gl_vkDestroyDebugUtilsMessengerEXT(
    VkInstance                   instance,
    VkDebugUtilsMessengerEXT     messenger,
    const VkAllocationCallbacks* pAllocator)
{
    (void)instance; (void)pAllocator;
    delete AS_GL(DebugMessenger, messenger);
}

// ============================================================================
// vkLoadOpenGLDispatch — 注册所有 gl_vkXxx 到全局函数指针
// ============================================================================

void vkLoadOpenGLDispatch()
{
    #define VK_GL(fn) fn = gl_##fn

    // Instance
    VK_GL(vkCreateInstance);
    VK_GL(vkDestroyInstance);
    VK_GL(vkEnumerateInstanceExtensionProperties);
    VK_GL(vkEnumerateInstanceLayerProperties);
    VK_GL(vkGetInstanceProcAddr);

    // Physical Device
    VK_GL(vkEnumeratePhysicalDevices);
    VK_GL(vkGetPhysicalDeviceProperties);
    VK_GL(vkGetPhysicalDeviceFeatures);
    VK_GL(vkGetPhysicalDeviceFeatures2);
    VK_GL(vkGetPhysicalDeviceMemoryProperties);
    VK_GL(vkGetPhysicalDeviceQueueFamilyProperties);
    VK_GL(vkEnumerateDeviceExtensionProperties);

    // Device & Queue
    VK_GL(vkCreateDevice);
    VK_GL(vkDestroyDevice);
    VK_GL(vkGetDeviceQueue);
    VK_GL(vkDeviceWaitIdle);

    // Surface
    VK_GL(vkDestroySurfaceKHR);
    VK_GL(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    VK_GL(vkGetPhysicalDeviceSurfaceFormatsKHR);
    VK_GL(vkGetPhysicalDeviceSurfacePresentModesKHR);
    VK_GL(vkGetPhysicalDeviceSurfaceSupportKHR);
#ifdef VK_USE_PLATFORM_WIN32_KHR
    VK_GL(vkCreateWin32SurfaceKHR);
#endif

    // Swapchain
    VK_GL(vkCreateSwapchainKHR);
    VK_GL(vkDestroySwapchainKHR);
    VK_GL(vkGetSwapchainImagesKHR);
    VK_GL(vkAcquireNextImageKHR);
    VK_GL(vkQueuePresentKHR);

    // Buffer
    VK_GL(vkCreateBuffer);
    VK_GL(vkDestroyBuffer);
    VK_GL(vkGetBufferMemoryRequirements);

    // Memory
    VK_GL(vkAllocateMemory);
    VK_GL(vkFreeMemory);
    VK_GL(vkBindBufferMemory);
    VK_GL(vkMapMemory);
    VK_GL(vkUnmapMemory);
    VK_GL(vkFlushMappedMemoryRanges);

    // Image
    VK_GL(vkCreateImage);
    VK_GL(vkDestroyImage);
    VK_GL(vkGetImageMemoryRequirements);
    VK_GL(vkBindImageMemory);
    VK_GL(vkCreateImageView);
    VK_GL(vkDestroyImageView);

    // Sampler
    VK_GL(vkCreateSampler);
    VK_GL(vkDestroySampler);

    // Command Pool & Buffer
    VK_GL(vkCreateCommandPool);
    VK_GL(vkDestroyCommandPool);
    VK_GL(vkResetCommandPool);
    VK_GL(vkAllocateCommandBuffers);
    VK_GL(vkFreeCommandBuffers);
    VK_GL(vkBeginCommandBuffer);
    VK_GL(vkEndCommandBuffer);
    VK_GL(vkResetCommandBuffer);

    // Command Recording
    VK_GL(vkCmdBeginRenderPass);
    VK_GL(vkCmdEndRenderPass);
    VK_GL(vkCmdBindPipeline);
    VK_GL(vkCmdBindDescriptorSets);
    VK_GL(vkCmdBindVertexBuffers);
    VK_GL(vkCmdBindIndexBuffer);
    VK_GL(vkCmdDraw);
    VK_GL(vkCmdDrawIndexed);
    VK_GL(vkCmdSetViewport);
    VK_GL(vkCmdSetScissor);
    VK_GL(vkCmdPushConstants);
    VK_GL(vkCmdCopyBuffer);
    VK_GL(vkCmdCopyBufferToImage);
    VK_GL(vkCmdCopyImageToBuffer);
    VK_GL(vkCmdPipelineBarrier);
    VK_GL(vkCmdBlitImage);

    // Synchronization
    VK_GL(vkWaitForFences);
    VK_GL(vkResetFences);
    VK_GL(vkQueueSubmit);
    VK_GL(vkQueueWaitIdle);

    // Pipeline
    VK_GL(vkCreatePipelineLayout);
    VK_GL(vkDestroyPipelineLayout);
    VK_GL(vkCreateShaderModule);
    VK_GL(vkDestroyShaderModule);
    VK_GL(vkCreateGraphicsPipelines);
    VK_GL(vkDestroyPipeline);

    // Render Pass & Framebuffer
    VK_GL(vkCreateRenderPass);
    VK_GL(vkDestroyRenderPass);
    VK_GL(vkCreateFramebuffer);
    VK_GL(vkDestroyFramebuffer);

    // Descriptor
    VK_GL(vkCreateDescriptorPool);
    VK_GL(vkDestroyDescriptorPool);
    VK_GL(vkCreateDescriptorSetLayout);
    VK_GL(vkDestroyDescriptorSetLayout);
    VK_GL(vkAllocateDescriptorSets);
    VK_GL(vkFreeDescriptorSets);
    VK_GL(vkUpdateDescriptorSets);

    // Sync Objects
    VK_GL(vkCreateSemaphore);
    VK_GL(vkDestroySemaphore);
    VK_GL(vkCreateFence);
    VK_GL(vkDestroyFence);

    // Debug
    VK_GL(vkCreateDebugUtilsMessengerEXT);
    VK_GL(vkDestroyDebugUtilsMessengerEXT);

    #undef VK_GL
}
