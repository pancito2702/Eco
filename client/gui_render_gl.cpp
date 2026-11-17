// eco-gui rendering with OpenGL 3.2 (Linux and macOS)
#define GL_SILENCE_DEPRECATION
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include "gui_render.hpp"
#include "screen.hpp"

namespace eco::render {

namespace {
GLuint tex = 0;
}

void windowHints() {
    // 3.2 core: the minimum macOS accepts; on Linux almost any GPU works
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
}

bool init(GLFWwindow* win) {
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);
    return ImGui_ImplGlfw_InitForOpenGL(win, true) && ImGui_ImplOpenGL3_Init("#version 150");
}

void newFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
}

void present(GLFWwindow* win) {
    int fw, fh;
    glfwGetFramebufferSize(win, &fw, &fh);
    glViewport(0, 0, fw, fh);
    glClearColor(0.105f, 0.115f, 0.13f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(win);
}

void shutdown() {
    releaseTexture();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
}

ImTextureID updateTexture(const Image& img) {
    if (!tex) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.w, img.h, 0, GL_BGRA, GL_UNSIGNED_BYTE, img.bgra.data());
    return (ImTextureID)(intptr_t)tex;
}

void releaseTexture() {
    if (tex) glDeleteTextures(1, &tex);
    tex = 0;
}

}  // namespace eco::render
