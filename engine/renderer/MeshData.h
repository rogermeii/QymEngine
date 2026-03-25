#pragma once

#include "renderer/Buffer.h"
#include <vector>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace QymEngine {

namespace detail {
    inline Vertex makeVertex(float px, float py, float pz,
                             float cr, float cg, float cb,
                             float u, float v,
                             float nx, float ny, float nz)
    {
        Vertex vert{};
        vert.pos      = glm::vec3(px, py, pz);
        vert.color    = glm::vec3(cr, cg, cb);
        vert.texCoord = glm::vec2(u, v);
        vert.normal   = glm::vec3(nx, ny, nz);
        return vert;
    }
} // namespace detail

// ---------------------------------------------------------------------------
// Quad: 4 vertices on XY plane (z=0), 6 indices, normal (0,0,1)
// ---------------------------------------------------------------------------
inline void generateQuad(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices)
{
    using detail::makeVertex;
    vertices.clear();
    vertices.push_back(makeVertex(-0.5f, -0.5f, 0.0f, 1,1,1, 0.0f, 1.0f, 0,0,1));
    vertices.push_back(makeVertex( 0.5f, -0.5f, 0.0f, 1,1,1, 1.0f, 1.0f, 0,0,1));
    vertices.push_back(makeVertex( 0.5f,  0.5f, 0.0f, 1,1,1, 1.0f, 0.0f, 0,0,1));
    vertices.push_back(makeVertex(-0.5f,  0.5f, 0.0f, 1,1,1, 0.0f, 0.0f, 0,0,1));
    indices = { 0, 1, 2, 2, 3, 0 };
}

// ---------------------------------------------------------------------------
// Cube: 24 vertices (4 per face, unique normals), 36 indices
// ---------------------------------------------------------------------------
inline void generateCube(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices)
{
    using detail::makeVertex;
    vertices.clear();
    indices.clear();

    // Helper: add a face (4 verts + 6 indices)
    auto addFace = [&](float p0x, float p0y, float p0z,
                       float p1x, float p1y, float p1z,
                       float p2x, float p2y, float p2z,
                       float p3x, float p3y, float p3z,
                       float nx, float ny, float nz)
    {
        uint32_t base = static_cast<uint32_t>(vertices.size());
        vertices.push_back(makeVertex(p0x,p0y,p0z, 1,1,1, 0,1, nx,ny,nz));
        vertices.push_back(makeVertex(p1x,p1y,p1z, 1,1,1, 1,1, nx,ny,nz));
        vertices.push_back(makeVertex(p2x,p2y,p2z, 1,1,1, 1,0, nx,ny,nz));
        vertices.push_back(makeVertex(p3x,p3y,p3z, 1,1,1, 0,0, nx,ny,nz));
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
        indices.push_back(base + 0);
    };

    // Front  (z = +0.5)
    addFace(-0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f,  0, 0, 1);
    // Back   (z = -0.5)
    addFace( 0.5f,-0.5f,-0.5f, -0.5f,-0.5f,-0.5f, -0.5f, 0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  0, 0,-1);
    // Right  (x = +0.5)
    addFace( 0.5f,-0.5f, 0.5f,  0.5f,-0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  0.5f, 0.5f, 0.5f,  1, 0, 0);
    // Left   (x = -0.5)
    addFace(-0.5f,-0.5f,-0.5f, -0.5f,-0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f,-0.5f, -1, 0, 0);
    // Top    (y = +0.5)
    addFace(-0.5f, 0.5f, 0.5f,  0.5f, 0.5f, 0.5f,  0.5f, 0.5f,-0.5f, -0.5f, 0.5f,-0.5f,  0, 1, 0);
    // Bottom (y = -0.5)
    addFace(-0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f, -0.5f,-0.5f, 0.5f,  0,-1, 0);
}

// ---------------------------------------------------------------------------
// Plane: 4 vertices on XZ plane (y=0), 6 indices, normal (0,1,0)
// ---------------------------------------------------------------------------
inline void generatePlane(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices)
{
    using detail::makeVertex;
    vertices.clear();
    vertices.push_back(makeVertex(-0.5f, 0.0f,  0.5f, 1,1,1, 0.0f, 0.0f, 0,1,0));
    vertices.push_back(makeVertex( 0.5f, 0.0f,  0.5f, 1,1,1, 1.0f, 0.0f, 0,1,0));
    vertices.push_back(makeVertex( 0.5f, 0.0f, -0.5f, 1,1,1, 1.0f, 1.0f, 0,1,0));
    vertices.push_back(makeVertex(-0.5f, 0.0f, -0.5f, 1,1,1, 0.0f, 1.0f, 0,1,0));
    indices = { 0, 1, 2, 2, 3, 0 };
}

// ---------------------------------------------------------------------------
// Sphere: UV sphere, 16 segments x 16 rings
// ---------------------------------------------------------------------------
inline void generateSphere(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices)
{
    vertices.clear();
    indices.clear();

    const int segments = 16;
    const int rings    = 16;
    const float radius = 0.5f;

    for (int r = 0; r <= rings; ++r)
    {
        float phi = static_cast<float>(M_PI) * static_cast<float>(r) / static_cast<float>(rings);
        float sinPhi = std::sin(phi);
        float cosPhi = std::cos(phi);

        for (int s = 0; s <= segments; ++s)
        {
            float theta = 2.0f * static_cast<float>(M_PI) * static_cast<float>(s) / static_cast<float>(segments);
            float sinTheta = std::sin(theta);
            float cosTheta = std::cos(theta);

            glm::vec3 normal(sinPhi * cosTheta, cosPhi, sinPhi * sinTheta);
            glm::vec3 pos = normal * radius;

            Vertex vert{};
            vert.pos      = pos;
            vert.color    = glm::vec3(1.0f, 1.0f, 1.0f);
            vert.texCoord = glm::vec2(
                static_cast<float>(s) / static_cast<float>(segments),
                static_cast<float>(r) / static_cast<float>(rings)
            );
            vert.normal   = normal;
            vertices.push_back(vert);
        }
    }

    for (int r = 0; r < rings; ++r)
    {
        for (int s = 0; s < segments; ++s)
        {
            uint32_t cur  = static_cast<uint32_t>(r * (segments + 1) + s);
            uint32_t next = static_cast<uint32_t>(cur + segments + 1);

            indices.push_back(cur);
            indices.push_back(next);
            indices.push_back(cur + 1);

            indices.push_back(cur + 1);
            indices.push_back(next);
            indices.push_back(next + 1);
        }
    }
}

} // namespace QymEngine
