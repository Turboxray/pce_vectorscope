#include "vce_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace vce {
namespace {

const double PI = 3.14159265358979323846;
double rad(double deg) { return deg * PI / 180.0; }

double carrier(double t, Carrier shape) {
    double ph = std::fmod(std::fmod(t, 6.0) + 6.0, 6.0);  // one cycle = 6 master clock ticks
    if (shape == Carrier::ThreeLevel) return (ph > 0.5 && ph < 2.5) ? 1.0 : (ph > 3.5 && ph < 5.5) ? -1.0 : 0.0;
    return ph < 3.0 ? 1.0 : -1.0;
}

}  // namespace

void rom(int r, int g, int b, int& y_code, int& by, int& ry) {
    y_code = (int)std::floor(1.33 * r + 2.61 * g + 0.49 * b + 0.5);
    by = 2 * b - r - g;
    ry = 2 * (r - g);
}

double picture_gain(const Params& p) {
    double span = p.map_white_ire - p.map_black_ire;
    return span > 1e-9 ? (p.post ? p.contrast : 1.0) / span : 0.0;
}

Palette build(const Params& p) {
    Palette pal;
    const double rot = rad(p.rotation_deg);
    const double v_axis = rot + rad(90.0 + p.quadrature_deg);
    // The post adjustments, all neutral when they are switched off.
    const double tint = p.post ? rad(p.tint_deg) : 0.0;
    const double gain = picture_gain(p);
    const double brightness = p.post ? p.brightness : 0.0;
    const double chroma_gain = gain * (p.post ? p.saturation : 1.0);  // chroma is as a scope reads it: against burst

    auto luma_ire = [&](int y_code) { return p.black_ire + y_code / 31.0 * (p.white_ire - p.black_ire); };

    // Luma mapping, decode and post adjustment: a luma level and a chroma vector to the final 8-bit colour.
    // Returns true if it clipped.
    auto show = [&](double y_ire, double u_ire, double v_ire, double decoded[3], uint8_t rgb[3]) {
        // Standard NTSC decode.
        double u = u_ire * std::cos(tint) - v_ire * std::sin(tint);
        double v = u_ire * std::sin(tint) + v_ire * std::cos(tint);
        double y = (y_ire - p.map_black_ire) * gain + brightness;
        double b_y = u * chroma_gain / 0.492;
        double r_y = v * chroma_gain / 0.877;
        double g_y = -(0.299 * r_y + 0.114 * b_y) / 0.587;
        decoded[0] = y + r_y;
        decoded[1] = y + g_y;
        decoded[2] = y + b_y;

        const double tol = 0.5 / 255.0;
        bool clipped = false;
        double out[3];
        for (int k = 0; k < 3; k++) {
            if (decoded[k] < -tol || decoded[k] > 1.0 + tol) clipped = true;
            out[k] = std::clamp(decoded[k], 0.0, 1.0);
        }
        for (int k = 0; k < 3; k++) rgb[k] = (uint8_t)std::clamp((int)std::nearbyint(255.0 * out[k]), 0, 255);
        return clipped;
    };

    for (int i = 0; i < 512; i++) {
        Entry& e = pal.e[i];
        e.g = (i >> 6) & 7;
        e.r = (i >> 3) & 7;
        e.b = i & 7;
        rom(e.r, e.g, e.b, e.y_code, e.by, e.ry);

        double a = p.chroma_ire * e.by;
        double c = p.chroma_ire * p.ry_gain * e.ry;
        e.u_ire = a * std::cos(rot) + c * std::cos(v_axis);
        e.v_ire = a * std::sin(rot) + c * std::sin(v_axis);
        e.y_ire = luma_ire(e.y_code);
        e.clipped = show(e.y_ire, e.u_ire, e.v_ire, e.decoded, e.rgb);
        if (e.clipped) pal.clipped++;
    }
    for (int n = 0; n < 32; n++) {
        Luma& l = pal.luma[n];
        double decoded[3];
        l.y_ire = luma_ire(n);
        l.clipped = show(l.y_ire, 0.0, 0.0, decoded, l.rgb);
    }
    return pal;
}

double envelope_peak(const Params& p, int by, int ry, Carrier shape) {
    double a = p.chroma_ire * by, c = p.chroma_ire * p.ry_gain * ry;
    if (shape == Carrier::Sine) {
        double q = rad(90.0 + p.quadrature_deg);
        return std::hypot(a + c * std::cos(q), c * std::sin(q));
    }
    // chroma_ire is the fundamental; the steps of a stepped carrier are smaller by this factor
    double fundamental = shape == Carrier::ThreeLevel ? 2.0 * std::sqrt(3.0) / PI : 4.0 / PI;
    double peak = 0;
    for (int i = 0; i < 24; i++) {
        double t = (i + 0.5) * 0.25;
        peak = std::max(peak, std::fabs(a * carrier(t, shape) + c * carrier(t - 1.5, shape)));
    }
    return peak / fundamental;
}

void rgb_to_yuv(const uint8_t rgb[3], double& y, double& u, double& v) {
    double r = rgb[0] / 255.0, g = rgb[1] / 255.0, b = rgb[2] / 255.0;
    y = 0.299 * r + 0.587 * g + 0.114 * b;
    u = 0.492 * (b - y);
    v = 0.877 * (r - y);
}

FitResult fit_to_palette(const uint8_t ref[512][3], Params& p) {
    FitResult res;
    // Luma first: reference Y against code / 31.
    double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    bool usable[512];
    for (int i = 0; i < 512; i++) {
        usable[i] = true;
        for (int k = 0; k < 3; k++)
            if (ref[i][k] == 0 || ref[i][k] == 255) usable[i] = false;
        if (!usable[i]) continue;
        int yc, by, ry;
        rom((i >> 3) & 7, (i >> 6) & 7, i & 7, yc, by, ry);
        double y, u, v;
        rgb_to_yuv(ref[i], y, u, v);
        double x = yc / 31.0;
        n += 1; sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    res.used = (int)n;
    if (n < 16) return res;
    double den = n * sxx - sx * sx;
    if (std::fabs(den) < 1e-12) return res;
    double slope = (n * sxy - sx * sy) / den;
    double offset = (sy - slope * sx) / n;

    Params q = p;
    q.post = false;
    q.contrast = 1.0;
    q.brightness = 0.0;
    q.saturation = 1.0;
    q.tint_deg = 0.0;
    if (std::fabs(slope - 1.0) < 0.004 && std::fabs(offset) < 0.004) {  // the reference shows the console's white at full scale
        q.map_white_ire = q.white_ire;
        q.map_black_ire = q.black_ire;
    } else {
        if (slope < 1e-9) return res;
        double span = (q.white_ire - q.black_ire) / slope;  // IRE from black to full white in the reference
        q.map_black_ire = q.black_ire - offset * span;
        q.map_white_ire = q.map_black_ire + span;
    }
    double gain = picture_gain(q);
    if (gain <= 0) return res;

    // Chroma: least squares for the 2x2 map from (B-Y, R-Y) counts to the (U, V) the reference shows.
    double bb = 0, br = 0, rr = 0, bu = 0, ru = 0, bv = 0, rv = 0;
    for (int i = 0; i < 512; i++) {
        if (!usable[i]) continue;
        int yc, by, ry;
        rom((i >> 3) & 7, (i >> 6) & 7, i & 7, yc, by, ry);
        double y, u, v;
        rgb_to_yuv(ref[i], y, u, v);
        u /= gain;
        v /= gain;
        bb += by * by; br += by * ry; rr += ry * ry;
        bu += by * u; ru += ry * u; bv += by * v; rv += ry * v;
    }
    double det = bb * rr - br * br;
    if (std::fabs(det) < 1e-9) return res;
    double m11 = (bu * rr - ru * br) / det, m12 = (ru * bb - bu * br) / det;  // U = m11 by + m12 ry
    double m21 = (bv * rr - rv * br) / det, m22 = (rv * bb - bv * br) / det;  // V = m21 by + m22 ry
    double k = std::hypot(m11, m21);
    if (k < 1e-9) return res;
    q.chroma_ire = k;
    q.rotation_deg = std::atan2(m21, m11) * 180.0 / PI;
    q.ry_gain = std::hypot(m12, m22) / k;
    double quad = std::atan2(m22, m12) * 180.0 / PI - q.rotation_deg - 90.0;
    while (quad > 180.0) quad -= 360.0;
    while (quad < -180.0) quad += 360.0;
    q.quadrature_deg = quad;

    double err = 0;
    for (int i = 0; i < 512; i++) {
        if (!usable[i]) continue;
        int yc, by, ry;
        rom((i >> 3) & 7, (i >> 6) & 7, i & 7, yc, by, ry);
        double y, u, v;
        rgb_to_yuv(ref[i], y, u, v);
        double du = u / gain - (m11 * by + m12 * ry), dv = v / gain - (m21 * by + m22 * ry);
        err += du * du + dv * dv;
    }
    res.rms_ire = std::sqrt(err / n);
    res.ok = true;
    p = q;
    return res;
}

namespace {
struct Field {
    const char* key;
    double Params::* d;
    bool Params::* b;
    int Params::* i;
};
const Field FIELDS[] = {
    {"white_ire", &Params::white_ire, nullptr, nullptr},
    {"black_ire", &Params::black_ire, nullptr, nullptr},
    {"chroma_ire", &Params::chroma_ire, nullptr, nullptr},
    {"ry_gain", &Params::ry_gain, nullptr, nullptr},
    {"rotation_deg", &Params::rotation_deg, nullptr, nullptr},
    {"quadrature_deg", &Params::quadrature_deg, nullptr, nullptr},
    {"map_white_ire", &Params::map_white_ire, nullptr, nullptr},
    {"map_black_ire", &Params::map_black_ire, nullptr, nullptr},
    {"post", nullptr, &Params::post, nullptr},
    {"contrast", &Params::contrast, nullptr, nullptr},
    {"brightness", &Params::brightness, nullptr, nullptr},
    {"saturation", &Params::saturation, nullptr, nullptr},
    {"tint_deg", &Params::tint_deg, nullptr, nullptr},
};
// Names the same settings had in files written by earlier versions.
const char* const RENAMED[][2] = {{"tv_white_ire", "map_white_ire"}, {"tv_black_ire", "map_black_ire"}, {"tv_controls", "post"}};
}  // namespace

std::string to_text(const Params& p) {
    std::string out = "# PCE vectorscope parameters\n";
    char line[128];
    for (const Field& f : FIELDS) {
        if (f.d) std::snprintf(line, sizeof line, "%s=%.10g\n", f.key, p.*(f.d));
        else if (f.b) std::snprintf(line, sizeof line, "%s=%d\n", f.key, p.*(f.b) ? 1 : 0);
        else std::snprintf(line, sizeof line, "%s=%d\n", f.key, p.*(f.i));
        out += line;
    }
    return out;
}

bool from_text(const std::string& text, Params& p) {
    Params q = p;
    int found = 0;
    int auto_contrast = -1;  // only in files written before the luma mapping existed
    bool acc = true;         // only in files written while chroma could be taken at its level on the wire
    double burst = BURST_PP_IRE;  // only in files written while the burst level was a setting; matters with acc off
    bool levels = false, controls = false, knobs = false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        const char* value = line.c_str() + eq + 1;
        for (const auto& name : RENAMED)
            if (key == name[0]) key = name[1];
        for (const Field& f : FIELDS) {
            if (key != f.key) continue;
            if (f.d) q.*(f.d) = std::strtod(value, nullptr);
            else if (f.b) q.*(f.b) = std::atoi(value) != 0;
            else q.*(f.i) = std::atoi(value);
            found++;
        }
        if (key == "auto_contrast") {
            auto_contrast = std::atoi(value) != 0 ? 1 : 0;
            found++;
        } else if (key == "acc") {
            acc = std::atoi(value) != 0;
            found++;
        } else if (key == "burst_pp_ire") {
            burst = std::strtod(value, nullptr);
            found++;
        } else if (key == "map_white_ire" || key == "map_black_ire")
            levels = true;
        else if (key == "post")
            controls = true;
        else if (key == "contrast" || key == "brightness" || key == "saturation" || key == "tint_deg")
            knobs = true;
    }
    if (found == 0) return false;
    // An older file: with auto_contrast on, the console's white and black were full scale and contrast was not used;
    // with it off, contrast 1 put 100 IRE at full scale and 0 IRE at black.
    if (auto_contrast >= 0 && !levels) {
        q.map_white_ire = auto_contrast ? q.white_ire : 100.0;
        q.map_black_ire = auto_contrast ? q.black_ire : 0.0;
        if (auto_contrast) q.contrast = 1.0;
    }
    // A file that sets an adjustment without saying whether they are in use: they are, if any is off neutral.
    if (knobs && !controls) q.post = q.contrast != 1.0 || q.brightness != 0.0 || q.saturation != 1.0 || q.tint_deg != 0.0;
    // An older file with acc off: chroma was taken at burst / 40 of what a scope reads. That is a colour setting.
    if (!acc) {
        if (!q.post) q.contrast = 1.0, q.brightness = 0.0, q.saturation = 1.0, q.tint_deg = 0.0;  // not in use
        q.post = true;
        q.saturation *= burst / 40.0;
    }
    // The picture-tube settings of older files (tube, primaries, white_point, crt_gamma) are not read: that stage is gone.
    p = q;
    return true;
}

}  // namespace vce
