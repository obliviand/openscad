#include "mouseselector.h"

#include <QOpenGLFramebufferObject>
/**
 * The selection is making use of a special shader, that renders each object in a color
 * that is derived from its index(), by using the first 24 bits of the identifier as a
 * 3-tuple for color.
 *
 * This module defines the infrastructure
 */

#define OPENGL_TEST(place) \
{ \
	auto err = glGetError(); \
	if (err != GL_NO_ERROR) { \
		fprintf(stderr, "OpenGL error " place ":\n %s\n\n", gluErrorString(err)); \
	} \
}

MouseSelector::MouseSelector(GLView *view) {
  this->view = view;
  if (view && !view->has_shaders) {
    return;
  }
  this->init_shader();

  if (view) this->reset(view);
}

/**
 * Resize the framebuffer whenever it changed
 */
void MouseSelector::reset(GLView *view) {
  this->view = view;
  this->setup_framebuffer(view);
}

/**
 * Initialize the used shaders and setup the shaderinfo_t struct
 */
void MouseSelector::init_shader() {
  /*
    Attributes:
      * idcolor - index of the currently selected object
  */
  const char *vs_source =
    "#version 130\n"
    "in int identifier;\n"
    "out vec4 frag_idcolor;\n"
    "void main() {\n"
    "  frag_idcolor = vec4(((identifier >> 0) & 0xff) / 255.0,\n"
    "                      ((identifier >> 8) & 0xff) / 255.0,\n"
    "                      ((identifier >> 16) & 0xff) / 255.0, \n"
    "                      1.0);\n"
    "  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
    "}\n";

  const char *fs_source =
    "#version 130\n"
    "in vec4 frag_idcolor;\n"
    "void main() {\n"
    "  gl_FragColor = frag_idcolor;\n"
    "}\n";

  GLenum err;
  int shaderstatus;

  auto vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, (const GLchar**)&vs_source, nullptr);
  glCompileShader(vs);
  glGetShaderiv(vs, GL_COMPILE_STATUS, &shaderstatus);
  if (shaderstatus != GL_TRUE) {
    int loglen;
    char logbuffer[1000];
    glGetShaderInfoLog(vs, sizeof(logbuffer), &loglen, logbuffer);
    fprintf(stderr, "OpenGL vertex shader Error:\n%.*s\n\n", loglen, logbuffer);
  }

  auto fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fs, 1, (const GLchar**)&fs_source, nullptr);
  glCompileShader(fs);
  glGetShaderiv(fs, GL_COMPILE_STATUS, &shaderstatus);
  if (shaderstatus != GL_TRUE) {
    int loglen;
    char logbuffer[1000];
    glGetShaderInfoLog(fs, sizeof(logbuffer), &loglen, logbuffer);
    fprintf(stderr, "OpenGL fragment shader Error:\n%.*s\n\n", loglen, logbuffer);
  }

  auto selecthader_prog = glCreateProgram();
  glAttachShader(selecthader_prog, vs);
  glAttachShader(selecthader_prog, fs);
  glLinkProgram(selecthader_prog);
  OPENGL_TEST("Linking Shader");

  GLint status;
  glGetProgramiv(selecthader_prog, GL_LINK_STATUS, &status);
  if (status == GL_FALSE) {
    int loglen;
    char logbuffer[1000];
    glGetProgramInfoLog(selecthader_prog, sizeof(logbuffer), &loglen, logbuffer);
    fprintf(stderr, "OpenGL Program Linker Error:\n%.*s\n\n", loglen, logbuffer);
  } else {
    int loglen;
    char logbuffer[1000];
    glGetProgramInfoLog(selecthader_prog, sizeof(logbuffer), &loglen, logbuffer);
    if (loglen > 0) {
      fprintf(stderr, "OpenGL Program Link OK:\n%.*s\n\n", loglen, logbuffer);
    }
    glValidateProgram(selecthader_prog);
    glGetProgramInfoLog(selecthader_prog, sizeof(logbuffer), &loglen, logbuffer);
    if (loglen > 0) {
      fprintf(stderr, "OpenGL Program Validation results:\n%.*s\n\n", loglen, logbuffer);
    }
  }

  this->shaderinfo.progid = selecthader_prog;
  this->shaderinfo.type = GLView::shaderinfo_t::SELECT_RENDERING;
  GLint identifier = glGetAttribLocation(selecthader_prog, "identifier");
  this->shaderinfo.data.select_rendering.identifier = identifier;
  if (identifier < 0) {
    fprintf(stderr, "GL symbol retrieval went wrong, id is negative\n\n");
  }

}

/**
 * Resize or create the framebuffer
 */
void MouseSelector::setup_framebuffer(const GLView *view) {
  if (!this->framebuffer ||
      this->framebuffer->width() != view->cam.pixel_width ||
      this->framebuffer->height() != view->cam.pixel_height) {
    this->framebuffer.reset(
      new QOpenGLFramebufferObject(
      view->cam.pixel_width,
      view->cam.pixel_width,
      QOpenGLFramebufferObject::Depth));
    this->framebuffer->release();

  }

}

/**
 * Setup the shaders, Projection and Model matrix and call the given renderer.
 * The renderer has to make sure, that the colors are defined accordingly, or
 * the selection wont work.
 *
 * returns 0 if no object was found
 */
int MouseSelector::select(const Renderer *renderer, int x, int y) {
  // x/y is originated topleft, so turn y around
  y = this->view->cam.pixel_height - y;

  if (x > this->view->cam.pixel_width || x < 0 ||
      y > this->view->cam.pixel_height || y < 0) {
    return -1;
  }

  // Initialize GL to draw to texture
  // Ideally a texture of only 1x1 or 2x2 pixels as a subset of the viewing frustrum
  // of the currently selected frame.
  // For now, i will use a texture the same size as the normal viewport
  // and select the identifier at the mouse coordinates
  this->framebuffer->bind();
  OPENGL_TEST("switch FBO");

  glClearColor(0, 0, 0, 1.0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

  glViewport(0, 0, this->view->cam.pixel_width, this->view->cam.pixel_height);
  this->view->setupCamera();
  glTranslated(this->view->cam.object_trans.x(),
               this->view->cam.object_trans.y(),
               this->view->cam.object_trans.z());

  glDisable(GL_LIGHTING);
  glDepthFunc(GL_LESS);
  glCullFace(GL_BACK);
  glDisable(GL_CULL_FACE);
  glEnable(GL_DEPTH_TEST);

  // call the renderer with the selector shader
  renderer->draw_with_shader(&this->shaderinfo);
  OPENGL_TEST("renderer->draw_with_shader");

  // Grab the color from the framebuffer and convert it back to an identifier
  GLubyte color[3] = { 0 };
  glReadPixels(x, y, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, color);
  OPENGL_TEST("glReadPixels");
  int index = (uint32_t)color[0] | ((uint32_t)color[1] << 8) | ((uint32_t)color[2] << 16);

  // ASDF
  //printf("select result: color %x %x %x (index %i)\n", color[0], color[1], color[2], index);

  // Switch the active framebuffer back to the default
  this->framebuffer->release();

  return index;
}
