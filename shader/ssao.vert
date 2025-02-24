#version 450
#extension GL_ARB_separate_shader_objects : enable

out gl_PerVertex {
  vec4 gl_Position;
  };

layout(push_constant, std140) uniform PushConstant {
  mat4 mvp;
  vec3 ambient;
  } ubo;

layout(location = 0) in  vec2 inPos;

layout(location = 0) out vec2 UV;
layout(location = 1) out vec2 Pos;
layout(location = 2) out mat4 mvpInv;

void main() {
  Pos         = inPos;
  UV          = inPos*vec2(0.5)+vec2(0.5);
  mvpInv      = inverse(ubo.mvp);
  gl_Position = vec4(inPos.xy, 1.0, 1.0);
  }
