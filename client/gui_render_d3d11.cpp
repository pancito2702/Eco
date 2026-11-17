// eco-gui rendering with Direct3D 11 (Windows). GLFW only provides the window and the input.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <cstring>

#include "imgui_impl_dx11.h"
#include "imgui_impl_glfw.h"

#include "gui_render.hpp"
#include "screen.hpp"

namespace eco::render {

namespace {
ID3D11Device* dev = nullptr;
ID3D11DeviceContext* ctx = nullptr;
IDXGISwapChain* swap = nullptr;
ID3D11RenderTargetView* rtv = nullptr;
int bufW = 0, bufH = 0;

ID3D11Texture2D* tex = nullptr;
ID3D11ShaderResourceView* srv = nullptr;
int texW = 0, texH = 0;

template <class T>
void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void createRT() {
    ID3D11Texture2D* back = nullptr;
    swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        dev->CreateRenderTargetView(back, nullptr, &rtv);
        back->Release();
    }
}
}  // namespace

void windowHints() { glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); }

bool init(GLFWwindow* win) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = glfwGetWin32Window(win);
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &sd, &swap, &dev, &got, &ctx);
    if (FAILED(hr))  // no usable GPU (remote desktop, VM...): software rendering
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                           &swap, &dev, &got, &ctx);
    if (FAILED(hr)) return false;
    createRT();
    glfwGetFramebufferSize(win, &bufW, &bufH);
    return ImGui_ImplGlfw_InitForOther(win, true) && ImGui_ImplDX11_Init(dev, ctx);
}

void newFrame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplGlfw_NewFrame();
}

void present(GLFWwindow* win) {
    int w, h;
    glfwGetFramebufferSize(win, &w, &h);
    if (w > 0 && h > 0 && (w != bufW || h != bufH)) {
        release(rtv);
        swap->ResizeBuffers(0, UINT(w), UINT(h), DXGI_FORMAT_UNKNOWN, 0);
        createRT();
        bufW = w;
        bufH = h;
    }
    const float clear[4] = {0.105f, 0.115f, 0.13f, 1.f};
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->ClearRenderTargetView(rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swap->Present(1, 0);
}

void shutdown() {
    releaseTexture();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    release(rtv);
    release(swap);
    release(ctx);
    release(dev);
}

ImTextureID updateTexture(const Image& img) {
    if (!tex || texW != img.w || texH != img.h) {
        releaseTexture();
        D3D11_TEXTURE2D_DESC d{};
        d.Width = UINT(img.w);
        d.Height = UINT(img.h);
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;  // the same order the image comes in: no conversion
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DYNAMIC;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateTexture2D(&d, nullptr, &tex))) return 0;
        if (FAILED(dev->CreateShaderResourceView(tex, nullptr, &srv))) { release(tex); return 0; }
        texW = img.w;
        texH = img.h;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        size_t row = size_t(img.w) * 4;
        for (int y = 0; y < img.h; ++y)
            std::memcpy(static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, img.bgra.data() + y * row, row);
        ctx->Unmap(tex, 0);
    }
    return (ImTextureID)(intptr_t)srv;
}

void releaseTexture() {
    release(srv);
    release(tex);
    texW = texH = 0;
}

}  // namespace eco::render
