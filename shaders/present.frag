#version 460

// PresentPass：HDR 颜色 + 模糊结果叠加，做 Reinhard 色调映射和 Gamma。

layout(set = 0, binding = 0) uniform sampler2D hdrTex;
layout(set = 0, binding = 1) uniform sampler2D blurTex;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 color = texture(hdrTex, vUv).rgb + texture(blurTex, vUv).rgb * 0.8;
    color = color / (1.0 + color);          // Reinhard 色调映射
    color = pow(color, vec3(1.0 / 2.2));    // Gamma 校正
    outColor = vec4(color, 1.0);
}
