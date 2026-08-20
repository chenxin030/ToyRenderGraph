#version 460

// 全屏三角形：BlurPass / PresentPass 共用，不输入顶点缓冲。

layout(location = 0) out vec2 vUv;

void main() {
    vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    vUv = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
