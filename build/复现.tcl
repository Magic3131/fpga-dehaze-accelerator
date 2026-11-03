# ============================================================
# dehaze v2.0 复现脚本（Vitis HLS 2022.2）
# 用法：
#   1. 把 SRC 改成你机器上 01_源码 的绝对路径（用正斜杠）
#   2. 在 Vitis HLS 2022.2 命令行执行：
#        vivado_hls -f 复现.tcl
#   全程约 26 分钟：csim ~4 分钟 / csynth ~1 分钟 / cosim ~20 分钟（16 个用例，事务数 16）
#
# 预期结果：
#   csim   16 个用例全 PASS，末行 "CSim done with 0 errors."
#   csynth 10.00 ns 约束，估算 7.300 ns；13 BRAM / 10 DSP / 5109 FF / 7388 LUT
#   cosim  Verilog Pass，Latency min/avg/max = 921644 / 928494 / 933587，16 个事务，总 14855909 拍
#   export 生成 xilinx_com_hls_dehaze_1_0.zip
# ============================================================

set SRC "C:/Users/32793/Desktop/交付/01_源码"

open_project -reset dehaze_repro
set_top dehaze
add_files "${SRC}/inspection_config.hpp"
add_files "${SRC}/dehaze.hpp"
add_files "${SRC}/dehaze.cpp"
add_files -tb "${SRC}/dehaze_test.cpp" -cflags "-Wno-unknown-pragmas" -csimflags "-Wno-unknown-pragmas"

open_solution "solution1" -flow_target vivado
set_part {xc7z020clg400-1}
create_clock -period 10 -name default
config_export -format ip_catalog -rtl verilog

csim_design
csynth_design
cosim_design
export_design -rtl verilog -format ip_catalog

exit
