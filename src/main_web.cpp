// Browser host for the ImGui user interface: Emscripten, GLFW (the contrib.glfw3 port) and WebGL 2.
// build_web.bat and the GitHub workflow build it; the page that comes out is what runs on github.io.
//
// A page cannot read or write the user's files by path. So:
//   - a file the user picks, or drops on the page, is copied into Emscripten's in-memory file system under /files,
//     and the app is given that path;
//   - a file the app saves is written under /save and handed to the browser as a download when the frame ends.
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES3/gl3.h>
#include <GLFW/glfw3.h>
#include <GLFW/emscripten_glfw3.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "app.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "platform.h"

namespace {

GLFWwindow* g_window = nullptr;
std::vector<GLuint> g_delete_later;
std::function<void(const std::string&)> g_then;  // what the app wants done with the file being picked
std::string g_download_path, g_download_name;    // a file the app is writing this frame, for the browser to save

// Wide text is UTF-32 here.
std::string narrow(const wchar_t* w) {
    std::string s;
    for (; w && *w; w++) {
        const uint32_t c = (uint32_t)*w;
        if (c < 0x80) s += (char)c;
        else if (c < 0x800) s += (char)(0xC0 | (c >> 6)), s += (char)(0x80 | (c & 63));
        else if (c < 0x10000) s += (char)(0xE0 | (c >> 12)), s += (char)(0x80 | ((c >> 6) & 63)), s += (char)(0x80 | (c & 63));
        else s += (char)(0xF0 | (c >> 18)), s += (char)(0x80 | ((c >> 12) & 63)), s += (char)(0x80 | ((c >> 6) & 63)), s += (char)(0x80 | (c & 63));
    }
    return s;
}

std::wstring widen(const std::string& s) {
    std::wstring w;
    for (size_t i = 0; i < s.size();) {
        const uint8_t b = (uint8_t)s[i];
        const int more = b < 0x80 ? 0 : b < 0xE0 ? 1 : b < 0xF0 ? 2 : 3;
        uint32_t c = more == 0 ? b : b & (0x3F >> more);
        for (int k = 1; k <= more && i + k < s.size(); k++) c = (c << 6) | ((uint8_t)s[i + k] & 63);
        w += (wchar_t)c;
        i += (size_t)more + 1;
    }
    return w;
}

}  // namespace

// ---------------------------------------------------------------- the page's side
EM_JS_DEPS(pce_page, "$FS,$UTF8ToString,$stringToNewUTF8");

// Makes the two folders, a hidden file box, and the handlers for files dropped on the page. A file that arrives is
// written under /files and its path put on a list: "P/files/name" when picked, "D/files/name" when dropped.
EM_JS(void, web_setup, (), {
    const page = { arrived: [] };
    Module.pce = page;
    for (const folder of ['/files', '/save']) {
        try { FS.mkdir(folder); } catch (e) {}
    }
    page.take = async (files, how) => {
        for (const file of files) {
            const data = new Uint8Array(await file.arrayBuffer());
            const path = '/files/' + file.name;
            FS.writeFile(path, data);
            page.arrived.push(how + path);
        }
    };
    const box = document.createElement('input');
    box.type = 'file';
    box.style.display = 'none';
    box.addEventListener('change', () => {
        page.take(Array.from(box.files), 'P');
        box.value = "";
    });
    document.body.appendChild(box);
    page.box = box;
    document.addEventListener('dragover', (e) => { e.preventDefault(); });
    document.addEventListener('drop', (e) => {
        e.preventDefault();
        if (e.dataTransfer) page.take(Array.from(e.dataTransfer.files), 'D');
    });
});

// Opens the browser's file box. accept: ".png,.jpg", or "" for any file.
EM_JS(void, web_pick, (const char* accept), {
    const box = Module.pce.box;
    box.accept = UTF8ToString(accept);
    box.click();
});

// The next file that has arrived, or null. The caller frees it.
EM_JS(char*, web_next_file, (), {
    const next = Module.pce.arrived.shift();
    return next ? stringToNewUTF8(next) : 0;
});

// Gives a file from the in-memory file system to the browser to save, and removes it from there.
EM_JS(int, web_download, (const char* path, const char* name), {
    const from = UTF8ToString(path);
    let data;
    try {
        data = FS.readFile(from);
        FS.unlink(from);
    } catch (e) {
        return 0;
    }
    const link = document.createElement('a');
    link.href = URL.createObjectURL(new Blob([data], { type: 'application/octet-stream' }));
    link.download = UTF8ToString(name);
    document.body.appendChild(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(link.href), 60000);
    return 1;
});

// ---------------------------------------------------------------- what the app asks of its host
namespace platform {

Texture create_texture(const uint8_t* rgba, int w, int h) {
    Texture t;
    GLint limit = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
    if (!rgba || w <= 0 || h <= 0 || w > limit || h > limit) return t;
    GLint bound = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glBindTexture(GL_TEXTURE_2D, (GLuint)bound);
    t.srv = (void*)(intptr_t)id;
    t.w = w;
    t.h = h;
    return t;
}

void destroy_texture(Texture& t) {
    if (t.srv) g_delete_later.push_back((GLuint)(intptr_t)t.srv);  // after this frame has been drawn
    t = Texture{};
}

void open_file(const wchar_t*, const wchar_t* filter, std::function<void(const std::string& path)> then) {
    // The patterns of the filter, "*.png;*.jpg", as the list a file box takes: ".png,.jpg". "*.*" is left out.
    std::string accept;
    for (const wchar_t* p = filter; p && *p;) {
        while (*p) p++;  // the name of this kind of file
        p++;
        const std::string patterns = narrow(p);
        while (*p) p++;
        p++;
        for (size_t at = 0; at < patterns.size();) {
            size_t end = patterns.find(';', at);
            if (end == std::string::npos) end = patterns.size();
            const std::string one = patterns.substr(at, end - at);
            at = end + 1;
            if (one.size() > 2 && one[0] == '*' && one[1] == '.' && one != "*.*") accept += (accept.empty() ? "" : ",") + one.substr(1);
        }
    }
    g_then = std::move(then);
    web_pick(accept.c_str());
}

std::string save_file_dialog(const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t* default_name) {
    std::string name = narrow(default_name);
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    if (name.empty()) name = "pce_vectorscope_file";
    g_download_name = name;
    g_download_path = "/save/" + name;
    remove(g_download_path.c_str());
    return g_download_path;
}

FILE* fopen_utf8(const std::string& path, const char* mode) { return fopen(path.c_str(), mode); }

std::wstring to_wide(const std::string& utf8) { return widen(utf8); }

}  // namespace platform

// ---------------------------------------------------------------- the frame
static void frame() {
    glfwPollEvents();

    // Files that have arrived since the last frame: a picked one goes to whoever asked for it, a dropped one to the
    // app to work out, as on Windows.
    while (char* next = web_next_file()) {
        const bool picked = next[0] == 'P';
        const std::string path = next + 1;
        free(next);
        if (picked && g_then) {
            std::function<void(const std::string&)> then = std::move(g_then);
            g_then = nullptr;
            then(path);
        } else
            app::drop_file(path);
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    app::frame();
    ImGui::Render();

    int w = 0, h = 0;
    glfwGetFramebufferSize(g_window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.06f, 0.06f, 0.07f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if (!g_delete_later.empty()) {
        glDeleteTextures((GLsizei)g_delete_later.size(), g_delete_later.data());
        g_delete_later.clear();
    }
    if (!g_download_path.empty()) {  // the app has written it by now, or decided not to
        web_download(g_download_path.c_str(), g_download_name.c_str());
        g_download_path.clear();
    }
}

int main(int argc, char** argv) {
    // The page passes on what its address asks for, such as ?view=pictures, as command line options.
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.push_back(argv[i]);
    app::run_cli(args);

    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_SCALE_FRAMEBUFFER, GLFW_TRUE);  // as many pixels as the screen has, not as the page counts
    emscripten_glfw_set_next_window_canvas_selector("#canvas");
    g_window = glfwCreateWindow(1280, 800, "PCE Vectorscope and Palette Generator", nullptr, nullptr);
    if (!g_window) return 1;
    glfwMakeContextCurrent(g_window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    io.Fonts->AddFontFromFileTTF("/DroidSans.ttf", 16.0f);  // put into the program's file system by the build
    ImGui_ImplGlfw_InitForOpenGL(g_window, true);
    ImGui_ImplGlfw_InstallEmscriptenCallbacks(g_window, "#canvas");  // the canvas follows the size of the page
    ImGui_ImplOpenGL3_Init("#version 300 es");

    web_setup();
    app::init(1.0f);  // sizes are in the page's own units; the browser scales them to the screen
    emscripten_set_main_loop(frame, 0, true);
    return 0;
}
