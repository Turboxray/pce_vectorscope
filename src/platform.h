// The few things the UI needs from Windows and Direct3D.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

namespace platform {

struct Texture {
    void* srv = nullptr;  // ID3D11ShaderResourceView*
    int w = 0, h = 0;
    explicit operator bool() const { return srv != nullptr; }
};

Texture create_texture(const uint8_t* rgba, int w, int h);
// Safe to call while the texture is still referenced by this frame's draw lists.
void destroy_texture(Texture& t);

// Filters use the Win32 form: L"Palette (*.pal)\0*.pal\0". Both return "" when cancelled. Paths are UTF-8.
std::string open_file_dialog(const wchar_t* title, const wchar_t* filter);
std::string save_file_dialog(const wchar_t* title, const wchar_t* filter, const wchar_t* default_ext, const wchar_t* default_name);

FILE* fopen_utf8(const std::string& path, const char* mode);

}  // namespace platform
