--------------------------------------------------------------------------------
-- File: vga_to_axis.vhd   (v3, 2026-09-27)
-- Purpose: 把 dvi2rgb 的 VGA 风格视频输出转成 AXI4-Stream 像素流，
--          喂给去雾 HLS 核的 in_stream。
--
-- v3 相对 v2 的改动：加了【输入弹性缓冲 FIFO】。
--
--   为什么必须加（2026-09-27 全链路仿真实测）：
--     dvi2rgb 的视频输出【没有 ready，物理上无法反压】，而且只在 VDE 期间供数
--     （1280/1650 = 78% 占空比）。而 HLS 核每处理完一行，流水线要排空 31 拍、
--     FSM 还要走几个状态 —— 这些周期里它【不消费】。若此时源正在供数，
--     v2 的"只要 VDE 就拉 TVALID"就会把像素白送出去（tready=0 就是丢），
--     丢掉的像素永久消失。
--     仿真数字：3,725,300 个输入像素里有 991,177（26.6%）被丢掉
--     → 核的行/列计数持续漂移 → 输出帧率在 1.20M~1.76M 周期之间乱跳
--     → 显示器锁不住 → 全黑。
--
--   加缓冲后（同一仿真）：丢掉 991,177 → 10,068（0.27%）。
--   本版再加 axis_to_vga.vhd v3 的输出缓冲后，两侧占空比约束都解除，
--   核可以按自己的节奏（~0.97 px/同步时钟）跑，而它只需要 0.78 px/时钟。
--
--   深度只要覆盖"一行内的相位差"（几十个像素），512 有大量余量；
--   FIFO 满时源侧仍然只能丢（无 ready 可拉），drop_cnt 会记下来，便于诊断。
--
-- v2 相对 v1 的改动（保留）：
--   * 数据通路整体延迟 2 拍，使 TLAST 精确打在每行最后一个有效像素上。
--   * 行场同步不再透传，输出侧由流重建时序（见 axis_to_vga.vhd）。
--
-- 时钟：PixelClk（dvi2rgb 恢复出的像素时钟，720p = 74.25MHz）
-- 复位：aresetn（低有效）
--------------------------------------------------------------------------------

library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity vga_to_axis is
    Port (
        PixelClk     : in  std_logic;
        aresetn      : in  std_logic;                     -- 低有效复位

        -- VGA 侧输入（来自 dvi2rgb）
        vid_pData    : in  std_logic_vector(23 downto 0); -- RBG 打包
        vid_pVDE     : in  std_logic;                     -- 数据有效
        vid_pHSync   : in  std_logic;
        vid_pVSync   : in  std_logic;

        -- AXI-Stream 侧输出（去 HLS 核 in_stream）
        -- 注意：HLS 核的 TUSER/TLAST 是 STD_LOGIC_VECTOR(0 downto 0)
        m_tdata  : out std_logic_vector(23 downto 0); -- RGB（R=23:16, G=15:8, B=7:0）
        m_tvalid : out std_logic;
        m_tready : in  std_logic;                     -- 来自核：FIFO 非空且核要数时才取走
        m_tkeep  : out std_logic_vector(2 downto 0);  -- 恒 0b111
        m_tstrb  : out std_logic_vector(2 downto 0);  -- 恒 0b111
        m_tlast  : out std_logic_vector(0 downto 0);  -- 行尾像素同拍
        m_tuser  : out std_logic_vector(0 downto 0);  -- 帧首有效像素同拍
        m_tid    : out std_logic_vector(0 downto 0);  -- 恒 0
        m_tdest  : out std_logic_vector(0 downto 0)   -- 恒 0
    );
end vga_to_axis;

architecture Behavioral of vga_to_axis is
    ----------------------------------------------------------------------------
    -- 输入弹性缓冲：一个字 = {tlast, tuser, tdata[23:0]}
    -- 用"组合读"实现（分布式 RAM），这样 m_tdata 与 m_tvalid 永远同拍，
    -- 不需要额外的 FWFT 前视逻辑，深度 512 只占约 200 个 LUT。
    ----------------------------------------------------------------------------
    -- ★★ 2026-10-08 实验记录：输入 FIFO 512 -> 4096 试过，已回退 ★★
    --
    -- 动机（仍然成立，值得以后再做）：
    --   去雾核【完全无视输入的 TUSER/TLAST】，自己硬数 921600 个像素当一帧。
    --   于是输入桥每丢 1 个像素，核的"帧起点"就相对源帧往后错 1 个像素，
    --   而且**永久累积**：丢够 1280 个就正好错一整行。
    --   核每帧重启时有约 1940 拍不吃输入的空档，512 深盖不住
    --   → 仿真实测每帧丢 1429 个像素(0.155%)。加到 4096 后仿真里降到 39（开机瞬态）。
    --
    -- 但上板结果：**黑屏**（只有 0.1 秒画面）。
    --   原因不在"丢像素"，而在输出桥的同步竞态：加大的 FIFO 给输入流多加了
    --   约 2 行延迟，把 TUSER 相对栅格的相位挪过了临界点，触发了
    --   "TUSER 帧锁切掉 VSYNC" 的隐患（详见 axis_to_vga.vhd 第六处修复的注释）。
    --
    -- 所以这里先回到 512（= 上板验证过"有稳定画面"的那一版），
    -- 等输出桥的同步问题彻底解决、并单独验证过之后，再重新评估加大深度。
    constant FIFO_DEPTH : integer := 512;
    subtype  fifo_word_t is std_logic_vector(25 downto 0);
    type     fifo_mem_t  is array (0 to FIFO_DEPTH-1) of fifo_word_t;

    signal   fifo_mem : fifo_mem_t;
    signal   wr_ptr   : integer range 0 to FIFO_DEPTH-1 := 0;
    signal   rd_ptr   : integer range 0 to FIFO_DEPTH-1 := 0;
    signal   fifo_cnt : integer range 0 to FIFO_DEPTH   := 0;

    -- ★ 强制分布式 RAM（组合读）。
    --   m_tdata <= fifo_mem(rd_ptr) 是组合输出，本拍读必须本拍就有数。
    --   若综合器把它换成 BRAM（同步读、延迟 1 拍），m_tdata 会比
    --   m_tvalid 晚一拍 → 首字错误 + 整体错位。这个属性把它钉死在
    --   分布式 RAM 上。（姊妹模块 axis_to_vga 正是踩了这个坑，上板黑屏。）
    attribute ram_style : string;
    attribute ram_style of fifo_mem : signal is "distributed";

    signal   wr_en    : std_logic;
    signal   rd_en    : std_logic;

    signal   fifo_full  : std_logic;
    signal   fifo_empty : std_logic;

    ----------------------------------------------------------------------------
    -- ★★★ 2026-10-08 对齐修复：让核的"帧起点"落在源帧首上 ★★★
    --
    -- 现象（上板实测，稳定复现）：画面**静止**，但整体水平偏移固定为 800 像素
    --   （显示第 0 列 = 源第 800 列；上下边界正常，因为 800 < 1280 没有整行分量）。
    --
    -- 机制（读源码得出，不是猜测）：
    --   去雾核【完全无视输入的 TUSER/TLAST】，输入与输出是同一个流水线、1:1，
    --   帧边界 = "它读到的第 1 个像素" 开始硬数 921600 个。
    --   所以要让它的一帧对齐源帧，唯一的办法是：
    --       **它读到的第 1 个像素必须是源帧首像素。**
    --
    --   而真实上电时序（program_dehaze.tcl）是：
    --     * PL 一配好像素时钟就起，源栅格自由运行 → 此刻源处于**帧中间**；
    --     * 本模块 FIFO 在 ~9 us 内被写满（512 深），而 rd_en 需要核给的
    --       m_tready，核还没启动 → **FIFO 就此冻结**，头部锁死在
    --       "PL 配置那一刻的源像素" = 一个帧中间的像素；
    --     * 6 秒后脚本才写 ap_start，核从冻结的头部取数
    --       → 帧起点 = 那个帧中间的像素 → 固定偏移 800。
    --
    --   这也解释了**为什么仿真一直复现不出来**：旧 tb 在第 40 拍就放开复位，
    --   那一刻源正好在帧首，冻结的头部本身就是帧首 → 偏移只剩输出流水线的 2 拍。
    --   （新 tb 已按真实时序放开复位 + 延后 ap_start，能复现这个偏移。）
    --
    -- 修法（不依赖任何常数，也不需要和 ap_start 握手）：
    --   核还没取过数（core_started=0）时，**每个源帧首都把 FIFO 清空**，
    --   使"帧首像素"成为 FIFO 头部。这样无论脚本什么时候写 ap_start，
    --   核取到的第一个像素一定是帧首像素 → 偏移恒为 0。
    --   核一旦取过数（core_started=1）就再也不清空，保证流连续、不丢像素。
    ----------------------------------------------------------------------------
    -- ★ 修订 2026-10-08 21:20（第一版写错了，探针实测定位）★
    --   第一版用 `core_started` latch 在 rd_en 上。探针实测：
    --     复位刚放开时 HLS 核的 s_axis_tready 会**短暂为 1**，rd_en 冒出一个脉冲
    --     → core_started 立刻变 1 → 每个帧首的 flush 永远不执行 → 修复完全无效
    --     （修复版仿真结果与基线逐字节相同）。
    --   探针同时确认了正确的判据：
    --     ap_start 之前：m_tready=0, fifo_cnt=512（FIFO 冻结）✓
    --     ap_start 之后：m_tready=1, fifo_cnt=0（核把 FIFO 抽空、跟得上）✓
    --   所以改用 hold_wr 状态机（见 wr_en 处），并且必须处理"冻结造成的流断裂"：
    --     FIFO 冻结【前】写入的那 512 个像素与冻结【后】的实时像素之间有一处
    --     约 6 秒的断裂；若核从冻结的头部开始读，第一帧之后就会错开。
    --     因此 WAIT 态必须把 FIFO 清空，让核的第一个像素正好是源帧首。
    signal   flush_now    : std_logic;              -- 源帧首那一拍（与 m_tuser 同拍）
    signal   hold_wr      : std_logic := '1';       -- '1' = 等待态（丢弃输入、清空 FIFO）
    signal   hold_cnt     : unsigned(17 downto 0) := (others => '0');
    constant HOLD_LIMIT   : unsigned(17 downto 0) := (others => '1');  -- 262143 拍 ≈ 3.5 ms

    -- 2 拍延迟的数据/有效通路（TLAST 精确对齐用）
    signal p_data1 : std_logic_vector(23 downto 0) := (others => '0');
    signal p_data2 : std_logic_vector(23 downto 0) := (others => '0');
    signal p_vde1  : std_logic := '0';
    signal p_vde2  : std_logic := '0';

    -- 帧首标志
    signal vsync_d : std_logic := '0';
    signal sof_arm : std_logic := '0';

    -- 通道重排后的像素（RBG -> RGB）
    signal pix_rgb  : std_logic_vector(23 downto 0);

    -- 调试用：被核反压而丢弃的像素计数（满时才会丢，可引到 ILA / LED）
    signal drop_cnt : unsigned(15 downto 0) := (others => '0');
begin
    ----------------------------------------------------------------------------
    -- 2 拍延迟通路
    ----------------------------------------------------------------------------
    process(PixelClk)
    begin
        if rising_edge(PixelClk) then
            if aresetn = '0' then
                p_data1 <= (others => '0');
                p_data2 <= (others => '0');
                p_vde1  <= '0';
                p_vde2  <= '0';
            else
                p_data1 <= vid_pData;
                p_data2 <= p_data1;
                p_vde1  <= vid_pVDE;
                p_vde2  <= p_vde1;
            end if;
        end if;
    end process;

    ----------------------------------------------------------------------------
    -- 通道重排：RBG -> RGB
    --   dvi2rgb vid_pData: R=23:16, B=15:8, G=7:0
    --   目标 m_tdata:      R=23:16, G=15:8, B=7:0
    ----------------------------------------------------------------------------
    pix_rgb <= p_data2(23 downto 16) & p_data2(7 downto 0) & p_data2(15 downto 8);

    ----------------------------------------------------------------------------
    -- TKEEP/TSTRB/TID/TDEST 恒为常数
    ----------------------------------------------------------------------------
    m_tkeep <= (others => '1');
    m_tstrb <= (others => '1');
    m_tid   <= (others => '0');
    m_tdest <= (others => '0');

    ----------------------------------------------------------------------------
    -- 帧首标志：VSync 下降沿武装，吐出第一个有效像素时撤销
    -- （720p 用的是正极性 VSync，同步期内为高电平）
    ----------------------------------------------------------------------------
    process(PixelClk)
    begin
        if rising_edge(PixelClk) then
            if aresetn = '0' then
                vsync_d <= '0';
                sof_arm <= '0';
            else
                vsync_d <= vid_pVSync;
                if (vid_pVSync = '0') and (vsync_d = '1') then
                    sof_arm <= '1';
                elsif p_vde2 = '1' then
                    sof_arm <= '0';
                end if;
            end if;
        end if;
    end process;

    ----------------------------------------------------------------------------
    -- FIFO 写侧：源像素直接进（源无法反压），只有满时才丢
    --   ★ 对齐期间例外：即便 FIFO 满，源帧首那一拍也必须写进去，
    --     因为本拍像素将直接成为新的 FIFO 头部（见上文对齐修复）
    ----------------------------------------------------------------------------
    flush_now <= p_vde2 and sof_arm;          -- 与 m_tuser 同拍
    -- FEED（hold_wr=0）正常写；WAIT（hold_wr=1）不写；
    -- 放行那一拍（flush_now）必须强制写进去，因为它将成为 FIFO 的头一个像素。
    wr_en     <= p_vde2 and (not fifo_full) and ((not hold_wr) or flush_now);

    ----------------------------------------------------------------------------
    -- hold_wr 状态机：保证"核读到的第一个像素 = 源帧首像素"，且其后的流连续
    --
    --   FEED -> WAIT : 输入 FIFO 连续满 HOLD_LIMIT 拍 = 核一直没在取数
    --                  （ap_start 之前就是这样；正常运行时核把 FIFO 抽空，永不触发）
    --   WAIT 态      : 不写 + 清空 FIFO，于是核即使 tready=1 也拿不到数据
    --   WAIT -> FEED : 源帧首那一拍放行（并用 flush_now 强制写入该像素）
    --
    --   为什么不继续用"一帧首清空一次"：FIFO 冻结【前】写入的像素与冻结【后】的
    --   实时像素之间有约 6 秒的断裂，核从冻结头部开始读只能对齐第一帧。
    --   WAIT 态把 FIFO 彻底清空后，核的第一个像素就是源帧首，之后流是连续的。
    ----------------------------------------------------------------------------
    process(PixelClk)
    begin
        if rising_edge(PixelClk) then
            if aresetn = '0' then
                hold_wr  <= '1';
                hold_cnt <= (others => '0');
            else
                if fifo_full = '1' then
                    if hold_cnt /= HOLD_LIMIT then
                        hold_cnt <= hold_cnt + 1;
                    end if;
                else
                    hold_cnt <= (others => '0');
                end if;

                if hold_cnt = HOLD_LIMIT then
                    hold_wr <= '1';                                  -- 核没在取数 -> 回等待
                elsif (p_vde2 = '1') and (sof_arm = '1') then
                    hold_wr <= '0';                                  -- 源帧首 -> 放行
                end if;
            end if;
        end if;
    end process;

    fifo_full  <= '1' when fifo_cnt >= FIFO_DEPTH else '0';
    fifo_empty <= '1' when fifo_cnt = 0 else '0';

    -- 读侧：核要数（tready）且 FIFO 非空时弹出一个
    rd_en <= m_tready and (not fifo_empty);

    ----------------------------------------------------------------------------
    -- FIFO 存储体（写用同步、读用组合 → 推断为分布式 RAM）
    ----------------------------------------------------------------------------
    process(PixelClk)
    begin
        if rising_edge(PixelClk) then
            if wr_en = '1' then
                fifo_mem(wr_ptr) <= (p_vde2 and (not p_vde1)) &      -- tlast
                                    (p_vde2 and sof_arm) &           -- tuser
                                    pix_rgb;
                if wr_ptr = FIFO_DEPTH-1 then
                    wr_ptr <= 0;
                else
                    wr_ptr <= wr_ptr + 1;
                end if;
            end if;

            if aresetn = '0' then
                wr_ptr   <= 0;
                rd_ptr   <= 0;
                fifo_cnt <= 0;
            else
                if rd_en = '1' and (wr_en = '0' or wr_ptr /= rd_ptr or fifo_cnt >= 2) then
                    if rd_ptr = FIFO_DEPTH-1 then
                        rd_ptr <= 0;
                    else
                        rd_ptr <= rd_ptr + 1;
                    end if;
                end if;

                if    (wr_en = '1') and (rd_en = '0') then
                    fifo_cnt <= fifo_cnt + 1;
                elsif (wr_en = '0') and (rd_en = '1') then
                    fifo_cnt <= fifo_cnt - 1;
                end if;

                -- ★ hold_wr 状态机 / 帧首对齐
                --   注意 flush_now 必须优先：放行那一拍 hold_wr 还是 1（寄存器），
                --   若让 hold_wr 优先就会把刚写进去的帧首像素又清掉。
                if flush_now = '1' then
                    rd_ptr   <= wr_ptr;      -- 本拍写入的"帧首像素"成为新的头部
                    fifo_cnt <= 1;
                elsif hold_wr = '1' then
                    rd_ptr   <= wr_ptr;      -- 等待态：保持 FIFO 为空
                    fifo_cnt <= 0;
                end if;
            end if;
        end if;
    end process;

    ----------------------------------------------------------------------------
    -- AXI-Stream 输出：数据与 valid 同拍（组合读）
    ----------------------------------------------------------------------------
    m_tdata     <= fifo_mem(rd_ptr)(23 downto 0);
    m_tuser(0)  <= fifo_mem(rd_ptr)(24);
    m_tlast(0)  <= fifo_mem(rd_ptr)(25);
    m_tvalid    <= (not fifo_empty);

    ----------------------------------------------------------------------------
    -- 丢弃计数（只在 FIFO 满时增长；正常应恒为 0）
    ----------------------------------------------------------------------------
    process(PixelClk)
    begin
        if rising_edge(PixelClk) then
            if aresetn = '0' then
                drop_cnt <= (others => '0');
            elsif (p_vde2 = '1') and (fifo_full = '1') then
                drop_cnt <= drop_cnt + 1;
            end if;
        end if;
    end process;

end Behavioral;
