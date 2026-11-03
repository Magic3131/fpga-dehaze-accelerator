# PS 端软件基准 —— 怎么跑

目的：在 PYNQ-Z2 的 ARM Cortex-A9 上跑**同一算法的软件实现**，量出每帧耗时，
和 PL 端（FPGA 硬件）做公平对比。

---

## 一、为什么用 `ps_sw_dehaze.cpp`，不用 `01_源码/`

| | `ps_sw_dehaze.cpp` | `01_源码/dehaze.cpp` |
|---|---|---|
| 依赖 | 只要标准 C++11 | 需要 `hls::stream` / `ap_uint` / `ap_axi_sdata.h` |
| 板子上能否直接编 | ✅ `g++ -O2` 就行 | ❌ 要带 Vitis HLS 头文件 |
| 结果偏快还是偏慢 | 正常（就是软件实现的真实速度） | **偏慢**（`ap_uint` 位精确运算 + stream 模拟开销） |
| 用途 | ✅ 做基线 | 只适合给 HLS 用 |

用 `01_源码` 跑会人为拉低软件成绩，加速比虚高，评委容易挑。**所以用 `ps_sw_dehaze.cpp`。**

它已经跟 RTL 的定点规则逐位对齐（倒数表 `55552/i`、`65280/i`、算术右移 8、clamp 等）。

---

## 二、两种跑法（任选）

### 方式 A：直接用编好的 ARM 程序（最省事）

`ps_sw_dehaze_arm` 已经用 `arm-linux-gnueabihf-g++ -O2 -static` 编好，
**全静态链接，不依赖板子上的库版本**，拷过去就能跑。

```bash
# 在板子上
chmod +x ps_sw_dehaze_arm
./ps_sw_dehaze_arm 1 30
```

### 方式 B：在板子上现编

```bash
g++ -O2 -std=c++11 ps_sw_dehaze.cpp -o ps_sw_dehaze
./ps_sw_dehaze 1 30
```

> 如果板子上没有 `g++`：`sudo apt install -y g++`（需要联网），或者直接用方式 A。

### 文件怎么传上去

- SD 卡：拷到 `/home/xilinx/` 下
- 或者 `scp ps_sw_dehaze_arm xilinx@<板子IP>:/home/xilinx/`

---

## 三、参数说明

```
./ps_sw_dehaze_arm <mode> <frames>
```

| mode | 含义 | 什么时候用 |
|---|---|---|
| 0 | 旁路（只拷贝） | 测内存带宽用 |
| **1** | **只去雾** | **★ 和我们的硬件配置对应**（`ctrl_reg=0x49`，sobel 关） |
| 2 | 只 Sobel | 单独测 Sobel |
| 3 | 去雾 + Sobel | 测 IP 的完整能力（sobel 也开） |
| 7 | 去雾 + 叠加 | — |

`frames` 建议 **30**（多帧取平均，压掉缓存冷启动的影响）。

---

## 四、建议跑三组，都记下来

```bash
./ps_sw_dehaze_arm 1 30    # 主对比：只去雾
./ps_sw_dehaze_arm 3 30    # 去雾 + Sobel（IP 的完整功能）
./ps_sw_dehaze_arm 0 30    # 纯拷贝，用来看内存下限
```

顺带记一下 CPU 实际主频：

```bash
cat /proc/cpuinfo | grep -i "cpu MHz\|BogoMIPS"
```

> 主频要写进报告。Zynq-7020 的 A9 是 666.67 MHz（有时会跑在 650），
> 两个值算出来的每像素周期数略有差别。

---

## 五、结果怎么读

程序会打印：

```
per frame       : XXX.XXX ms  (YY.Y fps)
checksum        : 0x................
RTL baseline    : 928290 cycles @ 74.25 MHz = 12.502 ms/frame (80.0 fps)
speedup (RTL/SW): Z.Zx
```

- `per frame` = 软件每帧耗时，**这就是要写进报告的数**
- `speedup` = 12.502 ÷ 软件耗时
- `checksum` 只要每次跑都一样，说明计算是确定性的（没被优化掉）

> 注意：`speedup` 那行用的是 **12.502 ms**，这是 PL 端的**推算值**。
> 等 PL 侧实测读出来（`mrd 0x43C20000`），把这里换成实测值再算一遍。

---

## 六、报告里必须写清楚的一句

> 软件基准为**与 RTL 定点规则逐位一致的标量 C++ 实现**，
> 在 Cortex-A9 @ ___ MHz 单核、`-O2` 下测得；
> **未做 NEON 向量化、未做多线程、未做手写汇编**。

不写这句，评委问"你有没有优化过软件版"就答不上来。
