#include "UIAutomation.h"
#include "renderer/Renderer.h"
#include "scene/Scene.h"
#include "scene/Camera.h"
#include "core/Log.h"

#include <imgui.h>
#include <json.hpp>
#include <SDL.h>

#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstring>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace QymEngine {

// Static members
std::vector<WidgetRect> UIAutomation::s_widgets;
std::map<std::string, UIAutomation::PanelRect> UIAutomation::s_panels;

void UIAutomation::beginFrame() {
    s_widgets.clear();
    s_panels.clear();
}

void UIAutomation::recordWidget(const std::string& id, const std::string& label) {
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    s_widgets.push_back({id, label, min.x, min.y, max.x - min.x, max.y - min.y});
}

void UIAutomation::recordPanel(const std::string& name) {
    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();
    s_panels[name] = {pos.x, pos.y, size.x, size.y};
}

void UIAutomation::pollAndExecute(Renderer& renderer, Scene& scene, Camera& camera, SDL_Window* window) {
    if (!m_initialized) {
        m_commandPath = std::string(ASSETS_DIR) + "/../captures/command.json";
        m_resultPath = std::string(ASSETS_DIR) + "/../captures/command_result.json";
        m_initialized = true;
    }

    // Check if command file exists
    std::ifstream file(m_commandPath);
    if (!file.good())
        return;

    // Read command
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();

    // Delete command file immediately to avoid re-processing
    std::remove(m_commandPath.c_str());

    if (content.empty())
        return;

    executeCommand(content, renderer, scene, camera, window);
}

void UIAutomation::writeResult(const std::string& json) {
    // Ensure captures directory exists
    std::string dir = std::string(ASSETS_DIR) + "/../captures";
    std::filesystem::create_directories(dir);

    std::ofstream out(m_resultPath);
    if (out.is_open()) {
        out << json;
        out.close();
    }
}

void UIAutomation::executeCommand(const std::string& jsonStr, Renderer& renderer, Scene& scene, Camera& camera, SDL_Window* window) {
    try {
        nlohmann::json j = nlohmann::json::parse(jsonStr);
        std::string command = j.value("command", "");
        nlohmann::json params = j.value("params", nlohmann::json::object());

        if (command == "mouse_click") {
            int x = params.value("x", 0);
            int y = params.value("y", 0);
            std::string buttonStr = params.value("button", "left");
            int button = (buttonStr == "right") ? 1 : 0;
            injectMouseClick(x, y, button, window);
            writeResult("{\"status\":\"ok\",\"command\":\"mouse_click\"}");

        } else if (command == "mouse_double_click") {
            int x = params.value("x", 0);
            int y = params.value("y", 0);
            injectMouseDoubleClick(x, y, window);
            writeResult("{\"status\":\"ok\",\"command\":\"mouse_double_click\"}");

        } else if (command == "mouse_drag") {
            auto from = params.value("from", std::vector<int>{0, 0});
            auto to = params.value("to", std::vector<int>{0, 0});
            std::string buttonStr = params.value("button", "left");
            int button = (buttonStr == "right") ? 1 : 0;
            injectMouseDrag(from[0], from[1], to[0], to[1], button, window);
            writeResult("{\"status\":\"ok\",\"command\":\"mouse_drag\"}");

        } else if (command == "mouse_scroll") {
            int x = params.value("x", 0);
            int y = params.value("y", 0);
            int delta = params.value("delta", 0);
            injectMouseScroll(x, y, delta, window);
            writeResult("{\"status\":\"ok\",\"command\":\"mouse_scroll\"}");

        } else if (command == "key_press") {
            std::string key = params.value("key", "");
            bool ctrl = params.value("ctrl", false);
            bool shift = params.value("shift", false);
            bool alt = params.value("alt", false);
            injectKeyPress(key, ctrl, shift, alt, window);
            writeResult("{\"status\":\"ok\",\"command\":\"key_press\"}");

        } else if (command == "key_combo") {
            std::string key = params.value("key", "");
            bool ctrl = params.value("ctrl", false);
            bool shift = params.value("shift", false);
            bool alt = params.value("alt", false);
            injectKeyPress(key, ctrl, shift, alt, window);
            writeResult("{\"status\":\"ok\",\"command\":\"key_combo\"}");

        } else if (command == "text_input") {
            std::string text = params.value("text", "");
            injectTextInput(text, window);
            writeResult("{\"status\":\"ok\",\"command\":\"text_input\"}");

        } else if (command == "get_ui_layout") {
            nlohmann::json result;
            result["status"] = "ok";
            result["command"] = "get_ui_layout";

            // Panels
            nlohmann::json panelsJson;
            for (auto& [name, rect] : s_panels) {
                panelsJson[name] = {
                    {"x", rect.x}, {"y", rect.y},
                    {"w", rect.w}, {"h", rect.h}
                };
            }
            result["panels"] = panelsJson;

            // Widgets
            nlohmann::json widgetsJson = nlohmann::json::array();
            for (auto& w : s_widgets) {
                widgetsJson.push_back({
                    {"id", w.id}, {"label", w.label},
                    {"x", w.x}, {"y", w.y},
                    {"w", w.w}, {"h", w.h}
                });
            }
            result["widgets"] = widgetsJson;

            writeResult(result.dump(2));

        } else if (command == "get_scene_info") {
            nlohmann::json result;
            result["status"] = "ok";
            result["command"] = "get_scene_info";
            result["scene_name"] = scene.name;

            nlohmann::json nodesJson = nlohmann::json::array();
            scene.traverseNodes([&](Node* node) {
                nlohmann::json nj;
                nj["name"] = node->name;
                nj["type"] = (node->nodeType == NodeType::DirectionalLight) ? "DirectionalLight" : "Mesh";
                nj["position"] = {
                    node->transform.position.x,
                    node->transform.position.y,
                    node->transform.position.z
                };
                nj["rotation"] = {
                    node->transform.rotation.x,
                    node->transform.rotation.y,
                    node->transform.rotation.z
                };
                nj["scale"] = {
                    node->transform.scale.x,
                    node->transform.scale.y,
                    node->transform.scale.z
                };

                if (node->nodeType == NodeType::Mesh) {
                    if (!node->meshPath.empty())
                        nj["mesh"] = node->meshPath;
                    else {
                        const char* meshNames[] = {"None", "Quad", "Cube", "Plane", "Sphere"};
                        int idx = static_cast<int>(node->meshType);
                        nj["mesh"] = (idx >= 0 && idx < 5) ? meshNames[idx] : "Unknown";
                    }
                    if (!node->materialPath.empty())
                        nj["material"] = node->materialPath;
                }

                bool isSelected = scene.isNodeSelected(node);
                nj["selected"] = isSelected;

                nodesJson.push_back(nj);
            });
            result["nodes"] = nodesJson;
            writeResult(result.dump(2));

        } else if (command == "get_draw_stats") {
            nlohmann::json result;
            result["status"] = "ok";
            result["command"] = "get_draw_stats";
            result["draw_calls"] = renderer.getLastDrawCallCount();
            result["triangles"] = renderer.getLastTriangleCount();
            writeResult(result.dump(2));

        } else if (command == "capture_frame") {
            // Create the trigger file for RenderDoc capture
            std::string triggerPath = std::string(ASSETS_DIR) + "/../captures/trigger";
            std::filesystem::create_directories(std::string(ASSETS_DIR) + "/../captures");
            std::ofstream trigger(triggerPath);
            trigger.close();
            writeResult("{\"status\":\"ok\",\"command\":\"capture_frame\"}");

        } else if (command == "screenshot") {
            std::string path = params.value("path", "captures/screenshot.png");
            // If relative, make it relative to project root
            if (path.find(':') == std::string::npos && path[0] != '/') {
                path = std::string(ASSETS_DIR) + "/../" + path;
            }
            // Ensure directory exists
            std::filesystem::create_directories(std::filesystem::path(path).parent_path());

            if (saveScreenshot(renderer, path)) {
                writeResult("{\"status\":\"ok\",\"command\":\"screenshot\",\"path\":\"" + path + "\"}");
            } else {
                writeResult("{\"status\":\"error\",\"command\":\"screenshot\",\"message\":\"Failed to save screenshot\"}");
            }

        } else {
            writeResult("{\"status\":\"error\",\"message\":\"Unknown command: " + command + "\"}");
        }

    } catch (const std::exception& e) {
        writeResult(std::string("{\"status\":\"error\",\"message\":\"") + e.what() + "\"}");
    }
}

// ========================================================================
// Input injection via SDL_PushEvent
// ========================================================================

void UIAutomation::injectMouseClick(int x, int y, int button, SDL_Window* window) {
    uint32_t sdlButton = (button == 1) ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
    uint32_t windowID = SDL_GetWindowID(window);

    // Mouse move to position
    SDL_Event moveEvent{};
    moveEvent.type = SDL_MOUSEMOTION;
    moveEvent.motion.windowID = windowID;
    moveEvent.motion.x = x;
    moveEvent.motion.y = y;
    SDL_PushEvent(&moveEvent);

    // Mouse down
    SDL_Event downEvent{};
    downEvent.type = SDL_MOUSEBUTTONDOWN;
    downEvent.button.windowID = windowID;
    downEvent.button.button = static_cast<uint8_t>(sdlButton);
    downEvent.button.clicks = 1;
    downEvent.button.x = x;
    downEvent.button.y = y;
    SDL_PushEvent(&downEvent);

    // Mouse up
    SDL_Event upEvent{};
    upEvent.type = SDL_MOUSEBUTTONUP;
    upEvent.button.windowID = windowID;
    upEvent.button.button = static_cast<uint8_t>(sdlButton);
    upEvent.button.clicks = 1;
    upEvent.button.x = x;
    upEvent.button.y = y;
    SDL_PushEvent(&upEvent);
}

void UIAutomation::injectMouseDoubleClick(int x, int y, SDL_Window* window) {
    uint32_t windowID = SDL_GetWindowID(window);

    // Move to position
    SDL_Event moveEvent{};
    moveEvent.type = SDL_MOUSEMOTION;
    moveEvent.motion.windowID = windowID;
    moveEvent.motion.x = x;
    moveEvent.motion.y = y;
    SDL_PushEvent(&moveEvent);

    // First click
    SDL_Event down1{};
    down1.type = SDL_MOUSEBUTTONDOWN;
    down1.button.windowID = windowID;
    down1.button.button = SDL_BUTTON_LEFT;
    down1.button.clicks = 1;
    down1.button.x = x;
    down1.button.y = y;
    SDL_PushEvent(&down1);

    SDL_Event up1{};
    up1.type = SDL_MOUSEBUTTONUP;
    up1.button.windowID = windowID;
    up1.button.button = SDL_BUTTON_LEFT;
    up1.button.clicks = 1;
    up1.button.x = x;
    up1.button.y = y;
    SDL_PushEvent(&up1);

    // Second click (double-click)
    SDL_Event down2{};
    down2.type = SDL_MOUSEBUTTONDOWN;
    down2.button.windowID = windowID;
    down2.button.button = SDL_BUTTON_LEFT;
    down2.button.clicks = 2;
    down2.button.x = x;
    down2.button.y = y;
    SDL_PushEvent(&down2);

    SDL_Event up2{};
    up2.type = SDL_MOUSEBUTTONUP;
    up2.button.windowID = windowID;
    up2.button.button = SDL_BUTTON_LEFT;
    up2.button.clicks = 2;
    up2.button.x = x;
    up2.button.y = y;
    SDL_PushEvent(&up2);
}

void UIAutomation::injectMouseDrag(int fromX, int fromY, int toX, int toY, int button, SDL_Window* window) {
    uint32_t sdlButton = (button == 1) ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT;
    uint32_t windowID = SDL_GetWindowID(window);

    // Move to start
    SDL_Event move{};
    move.type = SDL_MOUSEMOTION;
    move.motion.windowID = windowID;
    move.motion.x = fromX;
    move.motion.y = fromY;
    SDL_PushEvent(&move);

    // Press
    SDL_Event down{};
    down.type = SDL_MOUSEBUTTONDOWN;
    down.button.windowID = windowID;
    down.button.button = static_cast<uint8_t>(sdlButton);
    down.button.x = fromX;
    down.button.y = fromY;
    SDL_PushEvent(&down);

    // Move to end (with button state)
    SDL_Event drag{};
    drag.type = SDL_MOUSEMOTION;
    drag.motion.windowID = windowID;
    drag.motion.x = toX;
    drag.motion.y = toY;
    drag.motion.state = SDL_BUTTON(sdlButton);
    SDL_PushEvent(&drag);

    // Release
    SDL_Event up{};
    up.type = SDL_MOUSEBUTTONUP;
    up.button.windowID = windowID;
    up.button.button = static_cast<uint8_t>(sdlButton);
    up.button.x = toX;
    up.button.y = toY;
    SDL_PushEvent(&up);
}

void UIAutomation::injectMouseScroll(int x, int y, int delta, SDL_Window* window) {
    uint32_t windowID = SDL_GetWindowID(window);

    // Move to position first
    SDL_Event move{};
    move.type = SDL_MOUSEMOTION;
    move.motion.windowID = windowID;
    move.motion.x = x;
    move.motion.y = y;
    SDL_PushEvent(&move);

    // Scroll event
    SDL_Event scroll{};
    scroll.type = SDL_MOUSEWHEEL;
    scroll.wheel.windowID = windowID;
    scroll.wheel.x = 0;
    scroll.wheel.y = delta;
    scroll.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    SDL_PushEvent(&scroll);
}

void UIAutomation::injectKeyPress(const std::string& key, bool ctrl, bool shift, bool alt, SDL_Window* window) {
    SDL_Keycode keycode = SDLK_UNKNOWN;

    // Map common key names to SDL keycodes
    if (key == "F12") keycode = SDLK_F12;
    else if (key == "F11") keycode = SDLK_F11;
    else if (key == "F10") keycode = SDLK_F10;
    else if (key == "F9") keycode = SDLK_F9;
    else if (key == "F8") keycode = SDLK_F8;
    else if (key == "F7") keycode = SDLK_F7;
    else if (key == "F6") keycode = SDLK_F6;
    else if (key == "F5") keycode = SDLK_F5;
    else if (key == "F4") keycode = SDLK_F4;
    else if (key == "F3") keycode = SDLK_F3;
    else if (key == "F2") keycode = SDLK_F2;
    else if (key == "F1") keycode = SDLK_F1;
    else if (key == "Return" || key == "Enter") keycode = SDLK_RETURN;
    else if (key == "Escape" || key == "Esc") keycode = SDLK_ESCAPE;
    else if (key == "Delete" || key == "Del") keycode = SDLK_DELETE;
    else if (key == "Tab") keycode = SDLK_TAB;
    else if (key == "Backspace") keycode = SDLK_BACKSPACE;
    else if (key == "Space") keycode = SDLK_SPACE;
    else if (key == "Up") keycode = SDLK_UP;
    else if (key == "Down") keycode = SDLK_DOWN;
    else if (key == "Left") keycode = SDLK_LEFT;
    else if (key == "Right") keycode = SDLK_RIGHT;
    else if (key == "Home") keycode = SDLK_HOME;
    else if (key == "End") keycode = SDLK_END;
    else if (key == "PageUp") keycode = SDLK_PAGEUP;
    else if (key == "PageDown") keycode = SDLK_PAGEDOWN;
    else if (key == "Insert") keycode = SDLK_INSERT;
    else if (key.length() == 1) {
        char c = key[0];
        // For letters, SDL keycodes are lowercase ASCII
        if (c >= 'A' && c <= 'Z')
            keycode = static_cast<SDL_Keycode>(c + 32); // to lowercase
        else
            keycode = static_cast<SDL_Keycode>(c);
    }

    SDL_Scancode scancode = SDL_GetScancodeFromKey(keycode);
    uint32_t windowID = SDL_GetWindowID(window);

    // Set modifier state
    uint16_t mod = 0;
    if (ctrl) mod |= KMOD_CTRL;
    if (shift) mod |= KMOD_SHIFT;
    if (alt) mod |= KMOD_ALT;

    // Key down
    SDL_Event down{};
    down.type = SDL_KEYDOWN;
    down.key.windowID = windowID;
    down.key.keysym.sym = keycode;
    down.key.keysym.scancode = scancode;
    down.key.keysym.mod = mod;
    down.key.state = SDL_PRESSED;
    SDL_PushEvent(&down);

    // Key up
    SDL_Event up{};
    up.type = SDL_KEYUP;
    up.key.windowID = windowID;
    up.key.keysym.sym = keycode;
    up.key.keysym.scancode = scancode;
    up.key.keysym.mod = mod;
    up.key.state = SDL_RELEASED;
    SDL_PushEvent(&up);
}

void UIAutomation::injectTextInput(const std::string& text, SDL_Window* window) {
    // SDL text input events can carry up to 32 bytes per event
    // Send one event per character for simplicity
    for (size_t i = 0; i < text.size(); ) {
        SDL_Event event{};
        event.type = SDL_TEXTINPUT;
        event.text.windowID = SDL_GetWindowID(window);

        // Copy up to 31 chars (leaving room for null terminator)
        size_t len = std::min(text.size() - i, (size_t)31);
        std::memcpy(event.text.text, text.c_str() + i, len);
        event.text.text[len] = '\0';

        SDL_PushEvent(&event);
        i += len;
    }
}

// ========================================================================
// Screenshot - read back offscreen image and save as PNG
// ========================================================================

bool UIAutomation::saveScreenshot(Renderer& renderer, const std::string& path) {
    if (!renderer.isOffscreenReady())
        return false;

    VkDevice device = renderer.getContext().getDevice();
    VkPhysicalDevice physDevice = renderer.getContext().getPhysicalDevice();
    uint32_t width = renderer.getOffscreenWidth();
    uint32_t height = renderer.getOffscreenHeight();

    // Wait for all GPU operations to complete
    vkDeviceWaitIdle(device);

    // Create staging buffer
    VkDeviceSize imageSize = width * height * 4;

    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, stagingBuffer, &memReqs);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = renderer.getContext().findMemoryType(
        memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    if (vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory) != VK_SUCCESS) {
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        return false;
    }

    vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

    // Use single-time command buffer to copy image to staging buffer
    VkCommandBuffer cmd = renderer.getCommandManager().beginSingleTimeCommands(device);

    // Get offscreen image handle via Renderer internals
    // The offscreen image is currently in SHADER_READ_ONLY layout after rendering
    // Transition: SHADER_READ_ONLY -> TRANSFER_SRC
    VkImageMemoryBarrier toTransferSrc{};
    toTransferSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransferSrc.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toTransferSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toTransferSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferSrc.image = renderer.getOffscreenImage();
    toTransferSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toTransferSrc.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toTransferSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransferSrc);

    // Copy image to buffer
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyImageToBuffer(cmd, renderer.getOffscreenImage(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           stagingBuffer, 1, &region);

    // Transition back: TRANSFER_SRC -> SHADER_READ_ONLY
    VkImageMemoryBarrier toShaderRead{};
    toShaderRead.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toShaderRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toShaderRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toShaderRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toShaderRead.image = renderer.getOffscreenImage();
    toShaderRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toShaderRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toShaderRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toShaderRead);

    renderer.getCommandManager().endSingleTimeCommands(
        device, renderer.getContext().getGraphicsQueue(), cmd);

    // Map staging buffer and read pixels
    void* data;
    vkMapMemory(device, stagingMemory, 0, imageSize, 0, &data);

    // Format is B8G8R8A8_SRGB, need to swizzle B and R for PNG (RGBA)
    std::vector<uint8_t> pixels(width * height * 4);
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (uint32_t i = 0; i < width * height; i++) {
        pixels[i * 4 + 0] = src[i * 4 + 2]; // R <- B
        pixels[i * 4 + 1] = src[i * 4 + 1]; // G <- G
        pixels[i * 4 + 2] = src[i * 4 + 0]; // B <- R
        pixels[i * 4 + 3] = src[i * 4 + 3]; // A <- A
    }

    vkUnmapMemory(device, stagingMemory);

    // Save as PNG
    int result = stbi_write_png(path.c_str(), width, height, 4,
                                pixels.data(), width * 4);

    // Cleanup
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);

    if (result) {
        Log::info("Screenshot saved: " + path);
    } else {
        Log::error("Failed to save screenshot: " + path);
    }

    return result != 0;
}

} // namespace QymEngine
