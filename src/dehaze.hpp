#ifndef INSPECTION_DEHAZE_HPP
#define INSPECTION_DEHAZE_HPP
#include <hls_stream.h>
#include <ap_int.h>
#include <ap_axi_sdata.h>
#include "inspection_config.hpp"
typedef hls::axis<ap_uint<24>,1,1,1> axis_pixel_t;
void dehaze(hls::stream<axis_pixel_t>& in_stream,
            hls::stream<axis_pixel_t>& out_stream,
            ap_uint<32> ctrl_reg, ap_uint<32> threshold_reg);
#endif
