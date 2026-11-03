# ============================================================
# PYNQ-Sobel-Accelerator - 顶层引脚约束 (build_stage5.xdc)
# 针对 wrapper 端口名：TMDS_Clk_p_0 / TMDS_Clk_n_0 (HDMI IN)
#                      TMDS_Data_p_0[2:0] / TMDS_Data_n_0[2:0] (HDMI IN)
#                      TMDS_Clk_p_1 / TMDS_Clk_n_1 (HDMI OUT)
#                      TMDS_Data_p_1[2:0] / TMDS_Data_n_1[2:0] (HDMI OUT)
# 依据：PYNQ-Z2 Reference Manual v1.0 Table 7
#       + Digilent PYNQ-Z2 HDMI 参考设计
# ============================================================

# ============================================================
# 一、HDMI 输入（AD7611 / dvi2rgb）—— TMDS_33 差分输入
# ============================================================

# TMDS 输入时钟差分对
set_property -dict { PACKAGE_PIN N18  IOSTANDARD TMDS_33 } [get_ports TMDS_Clk_p_0]
set_property -dict { PACKAGE_PIN P19  IOSTANDARD TMDS_33 } [get_ports TMDS_Clk_n_0]

# TMDS 输入数据通道 0
set_property -dict { PACKAGE_PIN V20  IOSTANDARD TMDS_33 } [get_ports {TMDS_Data_p_0[0]}]
set_property -dict { PACKAGE_PIN W20  IOSTANDARD TMDS_33 } [get_ports {TMDS_Data_n_0[0]}]

# TMDS 输入数据通道 1
set_property -dict { PACKAGE_PIN T20  IOSTANDARD TMDS_33 } [get_ports {TMDS_Data_p_0[1]}]
set_property -dict { PACKAGE_PIN U20  IOSTANDARD TMDS_33 } [get_ports {TMDS_Data_n_0[1]}]

# TMDS 输入数据通道 2
set_property -dict { PACKAGE_PIN N20  IOSTANDARD TMDS_33 } [get_ports {TMDS_Data_p_0[2]}]
set_property -dict { PACKAGE_PIN P20  IOSTANDARD TMDS_33 } [get_ports {TMDS_Data_n_0[2]}]

# ============================================================
# 二、HDMI 输出（ADV7511 / rgb2dvi）—— TMDS_33 差分输出
# ============================================================

# TMDS 输出时钟差分对
set_property -dict { PACKAGE_PIN L16  IOSTANDARD TMDS_33 } [get_ports TMDS_Clk_p_1]
set_property -dict { PACKAGE_PIN L17  IOSTANDARD TMDS_33 } [get_ports TMDS_Clk_n_1]

# TMDS 输出数据通道 0
set_property -dict { PACKAGE_PIN K17  IOSTANDARD TMDS_33 } [get_ports {TMDS_Data_p_1[0]}]
set_property -dict { PACKAGE_PIN K18  IOSTANDARD TMDS_33 } [get_ports {TMDS_Data_n_1[0]}]

# TMDS 输出数据通道 1
set_property -dict { PACKAGE_PIN K19  IOSTANDARD TMDS_33 } [get_ports {TMDS_Data_p_1[1]}]
set_property -dict { PACKAGE_PIN J19  IOSTANDARD TMDS_33 } [get_ports {TMDS_Data_n_1[1]}]

# TMDS 输出数据通道 2
set_property -dict { PACKAGE_PIN J18  IOSTANDARD TMDS_33 } [get_ports {TMDS_Data_p_1[2]}]
set_property -dict { PACKAGE_PIN H18  IOSTANDARD TMDS_33 } [get_ports {TMDS_Data_n_1[2]}]

# ============================================================
# 三、DDC（HDMI 输入读 EDID 的 I2C）—— LVCMOS33
#     已核对 wrapper：DDC 顶层端口是【单根 inout】：
#       audio_i2c_0_scl_io  -> U14（SCL）
#       audio_i2c_0_sda_io  -> U15（SDA）
#     Vivado 已自动加 IOBUF，约束直接针对这两个 inout 端口即可。
#     若第一阶段不读 EDID（用 dvi2rgb 内部 DDC ROM 预设 1080p），
#     这一段可以整个注释掉，DDC 引脚悬空。
# ============================================================
set_property -dict { PACKAGE_PIN U14  IOSTANDARD LVCMOS33 } [get_ports audio_i2c_0_scl_io]
set_property -dict { PACKAGE_PIN U15  IOSTANDARD LVCMOS33 } [get_ports audio_i2c_0_sda_io]

# ============================================================
# 四、HPD 热插拔检测 —— LVCMOS33
#     重要：dvi2rgb 和 rgb2dvi 两个 IP 都【没有】HPD 引脚（已查证源码）。
#     如需 HPD，需额外加 AXI GPIO 引出，这是第二阶段优化项。
#     第一阶段（直通跑通）先不接 HPD。
#     HDMI 输入 HPD（输出）= T19；HDMI 输出 HPD（输入）= R19
# ============================================================
# （第一阶段不接 HPD，以下约束留作第二阶段参考）
# set_property -dict { PACKAGE_PIN T19  IOSTANDARD LVCMOS33 } [get_ports hdmi_rx_hpd]
# set_property -dict { PACKAGE_PIN R19  IOSTANDARD LVCMOS33 } [get_ports hdmi_tx_hpd]

# ============================================================
# 五、axi_iic —— 已删除（build_stage7.tcl）
#     背景：PYNQ-Z2 的 HDMI 输出芯片 ADV7511 走 PS I2C0（MIO50/51），
#     不需要 PL axi_iic。之前照搬 Zybo 方案加的 axi_iic 导致
#     6 个无用端口无法放置、实现失败，已在 stage7 彻底删除。
#     因此本文件不再需要 axi_iic 的任何引脚约束。
# ============================================================

# ============================================================
# 六、时钟约束
#     TMDS 输入时钟（来自 HDMI 源）
#     1080p@60Hz 像素时钟 = 148.5MHz -> 周期 6.734ns
#     720p@60Hz  像素时钟 = 74.25MHz -> 周期 13.468ns
# ============================================================
# 720p 模式（默认，稳定）
# create_clock -period 6.734 -waveform {0.000 3.367} [get_ports TMDS_Clk_p_0]
create_clock -period 13.468 -waveform {0.000 6.734} [get_ports TMDS_Clk_p_0]

# ============================================================
# 八、上板验证必需信号（Stage 10 新增，2026-09-12）
# ============================================================
# ① HDMI 输入 HPD（T19）—— PL 必须拉高
#    依据：PYNQ-Z2 Reference Manual v1.0 Table 7「Hot-plug assert output T19」
#          官方 PYNQ base overlay 同样把 T19 约束为 hdmi_in_hpd[0]
#    作用：向信源（笔记本）断言"接收端已就绪"，否则信源可能不输出 TMDS
#    对应 BD：xlconstant_2(常量 1) -> 顶层端口 hdmi_rx_hpd
#    注意：XDC 不支持 Tcl 的 if/llength，直接 set_property 即可。
#          wrapper 里 hdmi_rx_hpd 是 [0:0] 一位向量，get_ports hdmi_rx_hpd 匹配整组，
#          set_property 的 IOSTANDARD/PACKAGE_PIN 会自动作用到唯一个位上。
set_property -dict { PACKAGE_PIN T19  IOSTANDARD LVCMOS33 } [get_ports hdmi_rx_hpd]

# ② HDMI 输入锁定指示（LED0 / R14）—— 调试用点灯
#    dvi2rgb_0/pLocked=1（已锁定）时 LED0 亮
#    LED0 亮 = TMDS 时钟恢复成功，输入侧通了；灭 = 输入侧没锁上
set_property -dict { PACKAGE_PIN R14  IOSTANDARD LVCMOS33 } [get_ports hdmi_in_locked]

# ============================================================
# 七、时钟组约束（跨时钟域）
#     说明：dvi2rgb/rgb2dvi 内部的跨时钟域 false_path 已由
#     IP 自带的 xdc（dvi2rgb.xdc / rgb2dvi.xdc）在综合时自动应用，
#     顶层不需要额外 set_clock_groups。
#     如果后续综合报「clock 未定义」或「找不到 FCLK_CLK0 引脚」，
#     再把下面这段按实际网表名补上即可。
# ============================================================
# set_clock_groups -asynchronous \
#     -group [get_clocks TMDS_Clk_p_0] \
#     -group [get_clocks FCLK_CLK0]

# ============================================================
# 八、性能监测计数器（2026-11-02 新增）
#     pm_cycles / pm_frames 在 PixelClk 域，每帧只变一次（稳定 16.6 ms），
#     用 toggle 握手跨到 clk_fpga_0 域。以下三条只切掉这几条准静态路径，
#     不影响 AXI 时钟转换器本身的约束。
# ============================================================
# 注意：XDC 用受限 Tcl 解释器，不能定义 proc，只能写纯约束命令。
# 用 -quiet 防止网表里匹配不到时报错。
set_false_path -quiet -to [get_cells -quiet -hier -filter {NAME =~ *pm_cycles_s_reg*}]
set_false_path -quiet -to [get_cells -quiet -hier -filter {NAME =~ *pm_frames_s_reg*}]
set_false_path -quiet -to [get_cells -quiet -hier -filter {NAME =~ *pm_upd_d1_reg*}]
