#version 460

// ScenePass：正向光照，输出世界坐标/法线/颜色给 fragment 使用。

layout(push_constant) uniform Push {
    mat4 model;
} pc;

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    mat4 lightVP;
    vec4 lightDir;
    vec4 cameraPos;
} ubo;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec3 vColor;

void main() {
    vec4 world = pc.model * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(pc.model) * aNormal;
    vColor = aColor;
    gl_Position = ubo.viewProj * world;
}
