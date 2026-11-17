// HuC6260 (VCE) composite colour model: matrix ROM -> signal levels -> TV decode -> 8-bit RGB.
#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace vce {

struct Params {
    // --- Console: what the chip and its mixing resistors put on the composite pin.
    double white_ire = 89.0;        // luma code 31, with sync at 40 IRE
    double black_ire = 0.0;         // luma code 0
    double chroma_ire = 2.14;       // one B-Y count: fundamental amplitude, with burst called 40 IRE p-p
    double ry_gain = 8.2 / 6.2;     // R-Y is mixed in this much stronger than B-Y (8.2k / 6.2k)
    double rotation_deg = -8.5;     // whole chroma pattern against burst
    double quadrature_deg = 0.0;    // R-Y axis error from 90 degrees
    double burst_pp_ire = 37.0;     // burst as it really is, with sync at 40 IRE
    // --- Television.
    bool acc = true;                // chroma gain follows burst
    bool auto_contrast = true;      // contrast and brightness set so code 0 = black, code 31 = full white
    double contrast = 1.0;          // used when auto_contrast is off: 1.0 puts 100 IRE at full white
    double brightness = 0.0;
    double saturation = 1.0;
    double tint_deg = 0.0;
    // --- Picture tube.
    int tube = 0;                   // 0 = decoded values go straight out as sRGB, 1 = phosphors and gamma
    int primaries = 0;              // 0 SMPTE C, 1 NTSC 1953, 2 Rec.709
    int white_point = 0;            // 0 D65, 1 Illuminant C, 2 9300 K
    double crt_gamma = 2.4;

    bool operator==(const Params&) const = default;
};

struct Entry {
    int r, g, b;                    // 0..7
    int y_code, by, ry;             // matrix ROM: luma 0..31, signed chroma counts
    double y_ire;                   // luma level
    double u_ire, v_ire;            // chroma vector on the scope's B-Y and R-Y axes, burst = 40 IRE p-p
    double decoded[3];              // R'G'B' as the TV decodes it, before any clipping (0..1 nominal)
    uint8_t rgb[3];                 // final 8-bit colour
    bool clipped;
};

struct Palette {
    std::array<Entry, 512> e;       // index = G<<6 | R<<3 | B
    int clipped = 0;
};

enum class Carrier { Sine, ThreeLevel, Square };

void rom(int r, int g, int b, int& y_code, int& by, int& ry);
Palette build(const Params& p);

// Picture gain of the TV model: decoded value per IRE.
double picture_gain(const Params& p);

// Peak of the modulated chroma for a given carrier shape, in the same units as chroma_ire.
double envelope_peak(const Params& p, int by, int ry, Carrier shape);

// Y, and chroma scaled the NTSC way (U = 0.492 (B-Y), V = 0.877 (R-Y)), from 8-bit RGB. All 0..1 based.
void rgb_to_yuv(const uint8_t rgb[3], double& y, double& u, double& v);

struct FitResult {
    bool ok = false;
    int used = 0;                   // entries that were not clipped in the reference
    double rms_ire = 0;             // chroma error left after the fit
};
// Set the console chroma numbers (and contrast, if needed) so the model reproduces a 512-colour palette.
FitResult fit_to_palette(const uint8_t ref[512][3], Params& p);

std::string to_text(const Params& p);
bool from_text(const std::string& text, Params& p);

}  // namespace vce
