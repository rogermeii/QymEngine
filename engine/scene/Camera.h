#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace QymEngine {

class Camera {
public:
    glm::vec3 target = {0.0f, 0.0f, 0.0f};
    float distance = 5.0f;
    float yaw = -45.0f;
    float pitch = 30.0f;
    float fov = 45.0f;
    float nearPlane = 0.1f;
    float farPlane = 100.0f;

    glm::vec3 getPosition() const {
        float yawRad = glm::radians(yaw);
        float pitchRad = glm::radians(pitch);
        glm::vec3 offset = {
            distance * cos(pitchRad) * cos(yawRad),
            distance * sin(pitchRad),
            distance * cos(pitchRad) * sin(yawRad)
        };
        return target + offset;
    }

    glm::mat4 getViewMatrix() const {
        return glm::lookAt(getPosition(), target, glm::vec3(0.0f, 1.0f, 0.0f));
    }

    glm::mat4 getProjMatrix(float aspect) const {
        glm::mat4 proj = glm::perspective(glm::radians(fov), aspect, nearPlane, farPlane);
        proj[1][1] *= -1;
        return proj;
    }

    void orbit(float deltaYaw, float deltaPitch) {
        yaw += deltaYaw;
        pitch = std::clamp(pitch + deltaPitch, -89.0f, 89.0f);
    }

    void pan(float deltaX, float deltaY) {
        glm::vec3 forward = glm::normalize(target - getPosition());
        glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
        glm::vec3 up = glm::normalize(glm::cross(right, forward));
        target += right * deltaX * distance + up * deltaY * distance;
    }

    void zoom(float delta) {
        distance = std::clamp(distance + delta, 0.1f, 100.0f);
    }
};

} // namespace QymEngine
