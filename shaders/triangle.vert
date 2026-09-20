#version 450

// 顶点位置和颜色由 CPU 端通过顶点缓冲传入，不再是着色器里写死的
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

void main() {
  gl_Position = vec4(inPosition, 0.0, 1.0);
  fragColor = inColor;
}
