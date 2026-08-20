#version 460

// BlurPass：3x3 盒式模糊，把 HDR 颜色模糊到 BlurBuffer。

layout(set = 0, binding = 0) uniform sampler2D colorTex;

layout(push_constant) uniform Push {
    vec2 texelSize;
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 sum = vec4(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            sum += texture(colorTex, vUv + vec2(float(x), float(y)) * pc.texelSize);
        }
    }
    outColor = sum / 9.0;
}
