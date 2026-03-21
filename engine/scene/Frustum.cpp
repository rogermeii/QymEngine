#include "scene/Frustum.h"
#include <cmath>
#include <algorithm>

namespace QymEngine {

void Frustum::update(const glm::mat4& vp)
{
    // Gribb/Hartmann method: extract frustum planes from VP matrix
    // GLM is column-major: vp[col][row]
    // row0 = (vp[0][0], vp[1][0], vp[2][0], vp[3][0])
    // row1 = (vp[0][1], vp[1][1], vp[2][1], vp[3][1])
    // row2 = (vp[0][2], vp[1][2], vp[2][2], vp[3][2])
    // row3 = (vp[0][3], vp[1][3], vp[2][3], vp[3][3])

    // Left:   row3 + row0
    m_planes[0] = glm::vec4(
        vp[0][3] + vp[0][0],
        vp[1][3] + vp[1][0],
        vp[2][3] + vp[2][0],
        vp[3][3] + vp[3][0]);

    // Right:  row3 - row0
    m_planes[1] = glm::vec4(
        vp[0][3] - vp[0][0],
        vp[1][3] - vp[1][0],
        vp[2][3] - vp[2][0],
        vp[3][3] - vp[3][0]);

    // Bottom: row3 + row1
    m_planes[2] = glm::vec4(
        vp[0][3] + vp[0][1],
        vp[1][3] + vp[1][1],
        vp[2][3] + vp[2][1],
        vp[3][3] + vp[3][1]);

    // Top:    row3 - row1
    m_planes[3] = glm::vec4(
        vp[0][3] - vp[0][1],
        vp[1][3] - vp[1][1],
        vp[2][3] - vp[2][1],
        vp[3][3] - vp[3][1]);

    // Near:   row3 + row2
    m_planes[4] = glm::vec4(
        vp[0][3] + vp[0][2],
        vp[1][3] + vp[1][2],
        vp[2][3] + vp[2][2],
        vp[3][3] + vp[3][2]);

    // Far:    row3 - row2
    m_planes[5] = glm::vec4(
        vp[0][3] - vp[0][2],
        vp[1][3] - vp[1][2],
        vp[2][3] - vp[2][2],
        vp[3][3] - vp[3][2]);

    // Normalize each plane
    for (auto& plane : m_planes) {
        float len = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        if (len > 0.0f) {
            plane /= len;
        }
    }
}

bool Frustum::isVisible(const AABB& aabb) const
{
    // For each plane, find the "positive vertex" (corner most in the direction of the plane normal)
    // If the positive vertex is behind the plane, the AABB is fully outside
    for (const auto& plane : m_planes) {
        glm::vec3 pVertex;
        pVertex.x = (plane.x >= 0.0f) ? aabb.max.x : aabb.min.x;
        pVertex.y = (plane.y >= 0.0f) ? aabb.max.y : aabb.min.y;
        pVertex.z = (plane.z >= 0.0f) ? aabb.max.z : aabb.min.z;

        float dist = plane.x * pVertex.x + plane.y * pVertex.y + plane.z * pVertex.z + plane.w;
        if (dist < 0.0f) {
            return false; // Fully outside this plane
        }
    }
    return true;
}

bool Frustum::isVisible(const AABB& localAABB, const glm::mat4& worldMatrix) const
{
    // Transform local AABB to world-space AABB
    glm::vec3 center = (localAABB.min + localAABB.max) * 0.5f;
    glm::vec3 extent = (localAABB.max - localAABB.min) * 0.5f;

    glm::vec3 worldCenter = glm::vec3(worldMatrix * glm::vec4(center, 1.0f));

    // For each axis of worldAABB, accumulate abs contributions from model matrix columns
    glm::vec3 worldExtent(0.0f);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            worldExtent[i] += std::abs(worldMatrix[j][i]) * extent[j];
        }
    }

    AABB worldAABB;
    worldAABB.min = worldCenter - worldExtent;
    worldAABB.max = worldCenter + worldExtent;

    return isVisible(worldAABB);
}

} // namespace QymEngine
