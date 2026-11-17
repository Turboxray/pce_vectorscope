// HuC6260 (VCE) composite colour model: matrix ROM -> signal levels -> NTSC decode -> 8-bit RGB.
#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace vce {

// Burst on the wire, peak to peak, with sync at 40 IRE: measured 37 to 40. Not a setting. The chroma steps are stated
// against burst, so the palette and the vectorscope do not depend on it; the waveform view draws burst and the chroma
// envelopes at this level.
constexpr double BURST_PP_IRE = 37.0;

struct Params {
    // --- Console: what the chip and its mixing resistors put on the composite pin.
    double white_ire = 89.0;        // luma code 31, with sync at 40 IRE
    double black_ire = 0.0;         // luma code 0
    double chroma_ire = 2.14;       // one B-Y count: fundamental amplitude, with burst called 40 IRE p-p
    double ry_gain = 8.2 / 6.2;     // R-Y is mixed in this much stronger than B-Y (8.2k / 6.2k)
    double rotation_deg = -8.5;     // whole chroma pattern against burst
    double quadrature_deg = 0.0;    // R-Y axis error from 90 degrees
    // --- Luma mapping: the signal levels that come out as full white and as black.
    double map_white_ire = 89.0;    // max white; the default is the console's own white, so code 31 is full scale
    double map_black_ire = 0.0;
    // --- Post adjustment: changes to the decoded picture. Not part of the console's signal.
    bool post = false;             // the adjustments below are in use; off, they all count as neutral
    double contrast = 1.0;          // picture gain, luma and chroma: 1.0 leaves max white at full scale
    double brightness = 0.0;        // added after contrast, as a fraction of full scale
    double saturation = 1.0;
    double tint_deg = 0.0;

    bool operator==(const Params&) const = default;
};

struct Entry {
    int r, g, b;                    // 0..7
    int y_code, by, ry;             // matrix ROM: luma 0..31, signed chroma counts
    double y_ire;                   // luma level
    double u_ire, v_ire;            // chroma vector on the scope's B-Y and R-Y axes, burst = 40 IRE p-p
    double decoded[3];              // R'G'B' as decoded, before any clipping (0..1 nominal)
    uint8_t rgb[3];                 // final 8-bit colour
    bool clipped;
};

struct Luma {                       // one luma code with no chroma
    double y_ire;
    uint8_t rgb[3];                 // final 8-bit colour
    bool clipped;
};

struct Palette {
    std::array<Entry, 512> e;       // index = G<<6 | R<<3 | B
    int clipped = 0;                // how many of the 512 clip
    std::array<Luma, 32> luma;      // every luma code, 0..31; only eight of them are greys the console can make
};

enum class Carrier { Sine, ThreeLevel, Square };

void rom(int r, int g, int b, int& y_code, int& by, int& ry);
Palette build(const Params& p);

// Picture gain of the decode: decoded value per IRE.
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
// Set the console chroma numbers (and the luma mapping, if needed) so the model reproduces a 512-colour palette.
// The post adjustments are switched off.
FitResult fit_to_palette(const uint8_t ref[512][3], Params& p);

std::string to_text(const Params& p);
bool from_text(const std::string& text, Params& p);

}  // namespace vce
