// User interface: model controls, vectorscope, waveform monitor and palette grid.
#include "app.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"  // SeparatorTextEx
#include "platform.h"
#include "stb_image.h"
#include "stb_image_write.h"
#include "vce_model.h"

// assets/vector_scope_overlay__v4.png, compiled in by cmake/embed.cmake
extern const unsigned char graticule_png[];
extern const size_t graticule_png_size;

namespace {

const double PI = 3.14159265358979323846;
float g_scale = 1.0f;
float S(float v) { return v * g_scale; }

// ---------------------------------------------------------------- state
vce::Params g_params;
vce::Palette g_pal;
bool g_dirty = true;

struct Group {  // palette entries that share one point on the scope
    int by, ry;
    double u, v;
    ImU32 col;
    std::vector<int> members;
};
std::vector<Group> g_groups;

bool g_have_ref = false;
uint8_t g_ref[512][3];
std::string g_ref_name, g_ref_path;
double g_ref_mean = 0;
int g_ref_max = 0, g_ref_max_index = 0;
bool g_compare = true;

int g_sort = 0, g_sort_built = -1;
bool g_grey_map = false;  // the 512 grid shows each colour as its luma alone
std::vector<int> g_order;
int g_selected = -1;
int g_hot = -1, g_hot_prev = -1;  // entry under the mouse: this frame, and last frame (used to draw highlights)
std::string g_status;

// The graticule is the overlay image from the older Python tool, a tracing of an NTSC vectorscope graticule.
// Measured on the image: the six box centres sit at the standard colour-bar angles within 0.2 degrees, at 418.5
// pixels per unit of U/V (0.2% spread), and the outer circle has a radius of 316.8 pixels. On the instrument the
// red box is at 43.9 IRE with burst at 40 IRE p-p (its own burst marks agree), so one unit of U/V is 69.375 IRE
// and the circle is at 52.5 IRE. The dots in the photo of the real scope (IMG_2902.jpg), measured against its
// boxes, give 2.15 IRE per B-Y count on this scale; measured against its burst they give 2.14.
const double GRAT_CX = 319.73, GRAT_CY = 319.52;  // centre of the image, in pixels from its top left corner
const double GRAT_PX_PER_IRE = 418.5 / 69.375;    // image pixels per IRE
const double GRAT_VIEW_IRE = 53.5;                // centre to the edge of the view at zoom 1: the whole image
// RGB data (a palette, a picture) is plotted at this many IRE per unit of U/V: the scale at which 75% RGB bars
// land in the boxes (0.75 * 92.5 = 69.375). That is what a vectorscope fed from an RGB source shows.
const double RGB_UNIT_IRE = 92.5;
// On the waveform, RGB data is drawn with black at 0 IRE and full white at 100, and its chroma to the same scale.
// With max white at 100 IRE and black level at 0, luma then sits where the console's own trace has it.
const double RGB_WAVE_IRE = 100.0;
platform::Texture g_graticule;

struct ScopeView {
    float zoom = 1.0f;
    float dot = 4.5f;
    float graticule = 0.85f;  // brightness of the graticule image; 0 hides it
    int bars = 0;             // scope gain: 0 = 75% bars land in the boxes, 1 = 100% bars do
    bool console = true, post = false, ref = true, image = true, photo = true, labels = true;
} g_view;

struct Photo {  // a picture of a real vectorscope, shown behind the plot
    platform::Texture tex;
    std::string name, path;
    float x = 0, y = 0, width = 170, rot = 0, opacity = 0.55f;
    bool mirror = false;
} g_photo;

const int HIST_N = 768;
const double HIST_EXTENT = 0.66;  // U and V at the edge of the image-plot texture
struct ImagePlot {
    std::string name, path;
    std::vector<uint32_t> colours, counts;
    uint64_t total = 0;
    platform::Texture tex;
    float intensity = 1.0f;
    int splat = 2;
    bool dirty = false;
    // For the waveform tab. Pixel counts by 8-bit level: the pure greys (R = G = B), and every pixel by its luma.
    std::array<uint32_t, 256> greys{}, lumas{};
    std::vector<uint32_t> parade;  // R, G and B against position across the picture: [channel][column][level]
    int parade_cols = 0;
    platform::Texture parade_tex;
} g_image;

std::vector<std::string> g_pending_images;  // dropped on the window; the user is asked what they are for
bool g_open_drop_popup = false;

std::vector<std::pair<std::string, std::string>> g_startup;  // (action, value) pairs from the command line
int g_start_view = -1;  // the tab to switch to on the next frame: 0 vectorscope, 1 waveform, 2 reference image
int g_layout = 0;       // 0: those three as tabs in the middle. 1: the pictures in the middle, the rest reduced beside them

// A reference image is a picture in plain 9-bit RGB: what the console is asked to show. It is prepared when loaded:
// every pixel is taken to its 9-bit colour, whatever the size of step in the file, and the picture is kept that way.
// Its tab shows it so, and as the palette makes it. Several can be loaded; one is shown at a time.
const uint16_t NO_COLOUR = 0xFFFF;
const uint8_t NINE_BIT[8] = {0, 36, 73, 109, 146, 182, 219, 255};  // the eight levels of 9-bit RGB, as 0 to 255
struct SourceImage {
    std::string name, path;
    int w = 0, h = 0;
    double step = 0;              // size of one level in the file: 32 to 36.5
    bool exact = true;            // false: it was not 9-bit RGB, and each colour was taken as the nearest of the 512
    int used = 0;                 // how many of the 512 colours it has
    std::vector<uint8_t> rgba;    // the prepared picture: each channel one of NINE_BIT, alpha 0 or 255
    std::vector<uint16_t> index;  // per pixel: G<<6 | R<<3 | B, or NO_COLOUR where the picture is transparent
};
std::vector<SourceImage> g_sources;

// A snapshot is a set of settings kept under a name, to compare later settings with. They last until the program
// closes, or go into a project file.
struct Snapshot {
    std::string name;
    vce::Params params;
    vce::Palette pal;
};
std::vector<Snapshot> g_snapshots;
int g_snapshot_serial = 0;  // for the name suggested for the next one

struct ImageView {
    int current = -1;    // which reference image is shown
    int assigned = 0;    // the snapshot picked from the list, counted from 1; 0 = none
    int against = 0;     // the source picture: 0 = the prepared image, otherwise the image through the assigned snapshot
    int mode = 0;       // 0: both pictures, side by side or one above the other. 1: one picture that flips between them
    bool flipped = false;  // flipping: the first picture is the one showing
    platform::Texture original, snapshot, mapped;
    bool dirty_original = false, dirty_snapshot = false, dirty_mapped = false;
    bool fit = true;     // the whole picture, whatever the size of the window
    float scale = 1.0f;  // screen pixels per pixel of the image
    float cx = 0, cy = 0;  // the point of the image at the centre of each picture
} g_imgview;

std::string g_project_path;  // the project file last opened or saved

enum { PATTERN_BARS, PATTERN_GREYS, PATTERN_LUMA, PATTERN_RAMPS, PATTERN_PARADE };  // the parade is of a plotted image
struct WaveView {
    int pattern = PATTERN_BARS, level = 7, shape = 0;
    bool console = true, post = false, image = true;
    bool lowpass = false;
} g_wave;

// ---------------------------------------------------------------- helpers
std::string base_name(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? path : path.substr(p + 1);
}

std::string extension(const std::string& path) {
    size_t p = path.find_last_of('.');
    if (p == std::string::npos) return "";
    std::string e = path.substr(p);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

// Files that came inside a project file, by the path the project names them with. A file that can be read from disk
// is read from there; one that cannot is looked for here. That is what lets a project open away from its pictures:
// on another machine, or in the browser, which cannot read a file by its path at all.
std::map<std::string, std::vector<uint8_t>> g_embedded;

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = platform::fopen_utf8(path, "rb");
    if (!f) {
        auto it = g_embedded.find(path);
        if (it == g_embedded.end()) return false;
        out = it->second;
        return true;
    }
    // Straight into the vector: a large buffer on the stack does not fit the small stack of the browser build.
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? (size_t)size : 0);
    const size_t got = out.empty() ? 0 : fread(out.data(), 1, out.size(), f);
    fclose(f);
    out.resize(got);
    return true;
}

bool write_file(const std::string& path, const void* data, size_t n) {
    FILE* f = platform::fopen_utf8(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
}

// A picture file as RGBA, or nullptr. Free it with stbi_image_free.
uint8_t* load_pixels(const std::string& path, int* w, int* h) {
    std::vector<uint8_t> file;
    int channels;
    if (!read_file(path, file) || file.empty()) return nullptr;
    return stbi_load_from_memory(file.data(), (int)file.size(), w, h, &channels, 4);
}

std::string base64(const std::vector<uint8_t>& in) {
    static const char* const digits = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const bool two = i + 1 < in.size(), three = i + 2 < in.size();
        const uint32_t v = ((uint32_t)in[i] << 16) | (two ? (uint32_t)in[i + 1] << 8 : 0u) | (three ? (uint32_t)in[i + 2] : 0u);
        out += digits[(v >> 18) & 63];
        out += digits[(v >> 12) & 63];
        out += two ? digits[(v >> 6) & 63] : '=';
        out += three ? digits[v & 63] : '=';
    }
    return out;
}

std::vector<uint8_t> unbase64(const std::string& in) {
    std::vector<uint8_t> out;
    out.reserve(in.size() / 4 * 3);
    uint32_t v = 0;
    int bits = 0;
    for (char c : in) {
        const int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (d < 0) continue;  // the padding at the end, or anything else that is not a digit
        v = (v << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)(v >> bits));
        }
    }
    return out;
}

int index_of(int r, int g, int b) { return (g << 6) | (r << 3) | b; }
ImU32 entry_colour(int i) { return IM_COL32(g_pal.e[i].rgb[0], g_pal.e[i].rgb[1], g_pal.e[i].rgb[2], 255); }

double phase_deg(double u, double v) {
    double a = std::atan2(v, u) * 180.0 / PI;
    return a < 0 ? a + 360.0 : a;
}

void load_graticule() {
    int w, h, n;
    uint8_t* data = stbi_load_from_memory(graticule_png, (int)graticule_png_size, &w, &h, &n, 4);
    if (!data) return;
    for (int i = 0; i < w * h; i++) {  // white lines on black become white with the line brightness as alpha
        uint8_t* p = data + (size_t)i * 4;
        p[3] = std::max(p[0], std::max(p[1], p[2]));
        p[0] = p[1] = p[2] = 255;
    }
    g_graticule = platform::create_texture(data, w, h);
    stbi_image_free(data);
}

void compare_with_reference() {
    double sum = 0;
    g_ref_max = 0;
    g_ref_max_index = 0;
    for (int i = 0; i < 512; i++)
        for (int k = 0; k < 3; k++) {
            int d = std::abs((int)g_pal.e[i].rgb[k] - (int)g_ref[i][k]);
            sum += d;
            if (d > g_ref_max) {
                g_ref_max = d;
                g_ref_max_index = i;
            }
        }
    g_ref_mean = sum / (512 * 3);
}

void build_order() {
    g_order.resize(512);
    for (int i = 0; i < 512; i++) g_order[i] = i;
    auto hue = [](int i) {
        const vce::Entry& e = g_pal.e[i];
        return (e.by == 0 && e.ry == 0) ? -1.0 : phase_deg(e.u_ire, e.v_ire);
    };
    if (g_sort == 1)
        std::stable_sort(g_order.begin(), g_order.end(), [&](int a, int b) {
            if (g_pal.e[a].y_code != g_pal.e[b].y_code) return g_pal.e[a].y_code < g_pal.e[b].y_code;
            return hue(a) < hue(b);
        });
    else if (g_sort == 2)
        std::stable_sort(g_order.begin(), g_order.end(), [&](int a, int b) {
            if (hue(a) != hue(b)) return hue(a) < hue(b);
            return g_pal.e[a].y_code < g_pal.e[b].y_code;
        });
    g_sort_built = g_sort;
}

void rebuild() {
    g_pal = vce::build(g_params);
    std::map<std::pair<int, int>, int> where;
    g_groups.clear();
    std::vector<std::array<double, 3>> sums;
    for (int i = 0; i < 512; i++) {
        const vce::Entry& e = g_pal.e[i];
        auto key = std::make_pair(e.by, e.ry);
        auto it = where.find(key);
        if (it == where.end()) {
            it = where.emplace(key, (int)g_groups.size()).first;
            g_groups.push_back(Group{e.by, e.ry, e.u_ire, e.v_ire, 0, {}});
            sums.push_back({0, 0, 0});
        }
        g_groups[it->second].members.push_back(i);
        for (int k = 0; k < 3; k++) sums[it->second][k] += e.rgb[k];
    }
    for (size_t n = 0; n < g_groups.size(); n++) {
        double m = (double)g_groups[n].members.size();
        g_groups[n].col = IM_COL32((int)(sums[n][0] / m), (int)(sums[n][1] / m), (int)(sums[n][2] / m), 255);
    }
    if (g_have_ref) compare_with_reference();
    g_sort_built = -1;
    g_imgview.dirty_mapped = true;
    g_dirty = false;
}

// ---------------------------------------------------------------- files
bool load_reference(const std::string& path) {
    std::vector<uint8_t> data;
    if (!read_file(path, data) || data.size() < 1536) {
        g_status = "Not a 512-colour .pal file: " + base_name(path);
        return false;
    }
    std::memcpy(g_ref, data.data(), 1536);
    g_have_ref = true;
    g_ref_name = base_name(path);
    g_ref_path = path;
    compare_with_reference();
    g_status = "Reference palette: " + g_ref_name;
    return true;
}

std::vector<uint8_t> pal_bytes(const vce::Palette& pal) {
    std::vector<uint8_t> out(1536);
    for (int i = 0; i < 512; i++)
        for (int k = 0; k < 3; k++) out[i * 3 + k] = pal.e[i].rgb[k];
    return out;
}

bool save_swatch_png(const vce::Palette& pal, const std::string& path) {
    const int cols = 32, rows = 16, cell = 16;
    std::vector<uint8_t> px((size_t)cols * cell * rows * cell * 3);
    for (int y = 0; y < rows * cell; y++)
        for (int x = 0; x < cols * cell; x++) {
            const vce::Entry& e = pal.e[(y / cell) * cols + x / cell];
            std::memcpy(&px[((size_t)y * cols * cell + x) * 3], e.rgb, 3);
        }
    return stbi_write_png(path.c_str(), cols * cell, rows * cell, 3, px.data(), cols * cell * 3) != 0;
}

void load_photo(const std::string& path);

// A file inside `folder` is written by its path from there, so a project and its pictures can be moved together.
std::string path_within(const std::string& file, const std::string& folder) {
    if (folder.empty() || file.size() <= folder.size()) return file;
    for (size_t i = 0; i < folder.size(); i++)  // without regard to case, as Windows paths are
        if (std::tolower((unsigned char)file[i]) != std::tolower((unsigned char)folder[i])) return file;
    return file.substr(folder.size());
}

// The parameter file also carries the alignment of the scope photo, so it does not have to be redone.
// A project names the photo by its path from the project's own folder.
std::string params_text(const std::string& project_folder = "") {
    std::string text = vce::to_text(g_params);
    if (g_photo.tex) {
        char line[128];
        snprintf(line, sizeof line, "photo_x=%.3f\nphoto_y=%.3f\nphoto_width=%.3f\nphoto_rot=%.3f\nphoto_mirror=%d\n", g_photo.x, g_photo.y,
                 g_photo.width, g_photo.rot, g_photo.mirror ? 1 : 0);
        text += line;
        text += "photo_file=" + path_within(g_photo.path, project_folder) + "\n";
    }
    return text;
}

std::string folder_of(const std::string& path) {  // with its last slash; "" for a bare file name
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? "" : path.substr(0, p + 1);
}

bool is_absolute(const std::string& path) { return path.size() > 1 && (path[1] == ':' || path[0] == '/' || path[0] == '\\'); }

// The settings in the text of a parameter file and, with_photo, the scope photo and its alignment.
// A photo named without a folder is looked for in `folder`.
bool params_from_text(const std::string& text, const std::string& folder, vce::Params& p, bool with_photo) {
    if (!vce::from_text(text, p)) return false;
    if (!with_photo) return true;
    size_t pos = 0;
    std::string photo_file;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos || line.compare(0, 6, "photo_") != 0) continue;
        std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "photo_file") photo_file = value;
        else if (key == "photo_x") g_photo.x = (float)std::atof(value.c_str());
        else if (key == "photo_y") g_photo.y = (float)std::atof(value.c_str());
        else if (key == "photo_width") g_photo.width = (float)std::atof(value.c_str());
        else if (key == "photo_rot") g_photo.rot = (float)std::atof(value.c_str());
        else if (key == "photo_mirror") g_photo.mirror = std::atoi(value.c_str()) != 0;
    }
    if (!photo_file.empty()) {
        // A bare file name is looked for beside the parameter file, so a capture and its alignment travel together.
        if (!is_absolute(photo_file)) photo_file = folder + photo_file;
        if (photo_file != g_photo.path) load_photo(photo_file);
    }
    return true;
}

bool load_params(const std::string& path, vce::Params& p, bool with_photo = false) {
    std::vector<uint8_t> data;
    if (!read_file(path, data)) return false;
    return params_from_text(std::string(data.begin(), data.end()), folder_of(path), p, with_photo);
}

void load_image_plot(const std::string& path) {
    int w, h;
    uint8_t* data = load_pixels(path, &w, &h);
    if (!data) {
        g_status = "Could not read image: " + base_name(path);
        return;
    }
    size_t total = (size_t)w * h, step = 1;
    while (total / step > 6000000) step++;
    std::unordered_map<uint32_t, uint32_t> seen;
    seen.reserve(1 << 16);
    g_image.total = 0;
    g_image.greys.fill(0);
    g_image.lumas.fill(0);
    const int cols = std::min(w, 512);
    g_image.parade_cols = cols;
    g_image.parade.assign((size_t)3 * cols * 256, 0);
    for (size_t i = 0; i < total; i += step) {
        const uint8_t* p = data + i * 4;
        if (p[3] < 128) continue;
        seen[(uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)]++;
        g_image.total++;
        if (p[0] == p[1] && p[1] == p[2]) g_image.greys[p[0]]++;
        g_image.lumas[std::clamp((int)std::lround(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]), 0, 255)]++;
        size_t col = (i % (size_t)w) * cols / w;
        for (int c = 0; c < 3; c++) g_image.parade[((size_t)c * cols + col) * 256 + p[c]]++;
    }
    stbi_image_free(data);
    g_image.colours.clear();
    g_image.counts.clear();
    for (auto& kv : seen) {
        g_image.colours.push_back(kv.first);
        g_image.counts.push_back(kv.second);
    }
    g_image.name = base_name(path);
    g_image.path = path;
    g_image.dirty = true;
    g_view.image = true;
    // On the waveform tab the picture shows with the greys, the luma codes or as a parade, not with the test patterns.
    g_wave.image = true;
    if (g_wave.pattern == PATTERN_BARS || g_wave.pattern == PATTERN_RAMPS) {
        bool any_grey = std::any_of(g_image.greys.begin(), g_image.greys.end(), [](uint32_t n) { return n > 0; });
        g_wave.pattern = any_grey ? PATTERN_GREYS : PATTERN_PARADE;
    }
    char msg[256];
    snprintf(msg, sizeof msg, "%s: %d x %d, %d distinct colours", g_image.name.c_str(), w, h, (int)g_image.colours.size());
    g_status = msg;
}

void remove_image() {
    g_image.colours.clear();
    g_image.counts.clear();
    g_image.greys.fill(0);
    g_image.lumas.fill(0);
    g_image.parade.clear();
    g_image.parade_cols = 0;
    platform::destroy_texture(g_image.tex);
    platform::destroy_texture(g_image.parade_tex);
    if (g_wave.pattern == PATTERN_PARADE) g_wave.pattern = PATTERN_BARS;
}

// The parade as a picture: three panels side by side, level 255 in the top row. A level shows brighter the larger
// the share of its column's pixels that sit on it.
void build_parade_texture() {
    platform::destroy_texture(g_image.parade_tex);
    const int cols = g_image.parade_cols;
    if (cols <= 0 || g_image.parade.empty()) return;
    static const uint8_t TINT[3][3] = {{255, 95, 95}, {95, 255, 115}, {115, 155, 255}};
    std::vector<uint8_t> px((size_t)3 * cols * 256 * 4);
    for (int row = 0; row < 256; row++)
        for (int c = 0; c < 3; c++)
            for (int j = 0; j < cols; j++) {  // colour everywhere, so the edges of a trace do not blend towards black
                uint8_t* p = &px[((size_t)row * 3 * cols + (size_t)c * cols + j) * 4];
                p[0] = TINT[c][0], p[1] = TINT[c][1], p[2] = TINT[c][2], p[3] = 0;
            }
    for (int c = 0; c < 3; c++)
        for (int j = 0; j < cols; j++) {
            const uint32_t* levels = &g_image.parade[((size_t)c * cols + j) * 256];
            uint64_t n = 0;
            for (int v = 0; v < 256; v++) n += levels[v];
            if (n == 0) continue;
            for (int v = 0; v < 256; v++) {
                if (levels[v] == 0) continue;
                double alpha = 0.30 + 0.70 * (1.0 - std::exp(-30.0 * levels[v] / (double)n));
                px[((size_t)(255 - v) * 3 * cols + (size_t)c * cols + j) * 4 + 3] = (uint8_t)(255.0 * alpha);
            }
        }
    g_image.parade_tex = platform::create_texture(px.data(), 3 * cols, 256);
}

void build_image_texture() {
    g_image.dirty = false;
    build_parade_texture();
    platform::destroy_texture(g_image.tex);
    if (g_image.colours.empty()) return;
    const int N = HIST_N;
    std::vector<float> acc((size_t)N * N * 4, 0.0f);  // weight, r, g, b
    int rad = g_image.splat;
    for (size_t n = 0; n < g_image.colours.size(); n++) {
        uint32_t c = g_image.colours[n];
        uint8_t rgb[3] = {(uint8_t)(c & 255), (uint8_t)((c >> 8) & 255), (uint8_t)((c >> 16) & 255)};
        double y, u, v;
        vce::rgb_to_yuv(rgb, y, u, v);
        int cx = (int)((u / HIST_EXTENT * 0.5 + 0.5) * N), cy = (int)((0.5 - v / HIST_EXTENT * 0.5) * N);
        float wgt = (float)g_image.counts[n];
        for (int dy = -rad; dy <= rad; dy++)
            for (int dx = -rad; dx <= rad; dx++) {
                if (dx * dx + dy * dy > rad * rad + rad) continue;
                int x = cx + dx, yy = cy + dy;
                if (x < 0 || yy < 0 || x >= N || yy >= N) continue;
                float* a = &acc[((size_t)yy * N + x) * 4];
                a[0] += wgt;
                a[1] += wgt * rgb[0];
                a[2] += wgt * rgb[1];
                a[3] += wgt * rgb[2];
            }
    }
    std::vector<uint8_t> px((size_t)N * N * 4, 0);
    double norm = g_image.intensity * 3000.0 / (double)std::max<uint64_t>(1, g_image.total);
    for (size_t i = 0; i < (size_t)N * N; i++) {
        const float* a = &acc[i * 4];
        if (a[0] <= 0) continue;
        float r = a[1] / a[0], g = a[2] / a[0], b = a[3] / a[0];
        float m = std::max(r, std::max(g, b));
        if (m < 110.0f) {  // dark colours would vanish against the scope background
            float k = 110.0f / std::max(m, 12.0f);
            r = std::min(255.0f, r * k + 20.0f);
            g = std::min(255.0f, g * k + 20.0f);
            b = std::min(255.0f, b * k + 20.0f);
        }
        double alpha = 0.35 + 0.65 * (1.0 - std::exp(-a[0] * norm));
        px[i * 4 + 0] = (uint8_t)r;
        px[i * 4 + 1] = (uint8_t)g;
        px[i * 4 + 2] = (uint8_t)b;
        px[i * 4 + 3] = (uint8_t)(255.0 * alpha);
    }
    g_image.tex = platform::create_texture(px.data(), N, N);
}

void load_photo(const std::string& path) {
    int w, h;
    uint8_t* data = load_pixels(path, &w, &h);
    if (!data) {
        g_status = "Could not read image: " + base_name(path);
        return;
    }
    platform::destroy_texture(g_photo.tex);
    g_photo.tex = platform::create_texture(data, w, h);
    stbi_image_free(data);
    g_photo.name = base_name(path);
    g_photo.path = path;
    g_view.photo = true;
    g_status = g_photo.tex ? "Photo behind the scope: " + g_photo.name : "Could not create a texture for " + g_photo.name;
}

// ---------------------------------------------------------------- shared widgets
bool slide(const char* label, double* v, double lo, double hi, const char* fmt, const char* tip) {
    bool changed = ImGui::SliderScalar(label, ImGuiDataType_Double, v, &lo, &hi, fmt);
    if (tip) ImGui::SetItemTooltip("%s", tip);
    if (changed) g_dirty = true;
    return changed;
}

void swatch(const uint8_t rgb[3], float size) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + size, p.y + size), IM_COL32(rgb[0], rgb[1], rgb[2], 255));
    ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + size, p.y + size), IM_COL32(120, 120, 120, 255));
}

void describe_entry(int i) {
    const vce::Entry& e = g_pal.e[i];
    swatch(e.rgb, ImGui::GetTextLineHeight() * 2.2f);
    if (g_have_ref) {
        ImGui::SameLine(0, 2);
        swatch(g_ref[i], ImGui::GetTextLineHeight() * 2.2f);
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("$%03X   G%d R%d B%d", i, e.g, e.r, e.b);
    ImGui::Text("#%02X%02X%02X  (%d, %d, %d)%s", e.rgb[0], e.rgb[1], e.rgb[2], e.rgb[0], e.rgb[1], e.rgb[2], e.clipped ? "   clipped" : "");
    ImGui::EndGroup();
    ImGui::Text("ROM     Y %d    B-Y %+d    R-Y %+d", e.y_code, e.by, e.ry);
    double amp = std::hypot(e.u_ire, e.v_ire);
    if (amp > 1e-6)
        ImGui::Text("Signal  luma %.1f IRE, chroma %.1f IRE at %.1f deg", e.y_ire, amp, phase_deg(e.u_ire, e.v_ire));
    else
        ImGui::Text("Signal  luma %.1f IRE, no chroma", e.y_ire);
    if (e.clipped) ImGui::Text("Decoded R %.3f  G %.3f  B %.3f", e.decoded[0], e.decoded[1], e.decoded[2]);
    if (g_have_ref)
        ImGui::Text("Reference #%02X%02X%02X   model - reference (%+d, %+d, %+d)", g_ref[i][0], g_ref[i][1], g_ref[i][2],
                    e.rgb[0] - g_ref[i][0], e.rgb[1] - g_ref[i][1], e.rgb[2] - g_ref[i][2]);
}

// ---------------------------------------------------------------- presets
struct Preset {
    const char* name;
    const char* tip;
};
const Preset PRESETS[] = {
    {"Measured console", "The default: levels, chroma step, R-Y gain and the -8.5 degree rotation,\nas measured on the scope and waveform-monitor photos."},
};
const int PRESET_COUNT = (int)(sizeof PRESETS / sizeof PRESETS[0]);

vce::Params preset_params(int) { return vce::Params(); }  // the one preset is the model's defaults

// What the sliders were last set from: a preset, a parameter file or a fit. Starts as the measured console.
std::string g_loaded_name = PRESETS[0].name;
vce::Params g_loaded;

void mark_loaded(const std::string& name) {
    g_loaded_name = name;
    g_loaded = g_params;
}

int current_preset() {
    for (int i = 0; i < PRESET_COUNT; i++)
        if (g_params == preset_params(i)) return i;
    return -1;
}

// ---------------------------------------------------------------- left panel
// The panel shows the B-Y and R-Y steps; the model keeps the B-Y step and the R-Y / B-Y ratio. Locked, a change to
// either step scales both and the ratio stays. Unlocked, the other step stays and the ratio changes.
bool g_lock_chroma = true;

// The post adjustments and the "Post" traces are not part of what the tool models. They stay out of the window until
// they are switched on in Settings, and say "Not Recommended" wherever they show. Hidden always means not applied.
bool g_show_post = false;

void hide_post() {
    g_show_post = false;
    if (g_params.post) {
        g_params.post = false;
        g_dirty = true;
    }
    g_view.post = g_wave.post = false;
}

ImFont* bold_font() {  // main.cpp loads the bold face second; without it the regular face is used
    const ImVector<ImFont*>& fonts = ImGui::GetIO().Fonts->Fonts;
    return fonts.Size > 1 ? fonts[1] : nullptr;
}

void not_recommended(const char* text) {  // bold and red
    const ImVec4 red(1.0f, 0.22f, 0.22f, 1.0f);
    ImFont* bold = bold_font();
    ImGui::PushFont(bold, 0.0f);
    ImGui::TextColored(red, "%s", text);
    ImGui::PopFont();
    if (!bold) {  // no bold face (the browser build has one font): the text again, a little to the right
        const ImVec2 p = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + 0.7f, p.y), ImGui::GetColorU32(red), text);
    }
}

// A modal in the middle of the window, on the window's own background: on the darker popup background an unticked
// checkbox cannot be told from what is behind it.
bool begin_modal(const char* name) {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    bool open = ImGui::BeginPopupModal(name, nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleColor();
    return open;
}

void settings_popup() {
    if (!begin_modal("Settings")) return;
    bool show = g_show_post;
    if (ImGui::Checkbox("Show post adjustment", &show)) {
        if (show) g_show_post = true;
        else hide_post();
    }
    ImGui::SameLine();
    not_recommended("Not Recommended");
    ImGui::TextUnformatted("Contrast, brightness, colour and tint are not part of the console's signal.\n"
                           "Set them in the emulator or on the display. Do not bake them into the palette file.");
    ImGui::TextDisabled("This shows those controls, and the \"Post\" boxes on the vectorscope and waveform.\n"
                        "Switching it off also switches the adjustments off.");
    ImGui::Spacing();
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Asks before every setting goes back to its default. The same as picking the preset from the list.
void reset_popup() {
    if (!begin_modal("Reset settings")) return;
    ImGui::TextUnformatted("Are you sure?");
    ImGui::TextDisabled("Every setting goes back to its default: \"%s\".", PRESETS[0].name);
    ImGui::Spacing();
    if (ImGui::Button("Reset")) {
        g_params = preset_params(0);
        mark_loaded(PRESETS[0].name);
        g_dirty = true;
        g_status = "Settings reset";
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------- reference images, snapshots, projects
// Whether a picture is plain 9-bit RGB: every channel value one of eight evenly spaced levels, counted from 0.
// Files differ in the size of the step (32 gives 0 to 224, 36 gives 0 to 252, 255 / 7 gives 0 to 255), so the step
// is found, not assumed. A value such as 27 is on no such ramp, whatever the step.
struct NineBit {
    double step = 255.0 / 7;
    bool exact = false;
    std::vector<int> odd;  // the channel values that are on no ramp
    uint8_t level[256];    // channel value -> 0..7
};

NineBit find_nine_bit(const uint8_t* rgba, size_t pixels) {
    uint64_t hist[256] = {}, total = 0;
    for (size_t i = 0; i < pixels; i++) {
        const uint8_t* p = rgba + i * 4;
        if (p[3] < 128) continue;
        hist[p[0]]++, hist[p[1]]++, hist[p[2]]++;
        total += 3;
    }
    const double tolerance = 1.0;  // rounding or truncation in whatever wrote the file
    auto error = [](int v, double step) { return std::fabs(v - step * std::clamp((int)std::lround(v / step), 0, 7)); };
    double best = 255.0 / 7, best_worst = 1e9;
    uint64_t best_fit = 0;
    for (int k = 3200; k <= 3650; k++) {  // steps of 32.00 to 36.50: the one most of the picture sits on
        double step = k / 100.0, worst = 0;
        uint64_t fit = 0;
        for (int v = 0; v < 256; v++) {
            if (hist[v] == 0) continue;
            double e = error(v, step);
            if (e > tolerance) continue;
            fit += hist[v];
            worst = std::max(worst, e);
        }
        if (fit > best_fit || (fit == best_fit && worst < best_worst)) best = step, best_fit = fit, best_worst = worst;
    }
    NineBit r;
    r.exact = best_fit == total;
    // Nearly all of it on one ramp: the rest are strays, taken to the nearest level of that ramp. Otherwise it is
    // some other kind of picture, and its levels are taken evenly across 0 to 255.
    r.step = best_fit * 10 >= total * 9 ? best : 255.0 / 7;
    for (int v = 0; v < 256; v++) {
        if (hist[v] != 0 && error(v, best) > tolerance) r.odd.push_back(v);
        r.level[v] = (uint8_t)std::clamp((int)std::lround(v / r.step), 0, 7);
    }
    return r;
}

struct PendingSource {  // a picture that is not 9-bit RGB, waiting for the user to say whether to use it
    SourceImage img;
    std::vector<int> odd;
};
std::vector<PendingSource> g_pending_sources;
bool g_open_source_warning = false;

void select_source(int i) {
    ImageView& v = g_imgview;
    v.current = i;
    v.dirty_original = v.dirty_snapshot = v.dirty_mapped = true;
    v.fit = true;
}

void add_source(SourceImage&& img) {
    char msg[320];
    snprintf(msg, sizeof msg, "%s: %d x %d, %d of the 512 colours%s", img.name.c_str(), img.w, img.h, img.used,
             img.exact ? "" : "; not 9-bit RGB, nearest colours used");
    g_status = msg;
    size_t at = 0;
    while (at < g_sources.size() && g_sources[at].path != img.path) at++;  // loaded again: it takes its old place
    if (at == g_sources.size()) g_sources.emplace_back();
    g_sources[at] = std::move(img);
    select_source((int)at);
    if (g_start_view < 0) g_start_view = 2;
}

bool read_source(const std::string& path, PendingSource& out) {
    int w, h;
    uint8_t* data = load_pixels(path, &w, &h);
    if (!data) {
        g_status = "Could not read image: " + base_name(path);
        return false;
    }
    if (w > 16384 || h > 16384) {
        stbi_image_free(data);
        g_status = base_name(path) + " is too large: 16384 pixels a side at most";
        return false;
    }
    SourceImage& img = out.img;
    img.name = base_name(path);
    img.path = path;
    img.w = w, img.h = h;
    img.rgba.assign(data, data + (size_t)w * h * 4);
    stbi_image_free(data);
    NineBit fit = find_nine_bit(img.rgba.data(), (size_t)w * h);
    img.step = fit.step;
    img.exact = fit.exact;
    out.odd = fit.odd;
    img.index.resize((size_t)w * h);
    bool seen[512] = {};
    for (size_t i = 0; i < img.index.size(); i++) {
        uint8_t* p = &img.rgba[i * 4];
        if (p[3] < 128) {
            p[3] = 0;
            img.index[i] = NO_COLOUR;
            continue;
        }
        p[3] = 255;
        const int r = fit.level[p[0]], g = fit.level[p[1]], b = fit.level[p[2]];
        p[0] = NINE_BIT[r], p[1] = NINE_BIT[g], p[2] = NINE_BIT[b];  // from here on the picture is 9-bit RGB
        int entry = index_of(r, g, b);
        img.index[i] = (uint16_t)entry;
        seen[entry] = true;
    }
    img.used = (int)std::count(seen, seen + 512, true);
    return true;
}

// ask: a picture that is not 9-bit RGB is held back until the user says to use it.
bool load_source(const std::string& path, bool ask = true) {
    PendingSource p;
    if (!read_source(path, p)) return false;
    if (p.img.exact || !ask)
        add_source(std::move(p.img));
    else {
        g_pending_sources.push_back(std::move(p));
        g_open_source_warning = true;
    }
    return true;
}

void drop_view_textures() {
    platform::destroy_texture(g_imgview.original);
    platform::destroy_texture(g_imgview.snapshot);
    platform::destroy_texture(g_imgview.mapped);
}

void remove_source(int i) {
    if (i < 0 || i >= (int)g_sources.size()) return;
    g_sources.erase(g_sources.begin() + i);
    if (g_sources.empty()) {
        g_imgview.current = -1;
        drop_view_textures();
    } else
        select_source(std::min(i, (int)g_sources.size() - 1));
}

// The picture with each of its colours replaced by what a palette gives for it.
platform::Texture map_source(const SourceImage& s, const vce::Palette& pal) {
    static std::vector<uint32_t> px;
    uint32_t lut[512];
    for (int i = 0; i < 512; i++) lut[i] = IM_COL32(pal.e[i].rgb[0], pal.e[i].rgb[1], pal.e[i].rgb[2], 255);
    px.resize(s.index.size());
    for (size_t i = 0; i < px.size(); i++) px[i] = s.index[i] == NO_COLOUR ? 0 : lut[s.index[i]];
    return platform::create_texture((const uint8_t*)px.data(), s.w, s.h);
}

void update_view_textures() {
    ImageView& v = g_imgview;
    if (v.current < 0 || v.current >= (int)g_sources.size()) return;
    const SourceImage& s = g_sources[v.current];
    if (v.dirty_original) {
        platform::destroy_texture(v.original);
        v.original = platform::create_texture(s.rgba.data(), s.w, s.h);
        v.dirty_original = false;
    }
    if (v.dirty_mapped) {
        platform::destroy_texture(v.mapped);
        v.mapped = map_source(s, g_pal);
        v.dirty_mapped = false;
    }
    if (v.dirty_snapshot) {
        platform::destroy_texture(v.snapshot);
        if (v.against > 0 && v.against <= (int)g_snapshots.size()) v.snapshot = map_source(s, g_snapshots[v.against - 1].pal);
        v.dirty_snapshot = false;
    }
}

// Assigns snapshot n (counted from 1; 0 = none). use: the source picture is the image through it, not the prepared image.
void compare_against(int n, bool use) {
    g_imgview.assigned = n;
    g_imgview.against = use ? n : 0;
    g_imgview.dirty_snapshot = true;
}

void add_snapshot(const std::string& name, const vce::Params& p) {
    g_snapshot_serial++;
    g_snapshots.push_back(Snapshot{name.empty() ? "Snapshot " + std::to_string(g_snapshot_serial) : name, p, vce::build(p)});
}

void delete_snapshot(int i) {
    if (i < 0 || i >= (int)g_snapshots.size()) return;
    g_snapshots.erase(g_snapshots.begin() + i);
    const int next = std::min(i + 1, (int)g_snapshots.size());  // the one that took its place, or the last, or none
    compare_against(next, next > 0 && g_imgview.against > 0);
}

// How a project names a file: by its path, from the project's own folder if it is inside it, and then the file itself
// as base64, so the project still opens where the file is not.
std::string file_lines(const char* key, const std::string& path, const std::string& folder) {
    std::string out = std::string(key) + "file=" + path_within(path, folder) + "\n";
    std::vector<uint8_t> bytes;
    if (read_file(path, bytes) && !bytes.empty()) out += std::string(key) + "data=" + base64(bytes) + "\n";
    return out;
}

// A project is everything in the window: the settings, the snapshots, and the pictures and palette that are loaded.
// It is text, in sections.
std::string project_text(const std::string& folder) {
    std::string out = "# PCE vectorscope project\n[settings]\n" + params_text(folder);
    out += std::string("lock_chroma=") + (g_lock_chroma ? "1" : "0") + "\n";
    if (g_photo.tex) {  // params_text has named the photo; this is the photo
        std::vector<uint8_t> bytes;
        if (read_file(g_photo.path, bytes) && !bytes.empty()) out += "photo_data=" + base64(bytes) + "\n";
    }
    for (const Snapshot& s : g_snapshots) out += "[snapshot]\nname=" + s.name + "\n" + vce::to_text(s.params);
    for (const SourceImage& s : g_sources) out += "[image]\n" + file_lines("", s.path, folder);
    out += "[view]\nimage=" + std::to_string(g_imgview.current) + "\nsnapshot=" + std::to_string(g_imgview.assigned) +
           "\nsnapshot_as_source=" + (g_imgview.against > 0 ? "1" : "0") + "\nflip=" + std::to_string(g_imgview.mode) + "\n";
    if (g_have_ref) out += "[reference_palette]\n" + file_lines("", g_ref_path, folder);
    if (!g_image.colours.empty()) out += "[plot]\n" + file_lines("", g_image.path, folder);
    return out;
}

std::string value_of(const std::string& body, const char* key) {  // the value on the line "key=value", or ""
    std::string start = std::string(key) + "=";
    size_t pos = 0;
    while (pos < body.size()) {
        size_t end = body.find('\n', pos);
        if (end == std::string::npos) end = body.size();
        if (body.compare(pos, start.size(), start) == 0) return body.substr(pos + start.size(), end - pos - start.size());
        pos = end + 1;
    }
    return "";
}

bool load_project(const std::string& path) {
    std::vector<uint8_t> data;
    if (!read_file(path, data)) {
        g_status = "Could not read " + base_name(path);
        return false;
    }
    std::vector<std::pair<std::string, std::string>> sections;  // name, lines
    std::string text(data.begin(), data.end());
    for (size_t pos = 0; pos < text.size();) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() > 2 && line.front() == '[' && line.back() == ']')
            sections.emplace_back(line.substr(1, line.size() - 2), "");
        else if (!sections.empty())
            sections.back().second += line + "\n";
    }
    bool any = false;
    for (const auto& s : sections) any = any || s.first == "settings";
    if (!any) {
        g_status = "No project found in " + base_name(path);
        return false;
    }

    // Everything that is loaded now makes way for what the project has.
    const std::string folder = folder_of(path);
    const int view = g_start_view;
    g_snapshots.clear();
    g_sources.clear();
    g_pending_sources.clear();
    g_imgview.current = -1;
    g_imgview.assigned = g_imgview.against = 0;
    g_embedded.clear();
    drop_view_textures();
    remove_image();
    g_have_ref = false;
    platform::destroy_texture(g_photo.tex);
    g_photo.path.clear();

    vce::Params p;  // from the defaults, not from what is on the sliders
    int image = 0, snapshot = 0;
    bool as_source = false;
    std::string missing;
    // The path a section names, with the file that came with it kept for when nothing is at that path.
    auto file_in = [&](const std::string& body, const char* file_key = "file", const char* data_key = "data") {
        std::string f = value_of(body, file_key);
        if (f.empty()) return f;
        if (!is_absolute(f)) f = folder + f;
        const std::string data = value_of(body, data_key);
        if (!data.empty()) g_embedded[f] = unbase64(data);
        return f;
    };
    auto could_not = [&](const std::string& f) { missing += (missing.empty() ? "" : ", ") + base_name(f); };
    for (const auto& [name, body] : sections) {
        const std::string file = name == "settings" ? file_in(body, "photo_file", "photo_data") : file_in(body);
        if (name == "settings") {
            params_from_text(body, folder, p, true);
            g_lock_chroma = value_of(body, "lock_chroma") != "0";
            if (!file.empty() && !g_photo.tex) could_not(file);
        } else if (name == "snapshot") {
            vce::Params q;
            vce::from_text(body, q);
            add_snapshot(value_of(body, "name"), q);
        } else if (name == "image") {
            if (!load_source(file, false)) could_not(file);
        } else if (name == "view") {
            image = std::atoi(value_of(body, "image").c_str());
            snapshot = std::atoi(value_of(body, "snapshot").c_str());
            as_source = value_of(body, "snapshot_as_source") == "1";
            g_imgview.mode = value_of(body, "flip") == "1" ? 1 : 0;
        } else if (name == "reference_palette") {
            if (!load_reference(file)) could_not(file);
        } else if (name == "plot") {
            load_image_plot(file);
            if (g_image.colours.empty()) could_not(file);
        }
    }
    g_params = p;
    mark_loaded(base_name(path));
    g_dirty = true;
    if (!g_sources.empty()) select_source(std::clamp(image, 0, (int)g_sources.size() - 1));
    snapshot = std::clamp(snapshot, 0, (int)g_snapshots.size());
    compare_against(snapshot, as_source && snapshot > 0);
    g_start_view = view;  // opening a project does not change the tab
    g_project_path = path;
    g_status = "Opened project " + base_name(path) + (missing.empty() ? "" : "; could not load " + missing);
    return true;
}

bool save_project(const std::string& path) {
    std::string text = project_text(folder_of(path));
    if (!write_file(path, text.data(), text.size())) {
        g_status = "Could not write " + base_name(path);
        return false;
    }
    g_project_path = path;
    g_status = "Saved project " + base_name(path);
    return true;
}

void set_by_step(vce::Params& p, double by, bool locked) {
    double ry = p.chroma_ire * p.ry_gain;
    p.chroma_ire = std::max(by, 0.01);
    if (!locked) p.ry_gain = ry / p.chroma_ire;
}

void set_ry_step(vce::Params& p, double ry, bool locked) {
    ry = std::max(ry, 0.01);
    if (locked && p.ry_gain > 1e-9)
        p.chroma_ire = ry / p.ry_gain;
    else if (p.chroma_ire > 1e-9)
        p.ry_gain = ry / p.chroma_ire;
}

void ui_model() {
    const wchar_t* project_filter = L"PCE vectorscope project (*.pcevs)\0*.pcevs\0All files\0*.*\0";
    if (ImGui::Button("Open project...")) platform::open_file(L"Open project", project_filter, [](const std::string& path) { load_project(path); });
    ImGui::SetItemTooltip("A project is everything in this window: the settings, the snapshots, the pictures and the\n"
                          "reference palette. Opening one replaces what is loaded now.");
    ImGui::SameLine();
    if (ImGui::Button("Save project...")) {
        std::wstring name = platform::to_wide(g_project_path.empty() ? "project.pcevs" : g_project_path);
        std::string path = platform::save_file_dialog(L"Save project", project_filter, L"pcevs", name.c_str());
        if (!path.empty()) save_project(path);
    }
    ImGui::SetItemTooltip("Saves the settings, the snapshots, the loaded pictures and the reference palette, to pick up\n"
                          "again later. The pictures go into the file, so it opens anywhere, in the browser version too.\n"
                          "A picture that is still where it was is read from there, so changes to it are picked up.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##layout", &g_layout, "View: tabs\0View: pictures\0");
    ImGui::SetItemTooltip("Tabs: the vectorscope, the waveform and the reference image take turns in the middle.\n"
                          "Pictures: the two pictures of the reference image take the middle, and the vectorscope,\n"
                          "waveform and 512 colours are reduced beside them.");

    int preset = current_preset();
    std::string shown = preset >= 0 ? PRESETS[preset].name : g_params == g_loaded ? g_loaded_name : g_loaded_name + " (edited)";
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##preset", shown.c_str())) {
        for (int i = 0; i < PRESET_COUNT; i++) {
            if (ImGui::Selectable(PRESETS[i].name, i == preset)) {
                g_params = preset_params(i);
                mark_loaded(PRESETS[i].name);
                g_dirty = true;
            }
            ImGui::SetItemTooltip("%s", PRESETS[i].tip);
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("What the settings below were loaded from. \"(edited)\" means a slider has been moved since.");
    ImGui::PushItemWidth(-S(150));
    if (ImGui::Button("Load...")) {
        platform::open_file(L"Load parameters", L"Parameters (*.txt)\0*.txt\0All files\0*.*\0", [](const std::string& path) {
            if (load_params(path, g_params, true)) {
                mark_loaded(base_name(path));
                g_dirty = true;
                g_status = "Loaded " + base_name(path);
            } else
                g_status = "No parameters found in " + base_name(path);
        });
    }
    ImGui::SameLine();
    if (ImGui::Button("Save...")) {
        std::string path = platform::save_file_dialog(L"Save parameters", L"Parameters (*.txt)\0*.txt\0", L"txt", L"vce_parameters.txt");
        if (!path.empty()) {
            std::string text = params_text();
            g_status = write_file(path, text.data(), text.size()) ? "Saved " + base_name(path) : "Could not write " + base_name(path);
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(preset >= 0);  // nothing to reset
    if (ImGui::Button("Reset")) ImGui::OpenPopup("Reset settings");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(preset >= 0 ? "Every setting is at its default." : "Puts every setting back to its default. Asks first.");
    reset_popup();
    ImGui::SameLine();
    if (ImGui::Button("Settings...")) ImGui::OpenPopup("Settings");
    settings_popup();

    ImGui::SeparatorText("Console output");
    slide("White (IRE)", &g_params.white_ire, 60, 110, "%.1f",
          "Luma code 31 with sync at 40 IRE.\nMeasured 89 on the waveform monitor.");
    slide("Black (IRE)", &g_params.black_ire, -5, 15, "%.1f", "Luma code 0. Measured at blanking level: the console has no 7.5 IRE pedestal.");
    double by = g_params.chroma_ire;
    if (slide("B-Y per count", &by, 1.5, 3.0, "%.3f IRE",
              "Amplitude of one B-Y step, as a vectorscope set to burst = 40 IRE p-p reads it.\nThe scope photo gives 2.14 measured against its burst and 2.15 against its graticule boxes."))
        set_by_step(g_params, by, g_lock_chroma);
    double ry = g_params.chroma_ire * g_params.ry_gain;
    if (slide("R-Y per count", &ry, 2.0, 4.0, "%.3f IRE",
              "Amplitude of one R-Y step, on the same scale. R-Y is mixed in stronger than B-Y:\nthe mixing resistors give 8.2k / 6.2k = 1.3226 times the B-Y step; the scope photo gives 1.29 to 1.32."))
        set_ry_step(g_params, ry, g_lock_chroma);
    ImGui::Checkbox("Lock B-Y and R-Y", &g_lock_chroma);
    ImGui::SetItemTooltip("Ticked, the two steps move as one: changing either scales both and keeps their ratio.\n"
                          "Unticked, each moves on its own. Ticking it again changes nothing; it locks them at the ratio they now have.");
    ImGui::SameLine();
    ImGui::TextDisabled("R-Y / B-Y %.4f", g_params.ry_gain);
    slide("Rotation", &g_params.rotation_deg, -25, 25, "%.2f deg", "The whole chroma pattern against burst. The scope photo gives -8.3 to -8.7,\nand about ten consoles across the range showed much the same, so it is part of the default.");
    slide("Quadrature error", &g_params.quadrature_deg, -20, 20, "%.2f deg", "R-Y axis away from 90 degrees to the B-Y axis.");

    ImGui::SeparatorText("Luma mapping");
    slide("Max white (IRE)", &g_params.map_white_ire, 50, 120, "%.1f",
          "The signal level that comes out as full white. Anything above it clips.\n"
          "The default is the console's own white, 89 IRE.");
    slide("Black level (IRE)", &g_params.map_black_ire, -10, 20, "%.1f",
          "The signal level that comes out as black. Anything below it clips.\n"
          "The default is the console's own black, 0 IRE.");

    if (g_show_post) {
        const char* note = "(Not Recommended)";
        ImGui::PushFont(bold_font(), 0.0f);
        float note_w = ImGui::CalcTextSize(note).x;
        ImGui::PopFont();
        ImGui::SeparatorTextEx(0, "Post adjustment", nullptr, note_w + ImGui::GetStyle().ItemSpacing.x);  // room for the note in the rule
        ImGui::SameLine();
        not_recommended(note);
        if (ImGui::Checkbox("Enable post adjustment", &g_params.post)) g_dirty = true;
        ImGui::SetItemTooltip("Changes made to the colours after the signal is decoded. They are not part of the console's signal.\n"
                              "Unticked, they are left out of the palette; their settings are kept.");
        if (g_params.post) {
            slide("Contrast", &g_params.contrast, 0.5, 1.6, "%.3f", "Picture gain, luma and chroma together.\n1.000 leaves max white at full scale.");
            slide("Brightness", &g_params.brightness, -0.15, 0.15, "%.3f", "Added to the picture after contrast, as a fraction of full scale.");
            slide("Colour", &g_params.saturation, 0.0, 2.0, "%.3f", "Saturation: the gain on chroma alone.");
            slide("Tint", &g_params.tint_deg, -30, 30, "%.2f deg", "Hue: turns all the chroma by this angle.");
        }
    }

    ImGui::SeparatorText("Reference palette");
    if (ImGui::Button("Load .pal..."))
        platform::open_file(L"Reference palette", L"Palette (*.pal)\0*.pal\0All files\0*.*\0", [](const std::string& path) { load_reference(path); });
    if (g_have_ref) {
        ImGui::SameLine();
        if (ImGui::Button("Remove")) g_have_ref = false;
        ImGui::SameLine();
        if (ImGui::Button("Fit model to it")) {
            vce::FitResult fit = vce::fit_to_palette(g_ref, g_params);
            char msg[200];
            if (fit.ok)
                snprintf(msg, sizeof msg, "Fitted on %d colours, %.2f IRE rms chroma error left", fit.used, fit.rms_ire);
            else
                snprintf(msg, sizeof msg, "Could not fit: only %d unclipped colours", fit.used);
            g_status = msg;
            g_dirty = true;
            if (fit.ok) mark_loaded("Fitted to " + g_ref_name);
        }
        ImGui::SetItemTooltip("Sets the B-Y and R-Y steps, rotation and quadrature error\n(and the luma mapping if needed) so the model "
                              "reproduces this palette.%s", g_show_post ? "\nThe post adjustments are switched off and reset." : "");
    }
    if (g_have_ref) {
        ImGui::TextWrapped("%s", g_ref_name.c_str());
        ImGui::Text("Difference: mean %.2f, worst %d at $%03X", g_ref_mean, g_ref_max, g_ref_max_index);
        if (ImGui::IsItemClicked()) g_selected = g_ref_max_index;
    } else
        ImGui::TextDisabled("Drop a .pal file anywhere to compare.");

    ImGui::SeparatorText("Export");
    if (ImGui::Button("Save .pal...")) {
        std::string path = platform::save_file_dialog(L"Save palette", L"Palette (*.pal)\0*.pal\0", L"pal", L"vce_composite.pal");
        if (!path.empty()) {
            std::vector<uint8_t> bytes = pal_bytes(g_pal);
            g_status = write_file(path, bytes.data(), bytes.size()) ? "Saved " + base_name(path) : "Could not write " + base_name(path);
        }
    }
    ImGui::SetItemTooltip("512 colours, 3 bytes each (R, G, B), index = G<<6 | R<<3 | B.\nThis is the file Mesen 2 loads.");
    ImGui::SameLine();
    if (ImGui::Button("Save swatches .png...")) {
        std::string path = platform::save_file_dialog(L"Save swatches", L"PNG image (*.png)\0*.png\0", L"png", L"vce_composite.png");
        if (!path.empty()) g_status = save_swatch_png(g_pal, path) ? "Saved " + base_name(path) : "Could not write " + base_name(path);
    }
    if (g_pal.clipped > 0)
        ImGui::Text("%d of 512 colours clip", g_pal.clipped);
    else
        ImGui::Text("No colour clips");

    if (!g_status.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", g_status.c_str());
    }
    ImGui::PopItemWidth();
}

// ---------------------------------------------------------------- vectorscope
void scope_controls() {
    ImGui::Checkbox("Console", &g_view.console);
    ImGui::SetItemTooltip("The 512 colours where the console's signal puts them (169 distinct points).");
    if (g_show_post) {
        ImGui::SameLine();
        ImGui::Checkbox("Post", &g_view.post);
        ImGui::SetItemTooltip("The final 8-bit colours of the palette, after the luma mapping and the post adjustment,\n"
                              "plotted the way a scope shows an RGB picture: 75%% RGB bars land in the boxes.\n"
                              "A line shows how far clipping moved a colour.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_have_ref);
    ImGui::Checkbox("Reference", &g_view.ref);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("The reference palette, plotted the way a scope shows an RGB picture: 75%% RGB bars land in the boxes.");
    ImGui::SameLine();
    ImGui::Checkbox("Primaries", &g_view.labels);
    ImGui::SetItemTooltip("Ring the console's six full-level primaries (R, G, B, yellow, cyan, magenta).");
    ImGui::SameLine(0, S(20));
    ImGui::SetNextItemWidth(S(112));
    ImGui::Combo("##bars", &g_view.bars, "75% bars\000100% bars\0");
    ImGui::SetItemTooltip("Scope gain, like the 75%% / 100%% switch on the instrument: which colour bars land in the boxes.\n"
                          "At 75%% the burst reaches 0.38 of the circle; at 100%% everything is drawn at three quarters of that.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(80));
    ImGui::SliderFloat("Graticule", &g_view.graticule, 0.0f, 1.0f, "%.2f");
    ImGui::SetItemTooltip("Brightness of the graticule (vector_scope_overlay__v4.png). 0 hides it.\n"
                          "Its boxes are the 75%% bar targets: red at 43.9 IRE with burst at 40 IRE p-p. The circle is at 52.5 IRE.");

    ImGui::SetNextItemWidth(S(150));
    ImGui::SliderFloat("Zoom", &g_view.zoom, 0.5f, 8.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("1x shows the whole graticule. The mouse wheel over the scope does the same.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(110));
    ImGui::SliderFloat("Dot", &g_view.dot, 1.0f, 12.0f, "%.1f");
    ImGui::SameLine(0, S(20));
    if (ImGui::Button("Plot an image..."))
        platform::open_file(L"Image to plot", L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif\0All files\0*.*\0", load_image_plot);
    ImGui::SameLine();
    if (ImGui::Button("Photo behind scope..."))
        platform::open_file(L"Photo of a real vectorscope", L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All files\0*.*\0", load_photo);

    if (!g_image.colours.empty()) {
        ImGui::Checkbox("Image", &g_view.image);
        ImGui::SameLine();
        ImGui::TextUnformatted(g_image.name.c_str());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(110));
        if (ImGui::SliderFloat("Glow", &g_image.intensity, 0.05f, 20.0f, "%.2f", ImGuiSliderFlags_Logarithmic)) g_image.dirty = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(80));
        if (ImGui::SliderInt("Size", &g_image.splat, 0, 6)) g_image.dirty = true;
        ImGui::SameLine();
        if (ImGui::Button("Remove##image")) remove_image();
    }
    if (g_photo.tex) {
        ImGui::Checkbox("Photo", &g_view.photo);
        ImGui::SetItemTooltip("Line the photo up with the graticule using the mouse over the scope:\n"
                              "Shift + drag moves it, Shift + wheel resizes it, Ctrl + wheel turns it.\n"
                              "Saving the parameters also saves this alignment.");
        ImGui::SameLine();
        ImGui::PushItemWidth(S(64));
        ImGui::SliderFloat("Opacity", &g_photo.opacity, 0.0f, 1.0f, "%.2f");
        ImGui::SameLine();
        ImGui::DragFloat("X", &g_photo.x, 0.1f, -300, 300, "%.1f");
        ImGui::SameLine();
        ImGui::DragFloat("Y", &g_photo.y, 0.1f, -300, 300, "%.1f");
        ImGui::SameLine();
        ImGui::DragFloat("Width", &g_photo.width, 0.2f, 10, 2000, "%.1f");
        ImGui::SetItemTooltip("Width of the photo in IRE on the graticule. Drag, or Ctrl+click to type.");
        ImGui::SameLine();
        ImGui::DragFloat("Turn", &g_photo.rot, 0.05f, -180, 180, "%.2f");
        ImGui::PopItemWidth();
        ImGui::Checkbox("Mirror", &g_photo.mirror);
        ImGui::SameLine();
        if (ImGui::Button("Remove##photo")) platform::destroy_texture(g_photo.tex);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g_photo.name.c_str());
    }
}

// The plot alone, as large as the space it is given: the vectorscope tab, and the reduced view beside the pictures.
void scope_plot() {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float side = std::max(S(220), std::min(avail.x, avail.y));
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    p0.x += std::max(0.0f, (avail.x - side) * 0.5f);
    ImGui::SetCursorScreenPos(p0);
    ImGui::InvisibleButton("scope", ImVec2(side, side));
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    ImGuiIO& io = ImGui::GetIO();
    bool photo_on = g_photo.tex && g_view.photo;
    if (hovered && io.MouseWheel != 0.0f) {
        if (photo_on && io.KeyShift)
            g_photo.width = std::clamp(g_photo.width * std::pow(1.02f, io.MouseWheel), 10.0f, 2000.0f);
        else if (photo_on && io.KeyCtrl)
            g_photo.rot += io.MouseWheel * 0.25f;
        else
            g_view.zoom = std::clamp(g_view.zoom * std::pow(1.1f, io.MouseWheel), 0.5f, 8.0f);
    }
    // Two scales: the graticule (and a photo of one) is fixed, the signal is drawn through the scope's gain.
    float sg = (side * 0.5f - S(4)) / (float)GRAT_VIEW_IRE * g_view.zoom;  // pixels per IRE on the graticule
    float s = sg * (g_view.bars == 1 ? 0.75f : 1.0f);                       // pixels per IRE of signal
    if (photo_on && io.KeyShift && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        g_photo.x += io.MouseDelta.x / sg;
        g_photo.y -= io.MouseDelta.y / sg;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p1(p0.x + side, p0.y + side);
    dl->AddRectFilled(p0, p1, IM_COL32(7, 9, 11, 255), S(6));
    dl->PushClipRect(p0, p1, true);
    ImVec2 c(p0.x + side * 0.5f, p0.y + side * 0.5f);
    auto P = [&](double u, double v) { return ImVec2(c.x + (float)u * s, c.y - (float)v * s); };

    // photo of a real scope
    if (g_photo.tex && g_view.photo) {
        ImVec2 pc(c.x + g_photo.x * sg, c.y - g_photo.y * sg);
        float hw = g_photo.width * sg * 0.5f, hh = hw * (float)g_photo.tex.h / (float)g_photo.tex.w;
        float cs = std::cos(g_photo.rot * (float)PI / 180.0f), sn = std::sin(g_photo.rot * (float)PI / 180.0f);
        auto corner = [&](float x, float y) { return ImVec2(pc.x + x * cs + y * sn, pc.y - x * sn + y * cs); };
        float u0 = g_photo.mirror ? 1.0f : 0.0f, u1 = 1.0f - u0;
        dl->AddImageQuad((ImTextureID)(intptr_t)g_photo.tex.srv, corner(-hw, -hh), corner(hw, -hh), corner(hw, hh), corner(-hw, hh),
                         ImVec2(u0, 0), ImVec2(u1, 0), ImVec2(u1, 1), ImVec2(u0, 1), IM_COL32(255, 255, 255, (int)(g_photo.opacity * 255)));
    }

    // graticule
    const ImU32 text = IM_COL32(150, 170, 160, 255);
    if (g_graticule && g_view.graticule > 0.0f) {
        float k = sg / (float)GRAT_PX_PER_IRE;  // screen pixels per image pixel
        ImVec2 a(c.x - (float)GRAT_CX * k, c.y - (float)GRAT_CY * k);
        dl->AddImage((ImTextureID)(intptr_t)g_graticule.srv, a, ImVec2(a.x + g_graticule.w * k, a.y + g_graticule.h * k), ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(205, 230, 218, (int)(g_view.graticule * 255.0f + 0.5f)));
    } else {
        dl->AddLine(ImVec2(c.x - S(8), c.y), ImVec2(c.x + S(8), c.y), text);
        dl->AddLine(ImVec2(c.x, c.y - S(8)), ImVec2(c.x, c.y + S(8)), text);
    }
    // burst: 40 IRE p-p at 180 degrees
    dl->AddLine(c, P(-20, 0), IM_COL32(210, 190, 90, 255), 2.5f);

    // colours of a plotted image
    if (g_image.tex && g_view.image) {
        double ext = HIST_EXTENT * RGB_UNIT_IRE;
        dl->AddImage((ImTextureID)(intptr_t)g_image.tex.srv, P(-ext, ext), P(ext, -ext));
    }

    // reference palette
    if (g_have_ref && g_view.ref) {
        float k = std::max(2.0f, g_view.dot * 0.8f);
        for (int i = 0; i < 512; i++) {
            double y, u, v;
            vce::rgb_to_yuv(g_ref[i], y, u, v);
            ImVec2 p = P(u * RGB_UNIT_IRE, v * RGB_UNIT_IRE);
            dl->AddLine(ImVec2(p.x - k, p.y), ImVec2(p.x + k + 1, p.y), IM_COL32(255, 255, 255, 190));
            dl->AddLine(ImVec2(p.x, p.y - k), ImVec2(p.x, p.y + k + 1), IM_COL32(255, 255, 255, 190));
        }
    }

    // final colours, with clip displacement
    if (g_view.post) {
        float k = std::max(1.5f, g_view.dot * 0.6f);
        for (int i = 0; i < 512; i++) {
            const vce::Entry& e = g_pal.e[i];
            double y, u, v;
            vce::rgb_to_yuv(e.rgb, y, u, v);
            ImVec2 p = P(u * RGB_UNIT_IRE, v * RGB_UNIT_IRE);
            if (e.clipped) {  // from where the colour was before it clipped
                double dy = 0.299 * e.decoded[0] + 0.587 * e.decoded[1] + 0.114 * e.decoded[2];
                ImVec2 from = P(0.492 * (e.decoded[2] - dy) * RGB_UNIT_IRE, 0.877 * (e.decoded[0] - dy) * RGB_UNIT_IRE);
                dl->AddLine(from, p, IM_COL32(255, 90, 90, 150));
            }
            dl->AddRect(ImVec2(p.x - k, p.y - k), ImVec2(p.x + k + 1, p.y + k + 1), entry_colour(i));
        }
    }

    // console signal
    int near_group = -1;
    if (g_view.console) {
        float best = (g_view.dot + S(4)) * (g_view.dot + S(4));
        for (size_t n = 0; n < g_groups.size(); n++) {
            const Group& g = g_groups[n];
            ImVec2 p = P(g.u, g.v);
            dl->AddCircleFilled(p, g_view.dot, g.col, 16);
            dl->AddCircle(p, g_view.dot, IM_COL32(0, 0, 0, 150), 16);
            if (hovered) {
                float dx = io.MousePos.x - p.x, dy = io.MousePos.y - p.y;
                if (dx * dx + dy * dy < best) {
                    best = dx * dx + dy * dy;
                    near_group = (int)n;
                }
            }
        }
        if (g_view.labels) {
            static const struct { const char* name; int r, g, b; } TAGS[6] = {{"R", 7, 0, 0}, {"Mg", 7, 0, 7}, {"B", 0, 0, 7}, {"Cy", 0, 7, 7}, {"G", 0, 7, 0}, {"Yl", 7, 7, 0}};
            for (const auto& t : TAGS) {  // ring the six full-level primaries so they stand out from the lattice
                const vce::Entry& e = g_pal.e[index_of(t.r, t.g, t.b)];
                dl->AddCircle(P(e.u_ire, e.v_ire), g_view.dot + S(3), IM_COL32(235, 235, 235, 255), 20, 1.5f);
            }
        }
    }

    // highlights
    for (int which = 0; which < 2; which++) {
        int i = which == 0 ? g_selected : g_hot_prev;
        if (i < 0) continue;
        const vce::Entry& e = g_pal.e[i];
        ImVec2 p = P(e.u_ire, e.v_ire);
        if (which == 0) dl->AddLine(c, p, IM_COL32(255, 255, 255, 110));
        dl->AddCircle(p, g_view.dot + S(4), which == 0 ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 220, 120, 255), 24, 2.0f);
    }

    // readout under the mouse
    if (hovered) {
        double u = (io.MousePos.x - c.x) / s, v = (c.y - io.MousePos.y) / s;
        char label[64];
        snprintf(label, sizeof label, "%.1f IRE  %.1f deg", std::hypot(u, v), phase_deg(u, v));
        dl->AddText(ImVec2(p0.x + S(8), p0.y + S(6)), text, label);
    }
    dl->AddText(ImVec2(p0.x + S(8), p1.y - ImGui::GetFontSize() - S(6)), IM_COL32(210, 190, 90, 255), "yellow line: burst, 40 IRE p-p");
    if (g_view.bars == 1) {
        const char* note = "100% bars: gain x 0.75";
        dl->AddText(ImVec2(p1.x - ImGui::CalcTextSize(note).x - S(8), p1.y - ImGui::GetFontSize() - S(6)), text, note);
    }
    dl->PopClipRect();

    if (near_group >= 0) {
        const Group& g = g_groups[near_group];
        g_hot = g.members[0];
        if (clicked) g_selected = g.members[0];
        ImGui::BeginTooltip();
        ImGui::Text("B-Y %+d   R-Y %+d      %.1f IRE at %.1f deg", g.by, g.ry, std::hypot(g.u, g.v), phase_deg(g.u, g.v));
        ImGui::Text("%d colour%s here:", (int)g.members.size(), g.members.size() == 1 ? "" : "s");
        for (int i : g.members) {
            const vce::Entry& e = g_pal.e[i];
            swatch(e.rgb, ImGui::GetTextLineHeight());
            ImGui::SameLine();
            ImGui::Text("$%03X  G%d R%d B%d   Y %2d   #%02X%02X%02X%s", i, e.g, e.r, e.b, e.y_code, e.rgb[0], e.rgb[1], e.rgb[2], e.clipped ? "  clipped" : "");
        }
        ImGui::EndTooltip();
    }
}

// ---------------------------------------------------------------- waveform
struct Bar {
    std::string label;
    int index;   // palette entry, or -1 for a bare luma code
    int y_code;
};

std::vector<Bar> wave_bars() {
    std::vector<Bar> bars;
    char label[16];
    int L = g_wave.level;
    switch (g_wave.pattern) {
    case 0: {
        static const struct { const char* name; int r, g, b; } B8[8] = {{"W", 1, 1, 1}, {"Yl", 1, 1, 0}, {"Cy", 0, 1, 1}, {"G", 0, 1, 0}, {"Mg", 1, 0, 1}, {"R", 1, 0, 0}, {"B", 0, 0, 1}, {"K", 0, 0, 0}};
        for (const auto& b : B8) bars.push_back(Bar{b.name, index_of(b.r * L, b.g * L, b.b * L), 0});
        break;
    }
    case 1:
        for (int n = 0; n < 8; n++) {
            snprintf(label, sizeof label, "%d", n);
            bars.push_back(Bar{label, index_of(n, n, n), 0});
        }
        break;
    case 2:
        for (int n = 0; n < 32; n++) {
            snprintf(label, sizeof label, "%d", n);
            bars.push_back(Bar{label, -1, n});
        }
        break;
    default:
        for (int ch = 0; ch < 3; ch++)
            for (int n = 0; n < 8; n++) {
                snprintf(label, sizeof label, "%c%d", "RGB"[ch], n);
                bars.push_back(Bar{label, index_of(ch == 0 ? n : 0, ch == 1 ? n : 0, ch == 2 ? n : 0), 0});
            }
    }
    return bars;
}

// A plotted image on the waveform tab, as R, G and B against position across the picture.
void wave_parade(ImDrawList* dl, float left, float right, float y0, float y100, float label_y, ImU32 text) {
    const float gap = S(10), pw = (right - left - 2 * gap) / 3;
    const int cols = g_image.parade_cols;
    auto Yv = [&](double level) { return y0 + (float)(level / 255.0) * (y100 - y0); };
    for (int c = 0; c < 3; c++) {
        float xa = left + (pw + gap) * c, xb = xa + pw;
        dl->AddRectFilled(ImVec2(xa, y100), ImVec2(xb, y0), IM_COL32(255, 255, 255, 7));
        if (g_image.parade_tex && cols > 0)  // half a texel in from each side, so the panels do not run into each other
            dl->AddImage((ImTextureID)(intptr_t)g_image.parade_tex.srv, ImVec2(xa, Yv(255.5)), ImVec2(xb, Yv(-0.5)),
                         ImVec2((c * cols + 0.5f) / (3.0f * cols), 0), ImVec2(((c + 1) * cols - 0.5f) / (3.0f * cols), 1));
        const char* name = c == 0 ? "R" : c == 1 ? "G" : "B";
        dl->AddText(ImVec2((xa + xb - ImGui::CalcTextSize(name).x) * 0.5f, label_y), text, name);
    }
}

void wave_controls() {
    const bool have_image = !g_image.colours.empty();
    if (!have_image && g_wave.pattern == PATTERN_PARADE) g_wave.pattern = PATTERN_BARS;
    const bool parade = g_wave.pattern == PATTERN_PARADE;
    // The picture has greys and luma levels to set beside the generated ones, but no colour bars or ramps.
    const bool image_fits = g_wave.pattern == PATTERN_GREYS || g_wave.pattern == PATTERN_LUMA || parade;

    ImGui::BeginDisabled(parade);
    ImGui::Checkbox("Console", &g_wave.console);
    ImGui::SetItemTooltip("The signal the console puts out: the luma level and the chroma envelope around it, with sync at 40 IRE.\n"
                          "The luma mapping does not change it.");
    if (g_show_post) {
        ImGui::SameLine();
        ImGui::Checkbox("Post", &g_wave.post);
        ImGui::SetItemTooltip("The final 8-bit colours of the palette, drawn as a signal: black at 0 IRE, full white at 100,\n"
                              "chroma to the same scale. This is where the luma mapping, the post adjustment and clipping show.\n"
                              "With both ticked, each bar has the console on the left and Post on the right.");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!have_image || !image_fits || parade);
    ImGui::Checkbox("Image", &g_wave.image);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("The plotted image, with full white at 100 IRE. With \"Grey steps\" it shows one bar for each pure grey\n"
                          "(R = G = B) in the picture; with \"All 32 luma codes\", one bar for each luma level in it.\n"
                          "Tick it beside Console to set the picture against the generated levels, or on its own.");
    ImGui::SameLine(0, S(20));
    ImGui::SetNextItemWidth(S(190));
    ImGui::Combo("Pattern", &g_wave.pattern,
                 have_image ? "Colour bars\0Grey steps\0All 32 luma codes\0R, G and B ramps\0Image: R, G, B parade\0"
                            : "Colour bars\0Grey steps\0All 32 luma codes\0R, G and B ramps\0");
    if (g_wave.pattern == PATTERN_BARS) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(90));
        ImGui::SliderInt("Level", &g_wave.level, 1, 7);
    }
    ImGui::BeginDisabled(parade);
    ImGui::SetNextItemWidth(S(190));
    ImGui::Combo("Carrier", &g_wave.shape, "Fundamental only\0Three-level (+ + 0 - - 0)\0Square\0");
    ImGui::SetItemTooltip("Fundamental only is what a waveform monitor shows: its own bandwidth removes the carrier's harmonics.\n"
                          "The stepped shapes show the peaks a wide-band oscilloscope would see at the pin.");
    ImGui::SameLine();
    ImGui::Checkbox("Low-pass", &g_wave.lowpass);
    ImGui::EndDisabled();
    ImGui::SameLine(0, S(20));
    if (ImGui::Button("Plot an image...##wave"))
        platform::open_file(L"Image to plot", L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif\0All files\0*.*\0", load_image_plot);
    ImGui::SetItemTooltip("The same picture the vectorscope plots. Here it shows with \"Grey steps\", \"All 32 luma codes\"\n"
                          "and as an R, G, B parade; it has no colour bars or ramps to show.");
    if (have_image) {
        ImGui::SameLine();
        if (ImGui::Button("Remove##waveimage")) remove_image();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g_image.name.c_str());
    }
}

// The plot alone, as large as the space it is given: the waveform tab, and the reduced view beside the pictures.
void wave_plot() {
    if (g_image.colours.empty() && g_wave.pattern == PATTERN_PARADE) g_wave.pattern = PATTERN_BARS;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 size(std::max(S(220), avail.x), std::max(S(160), avail.y));
    ImGui::InvisibleButton("wave", size);
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p1(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, IM_COL32(7, 9, 11, 255), S(6));
    dl->PushClipRect(p0, p1, true);

    const float top_ire = 125.0f, bottom_ire = -45.0f;
    float left = p0.x + S(46), right = p1.x - S(12), top = p0.y + S(12), bottom = p1.y - S(26);
    auto Y = [&](double ire) { return top + (float)((top_ire - ire) / (top_ire - bottom_ire)) * (bottom - top); };
    const ImU32 grat = IM_COL32(125, 150, 138, 255), dim = IM_COL32(48, 60, 55, 255), text = IM_COL32(150, 170, 160, 255);
    for (int ire = -40; ire <= 120; ire += 10) {
        bool major = ire == 0 || ire == 100;
        dl->AddLine(ImVec2(left, Y(ire)), ImVec2(right, Y(ire)), major ? grat : dim, major ? 1.5f : 1.0f);
        if (ire % 20 == 0) {
            char label[16];
            snprintf(label, sizeof label, "%d", ire);
            ImVec2 ts = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(left - ts.x - S(6), Y(ire) - ts.y * 0.5f), text, label);
        }
    }

    const ImU32 trace = IM_COL32(120, 235, 150, 255), post_trace = IM_COL32(125, 185, 255, 255), image_trace = IM_COL32(235, 235, 225, 255);
    auto readout = [&](bool with_level) {  // the level under the mouse; for RGB data also as an 8-bit value
        if (!hovered) return;
        double ire = top_ire - (io.MousePos.y - top) / (bottom - top) * (top_ire - bottom_ire);
        int level = (int)std::lround(ire * 255.0 / RGB_WAVE_IRE);
        char label[48];
        if (with_level && level >= 0 && level <= 255)
            snprintf(label, sizeof label, "%.1f IRE  (%d)", ire, level);
        else
            snprintf(label, sizeof label, "%.1f IRE", ire);
        dl->AddText(ImVec2(left + S(6), top + S(2)), text, label);
    };
    if (g_wave.pattern == PATTERN_PARADE) {
        wave_parade(dl, left, right, Y(0), Y(RGB_WAVE_IRE), bottom + S(8), text);
        readout(true);
        dl->PopClipRect();
        return;
    }

    // sync and burst
    float sync_w = (right - left) * 0.11f, x = left;
    ImVec2 sync[6] = {ImVec2(x, Y(0)), ImVec2(x + sync_w * 0.12f, Y(0)), ImVec2(x + sync_w * 0.12f, Y(-40)),
                      ImVec2(x + sync_w * 0.45f, Y(-40)), ImVec2(x + sync_w * 0.45f, Y(0)), ImVec2(x + sync_w, Y(0))};
    dl->AddPolyline(sync, 6, trace, 0, 1.5f);
    if (!g_wave.lowpass) {
        float b = (float)vce::BURST_PP_IRE * 0.5f;
        dl->AddRectFilled(ImVec2(x + sync_w * 0.55f, Y(b)), ImVec2(x + sync_w * 0.9f, Y(-b)), IM_COL32(210, 190, 90, 110));
        dl->AddRect(ImVec2(x + sync_w * 0.55f, Y(b)), ImVec2(x + sync_w * 0.9f, Y(-b)), IM_COL32(210, 190, 90, 255));
    }

    std::vector<Bar> bars = wave_bars();
    float x0 = left + sync_w, bw = (right - x0) / (float)bars.size();
    vce::Carrier shape = g_wave.shape == 0 ? vce::Carrier::Sine : g_wave.shape == 1 ? vce::Carrier::ThreeLevel : vce::Carrier::Square;
    double burst_scale = vce::BURST_PP_IRE / 40.0;  // the steps are against burst at 40 IRE; this view shows the wire

    struct Level {  // a bar of one signal: its luma level and the peak of the chroma around it
        double luma, peak;
    };
    auto final_rgb = [&](const Bar& bar) { return bar.index >= 0 ? g_pal.e[bar.index].rgb : g_pal.luma[bar.y_code].rgb; };
    auto console_level = [&](const Bar& bar) {
        if (bar.index < 0) return Level{g_pal.luma[bar.y_code].y_ire, 0.0};
        const vce::Entry& e = g_pal.e[bar.index];
        return Level{e.y_ire, vce::envelope_peak(g_params, e.by, e.ry, shape) * burst_scale};
    };
    auto post_level = [&](const Bar& bar) {  // the final colour, encoded with a sine carrier
        double y, u, v;
        vce::rgb_to_yuv(final_rgb(bar), y, u, v);
        return Level{y * RGB_WAVE_IRE, std::hypot(u, v) * RGB_WAVE_IRE};
    };
    auto draw = [&](float xa, float xb, Level l, ImU32 col, ImU32 line) {
        if (!g_wave.lowpass && l.peak > 0.05) {
            dl->AddRectFilled(ImVec2(xa, Y(l.luma + l.peak)), ImVec2(xb, Y(l.luma - l.peak)), (col & 0x00FFFFFF) | 0x70000000);
            dl->AddLine(ImVec2(xa, Y(l.luma + l.peak)), ImVec2(xb, Y(l.luma + l.peak)), line, 1.5f);
            dl->AddLine(ImVec2(xa, Y(l.luma - l.peak)), ImVec2(xb, Y(l.luma - l.peak)), line, 1.5f);
            dl->AddLine(ImVec2(xa, Y(l.luma)), ImVec2(xb, Y(l.luma)), IM_COL32(255, 255, 255, 120));
        } else
            dl->AddLine(ImVec2(xa, Y(l.luma)), ImVec2(xb, Y(l.luma)), line, 2.0f);
    };

    // The plotted image, as the older Python tool drew it: one bar for each pure grey in the picture, or for each
    // luma level in it, in rising order across the same width as the generated bars and behind their traces.
    const bool show_image = g_wave.image && !g_image.colours.empty() && (g_wave.pattern == PATTERN_GREYS || g_wave.pattern == PATTERN_LUMA);
    const std::array<uint32_t, 256>& levels = g_wave.pattern == PATTERN_GREYS ? g_image.greys : g_image.lumas;
    int hot_level = -1;
    if (show_image) {
        int count = 0;
        for (uint32_t n : levels) count += n > 0;
        if (count == 0) dl->AddText(ImVec2(x0 + S(10), Y(60)), image_trace, "No pure greys (R = G = B) in this image");
        float iw = (right - x0) / (float)std::max(count, 1), pad = iw > 6 ? 1.0f : 0.0f;
        for (int v = 0, k = 0; v < 256; v++) {
            if (levels[v] == 0) continue;
            float xa = x0 + iw * k++, xb = xa + iw, y = Y(v * RGB_WAVE_IRE / 255.0);
            dl->AddRectFilled(ImVec2(xa + pad, y), ImVec2(xb - pad, Y(0)), IM_COL32(v, v, v, 255));
            if (iw > 6) dl->AddRect(ImVec2(xa + pad, y), ImVec2(xb - pad, Y(0) + 1), IM_COL32(95, 105, 100, 255));
            dl->AddLine(ImVec2(xa + pad, y), ImVec2(xb - pad, y), image_trace, 1.5f);
            char label[8];
            snprintf(label, sizeof label, "%d", v);
            ImVec2 ts = ImGui::CalcTextSize(label);
            if (ts.x + 2 < iw) dl->AddText(ImVec2(std::floor((xa + xb - ts.x) * 0.5f), y - ts.y - S(2)), image_trace, label);
            if (hovered && io.MousePos.x >= xa && io.MousePos.x < xb) hot_level = v;
        }
    }

    const bool both = g_wave.console && g_wave.post;  // then each bar is split: console on the left, post on the right
    const bool generated = g_wave.console || g_wave.post || !show_image;  // the picture on its own: no generated bars at all
    int hot_bar = -1;
    for (size_t n = 0; n < bars.size() && generated; n++) {
        const Bar& bar = bars[n];
        float xa = x0 + bw * n, xb = xa + bw, xm = both ? (xa + xb) * 0.5f : xb;
        const uint8_t* rgb = final_rgb(bar);
        ImU32 col = IM_COL32(rgb[0], rgb[1], rgb[2], 255);
        float pad = bw > 6 ? 1.0f : 0.0f;
        if (g_wave.console) draw(xa + pad, both ? xm : xb - pad, console_level(bar), col, trace);
        if (g_wave.post) draw(both ? xm : xa + pad, xb - pad, post_level(bar), col, post_trace);
        dl->AddRectFilled(ImVec2(xa + pad, bottom + S(4)), ImVec2(xb - pad, bottom + S(8)), col);
        ImVec2 ts = ImGui::CalcTextSize(bar.label.c_str());
        if (ts.x < bw) dl->AddText(ImVec2((xa + xb - ts.x) * 0.5f, bottom + S(8)), text, bar.label.c_str());
        if (hovered && io.MousePos.x >= xa && io.MousePos.x < xb) hot_bar = (int)n;
    }
    readout(show_image);
    if (g_wave.post || show_image) {  // which trace is which
        float line = ImGui::GetFontSize() + S(2), y = top + S(2) + line;
        if (g_wave.console) {
            dl->AddText(ImVec2(left + S(6), y), trace, "console");
            y += line;
        }
        if (g_wave.post) {
            dl->AddText(ImVec2(left + S(6), y), post_trace, "post");
            y += line;
        }
        if (show_image) dl->AddText(ImVec2(left + S(6), y), image_trace, "image");
    }
    dl->PopClipRect();

    if (hot_bar < 0 && hot_level < 0) return;
    ImGui::BeginTooltip();
    if (hot_bar >= 0) {
        const Bar& bar = bars[hot_bar];
        if (bar.index >= 0) {
            const vce::Entry& e = g_pal.e[bar.index];
            g_hot = bar.index;
            if (clicked) g_selected = bar.index;
            double peak = console_level(bar).peak;
            ImGui::Text("$%03X  G%d R%d B%d", bar.index, e.g, e.r, e.b);
            ImGui::Text("Luma %.1f IRE (code %d)", e.y_ire, e.y_code);
            ImGui::Text("Chroma %.1f IRE p-p, envelope %.1f to %.1f IRE", 2 * peak, e.y_ire - peak, e.y_ire + peak);
        } else
            ImGui::Text("Luma code %d: %.1f IRE", bar.y_code, g_pal.luma[bar.y_code].y_ire);
        if (g_wave.post) {
            const uint8_t* rgb = final_rgb(bar);
            Level l = post_level(bar);
            ImGui::Separator();
            ImGui::Text("Post #%02X%02X%02X: luma %.1f IRE", rgb[0], rgb[1], rgb[2], l.luma);
            if (l.peak > 0.05) ImGui::Text("Chroma %.1f IRE p-p, envelope %.1f to %.1f IRE", 2 * l.peak, l.luma - l.peak, l.luma + l.peak);
        }
    }
    if (hot_level >= 0) {
        if (hot_bar >= 0) ImGui::Separator();
        ImGui::Text("Image: %s %d, %.1f IRE", g_wave.pattern == PATTERN_GREYS ? "grey" : "luma", hot_level, hot_level * RGB_WAVE_IRE / 255.0);
        ImGui::Text("%.2f%% of the picture", 100.0 * levels[hot_level] / (double)std::max<uint64_t>(1, g_image.total));
    }
    ImGui::EndTooltip();
}

void ui_scope() {
    scope_controls();
    scope_plot();
}

void ui_wave() {
    wave_controls();
    wave_plot();
}

// ---------------------------------------------------------------- reference image
void snapshot_popup() {
    static char name[64];
    if (!begin_modal("Save snapshot")) return;
    ImGui::TextUnformatted("Keep the current settings under this name:");
    if (ImGui::IsWindowAppearing()) {
        snprintf(name, sizeof name, "Snapshot %d", g_snapshot_serial + 1);
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(S(320));
    bool enter = ImGui::InputText("##name", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::Spacing();
    const bool named = name[0] != 0;
    ImGui::BeginDisabled(!named);
    bool save = ImGui::Button("Save");
    ImGui::EndDisabled();
    if (save || (enter && named)) {
        add_snapshot(name, g_params);
        compare_against((int)g_snapshots.size(), true);  // the pictures now show it against whatever is set from here on
        g_status = "Snapshot saved: " + g_snapshots.back().name;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void restore_popup() {
    if (!begin_modal("Restore snapshot")) return;
    const int n = g_imgview.assigned - 1;
    if (n < 0 || n >= (int)g_snapshots.size()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::TextUnformatted("Are you sure?");
    ImGui::TextDisabled("The current settings are replaced by those of \"%s\".", g_snapshots[n].name.c_str());
    ImGui::Spacing();
    if (ImGui::Button("Restore")) {
        g_params = g_snapshots[n].params;
        mark_loaded(g_snapshots[n].name);
        g_dirty = true;
        g_status = "Settings restored from " + g_snapshots[n].name;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// Asks about each picture that is not 9-bit RGB, one at a time.
void source_warning_popup() {
    if (g_open_source_warning) {
        ImGui::OpenPopup("Not 9-bit RGB");
        g_open_source_warning = false;
    }
    if (!begin_modal("Not 9-bit RGB")) return;
    if (g_pending_sources.empty()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    PendingSource& p = g_pending_sources.front();
    std::string values;
    for (size_t i = 0; i < p.odd.size() && i < 12; i++) values += (i ? ", " : "") + std::to_string(p.odd[i]);
    if (p.odd.size() > 12) values += " ...";
    ImGui::TextUnformatted(p.img.name.c_str());
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.30f, 1.0f));
    ImGui::TextUnformatted("This does not look like plain 9-bit RGB.");
    ImGui::PopStyleColor();
    ImGui::TextUnformatted("Such a picture has eight evenly spaced levels in each channel, counted from 0.\n"
                           "Steps of 32, 34, 36 and 255 / 7 all count.");
    ImGui::Text("Here %d channel value%s on no such ramp: %s", (int)p.odd.size(), p.odd.size() == 1 ? " is" : "s are", values.c_str());
    ImGui::TextDisabled("Used anyway, each of its colours is taken as the nearest of the 512.");
    ImGui::Spacing();
    bool use = ImGui::Button("Use it anyway");
    ImGui::SameLine();
    bool skip = ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (use || skip) {
        if (use) add_source(std::move(p.img));
        g_pending_sources.erase(g_pending_sources.begin());
        if (g_pending_sources.empty()) ImGui::CloseCurrentPopup();  // otherwise the next one is asked about
    }
    ImGui::EndPopup();
}

// Puts the next item on the same line when it fits there, and starts a new line when it does not.
void flow(float width) {
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < width) ImGui::NewLine();
}

float button_width(const char* label) { return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2; }

void ui_refimage() {
    ImageView& v = g_imgview;
    if (v.assigned > (int)g_snapshots.size()) compare_against(0, false);
    const bool have = v.current >= 0 && v.current < (int)g_sources.size();
    auto tip = [](const char* text) {  // also on a control that is greyed out
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", text);
    };

    // Which picture.
    ImGui::BeginDisabled(g_sources.empty());
    ImGui::SetNextItemWidth(S(230));
    if (ImGui::BeginCombo("Image", have ? g_sources[v.current].name.c_str() : "none loaded")) {
        for (int i = 0; i < (int)g_sources.size(); i++) {
            ImGui::PushID(i);
            if (ImGui::Selectable(g_sources[i].name.c_str(), i == v.current)) select_source(i);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (have) {
        const SourceImage& s = g_sources[v.current];
        char about[256];
        snprintf(about, sizeof about, "%d x %d pixels, %d of the 512 colours. In the file the levels are %.4g apart.%s", s.w, s.h, s.used, s.step,
                 s.exact ? "" : "\nIt is not 9-bit RGB: each of its colours was taken as the nearest of the 512.");
        tip(about);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add image..."))
        platform::open_file(L"Reference image in 9-bit RGB", L"Images\0*.png;*.bmp;*.gif;*.tga;*.jpg;*.jpeg\0All files\0*.*\0",
                            [](const std::string& path) { load_source(path); });
    ImGui::SetItemTooltip("A picture in plain 9-bit RGB: eight evenly spaced levels in each channel, such as 0, 36, 72 ... 252.\n"
                          "It is prepared first: every pixel is taken to its 9-bit colour, whatever the size of step in the file.\n"
                          "Several can be loaded; pick one from the list.");
    if (have) {
        ImGui::SameLine();
        if (ImGui::Button("Remove##source")) {
            remove_source(v.current);
            return;
        }
        ImGui::SetItemTooltip("Takes this picture off the list. The file is not touched.");
        if (!g_sources[v.current].exact) {
            flow(ImGui::CalcTextSize("not 9-bit RGB").x);
            ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f), "not 9-bit RGB");
            ImGui::SetItemTooltip("Its values are not eight evenly spaced levels. Each colour was taken as the nearest of the 512.");
        }
    }

    // The snapshots. The one showing in the list is the assigned one: the one the source picture can be made from.
    ImGui::BeginDisabled(g_snapshots.empty());
    ImGui::SetNextItemWidth(S(230));
    if (ImGui::BeginCombo("Snapshot", v.assigned > 0 ? g_snapshots[v.assigned - 1].name.c_str() : "none saved")) {
        for (int i = 0; i < (int)g_snapshots.size(); i++) {
            ImGui::PushID(i);
            if (ImGui::Selectable(g_snapshots[i].name.c_str(), v.assigned == i + 1)) compare_against(i + 1, true);
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    tip(g_snapshots.empty() ? "No snapshot has been saved yet." : "The assigned snapshot: the one \"Snapshot as source\" shows, and the one Restore and Delete act on.");
    ImGui::SameLine();
    if (ImGui::Button("Save snapshot...")) ImGui::OpenPopup("Save snapshot");
    ImGui::SetItemTooltip("Keeps the current settings under a name, to compare later settings with.\n"
                          "Snapshots go into a saved project; otherwise they last until the program closes.");
    snapshot_popup();
    ImGui::SameLine();
    ImGui::BeginDisabled(v.assigned == 0);
    if (ImGui::Button("Restore")) ImGui::OpenPopup("Restore snapshot");
    ImGui::EndDisabled();
    tip(v.assigned == 0 ? "Save a snapshot first." : "Puts the settings back to this snapshot's. Asks first.");
    restore_popup();
    ImGui::SameLine();
    ImGui::BeginDisabled(v.assigned == 0);
    if (ImGui::Button("Delete")) delete_snapshot(v.assigned - 1);
    ImGui::EndDisabled();
    tip(v.assigned == 0 ? "Save a snapshot first." : "Takes this snapshot off the list.");

    if (!have) {
        ImGui::Spacing();
        ImGui::TextWrapped("Add or drop a picture in plain 9-bit RGB: eight evenly spaced levels in each channel, such as 0, 36, 72 ... 252. "
                           "It is shown as 9-bit RGB, and as the current settings make it.");
        return;
    }
    const SourceImage& s = g_sources[v.current];

    // What the source picture is, how the two pictures are laid out, and how large.
    bool as_source = v.against > 0;
    ImGui::BeginDisabled(v.assigned == 0);
    if (ImGui::Checkbox("Snapshot as source", &as_source)) compare_against(v.assigned, as_source);
    ImGui::EndDisabled();
    tip(v.assigned == 0 ? "Save a snapshot first.\nUntil then the source picture is the original image, as 9-bit RGB."
                        : "What the source picture is: the one the current settings are compared with.\n"
                          "Unticked: the original image, as 9-bit RGB.\n"
                          "Ticked: the image through the assigned snapshot.");
    flow(S(105));
    ImGui::SetNextItemWidth(S(105));
    ImGui::Combo("##mode", &v.mode, "Side by side\0Flip\0");
    ImGui::SetItemTooltip("Side by side: both pictures at once.\n"
                          "Flip: one picture, larger, that changes between the two in place.");
    if (v.mode == 1) {
        flow(button_width("Flip"));
        if (ImGui::Button("Flip")) v.flipped = !v.flipped;
        ImGui::SetItemTooltip("Changes which of the two is showing. The space bar does the same, and so does\na click with the right button on the picture.");
    }
    flow(S(105) + button_width("Fit") + button_width("1:1") + ImGui::GetStyle().ItemSpacing.x * 2);  // the three stay together
    ImGui::SetNextItemWidth(S(105));
    float zoom = v.scale;
    if (ImGui::SliderFloat("##zoom", &zoom, 0.1f, 64.0f, "Zoom %.2fx", ImGuiSliderFlags_Logarithmic)) v.scale = zoom, v.fit = false;
    ImGui::SetItemTooltip("Screen pixels for each pixel of the image. The mouse wheel over a picture does the same.\n"
                          "Zoomed in, drag with the left button to move around. A click picks the colour under the pointer.");
    ImGui::SameLine();
    if (ImGui::Button("Fit")) v.fit = true;
    ImGui::SetItemTooltip("The whole picture, as large as it goes.");
    ImGui::SameLine();
    if (ImGui::Button("1:1")) v.scale = 1.0f, v.fit = false;
    ImGui::SetItemTooltip("One screen pixel for each pixel of the image.");

    update_view_textures();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < S(120) || avail.y < S(120)) return;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("pictures", avail);
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const bool pressed = ImGui::IsItemActivated(), released = ImGui::IsItemDeactivated();
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Two pictures of the same size, side by side or one above the other, whichever shows them larger. Flipping,
    // there is one, with the whole area to itself.
    const int count = v.mode == 1 ? 1 : 2;
    const float gap = S(8), head = ImGui::GetTextLineHeight() + S(6);  // between the two pictures; the heading over each
    const float w = (float)s.w, h = (float)s.h;
    const float fit_side = std::min((avail.x - gap) * 0.5f / w, (avail.y - head) / h);
    const float fit_stack = std::min(avail.x / w, ((avail.y - gap) * 0.5f - head) / h);
    const bool side = fit_side >= fit_stack;
    const float fit = std::max(count == 1 ? std::min(avail.x / w, (avail.y - head) / h) : side ? fit_side : fit_stack, 0.01f);
    const ImVec2 pane = count == 1 ? avail
                        : side     ? ImVec2(std::floor((avail.x - gap) * 0.5f), avail.y)
                                   : ImVec2(avail.x, std::floor((avail.y - gap) * 0.5f));
    ImVec2 a[2], b[2];  // the corners of each picture's area, under its heading
    for (int k = 0; k < count; k++) {
        ImVec2 o = side ? ImVec2(p0.x + k * (pane.x + gap), p0.y) : ImVec2(p0.x, p0.y + k * (pane.y + gap));
        a[k] = ImVec2(o.x, o.y + head);
        b[k] = ImVec2(o.x + pane.x, o.y + pane.y);
    }
    const ImVec2 half((b[0].x - a[0].x) * 0.5f, (b[0].y - a[0].y) * 0.5f);
    int over = -1;  // the picture the pointer is on
    for (int k = 0; k < count && hovered; k++)
        if (io.MousePos.x >= a[k].x && io.MousePos.x < b[k].x && io.MousePos.y >= a[k].y && io.MousePos.y < b[k].y) over = k;
    if (count == 1 && ((hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) || (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Space, false))))
        v.flipped = !v.flipped;

    // Zoom about the pointer, and drag to move. Both pictures always show the same part of the image.
    static bool moved = false;
    if (pressed) moved = false;
    if (v.fit) v.scale = fit;
    v.scale = std::clamp(v.scale, 0.02f, 64.0f);
    if (over >= 0 && io.MouseWheel != 0.0f) {
        const ImVec2 off(io.MousePos.x - (a[over].x + half.x), io.MousePos.y - (a[over].y + half.y));
        const float qx = v.cx + off.x / v.scale, qy = v.cy + off.y / v.scale;  // the image point under the pointer
        float z = v.scale * std::pow(1.25f, io.MouseWheel);
        if (z >= 2.0f) {  // whole steps from 2x up, so every pixel of the image is the same size on screen
            z = std::round(z);
            if (z == v.scale) z += io.MouseWheel > 0 ? 1.0f : -1.0f;
        }
        z = std::clamp(z, std::min(fit, 1.0f) * 0.5f, 64.0f);
        v.cx = qx - off.x / z, v.cy = qy - off.y / z;
        v.scale = z;
        v.fit = false;
    }
    if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        moved = true;
        v.cx -= io.MouseDelta.x / v.scale, v.cy -= io.MouseDelta.y / v.scale;
    }
    // A picture smaller than its area stays in the middle; a larger one cannot be moved past its own edges.
    const float hx = half.x / v.scale, hy = half.y / v.scale;
    v.cx = w <= 2 * hx ? w * 0.5f : std::clamp(v.cx, hx, w - hx);
    v.cy = h <= 2 * hy ? h * 0.5f : std::clamp(v.cy, hy, h - hy);
    const bool can_move = w > 2 * hx || h > 2 * hy;
    if (over >= 0 && can_move) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

    const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    const bool nearest = v.scale >= 1.0f && pio.DrawCallback_SetSamplerNearest && pio.DrawCallback_SetSamplerLinear;
    const std::string first = v.against > 0 ? "Snapshot: " + g_snapshots[v.against - 1].name : "Original, as 9-bit RGB";
    const platform::Texture* tex[2] = {v.against > 0 ? &v.snapshot : &v.original, &v.mapped};
    const char* heading[2] = {first.c_str(), "Current settings"};
    if (count == 1 && !v.flipped) tex[0] = tex[1], heading[0] = heading[1];  // flipping: the one area shows either
    char about[96] = "space bar or right click flips";  // beside the first heading, where there is room for it
    if (count == 2) snprintf(about, sizeof about, "%d x %d, %d colours", s.w, s.h, s.used);
    ImVec2 origin[2];  // where the image's top left corner is on screen
    for (int k = 0; k < count; k++) {
        dl->AddText(ImVec2(a[k].x + S(2), a[k].y - head + S(2)), IM_COL32(205, 210, 205, 255), heading[k]);
        const float about_w = ImGui::CalcTextSize(about).x;
        if (k == 0 && ImGui::CalcTextSize(heading[k]).x + about_w + S(20) < b[k].x - a[k].x)
            dl->AddText(ImVec2(b[k].x - about_w - S(2), a[k].y - head + S(2)), IM_COL32(125, 130, 125, 255), about);
        dl->AddRectFilled(a[k], b[k], IM_COL32(7, 9, 11, 255), S(4));
        origin[k] = ImVec2(std::floor(a[k].x + half.x - v.cx * v.scale + 0.5f), std::floor(a[k].y + half.y - v.cy * v.scale + 0.5f));
        if (!*tex[k]) continue;
        dl->PushClipRect(a[k], b[k], true);
        if (nearest) dl->AddCallback(pio.DrawCallback_SetSamplerNearest, nullptr);  // enlarged pixels stay sharp
        dl->AddImage((ImTextureID)(intptr_t)tex[k]->srv, origin[k], ImVec2(origin[k].x + w * v.scale, origin[k].y + h * v.scale));
        if (nearest) dl->AddCallback(pio.DrawCallback_SetSamplerLinear, nullptr);
        dl->PopClipRect();
    }

    // The pixel under the pointer: marked on both pictures, described, and picked by a click.
    if (over < 0 || (active && moved)) return;
    const float fx = (io.MousePos.x - origin[over].x) / v.scale, fy = (io.MousePos.y - origin[over].y) / v.scale;
    if (fx < 0 || fy < 0 || fx >= w || fy >= h) return;
    const int px = (int)fx, py = (int)fy;
    for (int k = 0; k < count; k++) {
        const ImVec2 m(origin[k].x + (px + 0.5f) * v.scale, origin[k].y + (py + 0.5f) * v.scale);
        const float r = std::max(v.scale * 0.5f, S(3)) + 1;
        dl->PushClipRect(a[k], b[k], true);
        dl->AddRect(ImVec2(m.x - r, m.y - r), ImVec2(m.x + r, m.y + r), IM_COL32(0, 0, 0, 255), 0, 0, 3.0f);
        dl->AddRect(ImVec2(m.x - r, m.y - r), ImVec2(m.x + r, m.y + r), IM_COL32(255, 220, 120, 255), 0, 0, 1.5f);
        dl->PopClipRect();
    }
    const size_t at = (size_t)py * s.w + px;
    const uint16_t entry = s.index[at];
    ImGui::BeginTooltip();
    ImGui::Text("x %d, y %d", px, py);
    if (entry == NO_COLOUR)
        ImGui::TextDisabled("transparent");
    else {
        auto row = [](const uint8_t rgb[3], const char* what) {
            swatch(rgb, ImGui::GetTextLineHeight());
            ImGui::SameLine();
            ImGui::Text("#%02X%02X%02X  %s", rgb[0], rgb[1], rgb[2], what);
        };
        const vce::Entry& e = g_pal.e[entry];
        ImGui::Text("$%03X   G%d R%d B%d", entry, e.g, e.r, e.b);
        row(&s.rgba[at * 4], "9-bit RGB");
        if (v.against > 0) row(g_snapshots[v.against - 1].pal.e[entry].rgb, g_snapshots[v.against - 1].name.c_str());
        row(e.rgb, "current settings");
        g_hot = entry;
        if (released && !moved) g_selected = entry;
    }
    ImGui::EndTooltip();
}

// ---------------------------------------------------------------- right panel
// Black on a light colour, white on a dark one.
ImU32 ink(const uint8_t rgb[3]) {
    return 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2] > 128 ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
}

void clip_mark(ImDrawList* dl, ImVec2 a, float cell, const uint8_t rgb[3]) {
    float k = std::max(3.0f, cell * 0.4f);
    dl->AddTriangleFilled(a, ImVec2(a.x + k, a.y), ImVec2(a.x, a.y + k), ink(rgb));
}

// Under the grid, as final colours: the eight greys the console can make (R = G = B), then all 32 luma codes
// with no chroma. Each grey sits above its own luma code (0, 4, 9, 13, 18, 22, 27, 31).
void ui_greys(float cell) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mouse = ImGui::GetIO().MousePos;
    const float h = std::max(S(22), cell * 2.0f);
    const ImU32 white = IM_COL32(255, 255, 255, 255), amber = IM_COL32(255, 220, 120, 255);
    auto outline = [&](ImVec2 a, ImVec2 b, ImU32 col) {
        dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), IM_COL32(0, 0, 0, 255), 0, 0, 3.0f);
        dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), col, 0, 0, 1.5f);
    };
    auto value = [&](const uint8_t rgb[3], float xa, float xb, float y) {  // the 8-bit level, written on the swatch
        char label[16];
        if (rgb[0] == rgb[1] && rgb[1] == rgb[2])
            snprintf(label, sizeof label, "%d", rgb[0]);
        else
            snprintf(label, sizeof label, "%02X%02X%02X", rgb[0], rgb[1], rgb[2]);
        ImVec2 ts = ImGui::CalcTextSize(label);
        if (ts.x + 2 <= xb - xa) dl->AddText(ImVec2(std::floor((xa + xb - ts.x) * 0.5f), std::floor(y - ts.y * 0.5f)), ink(rgb), label);
    };

    ImGui::Spacing();
    ImGui::TextUnformatted("Greys: R = G = B = 0 to 7");
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("greys", ImVec2(cell * 32, h));
    int hot = ImGui::IsItemHovered() ? std::clamp((int)((mouse.x - p0.x) / (cell * 4)), 0, 7) : -1;
    bool clicked = ImGui::IsItemClicked();
    bool split = g_have_ref && g_compare;
    for (int n = 0; n < 8; n++) {
        int i = index_of(n, n, n);
        const vce::Entry& e = g_pal.e[i];
        ImVec2 a(p0.x + n * cell * 4, p0.y), b(a.x + cell * 4, a.y + h);
        float mid = split ? a.x + cell * 2 : b.x;
        dl->AddRectFilled(a, ImVec2(mid, b.y), entry_colour(i));
        value(e.rgb, a.x, mid, a.y + h * 0.5f);
        if (split) {
            dl->AddRectFilled(ImVec2(mid, a.y), b, IM_COL32(g_ref[i][0], g_ref[i][1], g_ref[i][2], 255));
            value(g_ref[i], mid, b.x, a.y + h * 0.5f);
        }
        if (e.clipped) clip_mark(dl, a, cell, e.rgb);
    }
    for (int n = 0; n < 8; n++) {
        int i = index_of(n, n, n);
        ImVec2 a(p0.x + n * cell * 4, p0.y), b(a.x + cell * 4, a.y + h);
        if (hot < 0 && i == g_hot_prev) outline(a, b, amber);
        if (i == g_selected) outline(a, b, white);
        if (n == hot) outline(a, b, amber);
    }
    if (hot >= 0) {
        int i = index_of(hot, hot, hot);
        g_hot = i;
        if (clicked) g_selected = i;
        ImGui::BeginTooltip();
        describe_entry(i);
        ImGui::EndTooltip();
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Luma codes 0 to 31, no chroma");
    p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("lumas", ImVec2(cell * 32, h));
    hot = ImGui::IsItemHovered() ? std::clamp((int)((mouse.x - p0.x) / cell), 0, 31) : -1;
    for (int n = 0; n < 32; n++) {
        const vce::Luma& l = g_pal.luma[n];
        ImVec2 a(p0.x + n * cell, p0.y);
        dl->AddRectFilled(a, ImVec2(a.x + cell, a.y + h), IM_COL32(l.rgb[0], l.rgb[1], l.rgb[2], 255));
        if (l.clipped) clip_mark(dl, a, cell, l.rgb);
    }
    if (hot >= 0) {
        const vce::Luma& l = g_pal.luma[hot];
        ImVec2 a(p0.x + hot * cell, p0.y);
        outline(a, ImVec2(a.x + cell, a.y + h), amber);
        ImGui::BeginTooltip();
        swatch(l.rgb, ImGui::GetTextLineHeight() * 2.2f);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("Luma code %d   %.1f IRE", hot, l.y_ire);
        ImGui::Text("#%02X%02X%02X  (%d, %d, %d)%s", l.rgb[0], l.rgb[1], l.rgb[2], l.rgb[0], l.rgb[1], l.rgb[2], l.clipped ? "   clipped" : "");
        ImGui::EndGroup();
        ImGui::EndTooltip();
    }
}

// A colour as the grid shows it: as it is or, with "Greyscale", as its luma alone.
ImU32 grid_colour(const uint8_t rgb[3], const uint8_t luma[3]) {
    const uint8_t* c = g_grey_map ? luma : rgb;
    return IM_COL32(c[0], c[1], c[2], 255);
}

void ui_palette() {
    ImGui::SetNextItemWidth(S(110));
    ImGui::Combo("Order", &g_sort, "VCE index\0Luma\0Hue\0");
    ImGui::SameLine();
    ImGui::Checkbox("Greyscale", &g_grey_map);
    ImGui::SetItemTooltip("Shows each of the 512 as its luma alone, with the chroma taken away:\nthe grey of its luma code, as a final colour.");
    if (g_have_ref) {
        const char* split = "Split with reference";
        const float needs = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize(split).x;
        ImGui::SameLine();
        if (ImGui::GetContentRegionAvail().x < needs) ImGui::NewLine();  // the narrow column of the pictures view
        ImGui::Checkbox(split, &g_compare);
        ImGui::SetItemTooltip("Left half of each cell: this model. Right half: the reference palette.");
    }
    if (g_sort_built != g_sort) build_order();

    const int cols = 32, rows = 16;
    float cell = std::max(4.0f, std::floor(ImGui::GetContentRegionAvail().x / cols));
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("grid", ImVec2(cell * cols, cell * rows));
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mouse = ImGui::GetIO().MousePos;
    int hot_cell = -1;
    if (hovered) {
        int cx = (int)((mouse.x - p0.x) / cell), cy = (int)((mouse.y - p0.y) / cell);
        if (cx >= 0 && cx < cols && cy >= 0 && cy < rows) hot_cell = cy * cols + cx;
    }
    for (int n = 0; n < 512; n++) {
        int i = g_order[n];
        const vce::Entry& e = g_pal.e[i];
        ImVec2 a(p0.x + (n % cols) * cell, p0.y + (n / cols) * cell), b(a.x + cell, a.y + cell);
        const uint8_t* grey = g_pal.luma[e.y_code].rgb;  // the luma code with no chroma
        if (g_have_ref && g_compare) {
            float mid = a.x + std::floor(cell * 0.5f);
            const uint8_t y = (uint8_t)std::lround(0.299 * g_ref[i][0] + 0.587 * g_ref[i][1] + 0.114 * g_ref[i][2]);
            const uint8_t ref_grey[3] = {y, y, y};  // a reference palette has no luma codes: its luma by the usual weights
            dl->AddRectFilled(a, ImVec2(mid, b.y), grid_colour(e.rgb, grey));
            dl->AddRectFilled(ImVec2(mid, a.y), b, grid_colour(g_ref[i], ref_grey));
        } else
            dl->AddRectFilled(a, b, grid_colour(e.rgb, grey));
        if (e.clipped) clip_mark(dl, a, cell, g_grey_map ? grey : e.rgb);
    }
    auto outline = [&](int entry, ImU32 col) {
        for (int n = 0; n < 512; n++)
            if (g_order[n] == entry) {
                ImVec2 a(p0.x + (n % cols) * cell, p0.y + (n / cols) * cell);
                dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(a.x + cell + 1, a.y + cell + 1), IM_COL32(0, 0, 0, 255), 0, 0, 3.0f);
                dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(a.x + cell + 1, a.y + cell + 1), col, 0, 0, 1.5f);
            }
    };
    if (g_hot_prev >= 0 && hot_cell < 0) outline(g_hot_prev, IM_COL32(255, 220, 120, 255));
    if (g_selected >= 0) outline(g_selected, IM_COL32(255, 255, 255, 255));
    if (hot_cell >= 0) {
        int i = g_order[hot_cell];
        g_hot = i;
        if (clicked) g_selected = i;
        outline(i, IM_COL32(255, 220, 120, 255));
        ImGui::BeginTooltip();
        describe_entry(i);
        ImGui::EndTooltip();
    }
    ui_greys(cell);
    if (g_pal.clipped > 0) ImGui::TextDisabled("A corner mark means the colour clipped.");

    ImGui::SeparatorText("Selected colour");
    if (g_selected >= 0)
        describe_entry(g_selected);
    else
        ImGui::TextDisabled("Click a colour here, on the scope or on the waveform.");
}

// The scope, waveform and reference image as plots only, small, for the top of the right column in the pictures view.
void reduced_views() {
    const float side = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("reduced", ImVec2(side, side + ImGui::GetFrameHeightWithSpacing()), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (ImGui::BeginTabBar("reduced_views")) {
        if (ImGui::BeginTabItem("Vectorscope")) {
            scope_plot();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Waveform")) {
            wave_plot();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

// What a dropped image is for. Several dropped together can only be reference images.
void drop_popup() {
    if (g_open_drop_popup) {
        ImGui::OpenPopup("Dropped image");
        g_open_drop_popup = false;
    }
    if (!begin_modal("Dropped image")) return;
    const bool several = g_pending_images.size() > 1;
    const ImVec2 size(S(330), 0);
    bool done = g_pending_images.empty();
    if (several)
        ImGui::Text("%d images", (int)g_pending_images.size());
    else if (!done)
        ImGui::TextUnformatted(base_name(g_pending_images[0]).c_str());
    ImGui::Spacing();
    if (!several && !done && ImGui::Button("Plot it on the scope and waveform", size)) {
        load_image_plot(g_pending_images[0]);
        done = true;
    }
    if (!done && ImGui::Button(several ? "Add them as reference images" : "Add it as a reference image", size)) {
        for (const std::string& file : g_pending_images) load_source(file);
        done = true;
    }
    if (!done) ImGui::SetItemTooltip("For a picture in plain 9-bit RGB. It is shown as it is, and as the settings make it.");
    if (!several && !done && ImGui::Button("Show it behind the scope", size)) {
        load_photo(g_pending_images[0]);
        done = true;
    }
    if (several) ImGui::TextDisabled("Drop one image on its own to plot it, or to show it behind the scope.");
    if (!done && ImGui::Button("Cancel", size)) done = true;
    if (done) {
        g_pending_images.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace

// ---------------------------------------------------------------- entry points
namespace app {

void init(float dpi_scale) {
    g_scale = dpi_scale;
    load_graticule();
    rebuild();
    for (const auto& [action, value] : g_startup) {
        if (action == "ref") load_reference(value);
        else if (action == "photo") load_photo(value);
        else if (action == "plot") load_image_plot(value);
        else if (action == "ref-image") load_source(value);
        else if (action == "select") g_selected = std::clamp((int)std::strtol(value.c_str(), nullptr, 0), 0, 511);
        else drop_file(value);
    }
    g_startup.clear();
}

void shutdown() {
    platform::destroy_texture(g_graticule);
    platform::destroy_texture(g_photo.tex);
    platform::destroy_texture(g_image.tex);
    platform::destroy_texture(g_image.parade_tex);
    drop_view_textures();
}

void drop_file(const std::string& path) {
    std::string ext = extension(path);
    if (ext == ".pal")
        load_reference(path);
    else if (ext == ".pcevs")
        load_project(path);
    else if (ext == ".txt") {
        if (load_params(path, g_params, true)) {
            mark_loaded(base_name(path));
            g_dirty = true;
            g_status = "Loaded " + base_name(path);
        } else
            g_status = "No parameters found in " + base_name(path);
    } else {
        g_pending_images.push_back(path);
        g_open_drop_popup = true;
    }
}

void frame() {
    if (g_params.post) g_show_post = true;  // loaded settings that use the post adjustments: never applied out of sight
    if (g_dirty) rebuild();
    if (g_image.dirty) build_image_texture();
    g_hot_prev = g_hot;
    g_hot = -1;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("main", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    // Two layouts, each with its own column widths. In the pictures view the right column is narrower and starts
    // with the scope and waveform, reduced.
    const bool pictures = g_layout == 1;
    if (ImGui::BeginTable(pictures ? "layout_pictures" : "layout", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                          ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("model", ImGuiTableColumnFlags_WidthFixed, S(370));
        ImGui::TableSetupColumn("scope", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("palette", ImGuiTableColumnFlags_WidthFixed, pictures ? S(330) : S(470));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("model_panel");
        ui_model();
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("scope_panel", ImVec2(0, 0), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (pictures)
            ui_refimage();
        else if (ImGui::BeginTabBar("views")) {
            if (ImGui::BeginTabItem("Vectorscope")) {
                ui_scope();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Waveform", nullptr, g_start_view == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ui_wave();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Reference image", nullptr, g_start_view == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ui_refimage();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        g_start_view = -1;
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("palette_panel");
        if (pictures) reduced_views();
        ui_palette();
        ImGui::EndChild();
        ImGui::EndTable();
    }
    drop_popup();
    source_warning_popup();
    ImGui::End();
}

int run_cli(const std::vector<std::string>& args) {
    if (args.empty()) return -1;
    vce::Params params;
    std::string pal_path, png_path, dump_path, params_path, loaded_name;
    for (size_t i = 0; i < args.size(); i++) {
        bool more = i + 1 < args.size();
        const std::string& a = args[i];
        if (a == "--params" && more) {
            if (!load_params(args[++i], params)) return 2;
            loaded_name = base_name(args[i]);
        } else if (a == "--export-pal" && more)
            pal_path = args[++i];
        else if (a == "--export-png" && more)
            png_path = args[++i];
        else if (a == "--dump" && more)
            dump_path = args[++i];
        else if (a == "--save-params" && more)
            params_path = args[++i];
        else if (a == "--fit" && more) {  // fit the model to a .pal before doing anything else
            std::vector<uint8_t> data;
            if (!read_file(args[++i], data) || data.size() < 1536) return 2;
            uint8_t ref[512][3];
            std::memcpy(ref, data.data(), 1536);
            if (!vce::fit_to_palette(ref, params).ok) return 3;
            loaded_name = "Fitted to " + base_name(args[i]);
        }
        else if ((a == "--ref" || a == "--photo" || a == "--plot" || a == "--ref-image" || a == "--select") && more)
            g_startup.emplace_back(a.substr(2), args[++i]);
        else if (a == "--view" && more) {
            const std::string& v = args[++i];
            g_layout = v == "pictures" ? 1 : 0;
            g_start_view = v == "wave" ? 1 : v == "image" ? 2 : 0;
        }
        else if (a == "--bars" && more)
            g_view.bars = args[++i] == "100" ? 1 : 0;
        else if (a == "--zoom" && more)
            g_view.zoom = std::clamp((float)std::atof(args[++i].c_str()), 0.5f, 8.0f);
        else if (a.rfind("--", 0) == 0)
            return 2;
        else
            g_startup.emplace_back("drop", a);  // a file given on its own is treated like a dropped file
    }
    if (pal_path.empty() && png_path.empty() && dump_path.empty() && params_path.empty()) {
        g_params = params;  // nothing to export: open the window with these settings
        if (!loaded_name.empty()) mark_loaded(loaded_name);
        return -1;
    }
    if (!params_path.empty()) {
        std::string text = vce::to_text(params);
        if (!write_file(params_path, text.data(), text.size())) return 1;
    }
    vce::Palette pal = vce::build(params);
    if (!pal_path.empty()) {
        std::vector<uint8_t> bytes = pal_bytes(pal);
        if (!write_file(pal_path, bytes.data(), bytes.size())) return 1;
    }
    if (!png_path.empty() && !save_swatch_png(pal, png_path)) return 1;
    if (!dump_path.empty()) {
        std::string out = "index\tR\tG\tB\tY\tBY\tRY\tluma_ire\tu_ire\tv_ire\tamp_ire\tphase_deg\tpeak_sine\tpeak_three\tpeak_square\tr8\tg8\tb8\tclipped\n";
        char line[320];
        for (int i = 0; i < 512; i++) {
            const vce::Entry& e = pal.e[i];
            snprintf(line, sizeof line, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.3f\t%.3f\t%.3f\t%.3f\t%.2f\t%.3f\t%.3f\t%.3f\t%d\t%d\t%d\t%d\n", i, e.r, e.g, e.b,
                     e.y_code, e.by, e.ry, e.y_ire, e.u_ire, e.v_ire, std::hypot(e.u_ire, e.v_ire), phase_deg(e.u_ire, e.v_ire),
                     vce::envelope_peak(params, e.by, e.ry, vce::Carrier::Sine), vce::envelope_peak(params, e.by, e.ry, vce::Carrier::ThreeLevel),
                     vce::envelope_peak(params, e.by, e.ry, vce::Carrier::Square), e.rgb[0], e.rgb[1], e.rgb[2], e.clipped ? 1 : 0);
            out += line;
        }
        if (!write_file(dump_path, out.data(), out.size())) return 1;
    }
    return 0;
}

}  // namespace app
