#include <iostream>
#include <hls_stream.h>
#include <ap_int.h>
#include <ap_axi_sdata.h>

#define WIDTH  1280
#define HEIGHT 720

typedef hls::axis<ap_uint<24>, 1, 1, 1> axis_pixel_t;

// 注意：函数名改成 dehaze 了，必须和 dehaze.cpp 保持一致
void dehaze(hls::stream<axis_pixel_t> &in_stream,
            hls::stream<axis_pixel_t> &out_stream,
            ap_uint<32> ctrl_reg,
            ap_uint<32> threshold_reg);

int main() {
    hls::stream<axis_pixel_t> in_stream;
    hls::stream<axis_pixel_t> out_stream;

    // 生成一张模拟有雾的测试图
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            axis_pixel_t p;

            // 1. 生成基础图形：背景是暗灰色(80)，中间有个亮块(200)
            ap_uint<8> base_val;
            if (x > 400 && x < 800 && y > 200 && y < 500) {
                base_val = 200; // 模拟清晰物体
            } else {
                base_val = 80;  // 模拟暗部背景
            }

            // 2. 叠加雾的效应：整体加一个偏置值（模拟雾天泛白），并限制最大值为255
            ap_uint<16> foggy_tmp = base_val + 60;
            ap_uint<8> foggy_val = (foggy_tmp > 255) ? (ap_uint<8>)255 : (ap_uint<8>)foggy_tmp;

            // 3. 填充 RGB 三通道（去雾内部会转灰度）
            p.data = (foggy_val << 16) | (foggy_val << 8) | foggy_val;

            p.user = (y == 0 && x == 0) ? 1 : 0;
            p.last = (x == WIDTH - 1) ? 1 : 0;
            p.id = 0; p.dest = 0; p.keep = 0xFF; p.strb = 0xFF;
            in_stream.write(p);
        }
    }

    // 调用去雾 IP（使能开启，阈值0）
    dehaze(in_stream, out_stream, 0x00000001, 0x00000000);

    // 统计输出：我们不再数“边缘像素”，而是统计“去雾后变暗的像素数量”
    int dark_pixels = 0;
    int total_pixels = WIDTH * HEIGHT;

    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            axis_pixel_t p = out_stream.read();
            ap_uint<8> out_val = p.data.range(23, 16); // 提取灰度

            // 如果输出像素小于100，说明去雾算法把它变暗了（去除了雾的白偏置）
            if (out_val < 100) {
                dark_pixels++;
            }
        }
    }

    std::cout << "去雾后变暗的像素数：" << dark_pixels << " (预期大于0)" << std::endl;
    std::cout << (dark_pixels > 0 ? ">>> C仿真通过！去雾逻辑生效！<<<" : ">>> C仿真失败：去雾未生效 <<<") << std::endl;
    return 0;
}
