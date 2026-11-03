// ============================================================
// ps_sw_dehaze.cpp — 去雾 + Sobel 的纯 C++ 软件实现（PS 端测速用）
//
// 用途：在 ARM（Cortex-A9 / PS 端）上编译运行，测纯软件实现的耗时，
//       作为与 FPGA 硬件加速（928,290 拍 @ 74.25 MHz = 12.502 ms/帧）对比的基线。
//
// 与 RTL 的关系：定点规则与 dehaze.cpp 逐位一致（同一套倒数表公式、
//       同样的 clamp 与算术右移），只把 hls::stream / ap_uint / axis
//       换成 std::vector 与标量循环。
//
// 编译： g++ -O2 -std=c++11 ps_sw_dehaze.cpp -o ps_sw_dehaze
// 运行： ./ps_sw_dehaze [mode] [frames]
//        mode   0=旁路 1=只去雾 2=只Sobel 3=去雾+Sobel 7=去雾+叠加(默认 3)
//        frames 计时帧数，默认 10
// ============================================================

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

const int kWidth  = 1280;
const int kHeight = 720;
const std::size_t kPixels = static_cast<std::size_t>(kWidth) * kHeight;

// 与寄存器写 0 时的默认值一致
struct Params {
    unsigned a     = 200;
    unsigned t_min = 26;
    unsigned omega = 230;
    unsigned edge  = 80;
};

struct GainPolicy {
    bool     limit_gain = false;
    unsigned limit      = 0;
    bool     protect    = false;
    unsigned bright     = 224;
    unsigned chroma     = 24;
};

inline std::uint8_t channel(std::uint32_t rgb, unsigned shift) {
    return static_cast<std::uint8_t>((rgb >> shift) & 0xFFu);
}

inline std::uint32_t pack_rgb(unsigned r, unsigned g, unsigned b) {
    return ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu);
}

// 与 dehaze.cpp 的 reciprocal_a / reciprocal_t 同式：
//   k_a(i) = round(55552 / i) ≈ 217 * 256 / i
//   k_t(i) = round(65280 / i) ≈ 255 * 256 / i
inline unsigned k_a(unsigned a) { return (55552u + a / 2u) / a; }
inline unsigned k_t(unsigned t) { return (65280u + t / 2u) / t; }

// 与 ap_int 的有符号算术右移 8 位一致
inline long long arithmetic_shift_8(long long v) {
    return v >= 0 ? v / 256 : -((-v + 255) / 256);
}

// ---------------- 测试图（与 dehaze_test.cpp 的 make_test_image 相同） ----------------
std::vector<std::uint32_t> make_test_image() {
    std::vector<std::uint32_t> image(kPixels);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            int base = (x * 255 / (kWidth - 1) + y * 73 / kHeight) & 0xFF;
            if ((((x / 96) + (y / 80)) & 1) != 0) {
                base = std::min(base + 35, 255);
            }

            int r = base;
            int g = (base * 3 + ((x / 8) & 1) * 60) / 4;
            int b = (base * 2 + ((y % 64) < 4 ? 100 : 0)) / 2;

            if (x >= 400 && x < 560 && y >= 200 && y < 360) { r = 40;  g = 45;  b = 38;  }
            if (y >= 560)                                   { r = 230; g = 48;  b = 52;  }
            if (x >= 1100)                                  { r = 250; g = 250; b = 20;  }
            if (x < 3 || y < 3 || x >= kWidth - 3 || y >= kHeight - 3) {
                r = ((x + y) & 1) ? 255 : 0;
                g = r;
                b = r;
            }

            image[static_cast<std::size_t>(y) * kWidth + x] = pack_rgb(r, g, b);
        }
    }
    return image;
}

// ---------------- 去雾 ----------------
// 输入 in、输出 out 由调用方预分配，函数内部不做动态分配，
// 这样计时的就是纯计算耗时。
void dehaze_sw(const std::vector<std::uint32_t> &in,
               std::vector<std::uint32_t>       &out,
               const Params &p, const GainPolicy &policy) {
    const unsigned base_k_a     = k_a(p.a);
    const unsigned selected_k_a = (base_k_a * p.omega + 108u) / 217u;

    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            // ① 3x3 暗通道，边界复制
            unsigned dark = 255;
            for (int dy = -1; dy <= 1; ++dy) {
                const int yy = std::max(0, std::min(kHeight - 1, y + dy));
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = std::max(0, std::min(kWidth - 1, x + dx));
                    const std::uint32_t q =
                        in[static_cast<std::size_t>(yy) * kWidth + xx];
                    const unsigned minimum = std::min(
                        static_cast<unsigned>(channel(q, 16)),
                        std::min(static_cast<unsigned>(channel(q, 8)),
                                 static_cast<unsigned>(channel(q, 0))));
                    dark = std::min(dark, minimum);
                }
            }

            // ②~④ 透射率
            int t = 255 - static_cast<int>((dark * selected_k_a) >> 8);
            t = std::max(static_cast<int>(p.t_min), std::min(255, t));

            // ⑤ 增益
            unsigned kt = k_t(static_cast<unsigned>(t));

            const std::uint32_t source =
                in[static_cast<std::size_t>(y) * kWidth + x];

            // ⑥ 增益限幅
            if (policy.limit_gain && kt > policy.limit) {
                kt = policy.limit;
            }
            // ⑦ 近白保护
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

            // ⑧ 恢复
            unsigned result[3];
            const unsigned shifts[3] = {16, 8, 0};
            for (unsigned c = 0; c < 3; ++c) {
                const long long difference =
                    static_cast<int>(channel(source, shifts[c])) -
                    static_cast<int>(p.a);
                long long value = static_cast<long long>(p.a) +
                                  arithmetic_shift_8(difference * kt);
                value = std::max(0LL, std::min(255LL, value));
                result[c] = static_cast<unsigned>(value);
            }

            out[static_cast<std::size_t>(y) * kWidth + x] =
                pack_rgb(result[0], result[1], result[2]);
        }
    }
}

// ---------------- Sobel ----------------
inline std::uint8_t rgb_to_gray(std::uint32_t rgb) {
    const unsigned r = channel(rgb, 16);
    const unsigned g = channel(rgb, 8);
    const unsigned b = channel(rgb, 0);
    return static_cast<std::uint8_t>((77u * r + 150u * g + 29u * b + 128u) >> 8);
}

void sobel_sw(const std::vector<std::uint32_t> &base,
              std::vector<std::uint32_t>       &out,
              unsigned threshold, bool overlay,
              std::vector<std::uint8_t>        &gray_buf) {
    for (std::size_t i = 0; i < kPixels; ++i) {
        gray_buf[i] = rgb_to_gray(base[i]);
    }

    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            int p[3][3];
            for (int dy = -1; dy <= 1; ++dy) {
                const int yy = std::max(0, std::min(kHeight - 1, y + dy));
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = std::max(0, std::min(kWidth - 1, x + dx));
                    p[dy + 1][dx + 1] =
                        gray_buf[static_cast<std::size_t>(yy) * kWidth + xx];
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
            const bool is_edge = magnitude >= threshold;
            const std::size_t index = static_cast<std::size_t>(y) * kWidth + x;

            if (overlay) {
                out[index] = is_edge ? 0xFFFFFFu : base[index];
            } else {
                const unsigned v = is_edge ? magnitude : 0u;
                out[index] = pack_rgb(v, v, v);
            }
        }
    }
}

// 校验和：防止编译器把整段计算优化掉
std::uint64_t checksum(const std::vector<std::uint32_t> &v) {
    std::uint64_t s = 1469598103934665603ULL;
    for (std::size_t i = 0; i < v.size(); i += 97) {
        s = (s ^ v[i]) * 1099511628211ULL;
    }
    return s;
}

} // namespace

int main(int argc, char **argv) {
    const unsigned mode   = (argc > 1) ? static_cast<unsigned>(std::atoi(argv[1])) : 3u;
    const int      frames = (argc > 2) ? std::atoi(argv[2]) : 10;
    if (frames <= 0) {
        std::fprintf(stderr, "frames must be > 0\n");
        return 1;
    }

    const bool do_dehaze  = (mode & 0x1) != 0;
    const bool do_sobel   = (mode & 0x2) != 0;
    const bool overlay    = (mode & 0x4) != 0;

    const Params     params;
    GainPolicy       policy;
    // 与寄存器 ctrl=73 等价：去雾 + 近白保护 + 增益限幅
    if (do_dehaze) {
        policy.protect    = true;
        policy.limit_gain = true;
        policy.limit      = 768;   // gain_q4(ctrl)<<4，ctrl 的 [15:8]=0 → 48<<4
    }

    const std::vector<std::uint32_t> input = make_test_image();
    std::vector<std::uint32_t> work(kPixels, 0);
    std::vector<std::uint32_t> out(kPixels, 0);
    std::vector<std::uint8_t>  gray_buf(kPixels, 0);

    // 预热一帧，不计时
    {
        const std::vector<std::uint32_t> *src = &input;
        if (do_dehaze) { dehaze_sw(input, work, params, policy); src = &work; }
        if (do_sobel)  { sobel_sw(*src, out, params.edge, overlay, gray_buf); }
        else           { out = *src; }
    }

    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) {
        const std::vector<std::uint32_t> *src = &input;
        if (do_dehaze) { dehaze_sw(input, work, params, policy); src = &work; }
        if (do_sobel)  { sobel_sw(*src, out, params.edge, overlay, gray_buf); }
        else           { out = *src; }
    }
    const auto t1 = std::chrono::steady_clock::now();

    const double total_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double per_frame_ms = total_ms / frames;
    const double fps = (per_frame_ms > 0.0) ? 1000.0 / per_frame_ms : 0.0;

    std::printf("mode            : %u%s%s%s\n", mode,
                do_dehaze ? " [dehaze]" : "",
                do_sobel  ? " [sobel]"  : "",
                (!do_sobel || overlay) ? "" : " [gray]");
    std::printf("frames          : %d\n", frames);
    std::printf("total           : %.3f ms\n", total_ms);
    std::printf("per frame       : %.3f ms  (%.2f fps)\n", per_frame_ms, fps);
    std::printf("checksum        : 0x%016llX\n",
                static_cast<unsigned long long>(checksum(out)));
    std::printf("\nRTL baseline    : 928290 cycles @ 74.25 MHz = 12.502 ms/frame (80.0 fps)\n");
    std::printf("speedup (RTL/SW): %.1fx\n", per_frame_ms / 12.502);
    return 0;
}
