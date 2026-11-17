// User interface: model controls, vectorscope, waveform monitor and palette grid.
#include "app.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>

#include "imgui.h"
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
std::string g_ref_name;
double g_ref_mean = 0;
int g_ref_max = 0, g_ref_max_index = 0;
bool g_compare = true;

int g_sort = 0, g_sort_built = -1;
std::vector<int> g_order;
int g_selected = -1;
int g_hot = -1, g_hot_prev = -1;  // entry under the mouse: this frame, and last frame (used to draw highlights)
std::string g_status;

// The graticule is the overlay image from the older Python tool, a tracing of an NTSC vectorscope graticule.
// Measured on the image: the six box centres sit at the standard colour-bar angles within 0.2 degrees, at 418.5
// pixels per unit of U/V (0.2% spread), and the outer circle has a radius of 316.8 pixels. The boxes of an NTSC
// graticule are for 75% bars with 7.5% setup, which a generator makes by scaling the whole picture to 92.5 IRE;
// burst stays at 40 IRE p-p. So one unit is 0.75 * 92.5 IRE and the circle is at 52.5 IRE. The dots in the
// photo of the real scope (IMG_2902.jpg), measured against its boxes, give 2.15 IRE per B-Y count on this scale;
// measured against its burst they give 2.14.
const double GRAT_CX = 319.73, GRAT_CY = 319.52;       // centre of the image, in pixels from its top left corner
const double GRAT_PX_PER_IRE = 418.5 / (0.75 * 92.5);  // image pixels per IRE
const double GRAT_VIEW_IRE = 53.5;                     // centre to the edge of the view at zoom 1: the whole image
// RGB data (a palette, a picture) is plotted as that picture encoded to NTSC with setup, so 75% RGB bars land
// in the boxes. That is what a vectorscope fed from an RGB source shows.
const double RGB_UNIT_IRE = 92.5;
platform::Texture g_graticule;

struct ScopeView {
    float zoom = 1.0f;
    float dot = 4.5f;
    float graticule = 0.85f;  // brightness of the graticule image; 0 hides it
    int bars = 0;             // scope gain: 0 = 75% bars land in the boxes, 1 = 100% bars do
    bool console = true, tv = false, ref = true, image = true, photo = true, labels = true;
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
    std::string name;
    std::vector<uint32_t> colours, counts;
    uint64_t total = 0;
    platform::Texture tex;
    float intensity = 1.0f;
    int splat = 2;
    bool dirty = false;
} g_image;

std::string g_pending_image;
bool g_open_drop_popup = false;

std::vector<std::pair<std::string, std::string>> g_startup;  // (action, value) pairs from the command line
int g_start_view = -1;

struct WaveView {
    int pattern = 0, level = 7, shape = 0;
    bool lowpass = false, measured = true;
} g_wave;

// Colour-bar envelope read off the user's waveform-monitor photo (IMG_2232.jpg, sync set to -40 IRE).
// Values are the outer edges of the trace, which is about TRACE_WIDTH thick.
struct Measured {
    int r, g, b;
    float top, bottom;
};
const Measured MEASURED[] = {
    {7, 7, 0, 105.9f, 51.2f}, {0, 7, 7, 102.3f, 21.7f}, {0, 7, 0, 93.6f, 11.2f},
    {7, 0, 7, 77.5f, -5.7f},  {7, 0, 0, 66.9f, -15.2f}, {0, 0, 7, 37.5f, -18.4f},
};
const float TRACE_WIDTH = 2.7f;

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

bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = platform::fopen_utf8(path, "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}

bool write_file(const std::string& path, const void* data, size_t n) {
    FILE* f = platform::fopen_utf8(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    fclose(f);
    return ok;
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

// The parameter file also carries the alignment of the scope photo, so it does not have to be redone.
std::string params_text() {
    std::string text = vce::to_text(g_params);
    if (g_photo.tex) {
        char line[128];
        snprintf(line, sizeof line, "photo_x=%.3f\nphoto_y=%.3f\nphoto_width=%.3f\nphoto_rot=%.3f\nphoto_mirror=%d\n", g_photo.x, g_photo.y,
                 g_photo.width, g_photo.rot, g_photo.mirror ? 1 : 0);
        text += line;
        text += "photo_file=" + g_photo.path + "\n";
    }
    return text;
}

bool load_params(const std::string& path, vce::Params& p, bool with_photo = false) {
    std::vector<uint8_t> data;
    if (!read_file(path, data)) return false;
    std::string text(data.begin(), data.end());
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
        bool absolute = photo_file.size() > 1 && (photo_file[1] == ':' || photo_file[0] == '/' || photo_file[0] == '\\');
        size_t slash = path.find_last_of("/\\");
        // A bare file name is looked for beside the parameter file, so a capture and its alignment travel together.
        if (!absolute && slash != std::string::npos) photo_file = path.substr(0, slash + 1) + photo_file;
        if (photo_file != g_photo.path) load_photo(photo_file);
    }
    return true;
}

void load_image_plot(const std::string& path) {
    int w, h, n;
    uint8_t* data = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!data) {
        g_status = "Could not read image: " + base_name(path);
        return;
    }
    size_t total = (size_t)w * h, step = 1;
    while (total / step > 6000000) step++;
    std::unordered_map<uint32_t, uint32_t> seen;
    seen.reserve(1 << 16);
    g_image.total = 0;
    for (size_t i = 0; i < total; i += step) {
        const uint8_t* p = data + i * 4;
        if (p[3] < 128) continue;
        seen[(uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)]++;
        g_image.total++;
    }
    stbi_image_free(data);
    g_image.colours.clear();
    g_image.counts.clear();
    for (auto& kv : seen) {
        g_image.colours.push_back(kv.first);
        g_image.counts.push_back(kv.second);
    }
    g_image.name = base_name(path);
    g_image.dirty = true;
    g_view.image = true;
    char msg[256];
    snprintf(msg, sizeof msg, "%s: %d x %d, %d distinct colours", g_image.name.c_str(), w, h, (int)g_image.colours.size());
    g_status = msg;
}

void build_image_texture() {
    g_image.dirty = false;
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
    int w, h, n;
    uint8_t* data = stbi_load(path.c_str(), &w, &h, &n, 4);
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
    {"Ideal, no rotation", "The chip and its mixing resistors only: the measured numbers with the rotation taken out."},
};
const int PRESET_COUNT = (int)(sizeof PRESETS / sizeof PRESETS[0]);

vce::Params preset_params(int i) {
    vce::Params p;
    if (i == 1) p.rotation_deg = 0;
    return p;
}

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
void ui_model() {
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
        std::string path = platform::open_file_dialog(L"Load parameters", L"Parameters (*.txt)\0*.txt\0All files\0*.*\0");
        if (!path.empty()) {
            if (load_params(path, g_params, true)) {
                mark_loaded(base_name(path));
                g_dirty = true;
                g_status = "Loaded " + base_name(path);
            } else
                g_status = "No parameters found in " + base_name(path);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Save...")) {
        std::string path = platform::save_file_dialog(L"Save parameters", L"Parameters (*.txt)\0*.txt\0", L"txt", L"vce_parameters.txt");
        if (!path.empty()) {
            std::string text = params_text();
            g_status = write_file(path, text.data(), text.size()) ? "Saved " + base_name(path) : "Could not write " + base_name(path);
        }
    }

    ImGui::SeparatorText("Console output");
    slide("White (IRE)", &g_params.white_ire, 60, 110, "%.1f",
          "Luma code 31 with sync at 40 IRE.\nMeasured 89 on the waveform monitor.");
    slide("Black (IRE)", &g_params.black_ire, -5, 15, "%.1f", "Luma code 0. Measured at blanking level: no setup.");
    slide("Chroma per count", &g_params.chroma_ire, 1.5, 3.0, "%.3f IRE",
          "Amplitude of one B-Y step, as a vectorscope set to burst = 40 IRE p-p reads it.\nThe scope photo gives 2.14 measured against its burst and 2.15 against its graticule boxes.");
    slide("R-Y / B-Y gain", &g_params.ry_gain, 1.0, 1.7, "%.4f",
          "How much stronger R-Y is mixed in than B-Y.\nThe mixing resistors give 8.2k / 6.2k = 1.3226; the scope photo gives 1.29 to 1.32.");
    slide("Rotation", &g_params.rotation_deg, -25, 25, "%.2f deg", "The whole chroma pattern against burst. The scope photo gives -8.3 to -8.7,\nand about ten consoles across the range showed much the same, so it is part of the default.");
    slide("Quadrature error", &g_params.quadrature_deg, -20, 20, "%.2f deg", "R-Y axis away from 90 degrees to the B-Y axis.");
    slide("Burst p-p", &g_params.burst_pp_ire, 30, 45, "%.1f IRE",
          "Burst as it really is with sync at 40 IRE. Measured 37 to 40.\nOnly matters to the waveform view, and to the palette when ACC is off.");

    ImGui::SeparatorText("Television");
    if (ImGui::Checkbox("Chroma gain follows burst (ACC)", &g_params.acc)) g_dirty = true;
    if (ImGui::Checkbox("Contrast set so white is full scale", &g_params.auto_contrast)) g_dirty = true;
    if (!g_params.auto_contrast) {
        slide("Contrast", &g_params.contrast, 0.5, 1.6, "%.3f", "1.000 puts 100 IRE at full scale.");
        slide("Brightness", &g_params.brightness, -0.15, 0.15, "%.3f", nullptr);
    }
    slide("Colour", &g_params.saturation, 0.0, 2.0, "%.3f", "The set's colour (saturation) control.");
    slide("Tint", &g_params.tint_deg, -30, 30, "%.2f deg", "The set's tint (hue) control.");

    ImGui::SeparatorText("Picture tube");
    if (ImGui::Combo("Output", &g_params.tube, "Decoded values as sRGB\0Phosphors and gamma\0")) g_dirty = true;
    if (g_params.tube == 1) {
        if (ImGui::Combo("Phosphors", &g_params.primaries, "SMPTE C\0NTSC 1953\0Rec.709\0")) g_dirty = true;
        if (ImGui::Combo("White", &g_params.white_point, "D65\0Illuminant C\0""9300 K\0")) g_dirty = true;
        slide("Gamma", &g_params.crt_gamma, 1.8, 2.8, "%.2f", nullptr);
    }

    ImGui::SeparatorText("Reference palette");
    if (ImGui::Button("Load .pal...")) {
        std::string path = platform::open_file_dialog(L"Reference palette", L"Palette (*.pal)\0*.pal\0All files\0*.*\0");
        if (!path.empty()) load_reference(path);
    }
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
        ImGui::SetItemTooltip("Sets chroma per count, R-Y gain, rotation and quadrature error\n(and contrast if needed) so the model "
                              "reproduces this palette.\nColour, tint and tube settings are reset.");
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
void ui_scope() {
    ImGui::Checkbox("Console", &g_view.console);
    ImGui::SetItemTooltip("The 512 colours where the console's signal puts them (169 distinct points).");
    ImGui::SameLine();
    ImGui::Checkbox("TV output", &g_view.tv);
    ImGui::SetItemTooltip("The final 8-bit colours, plotted the way a scope shows an RGB picture: 75%% RGB bars land in the boxes.\n"
                          "With the decoded values sent straight out, a line shows how far clipping moved a colour.");
    ImGui::SameLine();
    ImGui::BeginDisabled(!g_have_ref);
    ImGui::Checkbox("Reference", &g_view.ref);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("The reference palette, plotted the same way as TV output.");
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
                          "Its boxes are 75%% bars with 7.5%% setup: red at 43.9 IRE with burst at 40 IRE p-p. The circle is at 52.5 IRE.");

    ImGui::SetNextItemWidth(S(150));
    ImGui::SliderFloat("Zoom", &g_view.zoom, 0.5f, 8.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
    ImGui::SetItemTooltip("1x shows the whole graticule. The mouse wheel over the scope does the same.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(S(110));
    ImGui::SliderFloat("Dot", &g_view.dot, 1.0f, 12.0f, "%.1f");
    ImGui::SameLine(0, S(20));
    if (ImGui::Button("Plot an image...")) {
        std::string path = platform::open_file_dialog(L"Image to plot", L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.gif\0All files\0*.*\0");
        if (!path.empty()) load_image_plot(path);
    }
    ImGui::SameLine();
    if (ImGui::Button("Photo behind scope...")) {
        std::string path = platform::open_file_dialog(L"Photo of a real vectorscope", L"Images\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0All files\0*.*\0");
        if (!path.empty()) load_photo(path);
    }

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
        if (ImGui::Button("Remove##image")) {
            g_image.colours.clear();
            g_image.counts.clear();
            platform::destroy_texture(g_image.tex);
        }
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
    if (g_view.tv) {
        float k = std::max(1.5f, g_view.dot * 0.6f);
        for (int i = 0; i < 512; i++) {
            const vce::Entry& e = g_pal.e[i];
            double y, u, v;
            vce::rgb_to_yuv(e.rgb, y, u, v);
            ImVec2 p = P(u * RGB_UNIT_IRE, v * RGB_UNIT_IRE);
            if (e.clipped && g_params.tube == 0) {  // from where the colour was before it clipped
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

void ui_wave() {
    ImGui::SetNextItemWidth(S(190));
    ImGui::Combo("Pattern", &g_wave.pattern, "Colour bars\0Grey steps\0All 32 luma codes\0R, G and B ramps\0");
    if (g_wave.pattern == 0) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(90));
        ImGui::SliderInt("Level", &g_wave.level, 1, 7);
    }
    ImGui::SetNextItemWidth(S(190));
    ImGui::Combo("Carrier", &g_wave.shape, "Fundamental only\0Three-level (+ + 0 - - 0)\0Square\0");
    ImGui::SetItemTooltip("Fundamental only is what a waveform monitor shows: its own bandwidth removes the carrier's\n"
                          "harmonics, and with burst at 37 IRE it matches the measured bars within 7%%.\n"
                          "The stepped shapes show the peaks a wide-band oscilloscope would see at the pin.");
    ImGui::SameLine();
    ImGui::Checkbox("Low-pass", &g_wave.lowpass);
    ImGui::SameLine();
    ImGui::Checkbox("Measured", &g_wave.measured);
    ImGui::SetItemTooltip("Colour-bar envelope read off the waveform-monitor photo of the real console\n"
                          "(shown for full-level colour bars).");

    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 size(std::max(S(300), avail.x), std::max(S(220), avail.y));
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

    // sync and burst
    const ImU32 trace = IM_COL32(120, 235, 150, 255);
    float sync_w = (right - left) * 0.11f, x = left;
    ImVec2 sync[6] = {ImVec2(x, Y(0)), ImVec2(x + sync_w * 0.12f, Y(0)), ImVec2(x + sync_w * 0.12f, Y(-40)),
                      ImVec2(x + sync_w * 0.45f, Y(-40)), ImVec2(x + sync_w * 0.45f, Y(0)), ImVec2(x + sync_w, Y(0))};
    dl->AddPolyline(sync, 6, trace, 0, 1.5f);
    if (!g_wave.lowpass) {
        float b = (float)g_params.burst_pp_ire * 0.5f;
        dl->AddRectFilled(ImVec2(x + sync_w * 0.55f, Y(b)), ImVec2(x + sync_w * 0.9f, Y(-b)), IM_COL32(210, 190, 90, 110));
        dl->AddRect(ImVec2(x + sync_w * 0.55f, Y(b)), ImVec2(x + sync_w * 0.9f, Y(-b)), IM_COL32(210, 190, 90, 255));
    }

    std::vector<Bar> bars = wave_bars();
    float x0 = left + sync_w, bw = (right - x0) / (float)bars.size();
    vce::Carrier shape = g_wave.shape == 0 ? vce::Carrier::Sine : g_wave.shape == 1 ? vce::Carrier::ThreeLevel : vce::Carrier::Square;
    double burst_scale = g_params.burst_pp_ire / 40.0;
    int hot_bar = -1;
    for (size_t n = 0; n < bars.size(); n++) {
        const Bar& bar = bars[n];
        float xa = x0 + bw * n, xb = xa + bw;
        double luma, peak = 0;
        ImU32 col;
        if (bar.index >= 0) {
            const vce::Entry& e = g_pal.e[bar.index];
            luma = e.y_ire;
            if (!g_wave.lowpass) peak = vce::envelope_peak(g_params, e.by, e.ry, shape) * burst_scale;
            col = entry_colour(bar.index);
        } else {
            luma = g_params.black_ire + bar.y_code / 31.0 * (g_params.white_ire - g_params.black_ire);
            int grey = (int)(255.0 * bar.y_code / 31.0);
            col = IM_COL32(grey, grey, grey, 255);
        }
        float pad = bw > 6 ? 1.0f : 0.0f;
        if (peak > 0.05) {
            ImU32 fill = (col & 0x00FFFFFF) | 0x70000000;
            dl->AddRectFilled(ImVec2(xa + pad, Y(luma + peak)), ImVec2(xb - pad, Y(luma - peak)), fill);
            dl->AddLine(ImVec2(xa + pad, Y(luma + peak)), ImVec2(xb - pad, Y(luma + peak)), trace, 1.5f);
            dl->AddLine(ImVec2(xa + pad, Y(luma - peak)), ImVec2(xb - pad, Y(luma - peak)), trace, 1.5f);
            dl->AddLine(ImVec2(xa + pad, Y(luma)), ImVec2(xb - pad, Y(luma)), IM_COL32(255, 255, 255, 120));
        } else
            dl->AddLine(ImVec2(xa + pad, Y(luma)), ImVec2(xb - pad, Y(luma)), trace, 2.0f);
        dl->AddRectFilled(ImVec2(xa + pad, bottom + S(4)), ImVec2(xb - pad, bottom + S(8)), col);
        ImVec2 ts = ImGui::CalcTextSize(bar.label.c_str());
        if (ts.x < bw) dl->AddText(ImVec2((xa + xb - ts.x) * 0.5f, bottom + S(8)), text, bar.label.c_str());

        if (g_wave.measured && !g_wave.lowpass && bar.index >= 0)
            for (const Measured& m : MEASURED) {
                if (index_of(m.r, m.g, m.b) != bar.index) continue;
                const ImU32 mc = IM_COL32(255, 210, 60, 255);
                for (float ire : {m.top - TRACE_WIDTH * 0.5f, m.bottom + TRACE_WIDTH * 0.5f}) {
                    dl->AddLine(ImVec2(xa + bw * 0.2f, Y(ire)), ImVec2(xb - bw * 0.2f, Y(ire)), mc, 2.0f);
                    dl->AddCircleFilled(ImVec2(xa + bw * 0.2f, Y(ire)), S(3), mc);
                    dl->AddCircleFilled(ImVec2(xb - bw * 0.2f, Y(ire)), S(3), mc);
                }
            }
        if (hovered && io.MousePos.x >= xa && io.MousePos.x < xb) hot_bar = (int)n;
    }
    if (g_wave.measured && !g_wave.lowpass && g_wave.pattern == 0 && g_wave.level == 7)
        dl->AddText(ImVec2(right - S(250), top + S(2)), IM_COL32(255, 210, 60, 255), "yellow marks: measured on the console");
    if (hovered) {
        char label[32];
        snprintf(label, sizeof label, "%.1f IRE", top_ire - (io.MousePos.y - top) / (bottom - top) * (top_ire - bottom_ire));
        dl->AddText(ImVec2(left + S(6), top + S(2)), text, label);
    }
    dl->PopClipRect();

    if (hot_bar >= 0) {
        const Bar& bar = bars[hot_bar];
        ImGui::BeginTooltip();
        if (bar.index >= 0) {
            const vce::Entry& e = g_pal.e[bar.index];
            g_hot = bar.index;
            if (clicked) g_selected = bar.index;
            double peak = vce::envelope_peak(g_params, e.by, e.ry, shape) * burst_scale;
            ImGui::Text("$%03X  G%d R%d B%d", bar.index, e.g, e.r, e.b);
            ImGui::Text("Luma %.1f IRE (code %d)", e.y_ire, e.y_code);
            ImGui::Text("Chroma %.1f IRE p-p, envelope %.1f to %.1f IRE", 2 * peak, e.y_ire - peak, e.y_ire + peak);
            for (const Measured& m : MEASURED)
                if (index_of(m.r, m.g, m.b) == bar.index)
                    ImGui::Text("Measured envelope %.1f to %.1f IRE", m.bottom + TRACE_WIDTH * 0.5f, m.top - TRACE_WIDTH * 0.5f);
        } else
            ImGui::Text("Luma code %d: %.1f IRE", bar.y_code, g_params.black_ire + bar.y_code / 31.0 * (g_params.white_ire - g_params.black_ire));
        ImGui::EndTooltip();
    }
}

// ---------------------------------------------------------------- right panel
void ui_palette() {
    ImGui::SetNextItemWidth(S(130));
    ImGui::Combo("Order", &g_sort, "VCE index\0Luma\0Hue\0");
    if (g_have_ref) {
        ImGui::SameLine();
        ImGui::Checkbox("Split with reference", &g_compare);
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
        if (g_have_ref && g_compare) {
            float mid = a.x + std::floor(cell * 0.5f);
            dl->AddRectFilled(a, ImVec2(mid, b.y), entry_colour(i));
            dl->AddRectFilled(ImVec2(mid, a.y), b, IM_COL32(g_ref[i][0], g_ref[i][1], g_ref[i][2], 255));
        } else
            dl->AddRectFilled(a, b, entry_colour(i));
        if (e.clipped) {
            bool light = 0.299 * e.rgb[0] + 0.587 * e.rgb[1] + 0.114 * e.rgb[2] > 128;
            float k = std::max(3.0f, cell * 0.4f);
            dl->AddTriangleFilled(ImVec2(a.x, a.y), ImVec2(a.x + k, a.y), ImVec2(a.x, a.y + k), light ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255));
        }
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
    if (g_pal.clipped > 0) ImGui::TextDisabled("A corner mark means the colour clipped.");

    ImGui::SeparatorText("Selected colour");
    if (g_selected >= 0)
        describe_entry(g_selected);
    else
        ImGui::TextDisabled("Click a colour here, on the scope or on the waveform.");
}

void drop_popup() {
    if (g_open_drop_popup) {
        ImGui::OpenPopup("Dropped image");
        g_open_drop_popup = false;
    }
    ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Dropped image", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("%s", base_name(g_pending_image).c_str());
        ImGui::Spacing();
        if (ImGui::Button("Plot its colours on the scope")) {
            load_image_plot(g_pending_image);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Show it behind the scope")) {
            load_photo(g_pending_image);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
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
        else if (action == "select") g_selected = std::clamp((int)std::strtol(value.c_str(), nullptr, 0), 0, 511);
        else drop_file(value);
    }
    g_startup.clear();
}

void shutdown() {
    platform::destroy_texture(g_graticule);
    platform::destroy_texture(g_photo.tex);
    platform::destroy_texture(g_image.tex);
}

void drop_file(const std::string& path) {
    std::string ext = extension(path);
    if (ext == ".pal")
        load_reference(path);
    else if (ext == ".txt") {
        if (load_params(path, g_params, true)) {
            mark_loaded(base_name(path));
            g_dirty = true;
            g_status = "Loaded " + base_name(path);
        } else
            g_status = "No parameters found in " + base_name(path);
    } else {
        g_pending_image = path;
        g_open_drop_popup = true;
    }
}

void frame() {
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
    if (ImGui::BeginTable("layout", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV, ImGui::GetContentRegionAvail())) {
        ImGui::TableSetupColumn("model", ImGuiTableColumnFlags_WidthFixed, S(370));
        ImGui::TableSetupColumn("scope", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("palette", ImGuiTableColumnFlags_WidthFixed, S(470));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::BeginChild("model_panel");
        ui_model();
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("scope_panel", ImVec2(0, 0), 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (ImGui::BeginTabBar("views")) {
            if (ImGui::BeginTabItem("Vectorscope")) {
                ui_scope();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Waveform", nullptr, g_start_view == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ui_wave();
                ImGui::EndTabItem();
            }
            g_start_view = -1;
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("palette_panel");
        ui_palette();
        ImGui::EndChild();
        ImGui::EndTable();
    }
    drop_popup();
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
        else if ((a == "--ref" || a == "--photo" || a == "--plot" || a == "--select") && more)
            g_startup.emplace_back(a.substr(2), args[++i]);
        else if (a == "--view" && more)
            g_start_view = args[++i] == "wave" ? 1 : 0;
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
