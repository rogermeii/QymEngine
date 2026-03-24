#include "Window.h"
#include <stdexcept>

namespace QymEngine {

Window::Window(const WindowProps& props)
    : m_width(props.width), m_height(props.height), m_backend(props.backend)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());

    Uint32 flags = SDL_WINDOW_RESIZABLE;
    if (m_backend == RenderBackend::OpenGL) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, 1);
        flags |= SDL_WINDOW_OPENGL;
    } else if (m_backend == RenderBackend::GLES) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE, 1);
        flags |= SDL_WINDOW_OPENGL;
    } else if (m_backend == RenderBackend::Vulkan) {
        flags |= SDL_WINDOW_VULKAN;
    }
    if (props.maximized)
        flags |= SDL_WINDOW_MAXIMIZED;

    m_window = SDL_CreateWindow(
        props.title.c_str(),
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        m_width, m_height, flags);

    if (!m_window)
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());

    // Update actual size when maximized
    if (props.maximized) {
        int w, h;
        if (m_backend == RenderBackend::Vulkan)
            SDL_Vulkan_GetDrawableSize(m_window, &w, &h);
        else // OpenGL, GLES, D3D11, D3D12
            SDL_GL_GetDrawableSize(m_window, &w, &h);
        m_width = static_cast<uint32_t>(w);
        m_height = static_cast<uint32_t>(h);
    }
}

Window::~Window()
{
    SDL_DestroyWindow(m_window);
    SDL_Quit();
}

void Window::pollEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Forward to ImGui and other listeners
        if (m_eventCallback)
            m_eventCallback(event);

        switch (event.type) {
        case SDL_QUIT:
            m_shouldClose = true;
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                m_width = static_cast<uint32_t>(event.window.data1);
                m_height = static_cast<uint32_t>(event.window.data2);
                if (m_resizeCallback)
                    m_resizeCallback(event.window.data1, event.window.data2);
            }
            break;
        }
    }
}

bool Window::shouldClose() const
{
    return m_shouldClose;
}

void Window::requestClose()
{
    m_shouldClose = true;
}

} // namespace QymEngine
