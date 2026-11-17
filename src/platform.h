// The few things the UI needs from its host: Windows and Direct3D 11 (main.cpp), or a browser and WebGL (main_web.cpp).
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

namespace platform {

struct Texture {
    void* srv = nullptr;  // ID3D11ShaderResourceView*, or the name of an OpenGL texture
    int w = 0, h = 0;
    explicit operator bool() const { return srv != nullptr; }
};

Texture create_texture(const uint8_t* rgba, int w, int h);
// Safe to call while the texture is still referenced by this frame's draw lists.
void destroy_texture(Texture& t);

// Filters use the Win32 form: L"Palette (*.pal)\0*.pal\0". Paths are UTF-8.

// Asks for a file to open and calls `then` with its path; not at all when the user cancels. A browser has to read
// the file first, so there `then` is called some frames later, with a path in the browser's own file system.
void open_file(const wchar_t* title, const wchar_t* filter, std::function<void(const std::string& path)> then);

// Returns "" when cancelled. A browser does not ask: whatever is written to the path it returns is downloaded,
// under the default name, when the frame ends.
std::string save_file_dialog(const wchar_t* title, const wchar_t* filter, const wchar_t* default_ext, const wchar_t* default_name);

FILE* fopen_utf8(const std::string& path, const char* mode);
std::wstring to_wide(const std::string& utf8);

}  // namespace platform
