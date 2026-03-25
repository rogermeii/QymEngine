#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace QymEngine {

struct Transform {
    glm::vec3 position = {0.0f, 0.0f, 0.0f};
    glm::vec3 rotation = {0.0f, 0.0f, 0.0f};  // Euler angles in degrees
    glm::vec3 scale    = {1.0f, 1.0f, 1.0f};

    glm::mat4 getLocalMatrix() const {
        glm::mat4 mat(1.0f);
        mat = glm::translate(mat, position);
        mat = glm::rotate(mat, glm::radians(rotation.x), glm::vec3(1, 0, 0));
        mat = glm::rotate(mat, glm::radians(rotation.y), glm::vec3(0, 1, 0));
        mat = glm::rotate(mat, glm::radians(rotation.z), glm::vec3(0, 0, 1));
        mat = glm::scale(mat, scale);
        return mat;
    }
};

} // namespace QymEngine
