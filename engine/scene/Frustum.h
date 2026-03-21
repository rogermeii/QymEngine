#pragma once
#include <glm/glm.hpp>
#include <array>

namespace QymEngine {

struct AABB {
    glm::vec3 min = glm::vec3(0.0f);
    glm::vec3 max = glm::vec3(0.0f);
};

class Frustum {
public:
    // Extract 6 frustum planes from view-projection matrix (column-major GLM matrix)
    void update(const glm::mat4& viewProj);

    // Test if an AABB intersects the frustum (returns true if visible)
    bool isVisible(const AABB& aabb) const;

    // Test a world-space AABB (local AABB transformed by model matrix)
    bool isVisible(const AABB& localAABB, const glm::mat4& worldMatrix) const;

private:
    // Plane equation: ax + by + cz + d = 0, normal = (a,b,c), stored as vec4(a,b,c,d)
    std::array<glm::vec4, 6> m_planes; // left, right, bottom, top, near, far
};

} // namespace QymEngine
