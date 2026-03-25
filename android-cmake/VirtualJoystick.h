#pragma once
#include <SDL.h>
#include <imgui.h>
#include <glm/glm.hpp>
#include <cmath>

namespace QymEngine {

// A virtual touch joystick drawn with ImGui, using SDL touch events for multi-touch
class VirtualJoystick {
public:
    void setPosition(float cx, float cy, float radius) {
        m_cx = cx;
        m_cy = cy;
        m_radius = radius;
    }

    // Process SDL touch event. Returns true if this joystick consumed the event.
    bool processEvent(const SDL_Event& event) {
        float tx, ty;
        SDL_FingerID fingerId;

        if (event.type == SDL_FINGERDOWN) {
            // Convert normalized coords to screen pixels
            tx = event.tfinger.x * m_screenW;
            ty = event.tfinger.y * m_screenH;
            fingerId = event.tfinger.fingerId;

            float dx = tx - m_cx;
            float dy = ty - m_cy;
            float dist = std::sqrt(dx * dx + dy * dy);
            if (dist < m_radius * 1.5f && m_fingerId == -1) {
                m_fingerId = fingerId;
                updateDir(tx, ty);
                return true;
            }
        }
        else if (event.type == SDL_FINGERMOTION) {
            fingerId = event.tfinger.fingerId;
            if (fingerId == m_fingerId) {
                tx = event.tfinger.x * m_screenW;
                ty = event.tfinger.y * m_screenH;
                updateDir(tx, ty);
                return true;
            }
        }
        else if (event.type == SDL_FINGERUP) {
            fingerId = event.tfinger.fingerId;
            if (fingerId == m_fingerId) {
                m_fingerId = -1;
                m_dir = {0.0f, 0.0f};
                m_handleOffset = {0.0f, 0.0f};
                return true;
            }
        }
        return false;
    }

    void setScreenSize(float w, float h) {
        m_screenW = w;
        m_screenH = h;
    }

    // Draw the joystick using ImGui foreground draw list
    void draw() const {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        ImVec2 center(m_cx, m_cy);

        // Base
        dl->AddCircleFilled(center, m_radius, IM_COL32(60, 60, 60, 80), 32);
        dl->AddCircle(center, m_radius, IM_COL32(150, 150, 150, 120), 32, 2.0f);

        // Handle
        ImVec2 handlePos(m_cx + m_handleOffset.x, m_cy + m_handleOffset.y);
        float handleR = m_radius * 0.35f;
        bool active = (m_fingerId != -1);
        dl->AddCircleFilled(handlePos, handleR,
            active ? IM_COL32(255, 255, 255, 200) : IM_COL32(180, 180, 180, 120), 24);
    }

    // Normalized direction (-1 to +1)
    glm::vec2 getDirection() const { return m_dir; }
    bool isActive() const { return m_fingerId != -1; }

private:
    void updateDir(float tx, float ty) {
        float dx = tx - m_cx;
        float dy = ty - m_cy;
        float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > m_radius) {
            dx = dx / dist * m_radius;
            dy = dy / dist * m_radius;
        }
        m_handleOffset = {dx, dy};
        m_dir = {dx / m_radius, dy / m_radius};
    }

    float m_cx = 0, m_cy = 0, m_radius = 80.0f;
    float m_screenW = 1920, m_screenH = 1080;
    SDL_FingerID m_fingerId = -1;
    glm::vec2 m_dir{0.0f, 0.0f};
    glm::vec2 m_handleOffset{0.0f, 0.0f};
};

} // namespace QymEngine
