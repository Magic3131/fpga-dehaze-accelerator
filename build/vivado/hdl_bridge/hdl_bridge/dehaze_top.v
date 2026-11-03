// =============================================================================
// dehaze_top.v  --  new RTL top level for the dehaze HDMI pipeline
// -----------------------------------------------------------------------------
//   dvi2rgb --(VID_*)--> vga_to_axis --(DZH_IN_*)--> dehaze
//   dehaze  --(DZH_OUT_*)--> axis_to_vga --(RGB_*)--> rgb2dvi
//
// The block design (system_wrapper) keeps the whole HDMI / PS infrastructure.
// The two bridge modules and this top level live OUTSIDE the BD, because the
// module-reference flow (create_bd_cell -type module -reference) is unusable
// on this machine (hostname has no dot -> not a legal IP vendor domain).
//
// Port list == the ORIGINAL system_wrapper port list, so build_stage5.xdc
// needs no change at all.
//
// NOTE: vga_to_axis / axis_to_vga are VHDL entities, so they are instantiated
//       with POSITIONAL port mapping, in exactly the order declared in their
//       entity headers. Do not reorder.
// =============================================================================

`timescale 1 ps / 1 ps

module dehaze_top (
    // ---------------- PS DDR / fixed IO ----------------
    inout  [14:0] DDR_addr,
    inout  [ 2:0] DDR_ba,
    inout         DDR_cas_n,
    inout         DDR_ck_n,
    inout         DDR_ck_p,
    inout         DDR_cke,
    inout         DDR_cs_n,
    inout  [ 3:0] DDR_dm,
    inout  [31:0] DDR_dq,
    inout  [ 3:0] DDR_dqs_n,
    inout  [ 3:0] DDR_dqs_p,
    inout         DDR_odt,
    inout         DDR_ras_n,
    inout         DDR_reset_n,
    inout         DDR_we_n,
    inout         FIXED_IO_ddr_vrn,
    inout         FIXED_IO_ddr_vrp,
    inout  [53:0] FIXED_IO_mio,
    inout         FIXED_IO_ps_clk,
    inout         FIXED_IO_ps_porb,
    inout         FIXED_IO_ps_srstb,
    // ---------------- HDMI sink (input side) ----------------
    input         TMDS_Clk_n_0,
    input         TMDS_Clk_p_0,
    input  [ 2:0] TMDS_Data_n_0,
    input  [ 2:0] TMDS_Data_p_0,
    // ---------------- HDMI source (output side) ----------------
    output        TMDS_Clk_n_1,
    output        TMDS_Clk_p_1,
    output [ 2:0] TMDS_Data_n_1,
    output [ 2:0] TMDS_Data_p_1,
    // ---------------- DDC / HPD / lock ----------------
    inout         audio_i2c_0_scl_io,
    inout         audio_i2c_0_sda_io,
    output        hdmi_in_locked,
    output [ 0:0] hdmi_rx_hpd
);

    // -------------------------------------------------------------------------
    // pixel-clock plumbing coming out of the BD
    // -------------------------------------------------------------------------
    wire        pxl_clk;
    wire        pxl_aresetn;

    wire [23:0] vid_pdata;
    wire        vid_pvde;
    wire        vid_phsync;
    wire        vid_pvsync;

    // -------------------------------------------------------------------------
    // pixel stream : bridge u_in -> dehaze
    // -------------------------------------------------------------------------
    wire [23:0] dzh_in_tdata;
    wire        dzh_in_tvalid;
    wire        dzh_in_tready;
    wire [ 2:0] dzh_in_tkeep;
    wire [ 2:0] dzh_in_tstrb;
    wire [ 0:0] dzh_in_tlast;
    wire [ 0:0] dzh_in_tuser;
    wire [ 0:0] dzh_in_tid;
    wire [ 0:0] dzh_in_tdest;

    // -------------------------------------------------------------------------
    // pixel stream : dehaze -> bridge u_out
    // -------------------------------------------------------------------------
    wire [23:0] dzh_out_tdata;
    wire        dzh_out_tvalid;
    wire        dzh_out_tready;
    wire [ 2:0] dzh_out_tkeep;
    wire [ 2:0] dzh_out_tstrb;
    wire [ 0:0] dzh_out_tlast;
    wire [ 0:0] dzh_out_tuser;
    wire [ 0:0] dzh_out_tid;
    wire [ 0:0] dzh_out_tdest;

    // -------------------------------------------------------------------------
    // what actually feeds the AXIS -> VGA bridge
    //   default             : the dehaze HLS core output (normal design)
    //   -d DEHAZE_LOOPBACK  : the *input* bridge, i.e. the core is bypassed
    //                         (diagnostic build, see build_loopback_test.tcl)
    // Both bridges run on PixelClk, so the core can be cut out with wires only.
    // -------------------------------------------------------------------------
    wire [23:0] axs_tdata;
    wire        axs_tvalid;
    wire [ 2:0] axs_tkeep;
    wire [ 2:0] axs_tstrb;
    wire [ 0:0] axs_tlast;
    wire [ 0:0] axs_tuser;
    wire [ 0:0] axs_tid;
    wire [ 0:0] axs_tdest;

    // -------------------------------------------------------------------------
    // performance monitor  (added 2026-11-02)
    //
    // 量的是「去雾核处理一帧花了多少 PixelClk 拍」：
    //   起点 = 核收到该帧第 1 个像素（DZH_IN tuser & tvalid & tready）
    //   终点 = 第 921600 个输出像素被输出桥接收（DZH_OUT tvalid & tready）
    //
    // 结果通过 axi_gpio（0x43C20000）交给 PS：
    //   0x00 PM_CYCLES  上一帧的拍数
    //   0x08 PM_FRAMES  已测帧数
    //
    // pm_cycles/pm_frames 每帧才变一次（稳定 16.6 ms），用 toggle 握手跨到
    // clk_fpga_0 域；数据路径在 build_stage5.xdc 里声明为 false_path。
    // -------------------------------------------------------------------------
    wire        dbg_clk;
    reg  [31:0] pm_cyc;
    reg  [31:0] pm_t0;
    reg         pm_armed;
    reg  [31:0] pm_cycles;
    reg  [31:0] pm_frames;
    reg         pm_upd;
    reg         pm_upd_d1, pm_upd_d2, pm_upd_d3;
    reg  [31:0] pm_cycles_s;
    reg  [31:0] pm_frames_s;

`ifdef DEHAZE_LOOPBACK
    assign axs_tdata  = dzh_in_tdata;
    assign axs_tvalid = dzh_in_tvalid;
    assign axs_tkeep  = dzh_in_tkeep;
    assign axs_tstrb  = dzh_in_tstrb;
    assign axs_tlast  = dzh_in_tlast;
    assign axs_tuser  = dzh_in_tuser;
    assign axs_tid    = dzh_in_tid;
    assign axs_tdest  = dzh_in_tdest;
`else
    assign axs_tdata  = dzh_out_tdata;
    assign axs_tvalid = dzh_out_tvalid;
    assign axs_tkeep  = dzh_out_tkeep;
    assign axs_tstrb  = dzh_out_tstrb;
    assign axs_tlast  = dzh_out_tlast;
    assign axs_tuser  = dzh_out_tuser;
    assign axs_tid    = dzh_out_tid;
    assign axs_tdest  = dzh_out_tdest;
`endif

    // -------------------------------------------------------------------------
    // video going back into rgb2dvi
    // -------------------------------------------------------------------------
    wire [23:0] rgb_pdata;
    wire        rgb_pvde;
    wire        rgb_phsync;
    wire        rgb_pvsync;

    // =========================================================================
    // 1. block design : HDMI infra + PS + dehaze HLS core
    // =========================================================================
    system_wrapper u_sys (
        // PS
        .DDR_addr           (DDR_addr),
        .DDR_ba             (DDR_ba),
        .DDR_cas_n          (DDR_cas_n),
        .DDR_ck_n           (DDR_ck_n),
        .DDR_ck_p           (DDR_ck_p),
        .DDR_cke            (DDR_cke),
        .DDR_cs_n           (DDR_cs_n),
        .DDR_dm             (DDR_dm),
        .DDR_dq             (DDR_dq),
        .DDR_dqs_n          (DDR_dqs_n),
        .DDR_dqs_p          (DDR_dqs_p),
        .DDR_odt            (DDR_odt),
        .DDR_ras_n          (DDR_ras_n),
        .DDR_reset_n        (DDR_reset_n),
        .DDR_we_n           (DDR_we_n),
        .FIXED_IO_ddr_vrn   (FIXED_IO_ddr_vrn),
        .FIXED_IO_ddr_vrp   (FIXED_IO_ddr_vrp),
        .FIXED_IO_mio       (FIXED_IO_mio),
        .FIXED_IO_ps_clk    (FIXED_IO_ps_clk),
        .FIXED_IO_ps_porb   (FIXED_IO_ps_porb),
        .FIXED_IO_ps_srstb  (FIXED_IO_ps_srstb),
        // HDMI
        .TMDS_Clk_n_0       (TMDS_Clk_n_0),
        .TMDS_Clk_p_0       (TMDS_Clk_p_0),
        .TMDS_Data_n_0      (TMDS_Data_n_0),
        .TMDS_Data_p_0      (TMDS_Data_p_0),
        .TMDS_Clk_n_1       (TMDS_Clk_n_1),
        .TMDS_Clk_p_1       (TMDS_Clk_p_1),
        .TMDS_Data_n_1      (TMDS_Data_n_1),
        .TMDS_Data_p_1      (TMDS_Data_p_1),
        .audio_i2c_0_scl_io (audio_i2c_0_scl_io),
        .audio_i2c_0_sda_io (audio_i2c_0_sda_io),
        .hdmi_in_locked     (hdmi_in_locked),
        .hdmi_rx_hpd        (hdmi_rx_hpd),
        // ---- exported to the bridges ----
        .PXL_CLK            (pxl_clk),
        .PXL_ARESETN        (pxl_aresetn),
        .VID_PDATA          (vid_pdata),
        .VID_PVDE           (vid_pvde),
        .VID_PHSYNC         (vid_phsync),
        .VID_PVSYNC         (vid_pvsync),
        .RGB_PDATA          (rgb_pdata),
        .RGB_PVDE           (rgb_pvde),
        .RGB_PHSYNC         (rgb_phsync),
        .RGB_PVSYNC         (rgb_pvsync),
        .DZH_IN_tdata       (dzh_in_tdata),
        .DZH_IN_tvalid      (dzh_in_tvalid),
        .DZH_IN_tready      (dzh_in_tready),
        .DZH_IN_tkeep       (dzh_in_tkeep),
        .DZH_IN_tstrb       (dzh_in_tstrb),
        .DZH_IN_tlast       (dzh_in_tlast),
        .DZH_IN_tuser       (dzh_in_tuser),
        .DZH_IN_tid         (dzh_in_tid),
        .DZH_IN_tdest       (dzh_in_tdest),
        .DZH_OUT_tdata      (dzh_out_tdata),
        .DZH_OUT_tvalid     (dzh_out_tvalid),
        .DZH_OUT_tready     (dzh_out_tready),
        .DZH_OUT_tkeep      (dzh_out_tkeep),
        .DZH_OUT_tstrb      (dzh_out_tstrb),
        .DZH_OUT_tlast      (dzh_out_tlast),
        .DZH_OUT_tuser      (dzh_out_tuser),
        .DZH_OUT_tid        (dzh_out_tid),
        .DZH_OUT_tdest      (dzh_out_tdest),
        // ---- performance monitor (axi_gpio @ 0x43C20000) ----
        .DBG_CLK            (dbg_clk),
        .DBG_CYCLES         (pm_cycles_s),
        .DBG_FRAMES         (pm_frames_s)
    );

    // =========================================================================
    // 2. VGA -> AXIS bridge   (VHDL entity : POSITIONAL mapping)
    //
    // entity vga_to_axis port order :
    //   PixelClk, aresetn,
    //   vid_pData, vid_pVDE, vid_pHSync, vid_pVSync,
    //   m_tdata, m_tvalid, m_tready, m_tkeep, m_tstrb,
    //   m_tlast, m_tuser, m_tid, m_tdest
    // =========================================================================
    vga_to_axis u_in (
        pxl_clk,            // PixelClk
        pxl_aresetn,        // aresetn
        vid_pdata,          // vid_pData
        vid_pvde,           // vid_pVDE
        vid_phsync,         // vid_pHSync
        vid_pvsync,         // vid_pVSync
        dzh_in_tdata,       // m_tdata
        dzh_in_tvalid,      // m_tvalid
        dzh_in_tready,      // m_tready
        dzh_in_tkeep,       // m_tkeep
        dzh_in_tstrb,       // m_tstrb
        dzh_in_tlast,       // m_tlast
        dzh_in_tuser,       // m_tuser
        dzh_in_tid,         // m_tid
        dzh_in_tdest        // m_tdest
    );

    // =========================================================================
    // 3. AXIS -> VGA bridge   (VHDL entity : POSITIONAL mapping)
    //
    // entity axis_to_vga port order :
    //   PixelClk, aresetn,
    //   s_tdata, s_tvalid, s_tready, s_tuser, s_tlast,
    //   s_tkeep, s_tstrb, s_tid, s_tdest,
    //   vid_pData, vid_pVDE, vid_pHSync, vid_pVSync
    // =========================================================================
    axis_to_vga u_out (
        pxl_clk,            // PixelClk
        pxl_aresetn,        // aresetn
        axs_tdata,          // s_tdata
        axs_tvalid,         // s_tvalid
        dzh_out_tready,     // s_tready   (always back-pressures the core)
        axs_tuser,          // s_tuser
        axs_tlast,          // s_tlast
        axs_tkeep,          // s_tkeep
        axs_tstrb,          // s_tstrb
        axs_tid,            // s_tid
        axs_tdest,          // s_tdest
        rgb_pdata,          // vid_pData
        rgb_pvde,           // vid_pVDE
        rgb_phsync,         // vid_pHSync
        rgb_pvsync          // vid_pVSync
    );

    // =========================================================================
    // 5. performance monitor  (PixelClk domain)
    //
    //   pm_cycles = 输入帧首 → 输出帧首 的拍数（核的流水线延迟 / 单帧处理时间）
    //
    //   为什么不用"数满 921600 个输出像素"：
    //     每个帧周期里输出有效像素正好 921600 个，而帧首又会把计数器清零，
    //     "帧尾完成"与"下次帧首复位"会撞在同一拍，差一拍就永远数不满
    //     （实测 PM_FRAMES 一直为 0 就是这个原因）。
    //   改成两个 TUSER 相减后，不依赖任何魔数，也不存在竞争。
    // =========================================================================
    always @(posedge pxl_clk) begin
        if (!pxl_aresetn) begin
            pm_cyc    <= 32'd0;
            pm_t0     <= 32'd0;
            pm_armed  <= 1'b0;
            pm_cycles <= 32'd0;
            pm_frames <= 32'd0;
            pm_upd    <= 1'b0;
        end else begin
            pm_cyc <= pm_cyc + 32'd1;

            // 起点：核收到该帧第 1 个像素
            if (dzh_in_tuser[0] && dzh_in_tvalid) begin
                pm_t0    <= pm_cyc;
                pm_armed <= 1'b1;
            end

            // 终点：核吐出该帧第 1 个像素
            if (pm_armed && dzh_out_tuser[0] && dzh_out_tvalid) begin
                pm_cycles <= pm_cyc - pm_t0;
                pm_frames <= pm_frames + 32'd1;
                pm_upd    <= ~pm_upd;
                pm_armed  <= 1'b0;
            end
        end
    end

    // ---- toggle 握手跨到 clk_fpga_0 域（数据每帧才变一次，采样时早已稳定）----
    always @(posedge dbg_clk) begin
        pm_upd_d1 <= pm_upd;
        pm_upd_d2 <= pm_upd_d1;
        pm_upd_d3 <= pm_upd_d2;
        if (pm_upd_d2 != pm_upd_d3) begin
            pm_cycles_s <= pm_cycles;
            pm_frames_s <= pm_frames;
        end
    end

endmodule
