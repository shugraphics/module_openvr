// ======================================================================== //
// Copyright 2009-2017 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include <string>
#include <array>
#include <iostream>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <sstream>

#include <glm/glm.hpp>
#include <glm/ext.hpp>
#include <GL/gl3w.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <ospray/ospray.h>
#include <ospcommon/library.h>

// sg stuff
#include "common/sg/SceneGraph.h"
#include "common/sg/Renderer.h"
#include "common/sg/common/FrameBuffer.h"
#include "common/sg/importer/Importer.h"
#include "ospcommon/utility/TimeStamp.h"
#include "sg/common/FrameBuffer.h"
#include "ospcommon/FileName.h"
#include "ospcommon/networking/Socket.h"
#include "ospcommon/vec.h"
#include "ospcommon/utility/SaveImage.h"
#include "sg/geometry/TriangleMesh.h"
#include "widgets/imguiViewer.h"
#include "common/util/AsyncRenderEngine.h"

#include "openvr_display.h"
#include "gldebug.h"

using namespace ospcommon;
using namespace ospray;

static const std::array<float, 42> CUBE_STRIP = {
  1, 1, -1,
  -1, 1, -1,
  1, 1, 1,
  -1, 1, 1,
  -1, -1, 1,
  -1, 1, -1,
  -1, -1, -1,
  1, 1, -1,
  1, -1, -1,
  1, 1, 1,
  1, -1, 1,
  -1, -1, 1,
  1, -1, -1,
  -1, -1, -1
};

const int MIRROR_WIDTH = 720;
const int MIRROR_HEIGHT = 800;

const static std::string vsrc = R"(
#version 330 core
layout(location = 0) in vec3 pos;
uniform mat4 proj_view;
out vec3 vdir;
void main(void) {
  gl_Position = proj_view * vec4(pos, 1);
  vdir = pos.xyz;
}
)";

const static std::string fsrc = R"(
#version 330 core
uniform sampler2D envmap;
out vec4 color;
in vec3 vdir;
void main(void) {
  const float PI = 3.1415926535897932384626433832795;

  vec3 dir = normalize(vdir);
  // Note: The panoramic camera uses flipped theta/phi terminology
  // compared to wolfram alpha or other parametric sphere equations
  // In the map phi goes along x from [0, 2pi] and theta goes along y [0, pi]
  float u = (atan(dir.z, dir.x) + PI / 2) / (2 * PI);
  float v = acos(dir.y) / PI;
  color = texture(envmap, vec2(u, v));
}
)";


//
// sg stuff
//
std::vector<std::string> files;
std::string initialRendererType;
bool addPlane = true;
bool debug = false;
bool fullscreen = false;
bool print = false;

void parseCommandLine(int ac, const char **&av)
{
  for (int i = 1; i < ac; i++) {
    const std::string arg = av[i];
    if (arg == "-np" || arg == "--no-plane") {
      addPlane = false;
    } else if (arg == "-d" || arg == "--debug") {
      debug = true;
    } else if (arg == "-r" || arg == "--renderer") {
      initialRendererType = av[++i];
    } else if (arg == "-m" || arg == "--module") {
      ospLoadModule(av[++i]);
    } else if (arg == "--print") {
      print=true;
    } else if (arg == "--fullscreen") {
      fullscreen = true;
    } else if (arg[0] != '-') {
      files.push_back(av[i]);
    }
  }
}

//parse command line arguments containing the format:
//  -nodeName:...:nodeName=value,value,value
void parseCommandLineSG(int ac, const char **&av, sg::Node &root)
{
  for(int i=1;i < ac; i++) {
    std::string arg(av[i]);
    size_t f;
    std::string value("");
    if (arg.size() < 2 || arg[0] != '-')
      continue;

    while ((f = arg.find(":")) != std::string::npos ||
        (f = arg.find(",")) != std::string::npos) {
      arg[f] = ' ';
    }

    f = arg.find("=");
    if (f != std::string::npos)
      value = arg.substr(f+1,arg.size());

    if (value != "") {
      std::stringstream ss;
      ss << arg.substr(1,f-1);
      std::string child;
      std::reference_wrapper<sg::Node> node_ref = root;
      while (ss >> child) {
        node_ref = node_ref.get().childRecursive(child);
      }
      auto &node = node_ref.get();
      //Carson: TODO: reimplement with a way of determining type of node value
      //  currently relies on exception on value cast
      try {
        node.valueAs<std::string>();
        node.setValue(value);
      } catch(...) {};
      try {
        std::stringstream vals(value);
        float x;
        vals >> x;
        node.valueAs<float>();
        node.setValue(x);
      } catch(...) {}
      try {
        std::stringstream vals(value);
        int x;
        vals >> x;
        node.valueAs<int>();
        node.setValue(x);
      } catch(...) {}
      try {
        std::stringstream vals(value);
        bool x;
        vals >> x;
        node.valueAs<bool>();
        node.setValue(x);
      } catch(...) {}
      try {
        std::stringstream vals(value);
        float x,y,z;
        vals >> x >> y >> z;
        node.valueAs<ospcommon::vec3f>();
        node.setValue(ospcommon::vec3f(x,y,z));
      } catch(...) {}
      try {
        std::stringstream vals(value);
        int x,y;
        vals >> x >> y;
        node.valueAs<ospcommon::vec2i>();
        node.setValue(ospcommon::vec2i(x,y));
      } catch(...) {}
    }
  }
}

//
// end sg stuff
//

const int PANORAMIC_HEIGHT = 512;
const int PANORAMIC_WIDTH = 2 * PANORAMIC_HEIGHT;

GLuint load_shader_program(const std::string &vshader_src, const std::string &fshader_src);

int main(int argc, const char **argv) {
  if (argc < 2) {
    std::cout << "Usage: ./osp360 <obj file>\n";
    return 1;
  }
  if (SDL_Init(SDL_INIT_EVERYTHING) != 0) {
    return 1;
  }

  ospInit(&argc, argv);

  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
  SDL_Window *window = SDL_CreateWindow("osp360", SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED, MIRROR_WIDTH, MIRROR_HEIGHT, SDL_WINDOW_OPENGL);

  if (!window) {
    return 1;
  }
  SDL_GLContext ctx = SDL_GL_CreateContext(window);
  SDL_GL_SetSwapInterval(1);

  if (gl3wInit()) {
    throw std::runtime_error("Failed to init gl3w");
  }
  for (int i = 0; i < argc; ++i) {
    if (std::strcmp(argv[i], "-gldebug") == 0) {
      register_debug_callback();
      break;
    }
  }

  // scene graph stuff
  ospcommon::LibraryRepository::getInstance()->add("ospray_sg");

  parseCommandLine(argc, argv);
  std::shared_ptr<sg::Frame> scenegraph = std::make_shared<sg::Frame>();
  sg::Node &renderer = scenegraph->child("renderer");

  renderer["maxDepth"].setValue(3);
  renderer["shadowsEnabled"].setValue(true);
  renderer["aoSamples"].setValue(1);
  renderer["aoDistance"].setValue(500.f);
  renderer["autoEpsilon"].setValue(false);
  auto panoramicCamera = sg::createNode("camera", "PanoramicCamera");

  scenegraph->setChild("camera", panoramicCamera);
  panoramicCamera->setParent(scenegraph);
  panoramicCamera->child("pos").setValue(ospcommon::vec3f{21, 200, -49});
  panoramicCamera->child("dir").setValue(ospcommon::vec3f{0, 0, 1});
  panoramicCamera->child("up").setValue(ospcommon::vec3f{0, -1, 0});
  renderer["spp"].setValue(-1);

  scenegraph->child("frameBuffer")["size"].setValue(ospcommon::vec2i(PANORAMIC_WIDTH, PANORAMIC_HEIGHT));
  scenegraph->add(sg::createNode("navFrameBuffer", "FrameBuffer"), "navFrameBuffer");
  if (!initialRendererType.empty()) {
    renderer["rendererType"].setValue(initialRendererType);
  }

  auto &lights = renderer["lights"];

  auto &sun = lights.createChild("sun", "DirectionalLight");
  sun["color"].setValue(vec3f(1.f,232.f/255.f,166.f/255.f));
  sun["direction"].setValue(vec3f(0.462f,-1.f,-.1f));
  sun["intensity"].setValue(2.5f);

  auto &bounce = lights.createChild("bounce", "DirectionalLight");
  bounce["color"].setValue(vec3f(127.f/255.f,178.f/255.f,255.f/255.f));
  bounce["direction"].setValue(vec3f(-.93,-.54f,-.605f));
  bounce["intensity"].setValue(1.25f);

  auto &ambient = lights.createChild("ambient", "AmbientLight");
  ambient["intensity"].setValue(3.9f);
  ambient["color"].setValue(vec3f(174.f/255.f,218.f/255.f,255.f/255.f));

  auto &world = renderer["world"];

  for (auto file : files) {
    FileName fn = file;
    if (fn.ext() == "ospsg") {
      sg::loadOSPSG(renderer.shared_from_this(), fn.str());
    } else {
      auto importerNode_ptr = sg::createNode(fn.name(), "Importer");
      auto &importerNode = *importerNode_ptr;
      importerNode["fileName"] = fn.str();
      auto &transform = world.createChild("transform_"+fn.str(), "Transform");
      transform.add(importerNode_ptr);
    }
  }

  parseCommandLineSG(argc, argv, *scenegraph);

  scenegraph->verify();
  scenegraph->commit();

  std::cout << "sg init finished" << std::endl;

  //
  // end sg init
  //

  GLuint tex;
  glGenTextures(1, &tex);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, PANORAMIC_WIDTH, PANORAMIC_HEIGHT, 0,
      GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glActiveTexture(GL_TEXTURE0);

  std::cout << "starting async renderer" << std::endl;
  AsyncRenderEngine async_renderer(scenegraph);
  async_renderer.start();

  glEnable(GL_DEPTH_TEST);
  glClearColor(0, 0, 0, 1);
  glClearDepth(1);

  GLuint vao, vbo;
  glGenVertexArrays(1, &vao);
  glGenBuffers(1, &vbo);
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(float) * CUBE_STRIP.size(),
      CUBE_STRIP.data(), GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0);

  GLuint shader = load_shader_program(vsrc, fsrc);
  glUseProgram(shader);

  glUniform1i(glGetUniformLocation(shader, "envmap"), 1);
  const GLuint proj_view_unif = glGetUniformLocation(shader, "proj_view");

  const glm::mat4 proj_view = glm::perspective(glm::radians(65.f),
		  static_cast<float>(MIRROR_WIDTH) / MIRROR_HEIGHT, 0.01f, 10.f)
    * glm::lookAt(glm::vec3(0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0));
  glUniformMatrix4fv(proj_view_unif, 1, GL_FALSE, glm::value_ptr(proj_view));

#ifdef OPENVR_ENABLED
  OpenVRDisplay vr_display;
#endif

  bool quit = false;
  bool interactiveCamera = false;
  bool interacting = false;
  sg::TimeStamp lastRenderTime;
  sg::TimeStamp lastUpdateTime;
  float stepsize = 2.f;
  while (!quit) {
    SDL_Event e;
    bool moved = false;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)){
        quit = true;
        break;
      } else if (e.type == SDL_KEYDOWN) {
		  switch (e.key.keysym.sym) {
			  case SDLK_1:
				  panoramicCamera->child("pos").setValue(ospcommon::vec3f{21, 200, -49});
				  break;
			  case SDLK_2:
				  panoramicCamera->child("pos").setValue(ospcommon::vec3f{800, 200, -49});
				  break;
			  case SDLK_3:
				  panoramicCamera->child("pos").setValue(ospcommon::vec3f{-1200, 200, -45});
				  break;
			  case SDLK_4:
				  panoramicCamera->child("pos").setValue(ospcommon::vec3f{-720, 600, 180});
				  break;
			  default: break;
		  }
	  }
	  /*
      else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_UP){
        renderer.setChild("camera", perspectiveCamera);
        auto eye = perspectiveCamera->child("pos").valueAs<ospcommon::vec3f>();
        auto dir = perspectiveCamera->child("dir").valueAs<ospcommon::vec3f>();
        eye += dir*stepsize;
        panoramicCamera->child("pos").setValue(eye);
		perspectiveCamera->child("pos").setValue(eye);
        moved = true;
        interactiveCamera = true;
        lastUpdateTime = sg::TimeStamp();
        interacting = true;
        break;
      }
      else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_DOWN){
        renderer.setChild("camera", perspectiveCamera);
        auto eye = perspectiveCamera->child("pos").valueAs<ospcommon::vec3f>();
        auto dir = perspectiveCamera->child("dir").valueAs<ospcommon::vec3f>();
        eye -= dir*stepsize;
        panoramicCamera->child("pos").setValue(eye);
		perspectiveCamera->child("pos").setValue(eye);
        interactiveCamera = true;
        moved = true;
        lastUpdateTime = sg::TimeStamp();
        interacting = true;
        break;
      }
	  */
      else if (e.type == SDL_KEYUP) {
        interacting = false;
      }
    }
    if (!moved && interactiveCamera && !interacting)
    {
      renderer.setChild("camera", panoramicCamera);
      panoramicCamera->markAsModified();
      panoramicCamera->setChildrenModified(sg::TimeStamp());
      interactiveCamera = false;
    }
    if (async_renderer.hasNewFrame()) {
      auto &mappedFB = async_renderer.mapFramebuffer();
      auto fbData = mappedFB.data();
      glActiveTexture(GL_TEXTURE1);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, PANORAMIC_WIDTH, PANORAMIC_HEIGHT, 0,
          GL_RGBA, GL_UNSIGNED_BYTE, fbData);
      async_renderer.unmapFramebuffer();
      glActiveTexture(GL_TEXTURE0);
      lastRenderTime = sg::TimeStamp();
    }

#ifdef OPENVR_ENABLED
    vr_display.begin_frame();
    for (size_t i = 0; i < 2; ++i) {
      glm::mat4 proj, view;
      vr_display.begin_eye(i, view, proj);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

      // Remove translation from the view matrix
      view[3] = glm::vec4(0, 0, 0, 1);
      glm::mat4 proj_view = proj * view;
      glUniformMatrix4fv(proj_view_unif, 1, GL_FALSE, glm::value_ptr(proj_view));

      glDrawArrays(GL_TRIANGLE_STRIP, 0, CUBE_STRIP.size() / 3);
    }
    vr_display.submit();
#endif

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, MIRROR_WIDTH, MIRROR_HEIGHT);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, CUBE_STRIP.size() / 3);
    SDL_GL_SwapWindow(window);
  }

  async_renderer.stop();

  glDeleteProgram(shader);
  glDeleteTextures(1, &tex);
  glDeleteBuffers(1, &vbo);
  glDeleteVertexArrays(1, &vao);

  SDL_GL_DeleteContext(ctx);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
GLuint compile_shader(const std::string &src, GLenum type) {
  GLuint shader = glCreateShader(type);
  const char *csrc = src.c_str();
  glShaderSource(shader, 1, &csrc, 0);
  glCompileShader(shader);

  GLint status;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
  if (status == GL_FALSE) {
    std::cout << "Shader compilation error:\n";
    GLint len;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
    std::vector<char> log(len, 0);
    glGetShaderInfoLog(shader, log.size(), 0, log.data());
    std::cout << log.data() << "\n";
    throw std::runtime_error("Shader compilation failed");
  }
  return shader;
}
GLuint load_shader_program(const std::string &vshader_src, const std::string &fshader_src) {
  GLuint vs = compile_shader(vshader_src, GL_VERTEX_SHADER);
  GLuint fs = compile_shader(fshader_src, GL_FRAGMENT_SHADER);
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);

  GLint status;
  glGetProgramiv(prog, GL_LINK_STATUS, &status);
  if (status == GL_FALSE) {
    std::cout << "Shader link error:\n";
    GLint len;
    glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
    std::vector<char> log(len, 0);
    glGetProgramInfoLog(prog, log.size(), 0, log.data());
    std::cout << log.data() << "\n";
    throw std::runtime_error("Shader link failed");
  }
  glDetachShader(prog, vs);
  glDetachShader(prog, fs);
  glDeleteShader(vs);
  glDeleteShader(fs);
  return prog;
}

