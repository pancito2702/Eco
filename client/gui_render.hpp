// What eco-gui needs from the graphics system. Two implementations:
//   gui_render_d3d11.cpp  Windows (Direct3D 11: the driver uses much less memory than with OpenGL)
//   gui_render_gl.cpp     Linux and macOS (OpenGL 3.2)
#pragma once
#include "imgui.h"

struct GLFWwindow;

namespace eco {
struct Image;

namespace render {
void windowHints();               // before glfwCreateWindow
bool init(GLFWwindow* win);       // after creating the window and the ImGui context
void newFrame();
void present(GLFWwindow* win);    // draws ImGui's content and presents it
void shutdown();

// A single texture for the screen being watched (created or resized by itself)
ImTextureID updateTexture(const Image& img);
void releaseTexture();
}  // namespace render

}  // namespace eco
