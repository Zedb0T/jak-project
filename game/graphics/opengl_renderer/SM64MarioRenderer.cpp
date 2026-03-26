#include "SM64MarioRenderer.h"

#include "game/external/sm64.h"
#include "game/graphics/opengl_renderer/Shader.h"

SM64MarioRenderer::SM64MarioRenderer() {}

SM64MarioRenderer::~SM64MarioRenderer() {
  destroy_gl();
}

void SM64MarioRenderer::init_gl() {
  if (m_gl_initialized)
    return;

  glGenVertexArrays(1, &m_vao);
  glGenBuffers(1, &m_vbo_position);
  glGenBuffers(1, &m_vbo_normal);
  glGenBuffers(1, &m_vbo_color);
  glGenBuffers(1, &m_vbo_uv);

  glBindVertexArray(m_vao);

  // Position attribute (location 0)
  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_position);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(0);

  // Normal attribute (location 1)
  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_normal);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(1);

  // Color attribute (location 2)
  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_color);
  glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(2);

  // UV attribute (location 3)
  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_uv);
  glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(3);

  glBindVertexArray(0);
  m_gl_initialized = true;
}

void SM64MarioRenderer::destroy_gl() {
  if (!m_gl_initialized)
    return;

  glDeleteVertexArrays(1, &m_vao);
  glDeleteBuffers(1, &m_vbo_position);
  glDeleteBuffers(1, &m_vbo_normal);
  glDeleteBuffers(1, &m_vbo_color);
  glDeleteBuffers(1, &m_vbo_uv);

  m_vao = m_vbo_position = m_vbo_normal = m_vbo_color = m_vbo_uv = 0;
  m_gl_initialized = false;
}

void SM64MarioRenderer::render(SharedRenderState* render_state, ScopedProfilerNode& prof) {
  if (!sm64_bridge::is_initialized())
    return;

  auto data = sm64_bridge::get_render_data();
  if (!data.valid || data.num_triangles == 0)
    return;

  if (!m_gl_initialized) {
    init_gl();
  }

  int num_verts = data.num_triangles * 3;

  // Upload vertex data
  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_position);
  glBufferData(GL_ARRAY_BUFFER, num_verts * 3 * sizeof(float), data.positions.data(),
               GL_STREAM_DRAW);

  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_normal);
  glBufferData(GL_ARRAY_BUFFER, num_verts * 3 * sizeof(float), data.normals.data(),
               GL_STREAM_DRAW);

  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_color);
  glBufferData(GL_ARRAY_BUFFER, num_verts * 3 * sizeof(float), data.colors.data(), GL_STREAM_DRAW);

  glBindBuffer(GL_ARRAY_BUFFER, m_vbo_uv);
  glBufferData(GL_ARRAY_BUFFER, num_verts * 2 * sizeof(float), data.uvs.data(), GL_STREAM_DRAW);

  // Activate shader
  render_state->shaders[ShaderId::SM64_MARIO].activate();
  auto shader = render_state->shaders[ShaderId::SM64_MARIO].id();

  // Set uniforms
  // Camera matrix (4x4) - stored as 4 Vector4f's, pass first element's data pointer
  glUniformMatrix4fv(glGetUniformLocation(shader, "camera_matrix"), 1, GL_FALSE,
                     render_state->camera_matrix[0].data());

  // Fog settings from camera_fog (x=constant, y=min, z=max)
  auto& fc = render_state->fog_color;
  float fog_col[4] = {fc.x() / 255.f, fc.y() / 255.f, fc.z() / 255.f, fc.w() / 255.f};
  glUniform4fv(glGetUniformLocation(shader, "fog_color"), 1, fog_col);
  glUniform1f(glGetUniformLocation(shader, "fog_min"), render_state->camera_fog.y());
  glUniform1f(glGetUniformLocation(shader, "fog_max"), render_state->camera_fog.z());

  // Texture
  GLuint tex = sm64_bridge::get_texture_gl_id();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, tex);
  glUniform1i(glGetUniformLocation(shader, "mario_tex"), 0);

  // Render state
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // Draw
  glBindVertexArray(m_vao);
  glDrawArrays(GL_TRIANGLES, 0, num_verts);
  glBindVertexArray(0);

  prof.add_draw_call();
  prof.add_tri(data.num_triangles);
}
