// Win32 + Direct3D 11 host for the ImGui user interface.
#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <shellapi.h>

#include <string>
#include <vector>

#include "app.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "platform.h"

static HWND g_hwnd = nullptr;
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swap_chain = nullptr;
static ID3D11RenderTargetView* g_target = nullptr;
static bool g_occluded = false;
static UINT g_resize_w = 0, g_resize_h = 0;
static std::vector<ID3D11ShaderResourceView*> g_release_later;

static std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

namespace platform {

Texture create_texture(const uint8_t* rgba, int w, int h) {
    Texture t;
    if (!g_device || w <= 0 || h <= 0) return t;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = (UINT)w;
    desc.Height = (UINT)h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = rgba;
    data.SysMemPitch = (UINT)w * 4;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_device->CreateTexture2D(&desc, &data, &tex))) return t;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = desc.Format;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = g_device->CreateShaderResourceView(tex, &view, &srv);
    tex->Release();
    if (FAILED(hr)) return t;
    t.srv = srv;
    t.w = w;
    t.h = h;
    return t;
}

void destroy_texture(Texture& t) {
    if (t.srv) g_release_later.push_back((ID3D11ShaderResourceView*)t.srv);
    t = Texture{};
}

std::string open_file_dialog(const wchar_t* title, const wchar_t* filter) {
    wchar_t path[2048] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = 2048;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    return GetOpenFileNameW(&ofn) ? narrow(path) : std::string();
}

std::string save_file_dialog(const wchar_t* title, const wchar_t* filter, const wchar_t* default_ext, const wchar_t* default_name) {
    wchar_t path[2048] = L"";
    if (default_name) lstrcpynW(path, default_name, 2048);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrTitle = title;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = 2048;
    ofn.lpstrDefExt = default_ext;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    return GetSaveFileNameW(&ofn) ? narrow(path) : std::string();
}

FILE* fopen_utf8(const std::string& path, const char* mode) {
    return _wfopen(widen(path).c_str(), widen(mode).c_str());
}

}  // namespace platform

static void create_render_target() {
    ID3D11Texture2D* back = nullptr;
    g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_target);
        back->Release();
    }
}

static void cleanup_render_target() {
    if (g_target) {
        g_target->Release();
        g_target = nullptr;
    }
}

static bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                               &g_swap_chain, &g_device, &level, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                           &g_swap_chain, &g_device, &level, &g_context);
    if (hr != S_OK) return false;
    create_render_target();
    return true;
}

static void cleanup_device() {
    cleanup_render_target();
    if (g_swap_chain) { g_swap_chain->Release(); g_swap_chain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) return 0;
        g_resize_w = (UINT)LOWORD(lparam);
        g_resize_h = (UINT)HIWORD(lparam);
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wparam;
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; i++) {
            wchar_t path[2048];
            if (DragQueryFileW(drop, i, path, 2048)) app::drop_file(narrow(path));
        }
        DragFinish(drop);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wparam & 0xfff0) == SC_KEYMENU) return 0;  // no ALT application menu
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    {
        int argc = 0;
        wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::vector<std::string> args;
        for (int i = 1; argv && i < argc; i++) args.push_back(narrow(argv[i]));
        if (argv) LocalFree(argv);
        int rc = app::run_cli(args);
        if (rc >= 0) return rc;
    }

    ImGui_ImplWin32_EnableDpiAwareness();
    float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"PceVectorscope";
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"PCE Vectorscope and Palette Generator", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           (int)(1560 * scale), (int)(900 * scale), nullptr, nullptr, instance, nullptr);
    if (!create_device(g_hwnd)) {
        cleanup_device();
        UnregisterClassW(wc.lpszClassName, instance);
        MessageBoxW(nullptr, L"Could not start Direct3D 11.", L"PCE Vectorscope", MB_ICONERROR);
        return 1;
    }
    DragAcceptFiles(g_hwnd, TRUE);
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    const char* font = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* bold = "C:\\Windows\\Fonts\\segoeuib.ttf";  // the second font: app.cpp uses it for "Not Recommended"
    if (GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES) {
        io.Fonts->AddFontFromFileTTF(font, 17.0f);
        if (GetFileAttributesA(bold) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(bold, 17.0f);
    }
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    app::init(scale);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (g_occluded && g_swap_chain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(10);
            continue;
        }
        g_occluded = false;
        if (g_resize_w != 0 && g_resize_h != 0) {
            cleanup_render_target();
            g_swap_chain->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            create_render_target();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        app::frame();
        ImGui::Render();

        const float clear[4] = {0.06f, 0.06f, 0.07f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_target, nullptr);
        g_context->ClearRenderTargetView(g_target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        HRESULT hr = g_swap_chain->Present(1, 0);
        g_occluded = (hr == DXGI_STATUS_OCCLUDED);

        for (ID3D11ShaderResourceView* srv : g_release_later) srv->Release();
        g_release_later.clear();
    }

    app::shutdown();
    for (ID3D11ShaderResourceView* srv : g_release_later) srv->Release();
    g_release_later.clear();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanup_device();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, instance);
    return 0;
}
