#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vector>

namespace demo {

struct Vertex {
    glm::vec3 pos;
    glm::vec3 normal;
    glm::vec3 color;
};

// 每帧上传给 shader 的数据（与 GLSL std140 布局对齐）。
struct FrameData {
    glm::mat4 viewProj;   // 64B
    glm::mat4 lightVP;    // 64B
    glm::vec4 lightDir;   // 16B（shader 里取 .xyz）
    glm::vec4 cameraPos;  // 16B
};

// 程序化几何：地面 + 3 个彩色立方体，零外部资产。（winding fix v2）
// 立方体每面 4 顶点（CCW，从外部看），共 24 顶点 36 索引。
inline void BuildGeometry(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices,
                          std::vector<glm::mat4>& models) {
    // 以 (u, v, n) 满足 u x v = n 的切线基生成外侧面，保证 CCW 绕序。
    const glm::vec3 faceNormals[6] = {
        {0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0},
    };
    const glm::vec3 faceU[6] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0},
    };
    const glm::vec3 faceV[6] = {
        {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1},
    };
    const glm::vec3 faceColors[6] = {
        {1.0f, 0.25f, 0.25f}, {0.25f, 1.0f, 0.25f}, {0.25f, 0.6f, 1.0f},
        {1.0f, 1.0f, 0.25f},  {1.0f, 0.25f, 1.0f},  {0.75f, 0.5f, 0.25f},
    };

    const auto addCubeFace = [&](const glm::vec3& n, const glm::vec3& u,
                                 const glm::vec3& v, const glm::vec3& color) {
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        const glm::vec3 c = n * 0.5f;
        vertices.push_back({c - (u + v) * 0.5f, n, color});
        vertices.push_back({c + (u - v) * 0.5f, n, color});
        vertices.push_back({c + (u + v) * 0.5f, n, color});
        vertices.push_back({c + (v - u) * 0.5f, n, color});
        indices.insert(indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    };
    for (int i = 0; i < 6; ++i) {
        addCubeFace(faceNormals[i], faceU[i], faceV[i], faceColors[i]);
    }

    // 地面：20x20 的平面
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    const glm::vec3 n{0, 1, 0};
    const glm::vec3 u{1, 0, 0};
    const glm::vec3 v{0, 0, -1};
    const float size = 10.0f;
    const glm::vec3 c{0, 0, 0};
    const glm::vec3 groundColor{0.55f, 0.55f, 0.6f};
    vertices.push_back({c - (u + v) * size, n, groundColor});
    vertices.push_back({c + (u - v) * size, n, groundColor});
    vertices.push_back({c + (u + v) * size, n, groundColor});
    vertices.push_back({c + (v - u) * size, n, groundColor});
    indices.insert(indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});

    // 场景布局：3 个立方体
    const auto cube = [](const glm::vec3& pos, float scale, float rotY) {
        return glm::translate(glm::mat4(1.0f), pos) * glm::rotate(glm::mat4(1.0f), rotY, glm::vec3(0, 1, 0)) *
               glm::scale(glm::mat4(1.0f), glm::vec3(scale));
    };
    models = {
        cube({-1.9f, 0.5f, -1.2f}, 1.0f, 0.3f),
        cube({0.3f, 0.35f, 0.8f}, 0.7f, -0.5f),
        cube({1.9f, 0.75f, -0.4f}, 1.5f, 0.8f),
    };
}

inline glm::mat4 ComputeLightVP() {
    const glm::vec3 lightDir = glm::normalize(glm::vec3(0.5f, -0.7f, 0.35f));
    const glm::vec3 lightPos = -lightDir * 12.0f;
    glm::mat4 proj = glm::orthoRH_ZO(-8.0f, 8.0f, -8.0f, 8.0f, 0.5f, 30.0f);
    proj[1][1] *= -1.0f;
    return proj * glm::lookAt(lightPos, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
}

// 每帧更新相机（环绕）与灯光数据。
inline void UpdateFrameData(FrameData& out, float time, float aspect) {
    const float radius = 6.5f;
    const float angle = time * 0.3f;
    const glm::vec3 cam(radius * std::cos(angle), 2.2f, radius * std::sin(angle));

    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(45.0f), aspect, 0.1f, 100.0f);
    proj[1][1] *= -1.0f;  // Y 轴翻转：适配 Vulkan NDC
    out.viewProj = proj *
                   glm::lookAt(cam, glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    out.cameraPos = glm::vec4(cam, 1.0f);
    out.lightVP = ComputeLightVP();
    out.lightDir = glm::vec4(glm::normalize(glm::vec3(0.5f, -0.7f, 0.35f)), 0.0f);
}

}  // namespace demo
