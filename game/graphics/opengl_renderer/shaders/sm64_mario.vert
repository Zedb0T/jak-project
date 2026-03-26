#version 430 core

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec3 in_color;
layout(location = 3) in vec2 in_uv;

uniform mat4 camera_matrix;
uniform float fog_min;
uniform float fog_max;

out vec3 frag_normal;
out vec3 frag_color;
out vec2 frag_uv;
out float frag_fog;

void main() {
  // Position is already in OpenGOAL world space (converted from SM64 in C++)
  vec4 world_pos = vec4(in_position, 1.0);
  gl_Position = camera_matrix * world_pos;

  frag_normal = in_normal;
  frag_color = in_color;
  frag_uv = in_uv;

  // Compute fog based on distance from camera
  float dist = length(gl_Position.xyz);
  frag_fog = clamp((dist - fog_min) / (fog_max - fog_min), 0.0, 1.0);
}
