#version 460

// ShadowPass：把模型顶点变换到灯光视角的裁剪空间，只输出深度。

layout(push_constant) uniform Push {
    mat4 model;
} pc;

layout(set = 0, binding = 0) uniform Frame {
    mat4 lightVP;
} ubo;

layout(location = 0) in vec3 aPos;

void main() {
    gl_Position = ubo.lightVP * pc.model * vec4(aPos, 1.0);
}
