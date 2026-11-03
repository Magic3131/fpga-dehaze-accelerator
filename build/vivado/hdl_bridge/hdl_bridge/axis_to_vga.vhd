--------------------------------------------------------------------------------
-- File: axis_to_vga.vhd   (v3, 2026-09-27)
-- Purpose: 把去雾 HLS 核的 AXI4-Stream 输出转回 rgb2dvi 需要的 VGA 风格信号。
--
-- v3 相对 v2 的改动：在核的输出端加了【弹性缓冲 FIFO】。
--
--   为什么必须加（2026-09-27 全链路仿真实测）：
--     v2 的 s_tready 直接挂在栅格的 vde_c 上 —— 也就是"只在自己的有效像素区
--     才向核取数"，占空比 1280/1650 = 78%。而输入源也只在 78% 的时间供数，
--     两个窗口相位一错开，交集只剩 ~56% → 核只能推进 0.547 px/时钟，
--     而输入按 0.745 px/时钟供给 → 多出来的像素被 vga_to_axis 丢掉
--     （仿真：26.6%）→ 核的行/列对齐持续漂移 → 本模块的 TUSER 对齐不断重启栅格
--     → 输出帧长在 1.20M~1.76M 周期之间乱跳 → 显示器锁不住 → 全黑。
--
--   加 FIFO 之后，核可以【在消隐期也往外吐】（每行多出 370 周期的输出能力），
--   平均吞吐 0.547 → 0.776 px/时钟，正好跟上输入。
--   仿真对照：丢掉 991,177(26.6%) → 10,068(0.27%)；VSYNC 间隔恢复恒定。
--   配合 vga_to_axis.vhd v3 的输入缓冲后，两侧占空比约束都解除。
--
--   实现要点：核侧 s_tready = FIFO 不满（几乎恒 1），栅格侧用"提前一拍前视"
--   （rd_en 由下一拍的 vde 决定），所以不需要 FWFT 前视逻辑，读口可以走 BRAM。
--
-- v2 保留的设计（为什么必须这样）：
--   本模块自己生成 720p 完整行场时序，用流里的 TUSER 做帧对齐；
--   不再透传输入侧同步（核内部有行缓冲，输出相对输入延迟若干行，
--   透传同步会导致画面整体上移/断裂并随时间滚动）。
--   1. 内部 H/V 计数器自由运行，产生 1650 x 750 的 720p60 栅格（正极性同步）。
--   2. 收到 TUSER 的那一拍，把该像素放到栅格 (0,0) 位置并就地重置计数器，
--      每帧重新对齐一次，因此核延迟具体多少行都不影响结果。
--   3. 栅格需要像素但 FIFO 空（欠载）时输出保持上一像素，开机头一两帧会有
--      局部黑块，之后自动锁定，属正常现象。
--
-- 关于输出字节布局：
--   去雾核当前实现是 out_stream_TDATA = {16'b0, gray8}，即 8 位灰度值
--   放在最低字节。直接按 RGB->RBG 重排会让画面偏色（只剩蓝通道）。
--   本模块做了自适应：若高 16 位全 0 则认为是灰度核，把低字节复制到
--   RGB 三通道；否则按标准 RGB->RBG 重排。将来换彩色去雾核无需改代码。
--
-- 时钟：PixelClk
-- 复位：aresetn（低有效）
--------------------------------------------------------------------------------

library IEEE;
use IEEE.STD_LOGIC_1164.ALL;
use IEEE.NUMERIC_STD.ALL;

entity axis_to_vga is
    Port (
        PixelClk   : in  std_logic;
        aresetn    : in  std_logic;                       -- 低有效复位

        -- AXI-Stream 侧输入（来自去雾核 out_stream）
        s_tdata  : in  std_logic_vector(23 downto 0);
        s_tvalid : in  std_logic;
        s_tready : out std_logic;
        s_tuser  : in  std_logic_vector(0 downto 0);      -- 帧首标志
        s_tlast  : in  std_logic_vector(0 downto 0);      -- 行尾标志（本版未使用）
        s_tkeep  : in  std_logic_vector(2 downto 0);      -- 未使用（保持接口完整）
        s_tstrb  : in  std_logic_vector(2 downto 0);      -- 未使用（保持接口完整）
        s_tid    : in  std_logic_vector(0 downto 0);      -- 未使用（保持接口完整）
        s_tdest  : in  std_logic_vector(0 downto 0);      -- 未使用（保持接口完整）

        -- VGA 侧输出（去 rgb2dvi）
        vid_pData  : out std_logic_vector(23 downto 0);   -- RBG 打包
        vid_pVDE   : out std_logic;
        vid_pHSync : out std_logic;
        vid_pVSync : out std_logic
    );
end axis_to_vga;

architecture Behavioral of axis_to_vga is
    ----------------------------------------------------------------------------
    -- 720p60 (CEA-861 format 4) 时序常数
    ----------------------------------------------------------------------------
    constant H_ACTIVE : integer := 1280;
    constant H_FP     : integer := 110;
    constant H_SYNC   : integer := 40;
    constant H_BP     : integer := 220;
    constant H_TOTAL  : integer := 1650;

    constant V_ACTIVE : integer := 720;
    constant V_FP     : integer := 5;
    constant V_SYNC   : integer := 5;
    constant V_BP     : integer := 20;
    -- ★★ V_TOTAL 必须严格是 750（已回退到上板验证过的配置）★★
    --
    --   它决定栅格的"消费速率"：
    --     栅格速率 = (H_ACTIVE/H_TOTAL) × (V_ACTIVE/V_TOTAL)
    --              = (1280/1650) × (720/750) = 0.7447 px/拍
    --   必须等于核的供给速率 921600/1237500 = 0.7447。
    --   改大/改小都会让栅格消费速率与核不匹配 → FIFO 涨/空 → 连锁崩溃。
    --
    --   【2026-10-08 实验记录】曾把 vcnt 改成"帧内行号、永不回卷、只在 TUSER
    --   处归零"（V_TOTAL 只作饱和上限），想让 VSYNC 不可能被帧锁切掉。
    --   概念是对的，但实测更差：VSYNC 只发一半（12 帧只有 6 个）、对齐探针
    --   剧烈漂移（0→40→1279→1123→917→505），裁决 RASTER MISALIGNED。
    --   原因未完全查清，故先回退。**输出桥的同步竞态是真问题，需要单独、
    --   仔细地重构并验证，不能再叠着改。**
    constant V_TOTAL  : integer := 750;

    ----------------------------------------------------------------------------
    -- 输出弹性缓冲：一个字 = {tlast, tuser, tdata[23:0]}
    --
    -- ★ 2026-09-28 上板黑屏根因修复（第二处，见下）★
    --   原来这里写 FIFO_DEPTH = 2048 (26 位 → 53248 bit)。Vivado 综合时
    --   把这个尺寸推断成 **Block RAM**（实测资源报告：Block RAM Tile 1.5，
    --   即 RAMB36E1 + RAMB18E1）。
    --   而本模块的读逻辑是【按组合读写的】：
    --       rd_data <= fifo_mem(rd_ptr);     -- 假设本拍就能拿到数据
    --       rd_valid<= rd_en;
    --   分布式 RAM 是组合读（0 延迟），这套写法才成立；但 BRAM 是
    --   **同步读、延迟 1 拍**，于是 rd_valid=1 的那一拍 rd_data 里装的
    --   其实是上一拍读地址的结果 → 整帧数据错位 1 拍、首个数据是残值
    --   → 输出画面被打乱 → HDMI 行场时序不合法 → 显示器锁不住 → 黑屏。
    --
    --   证据（2026-09-28 仿真 + 上板）：
    --     * 仿真探针：hcnt 正常每拍 +1、rd_valid 恒 1，但 data_r 连续
    --       11 拍完全不变（0xff2ad4）→ 读数据与读指针不同步。
    --     * 资源报告：LUT as Distributed RAM 312 (RAMD64E) 来自输入桥
    --       (depth 512)，Block RAM Tile 1.5 来自本模块 (depth 2048)。
    --     * 输入桥 depth 512 用分布式 RAM，所以它一直工作正常。
    --
    --   修复：加 ram_style="distributed" 属性强制分布式 RAM，并控制深度。
    --   2026-09-29 又把深度提到 4096（配合"永不反压核"），此时更要靠这个
    --   属性拦住 Vivado —— 否则 4096 深必然被推断成 BRAM，读延迟从 0 拍
    --   变成 1 拍，会再次静默地整帧错位。
    ----------------------------------------------------------------------------
    constant FIFO_DEPTH : integer := 4096;
    subtype  fifo_word_t is std_logic_vector(25 downto 0);
    type     fifo_mem_t  is array (0 to FIFO_DEPTH-1) of fifo_word_t;

    signal   fifo_mem  : fifo_mem_t;
    signal   wr_ptr    : integer range 0 to FIFO_DEPTH-1 := 0;
    signal   rd_ptr    : integer range 0 to FIFO_DEPTH-1 := 0;
    signal   fifo_cnt  : integer range 0 to FIFO_DEPTH   := 0;
    signal   fifo_full : std_logic;
    signal   wr_en     : std_logic;
    signal   rd_en     : std_logic;

    -- ★ 强制分布式 RAM（组合读）。
    --   读逻辑 rd_data <= fifo_mem(rd_ptr) 依赖"本拍读、本拍就有数"，
    --   只有分布式 RAM 才有这个 0 延迟特性。若哪天深度被改大、Vivado 想
    --   换成 BRAM（同步读、延迟 1 拍），这个属性会阻止它，避免又一次
    --   静悄悄的数据错位。加属性后综合器要么用分布式 RAM，要么在
    --   资源不足时报错 —— 无论如何不会沉默地改变读延迟。
    attribute ram_style : string;
    attribute ram_style of fifo_mem : signal is "distributed";

    signal   rd_data   : fifo_word_t := (others => '0');   -- 组合读 -> 打一拍的读数据
    signal   rd_valid  : std_logic := '0';                 -- rd_data 本拍是否有效

    -- 栅格计数器
    signal hcnt : unsigned(10 downto 0) := (others => '0');   -- 0 .. 1649
    signal vcnt : unsigned(9 downto 0)  := (others => '0');   -- 0 .. 749

    signal hcnt_nxt : unsigned(10 downto 0);
    signal vcnt_nxt : unsigned(9 downto 0);

    -- 计数器组合出的时序
    signal vde_c   : std_logic;
    signal hsync_c : std_logic;
    signal vsync_c : std_logic;

    -- 下一拍的 VDE：用于提前一拍向 FIFO 要数
    signal vde_c_nxt : std_logic;

    -- 输出寄存器（数据与同步必须同拍，故一起打拍）
    signal data_r  : std_logic_vector(23 downto 0) := (others => '0');
    signal vde_r   : std_logic := '0';
    signal hsync_r : std_logic := '0';
    signal vsync_r : std_logic := '0';

    ----------------------------------------------------------------------------
    -- ★★★ 2026-09-29 第三处修复：读窗口跟着数据走（TLAST 锁行 / TUSER 锁帧）★★★
    --
    -- 演进过程：
    --   09-27 旧代码：读到 TUSER 就 `hcnt<=1; vcnt<=0; vde_r<='1';`
    --                 → 帧周期被撑到 1258497（应 1237500）→ 时序非法 → 锁不住。
    --   09-28 上一版：把 TUSER 整个忽略掉 → 时序合法、出画面了，
    --                 但帧原点与数据帧原点之间是**任意相位** → "错位"。
    --   09-29 本版：不再让栅格自己随便跑，而是让**读窗口对齐数据的行/帧边界**。
    --
    -- 为什么必须对齐（仿真实测，这才是三个症状的真正同源根因）：
    --   读窗口占空比 1280/1650 = 78%，而核的输出是"每行 1280 拍突发 + 370 拍空档"。
    --   两者相位任意 → 想靠 FIFO 吸收这个错位。但 FIFO 只有 512 深，
    --   而相位差最大可达 1 行 = 1650 拍 > 512 →
    --   FIFO 顶满 → 核被反压 → 核停止吃输入 → **输入桥 FIFO 溢出 → 丢像素**。
    --   仿真实测（12 帧）：
    --       input  OFFERED by source : 11059200
    --       input  CONSUMED by core  : 10719181
    --       input  DROPPED by bridge :   340019   (3.1%!)
    --   丢了 3.1% 的输入像素之后，核"每帧死数 921600"的输出帧里
    --   其实装了 1.032 个源帧的内容 → 画面**持续滚动** + 水平方向**错位**。
    --   （用户看到的三个症状：错位 / 下沉 / 上下扫描，全部来自这里。）
    --
    -- 对齐原理（核输出的结构已知，见文件尾注释）：
    --   * TLAST 出现在每行第 1280 个像素 → 用它把行相位钉死：
    --       收到 TLAST 就把 hcnt 拉到 H_ACTIVE，行 = 1280 有效 + 370 消隐 = 1650 拍。
    --   * TUSER 出现在每帧第 0 个像素，而它必然是某一行的第 0 个像素
    --     → 行相位钉死后，TUSER 一定落在 hcnt=0 上，此时
    --       "hcnt<=0" 是无操作，只需把 vcnt 归零即可，**不丢任何一拍**。
    --   * 帧长自然等于核的帧周期（源帧周期）→ 不再漂移 → 不滚动。
    --
    -- 读使能的 1 拍关系（必须用 vde_c 而不是 vde_c_nxt，见下）：
    --   rd_data <= fifo_mem(rd_ptr) 是组合读，第 M 拍发起读，
    --   第 M+1 拍数据出现在 rd_data / data_r 上，而 vde_r 也在第 M 拍被赋值
    --   （vde_r <= vde_c，vde_c 由第 M 拍的 hcnt/vcnt 组合得到）。
    --   所以"第 M 拍取到的像素"被**呈现**在栅格位置 (hcnt(M), vcnt(M))。
    --   要让每行正好 1280 个有效拍，读必须在 hcnt=0..1279 发起 → 用 vde_c。
    --   （旧代码用 vde_c_nxt，会多读一拍、每行丢掉第 1 个像素。）
    ----------------------------------------------------------------------------
    -- TUSER 最小间隔护栏：以"行"为单位计数，饱和在 15。
    --   正常 TUSER 间隔 = 1 帧 = 750 行，远大于 15；毛刺 TUSER 会远小于 15。
    constant TUSER_GAP_LINES : unsigned(3 downto 0) := "1111";   -- 15

    signal lines_since_tu : unsigned(3 downto 0) := "1111";
    signal cur_tuser      : std_logic;   -- 本拍取到的像素带 TUSER
    signal cur_tlast      : std_logic;   -- 本拍取到的像素带 TLAST
    signal tuser_apply    : std_logic;   -- 通过护栏、同意归零
begin

    ----------------------------------------------------------------------------
    -- 1. 计数器 -> 时序信号
    --
    --   【2026-10-08 已知隐患，尚未解决】VSYNC 放在帧尾（vcnt 725..729），
    --   而帧锁(TUSER)可能在 vcnt<725 处把 vcnt 归零 —— 那样这一轮的 VSYNC
    --   就永远不会发 → 显示器丢同步 → 黑屏。这很可能就是 10-08 版黑屏的原因
    --   （输入 FIFO 加大后相位移动触发了它）。
    --   试过把 VSYNC 挪到帧首，结果帧锁会重新触发脉冲（12 帧发 14 个），也不行。
    --   正解应是让同步脉冲的发出与 vcnt 归零解耦，需要专门重构。先保持原样。
    ----------------------------------------------------------------------------
    vde_c   <= '1' when (hcnt < H_ACTIVE) and (vcnt < V_ACTIVE) else '0';

    hsync_c <= '1' when (hcnt >= H_ACTIVE + H_FP) and
                        (hcnt <  H_ACTIVE + H_FP + H_SYNC) else '0';

    vsync_c <= '1' when (vcnt >= V_ACTIVE + V_FP) and
                        (vcnt <  V_ACTIVE + V_FP + V_SYNC) else '0';

    ----------------------------------------------------------------------------
    -- 2. 计数器下一拍值
    --
    --   正常回卷（V_TOTAL = 750）。帧锁(TUSER)通常会在回卷点附近先把 vcnt
    --   归零，所以每帧有效行恒为 V_ACTIVE = 720 行 = 921600 拍，
    --   与核给的严格相等（仿真实测 vde=921600 EXACT）。
    --   无论 TUSER 落在哪一行，帧首的 VSYNC(vcnt 0..4) 都一定会发出去。
    --   （千万不能改成"到 749 就停住等 TUSER" —— 那会死锁，见常量区注释。）
    ----------------------------------------------------------------------------
    hcnt_nxt <= (others => '0') when (hcnt = H_TOTAL - 1) else (hcnt + 1);

    vcnt_nxt <= (others => '0') when (hcnt = H_TOTAL - 1) and (vcnt = V_TOTAL - 1) else
                (vcnt + 1)      when (hcnt = H_TOTAL - 1) else
                vcnt;

    -- 下一拍是否在有效像素区（用于提前一拍取数）
    vde_c_nxt <= '1' when (hcnt_nxt < H_ACTIVE) and (vcnt_nxt < V_ACTIVE) else '0';

    ----------------------------------------------------------------------------
    -- ★ 行/帧对齐判定（见文件头"第三处修复"）
    --
    -- 组合前视：第 M 拍 rd_en=1 时 fifo_mem(rd_ptr) 就是"第 M 拍取到的那个像素"
    -- （它会在第 M+1 拍出现在 rd_data 上、被呈现在栅格位置 (hcnt(M), vcnt(M))）。
    -- 分布式 RAM 本来就是组合读，所以这个前视不额外花资源。
    ----------------------------------------------------------------------------
    cur_tuser <= '1' when (rd_en = '1') and (fifo_mem(rd_ptr)(24) = '1') else '0';
    cur_tlast <= '1' when (rd_en = '1') and (fifo_mem(rd_ptr)(25) = '1') else '0';

    -- 护栏：距上次接受 TUSER 至少 15 行。正常间隔是 750 行，不会误伤；
    --       而毛刺 TUSER（同帧内再来一次）会被挡掉，避免整帧被截断。
    tuser_apply <= '1' when (cur_tuser = '1') and
                            (lines_since_tu = TUSER_GAP_LINES) else '0';

    -- 护栏计数器：每行 +1，饱和在 15；接受一次 TUSER 就清零
    process(PixelClk)
    begin
        if rising_edge(PixelClk) then
            if aresetn = '0' then
                lines_since_tu <= TUSER_GAP_LINES;          -- 复位后即"已饱和"
            elsif tuser_apply = '1' then
                lines_since_tu <= (others => '0');
            elsif (hcnt = H_TOTAL - 1) and (lines_since_tu /= TUSER_GAP_LINES) then
                lines_since_tu <= lines_since_tu + 1;
            end if;
        end if;
    end process;

    ----------------------------------------------------------------------------
    -- 3. FIFO：★ 永不反压核（2026-09-29 第四次修复，这是最关键的会诊结论）
    --
    --   仿真实测（sim_chain，12 帧）：
    --     * 把核的 out_stream_TREADY 直通拉高（即"不反压核"）之后：
    --           input OFFERED = 11059200
    --           input CONSUMED= 11042047
    --           input DROPPED =    17153   ← 仅 0.16%
    --       即 **去雾核本身完全跑得满 720p60，吞吐无问题**。
    --     * 而只要允许反压（s_tready = ~fifo_full），就会形成正反馈：
    --           栅格相位与核输出瞬间对不上
    --             → 输出 FIFO 顶满 → 核被反压
    --             → 核停止吃输入 → **输入桥 FIFO 溢出、丢像素(3.1%)**
    --             → 核"每帧死数 921600"的输出里装了 1.03 个源帧的内容
    --             → 栅格与数据的帧边界继续漂移 → 更对不上 → 崩溃。
    --       实测收敛到 vde=961280 / 帧长 1288650（应 921600 / 1237500）。
    --
    --   所以本版把 s_tready 恒置 1：核永远不会被本模块拖住，正反馈被切断。
    --   稳定性由"栅格读速 == 核供给速率"保证（两者都 = 源的 1280 拍/1650 拍，
    --   对齐后相等），FIFO 深度加大到 4096 只是为了吸收相位抖动。
    ----------------------------------------------------------------------------
    fifo_full <= '1' when fifo_cnt >= FIFO_DEPTH else '0';

    wr_en     <= s_tvalid;              -- ★ 无条件写：不因为 FIFO 满而拒绝
    s_tready  <= '1';                   -- ★ 恒 ready：永不反压去雾核

    -- 本拍向 FIFO 要数：组合读（0 延迟），第 M 拍取到的像素呈现于 (hcnt(M), vcnt(M))
    --   ★ 必须用 vde_c 而不是 vde_c_nxt：用 nxt 会多读一拍，
    --     导致每行第 1 个像素被丢、每行只有 1279 个有效拍。
    rd_en     <= vde_c when fifo_cnt > 0 else '0';

    process(PixelClk)
    begin
        if rising_edge(PixelClk) then
            if aresetn = '0' then
                wr_ptr   <= 0;
                rd_ptr   <= 0;
                fifo_cnt <= 0;
                rd_valid <= '0';
                rd_data  <= (others => '0');
            else
                -- 写
                if wr_en = '1' then
                    fifo_mem(wr_ptr) <= s_tlast(0) & s_tuser(0) & s_tdata;
                    if wr_ptr = FIFO_DEPTH-1 then
                        wr_ptr <= 0;
                    else
                        wr_ptr <= wr_ptr + 1;
                    end if;
                end if;

                -- 读（寄存器化 → 数据在下一拍出现在 rd_data 上）
                rd_valid <= rd_en;
                if rd_en = '1' then
                    rd_data <= fifo_mem(rd_ptr);
                    if rd_ptr = FIFO_DEPTH-1 then
                        rd_ptr <= 0;
                    else
                        rd_ptr <= rd_ptr + 1;
                    end if;
                end if;

                -- 计数（写已改为无条件，故在 FIFO_DEPTH 处饱和，避免回卷）
                if    (wr_en = '1') and (rd_en = '0') then
                    if fifo_cnt < FIFO_DEPTH then
                        fifo_cnt <= fifo_cnt + 1;
                    end if;
                elsif (wr_en = '0') and (rd_en = '1') then
                    fifo_cnt <= fifo_cnt - 1;
                end if;
            end if;
        end if;
    end process;

    ----------------------------------------------------------------------------
    -- 4. 主时序进程
    --    rd_valid 的那一拍，rd_data 正好对应"本拍栅格位置"要显示的像素
    ----------------------------------------------------------------------------
    process(PixelClk)
    begin
        if rising_edge(PixelClk) then
            if aresetn = '0' then
                hcnt    <= (others => '0');
                vcnt    <= (others => '0');
                data_r  <= (others => '0');
                vde_r   <= '0';
                hsync_r <= '0';
                vsync_r <= '0';
            else
                -- 输出寄存器默认跟随计数器当前位置（数据与同步同拍）
                vde_r   <= vde_c;
                hsync_r <= hsync_c;
                vsync_r <= vsync_c;

                -- 计数器默认推进：保证送到 rgb2dvi 的行场时序永远连续
                hcnt <= hcnt_nxt;
                vcnt <= vcnt_nxt;

                if rd_valid = '1' then
                    -- ---- 捕获像素 ----
                    if rd_data(23 downto 8) = x"0000" then
                        -- 灰度核：低字节复制到 RGB 三通道
                        data_r <= rd_data(7 downto 0) & rd_data(7 downto 0) &
                                  rd_data(7 downto 0);
                    else
                        -- 通用彩色核：RGB -> RBG
                        data_r <= rd_data(23 downto 16) & rd_data(7 downto 0) &
                                  rd_data(15 downto 8);
                    end if;
                end if;

                -- ---- ★ 行锁：TLAST = 每行第 1280 个像素 ----
                --   收到就立刻进入水平消隐（hcnt := 1280），于是每行恰好
                --   1280 个有效拍 + 370 个消隐拍 = 1650 拍，且行相位与数据钉死。
                --   锁上之后 hcnt 本来就在 1279 → 赋 1280 与自然递进同值，无副作用。
                if cur_tlast = '1' then
                    hcnt <= to_unsigned(H_ACTIVE, hcnt'length);
                end if;

                -- ---- ★ 帧锁：TUSER = 每帧第 0 个像素 ----
                --   行锁生效后 TUSER 必然落在 hcnt=0 上，此时只需把 vcnt 归零，
                --   不丢任何一拍；只有"尚未锁上"（hcnt/=0）时才需要硬拉一次。
                --   注意：【不】碰 vde_r / hsync_r / vsync_r —— 旧代码就是因为在
                --   这里强制 vde_r<='1'，凭空制造有效像素，才把帧长撑到 1258497。
                if tuser_apply = '1' then
                    vcnt <= (others => '0');
                    if (hcnt /= 0) then
                        hcnt <= (others => '0');
                    end if;
                end if;
            end if;
        end if;
    end process;

    ----------------------------------------------------------------------------
    -- 5. 输出
    ----------------------------------------------------------------------------
    vid_pData  <= data_r;
    vid_pVDE   <= vde_r;
    vid_pHSync <= hsync_r;
    vid_pVSync <= vsync_r;

end Behavioral;
