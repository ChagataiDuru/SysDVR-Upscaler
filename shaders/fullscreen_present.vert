#version 450

layout(location = 0) out vec2 screenUv;

void main() {
    vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    screenUv = position;
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}

