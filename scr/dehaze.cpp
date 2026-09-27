#include <hls_stream.h>
#include <ap_int.h>
#include <ap_axi_sdata.h>

#define WIDTH  1280
#define HEIGHT 720

typedef hls::axis<ap_uint<24>, 1, 1, 1> axis_pixel_t;

void dehaze(hls::stream<axis_pixel_t> &in_stream,
           hls::stream<axis_pixel_t> &out_stream,
           ap_uint<32> ctrl_reg,
           ap_uint<32> threshold_reg)
{
    #pragma HLS INTERFACE axis port=in_stream
    #pragma HLS INTERFACE axis port=out_stream
    #pragma HLS INTERFACE s_axilite port=ctrl_reg     bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=threshold_reg bundle=CTRL
    #pragma HLS INTERFACE s_axilite port=return       bundle=CTRL

    ap_uint<8> line0[WIDTH];
    ap_uint<8> line1[WIDTH];

    // 强制映射为双端口 BRAM，防止 1080p 下资源爆炸
    #pragma HLS BIND_STORAGE variable=line0 type=ram_2p impl=bram
    #pragma HLS BIND_STORAGE variable=line1 type=ram_2p impl=bram

    ap_uint<8> win[3][3];
    #pragma HLS ARRAY_PARTITION variable=win complete dim=0

    ap_uint<1> enable = ctrl_reg[0];
    ap_uint<8> threshold = threshold_reg.range(7, 0);

    for (int row = 0; row < HEIGHT; row++) {
        if (row % 2 == 0) {
            #pragma HLS PIPELINE II=1
            for (int col = 0; col < WIDTH; col++) {
                //每行开始时清空窗口，防止跨行残留
                if (col == 0) {
                    for (int i=0; i<3; i++) {
                        win[0][i] = 0; win[1][i] = 0; win[2][i] = 0;
                    }
                }

                axis_pixel_t pixel_in = in_stream.read();
                ap_uint<24> rgb = pixel_in.data;
                ap_uint<8> r = rgb.range(23, 16);
                ap_uint<8> g = rgb.range(15, 8);
                ap_uint<8> b = rgb.range(7, 0);

                // 显式使用16位中间变量 + 移位加法代替乘法
                ap_uint<16> gray_tmp = ((r<<6) + (r<<3) + (r<<2) + r) +
                                       ((g<<7) + (g<<4) + (g<<2) + (g<<1)) +
                                       ((b<<4) + (b<<3) + (b<<2) + b);
                ap_uint<8> gray = gray_tmp.range(15, 8);

                ap_uint<8> pix_row0 = line0[col];
                ap_uint<8> pix_row1 = line1[col];
                ap_uint<8> pix_row2 = gray;

                win[0][0]=win[0][1]; win[0][1]=win[0][2]; win[0][2]=pix_row0;
                win[1][0]=win[1][1]; win[1][1]=win[1][2]; win[1][2]=pix_row1;
                win[2][0]=win[2][1]; win[2][1]=win[2][2]; win[2][2]=pix_row2;

                line0[col] = gray;

                // 1. 求3x3窗口的暗通道（最小值）
                ap_uint<8> dark = win[0][0];
                if (win[0][1] < dark) dark = win[0][1];
                if (win[0][2] < dark) dark = win[0][2];
                if (win[1][0] < dark) dark = win[1][0];
                if (win[1][1] < dark) dark = win[1][1];
                if (win[1][2] < dark) dark = win[1][2];
                if (win[2][0] < dark) dark = win[2][0];
                if (win[2][1] < dark) dark = win[2][1];
                if (win[2][2] < dark) dark = win[2][2];

                // 2. 大气光 A（先固定写死 200，后续让队友C通过 AXI-Lite 传参）
                ap_uint<8> A = 200;
                // 防止 dark 超过 A，导致负数
                ap_uint<8> dark_clamped = (dark > A) ? A : dark;

                // 3. 计算透射率 t（硬件友好版：t = 255 - 218 * dark / 255）
                // 218 是 0.85 * 255 的整数近似值
                ap_uint<16> t_tmp = 255 - (dark_clamped * 218) / 255;
                ap_uint<8> t = (t_tmp < 102) ? (ap_uint<8>)102 : (ap_uint<8>)t_tmp; // 限制 t 不小于 0.4（102/255）

                // 4. 恢复图像（J = (I - A) / t + A）
                ap_uint<8> center_pixel = win[1][1]; // 当前中心像素
                ap_int<12> diff = (ap_int<12>)center_pixel - (ap_int<12>)A;
                ap_int<12> J_tmp = (diff * 255) / t; // 硬件里可以用查找表代替除法，先用 / 跑通逻辑
                ap_int<12> J = J_tmp + A;
                // 饱和截位
                ap_uint<8> out_gray;
                if (J < 0) out_gray = 0;
                else if (J > 255) out_gray = 255;
                else out_gray = (ap_uint<8>)J;

                // 边界保护
                bool is_boundary = (row < 2) || (col < 2) || (row >= HEIGHT - 2) || (col >= WIDTH - 2);
                if (is_boundary || (enable == 0)) out_gray = 0;

                axis_pixel_t pixel_out;
                pixel_out.data = (out_gray << 16) | (out_gray << 8) | out_gray;
                pixel_out.user = (row == 0 && col == 0) ? 1 : 0;
                pixel_out.last = (col == WIDTH - 1) ? 1 : 0;
                pixel_out.id=0; pixel_out.dest=0; pixel_out.keep=0xFF; pixel_out.strb=0xFF;
                out_stream.write(pixel_out);
            }
        } else {
            #pragma HLS PIPELINE II=1
            for (int col = 0; col < WIDTH; col++) {
                if (col == 0) {
                    for (int i=0; i<3; i++) {
                        win[0][i] = 0; win[1][i] = 0; win[2][i] = 0;
                    }
                }

                axis_pixel_t pixel_in = in_stream.read();
                ap_uint<24> rgb = pixel_in.data;
                ap_uint<8> r = rgb.range(23, 16);
                ap_uint<8> g = rgb.range(15, 8);
                ap_uint<8> b = rgb.range(7, 0);

                // 移位加法代替乘法
                ap_uint<16> gray_tmp = ((r<<6) + (r<<3) + (r<<2) + r) +
                                       ((g<<7) + (g<<4) + (g<<2) + (g<<1)) +
                                       ((b<<4) + (b<<3) + (b<<2) + b);
                ap_uint<8> gray = gray_tmp.range(15, 8);

                ap_uint<8> pix_row0 = line1[col];
                ap_uint<8> pix_row1 = line0[col];
                ap_uint<8> pix_row2 = gray;

                win[0][0]=win[0][1]; win[0][1]=win[0][2]; win[0][2]=pix_row0;
                win[1][0]=win[1][1]; win[1][1]=win[1][2]; win[1][2]=pix_row1;
                win[2][0]=win[2][1]; win[2][1]=win[2][2]; win[2][2]=pix_row2;

                line1[col] = gray;

                // ================= 去雾逻辑（奇数行，与偶数行完全一致） =================
                ap_uint<8> dark = win[0][0];
                if (win[0][1] < dark) dark = win[0][1];
                if (win[0][2] < dark) dark = win[0][2];
                if (win[1][0] < dark) dark = win[1][0];
                if (win[1][1] < dark) dark = win[1][1];
                if (win[1][2] < dark) dark = win[1][2];
                if (win[2][0] < dark) dark = win[2][0];
                if (win[2][1] < dark) dark = win[2][1];
                if (win[2][2] < dark) dark = win[2][2];

                ap_uint<8> A = 200;
                ap_uint<8> dark_clamped = (dark > A) ? A : dark;

                ap_uint<16> t_tmp = 255 - (dark_clamped * 218) / 255;
                ap_uint<8> t = (t_tmp < 102) ? (ap_uint<8>)102 : (ap_uint<8>)t_tmp;

                ap_uint<8> center_pixel = win[1][1];
                ap_int<12> diff = (ap_int<12>)center_pixel - (ap_int<12>)A;
                ap_int<12> J_tmp = (diff * 255) / t;
                ap_int<12> J = J_tmp + A;

                ap_uint<8> out_gray;
                if (J < 0) out_gray = 0;
                else if (J > 255) out_gray = 255;
                else out_gray = (ap_uint<8>)J;

                bool is_boundary = (row < 2) || (col < 2) || (row >= HEIGHT - 2) || (col >= WIDTH - 2);
                if (is_boundary || (enable == 0)) out_gray = 0;

                axis_pixel_t pixel_out;
                pixel_out.data = (out_gray << 16) | (out_gray << 8) | out_gray;
                pixel_out.user = (row == 0 && col == 0) ? 1 : 0;
                pixel_out.last = (col == WIDTH - 1) ? 1 : 0;
                pixel_out.id=0; pixel_out.dest=0; pixel_out.keep=0xFF; pixel_out.strb=0xFF;
                out_stream.write(pixel_out);
            }
        }
    }
}
