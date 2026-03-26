#pragma once

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/Shader.h"

#include "third-party/glad/include/glad/glad.h"

class SM64MarioRenderer {
 public:
  SM64MarioRenderer();
  ~SM64MarioRenderer();

  void render(SharedRenderState* render_state, ScopedProfilerNode& prof);

 private:
  void init_gl();
  void destroy_gl();

  GLuint m_vao = 0;
  GLuint m_vbo_position = 0;
  GLuint m_vbo_normal = 0;
  GLuint m_vbo_color = 0;
  GLuint m_vbo_uv = 0;
  bool m_gl_initialized = false;
};
