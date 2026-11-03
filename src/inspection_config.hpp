#ifndef INSPECTION_CONFIG_HPP
#define INSPECTION_CONFIG_HPP
#include <cstdint>

// Portable register definitions, shared by HLS, C++ tests and host tools.
// WIDTH/HEIGHT are compile-time constants; changing them requires a new IP.
#ifndef INSPECT_WIDTH
#define INSPECT_WIDTH 1280
#endif
#ifndef INSPECT_HEIGHT
#define INSPECT_HEIGHT 720
#endif
namespace inspection {
const unsigned WIDTH = INSPECT_WIDTH, HEIGHT = INSPECT_HEIGHT;
const unsigned DEHAZE = 1u, SOBEL = 2u, OVERLAY = 4u;
const unsigned PROTECT = 8u, AUTO_A = 16u, RESET_A = 32u, LIMIT_GAIN = 64u;
const unsigned DEFAULT_A = 200, DEFAULT_TMIN = 26, DEFAULT_OMEGA = 230;
const unsigned DEFAULT_EDGE = 80;
// Gain is Q4.4. Zero fields select defaults: 3x / 224 / 24.
inline unsigned gain_q4(unsigned ctrl) {
    unsigned q = (ctrl >> 8) & 255u;
    return q == 0 ? 48u : (q < 16 ? 16u : q);
}
inline unsigned bright_min(unsigned ctrl) {
    unsigned v = (ctrl >> 16) & 255u;
    return v ? v : 224u;
}
inline unsigned chroma_max(unsigned ctrl) {
    unsigned v = ctrl >> 24;
    return v ? v : 24u;
}
inline unsigned byte_default(unsigned word, unsigned shift, unsigned fallback) {
    unsigned v = (word >> shift) & 255u;
    return v ? v : fallback;
}
inline std::uint32_t parameters(unsigned a, unsigned t, unsigned omega, unsigned edge) {
    return ((edge & 255u) << 24) | ((a & 255u) << 16) |
           ((t & 255u) << 8) | (omega & 255u);
}
// Conservative starting presets, not calibrated for a particular camera.
const unsigned GENTLE_CTRL = DEHAZE | PROTECT | LIMIT_GAIN;
const unsigned ADAPTIVE_CTRL = GENTLE_CTRL | AUTO_A;
const unsigned GENTLE_PARAMS = (80u << 24) | (200u << 16) | (64u << 8) | 217u;
}
#endif
