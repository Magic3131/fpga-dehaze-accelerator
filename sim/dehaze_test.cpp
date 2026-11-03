// C++ testbench for the HLS dehaze + Sobel top.
// Run this file through Vitis HLS C simulation with dehaze.cpp.

#include <ap_axi_sdata.h>
#include <ap_int.h>
#include <hls_stream.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define WIDTH 1280
#define HEIGHT 720
#define PIXEL_COUNT (WIDTH * HEIGHT)

typedef hls::axis<ap_uint<24>, 1, 1, 1> axis_pixel_t;

void dehaze(hls::stream<axis_pixel_t> &in_stream,
            hls::stream<axis_pixel_t> &out_stream,
            ap_uint<32> ctrl_reg, ap_uint<32> threshold_reg);

namespace {

constexpr unsigned kDefaultA = 200;
constexpr unsigned kDefaultTMin = 26;
constexpr unsigned kDefaultOmega = 230;
constexpr unsigned kDefaultEdgeThreshold = 80;

struct DehazeParams {
    unsigned a;
    unsigned t_min;
    unsigned omega;
};

hls::stream<axis_pixel_t> input_stream("test_input");
hls::stream<axis_pixel_t> output_stream("test_output");

std::uint8_t channel(std::uint32_t rgb, unsigned shift) {
    return static_cast<std::uint8_t>((rgb >> shift) & 0xFFu);
}

std::uint32_t pack_rgb(unsigned r, unsigned g, unsigned b) {
    return ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu);
}

std::uint32_t pack_parameters(unsigned edge_threshold, unsigned a,
                              unsigned t_min, unsigned omega) {
    return (edge_threshold << 24) | (a << 16) | (t_min << 8) | omega;
}

std::vector<std::uint32_t> make_test_image() {
    std::vector<std::uint32_t> image(PIXEL_COUNT);
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            int base = (x * 255 / (WIDTH - 1) + y * 73 / HEIGHT) & 0xFF;
            if ((((x / 96) + (y / 80)) & 1) != 0) {
                base = std::min(base + 35, 255);
            }

            int r = base;
            int g = (base * 3 + ((x / 8) & 1) * 60) / 4;
            int b = (base * 2 + ((y % 64) < 4 ? 100 : 0)) / 2;

            // Add sharp colored shapes and edges, including near frame borders.
            if (x >= 400 && x < 560 && y >= 200 && y < 360) {
                r = 40; g = 45; b = 38;
            }
            if (y >= 560) {
                r = 230; g = 48; b = 52;
            }
            if (x >= 1100) {
                r = 250; g = 250; b = 20;
            }
            if (x < 3 || y < 3 || x >= WIDTH - 3 || y >= HEIGHT - 3) {
                r = (x + y) & 1 ? 255 : 0;
                g = r;
                b = r;
            }

            const std::size_t index = static_cast<std::size_t>(y) * WIDTH + x;
            image[index] = pack_rgb(r, g, b);
        }
    }
    return image;
}

unsigned k_a(unsigned a) {
    return (55552u + a / 2u) / a;
}

unsigned k_t(unsigned t) {
    return (65280u + t / 2u) / t;
}

// Match the arithmetic right shift used by ap_int for signed negative values.
long long arithmetic_shift_8(long long value) {
    if (value >= 0) {
        return value / 256;
    }
    return -((-value + 255) / 256);
}

struct GainPolicy {
    bool limit_gain = false;
    unsigned limit = 0;
    bool protect = false;
    unsigned bright = 224;
    unsigned chroma = 24;
};

std::vector<std::uint32_t> reference_dehaze(
    const std::vector<std::uint32_t> &input, DehazeParams params,
    GainPolicy policy = GainPolicy()) {
    const unsigned base_k_a = k_a(params.a);
    const unsigned selected_k_a =
        (base_k_a * params.omega + 108u) / 217u;
    std::vector<std::uint32_t> output(PIXEL_COUNT);

    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            unsigned dark = 255;
            for (int dy = -1; dy <= 1; ++dy) {
                const int yy = std::max(0, std::min(HEIGHT - 1, y + dy));
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = std::max(0, std::min(WIDTH - 1, x + dx));
                    const std::uint32_t p =
                        input[static_cast<std::size_t>(yy) * WIDTH + xx];
                    const unsigned minimum = std::min(
                        static_cast<unsigned>(channel(p, 16)),
                        std::min(static_cast<unsigned>(channel(p, 8)),
                                 static_cast<unsigned>(channel(p, 0))));
                    dark = std::min(dark, minimum);
                }
            }

            int t = 255 - static_cast<int>((dark * selected_k_a) >> 8);
            t = std::max(static_cast<int>(params.t_min), std::min(255, t));
            unsigned kt = k_t(static_cast<unsigned>(t));
            const std::uint32_t source =
                input[static_cast<std::size_t>(y) * WIDTH + x];
            if (policy.limit_gain && kt > policy.limit) {
                kt = policy.limit;
            }
            if (policy.protect) {
                const unsigned c0 = channel(source, 16);
                const unsigned c1 = channel(source, 8);
                const unsigned c2 = channel(source, 0);
                const unsigned lo = std::min(c0, std::min(c1, c2));
                const unsigned hi = std::max(c0, std::max(c1, c2));
                if ((lo >= policy.bright && hi - lo <= policy.chroma) ||
                    hi >= 250) {
                    kt = 256;
                }
            }

            unsigned result[3];
            const unsigned shifts[3] = {16, 8, 0};
            for (unsigned c = 0; c < 3; ++c) {
                const long long difference =
                    static_cast<int>(channel(source, shifts[c])) -
                    static_cast<int>(params.a);
                long long value = static_cast<long long>(params.a) +
                    arithmetic_shift_8(difference * kt);
                value = std::max(0LL, std::min(255LL, value));
                result[c] = static_cast<unsigned>(value);
            }
            output[static_cast<std::size_t>(y) * WIDTH + x] =
                pack_rgb(result[0], result[1], result[2]);
        }
    }
    return output;
}

std::uint8_t rgb_to_gray(std::uint32_t rgb) {
    const unsigned r = channel(rgb, 16);
    const unsigned g = channel(rgb, 8);
    const unsigned b = channel(rgb, 0);
    return static_cast<std::uint8_t>((77u * r + 150u * g + 29u * b + 128u) >> 8);
}

std::vector<std::uint32_t> reference_sobel(
    const std::vector<std::uint32_t> &base, unsigned edge_threshold,
    bool overlay) {
    std::vector<std::uint8_t> gray(PIXEL_COUNT);
    for (std::size_t i = 0; i < PIXEL_COUNT; ++i) {
        gray[i] = rgb_to_gray(base[i]);
    }

    std::vector<std::uint32_t> output(PIXEL_COUNT);
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            int p[3][3];
            for (int dy = -1; dy <= 1; ++dy) {
                const int yy = std::max(0, std::min(HEIGHT - 1, y + dy));
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = std::max(0, std::min(WIDTH - 1, x + dx));
                    p[dy + 1][dx + 1] =
                        gray[static_cast<std::size_t>(yy) * WIDTH + xx];
                }
            }

            int gx = p[0][2] + 2 * p[1][2] + p[2][2] -
                     p[0][0] - 2 * p[1][0] - p[2][0];
            int gy = p[2][0] + 2 * p[2][1] + p[2][2] -
                     p[0][0] - 2 * p[0][1] - p[0][2];
            gx = std::abs(gx);
            gy = std::abs(gy);
            const unsigned magnitude =
                static_cast<unsigned>(std::min(255, gx + gy));
            const bool is_edge = magnitude >= edge_threshold;
            const std::size_t index = static_cast<std::size_t>(y) * WIDTH + x;

            if (overlay) {
                output[index] = is_edge ? 0xFFFFFFu : base[index];
            } else {
                const unsigned value = is_edge ? magnitude : 0u;
                output[index] = pack_rgb(value, value, value);
            }
        }
    }
    return output;
}

bool run_case(const char *name, const std::vector<std::uint32_t> &input,
              const std::vector<std::uint32_t> &expected,
              std::uint32_t mode, std::uint32_t parameters) {
    if (!input_stream.empty() || !output_stream.empty()) {
        std::printf("[%s] stream was not empty before the case\n", name);
        return false;
    }

    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * WIDTH + x;
            axis_pixel_t pixel;
            pixel.data = input[index];
            pixel.user = (y == 0 && x == 0) ? 1 : 0;
            pixel.last = (x == WIDTH - 1) ? 1 : 0;
            pixel.id = 0;
            pixel.dest = 0;
            pixel.keep = 7;
            pixel.strb = 7;
            input_stream.write(pixel);
        }
    }

    dehaze(input_stream, output_stream,
           static_cast<ap_uint<32> >(mode),
           static_cast<ap_uint<32> >(parameters));

    unsigned bad_pixels = 0;
    unsigned bad_sof = 0;
    unsigned bad_eol = 0;
    int first_bad_x = -1;
    int first_bad_y = -1;
    std::uint32_t first_actual = 0;
    std::uint32_t first_expected = 0;

    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * WIDTH + x;
            if (output_stream.empty()) {
                std::printf("[%s] missing output at (%d,%d)\n", name, x, y);
                return false;
            }
            const axis_pixel_t pixel = output_stream.read();
            const std::uint32_t actual = static_cast<std::uint32_t>(pixel.data);
            if (actual != expected[index]) {
                if (bad_pixels == 0) {
                    first_bad_x = x;
                    first_bad_y = y;
                    first_actual = actual;
                    first_expected = expected[index];
                }
                ++bad_pixels;
            }
            if (pixel.user != ((y == 0 && x == 0) ? 1 : 0)) {
                ++bad_sof;
            }
            if (pixel.last != ((x == WIDTH - 1) ? 1 : 0)) {
                ++bad_eol;
            }
        }
    }

    const bool extra_output = !output_stream.empty();
    const bool no_input_left = input_stream.empty();
    const bool passed = bad_pixels == 0 && bad_sof == 0 && bad_eol == 0 &&
                        !extra_output && no_input_left;

    if (passed) {
        std::printf("[PASS] %-28s pixels=%d SOF/EOL OK\n", name, PIXEL_COUNT);
    } else {
        std::printf("[FAIL] %s: pixel mismatches=%u, SOF errors=%u, "
                    "EOL errors=%u, extra output=%d, unread input=%d\n",
                    name, bad_pixels, bad_sof, bad_eol,
                    extra_output ? 1 : 0, no_input_left ? 0 : 1);
        if (bad_pixels != 0) {
            std::printf("       first pixel mismatch (%d,%d): got 0x%06X, "
                        "expected 0x%06X\n",
                        first_bad_x, first_bad_y, first_actual, first_expected);
        }
    }
    return passed;
}

} // namespace

int main() {
    const std::vector<std::uint32_t> input = make_test_image();

    const DehazeParams defaults = {
        kDefaultA, kDefaultTMin, kDefaultOmega
    };
    const DehazeParams custom = {180, 32, 210};

    const std::vector<std::uint32_t> dehazed_default =
        reference_dehaze(input, defaults);
    const std::vector<std::uint32_t> edges_original_default =
        reference_sobel(input, kDefaultEdgeThreshold, false);
    const std::vector<std::uint32_t> edges_dehazed_default =
        reference_sobel(dehazed_default, kDefaultEdgeThreshold, false);
    const std::vector<std::uint32_t> overlay_dehazed_default =
        reference_sobel(dehazed_default, kDefaultEdgeThreshold, true);

    const std::vector<std::uint32_t> dehazed_custom =
        reference_dehaze(input, custom);
    const unsigned custom_threshold = 96;
    const std::uint32_t custom_registers =
        pack_parameters(custom_threshold, custom.a, custom.t_min, custom.omega);
    const std::vector<std::uint32_t> edges_original_custom =
        reference_sobel(input, custom_threshold, false);
    const std::vector<std::uint32_t> edges_dehazed_custom =
        reference_sobel(dehazed_custom, custom_threshold, false);
    const std::vector<std::uint32_t> overlay_original_custom =
        reference_sobel(input, custom_threshold, true);
    const std::vector<std::uint32_t> overlay_dehazed_custom =
        reference_sobel(dehazed_custom, custom_threshold, true);

    bool passed = true;
    passed &= run_case("raw_bypass", input, input, 0, 0);
    passed &= run_case("dehaze_default_parameters", input,
                       dehazed_default, 1, 0);
    passed &= run_case("sobel_only_default_threshold", input,
                       edges_original_default, 2, 0);
    passed &= run_case("dehaze_then_sobel_default", input,
                       edges_dehazed_default, 3, 0);
    passed &= run_case("dehaze_then_overlay_default", input,
                       overlay_dehazed_default, 7, 0);
    passed &= run_case("dehaze_custom_parameters", input,
                       dehazed_custom, 1, custom_registers);
    passed &= run_case("sobel_custom_threshold", input,
                       edges_original_custom, 2, custom_registers);
    passed &= run_case("dehaze_sobel_custom", input,
                       edges_dehazed_custom, 3, custom_registers);
    passed &= run_case("sobel_overlay_original", input,
                       overlay_original_custom, 6, custom_registers);
    passed &= run_case("dehaze_overlay_custom", input,
                       overlay_dehazed_custom, 7, custom_registers);
    passed &= run_case("repeat_frame_state", input,
                       edges_dehazed_custom, 3, custom_registers);

    // 65 = DEHAZE(1) | LIMIT_GAIN(64)；gain_q4 取 [15:8]=0 → 48，limit=48<<4=768
    GainPolicy limit_policy;
    limit_policy.limit_gain = true;
    limit_policy.limit = 768;
    passed &= run_case("dehaze_limit_gain", input,
                       reference_dehaze(input, defaults, limit_policy), 65, 0);

    // 9 = DEHAZE(1) | PROTECT(8)；ctrl 高字节为 0 → bright=224 / chroma=24
    GainPolicy protect_policy;
    protect_policy.protect = true;
    passed &= run_case("dehaze_protect", input,
                       reference_dehaze(input, defaults, protect_policy), 9, 0);

    // 17 = DEHAZE(1) | AUTO_A(16)
    // 第 1 帧 valid_a=false → A=seed=200，输出与默认参数完全一致
    passed &= run_case("auto_a_frame1_seed", input, dehazed_default, 17, 0);
    // 帧末 A 演化到 196，第 2 帧用它算期望值
    const DehazeParams a196 = {196, kDefaultTMin, kDefaultOmega};
    passed &= run_case("auto_a_frame2_evolved", input,
                       reference_dehaze(input, a196), 17, 0);
    // 49 = DEHAZE(1) | AUTO_A(16) | RESET_A(32)：A 拉回 seed，输出回到第 1 帧
    passed &= run_case("auto_a_reset", input, dehazed_default, 49, 0);


    if (!passed) {
        std::printf("TESTBENCH FAILED\n");
        return 1;
    }
    std::printf("ALL TESTS PASSED\n");
    return 0;
}
