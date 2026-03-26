#version 430 core

in vec3 frag_normal;
in vec3 frag_color;
in vec2 frag_uv;
in float frag_fog;

uniform sampler2D mario_tex;
uniform vec4 fog_color;

out vec4 out_color;

void main() {
  // Sample Mario texture atlas
  vec4 tex_color = texture(mario_tex, frag_uv);

  // Multiply by vertex color (SM64 provides per-vertex lighting in color)
  vec3 final_color = tex_color.rgb * frag_color;

  // Simple directional light enhancement
  vec3 light_dir = normalize(vec3(0.5, 1.0, 0.3));
  float diffuse = max(dot(normalize(frag_normal), light_dir), 0.0);
  float ambient = 0.4;
  float lighting = min(ambient + diffuse * 0.6, 1.0);

  final_color *= lighting;

  // Apply fog
  final_color = mix(final_color, fog_color.rgb, frag_fog);

  out_color = vec4(final_color, tex_color.a);

  // Discard fully transparent pixels
  if (out_color.a < 0.01) {
    discard;
  }
}
