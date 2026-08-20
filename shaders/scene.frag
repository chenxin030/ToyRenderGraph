#version 460

// ScenePass：Lambert 光照 + 阴影采样，输出到 HDR 颜色缓冲。

layout(set = 0, binding = 0) uniform Frame {
    mat4 viewProj;
    mat4 lightVP;
    vec4 lightDir;
    vec4 cameraPos;
} ubo;

layout(set = 0, binding = 1) uniform sampler2DShadow shadowMap;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec3 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 N = normalize(vNormal);
    float ndl = max(dot(N, -ubo.lightDir.xyz), 0.0);

    // 把世界坐标变换到灯光空间，采样阴影贴图
    vec4 lightSpace = ubo.lightVP * vec4(vWorldPos, 1.0);
    vec3 proj = lightSpace.xyz / lightSpace.w;
    vec2 uv = proj.xy * 0.5 + 0.5;
    float shadow = 1.0;
    if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) {
        shadow = texture(shadowMap, vec3(uv, proj.z - 0.002));
    }

    vec3 lit = vColor * (0.25 + 0.75 * ndl * shadow);
    outColor = vec4(lit, 1.0);
}
