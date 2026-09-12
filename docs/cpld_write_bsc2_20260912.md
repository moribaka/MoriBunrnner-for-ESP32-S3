# BSC2 烧录协议试验

用户澄清目标是烧录界面约 1.7M 提速，而不是读取吞吐。本阶段以实际编程时间、
任务总时间和写后独立全量校验为判据。前一轮读取 3.3 MB/s 不作为烧录成绩。

基线 ESP v2.32.131，应用 SHA256
110EC67A5B6A98692B2B2B7A39550D5C9FC0541216B5404FEBC3221ADB46573E；
AG32 batch SHA256 94EFE7052DF14813D0829E175BA520F8EE8B5C2B2C6D5A68E17C2604A43B3907。
Git 分支 `codex/cpld-write-bsc2-20260912`，从 `c870f15` 开始。
设备 COM26，当前 GBA 卡 MT28EW01GABA；首次状态无任务。

## 协议契约

- 原 Bacon 与 BSC1 保留。SPI mode0/40MHz、系统150MHz、原读写脉冲不改。
- BSC1 未配置状态的 flags bit7 宣告 BSC2 能力；旧驱动忽略该保留位。
- 新描述符 magic 为 BSC2，其余结构和 CRC32 与 BSC1 相同，仅允许 GBA
  AMD 缓冲编程 op3；非法操作在任何 WR 前拒绝。
- 写入包仍是页内数据、4字节对齐、CRC32。每个 CS 事务提交一包。
- 在 CPLD 模式下，CS0 低时状态序列器工作；BSC1 写入时原本未定义且主机
  不读取的 MISO 位现在也输出状态。原 Bacon、BSC1 已定义的描述符/数据、
  独立状态查询和读包格式保持不变。BSC2 在 MOSI 写入时使用该冻结状态。
  前8字节即包含 dummy、BA CE 01、flags，最短2字节载荷的包也能容纳。
- 数据包返回的 bit7 表示**另一缓冲槽在快照时空闲**，可授权发送下一包。
  该槽只有主机能填入，消费端只能释放，因此过时的正许可仍然保守有效。
  没许可时必须查询 RX_READY；绝不猜测 Flash 编程一定已完成。
- 许可不是已写入确认。每页 CRC 通过后才执行，ERROR 锁存，DONE 核对总量。
  发送过程中晚到的 CRC/编程错误仍由后续状态或最终 DONE 等待报告；不重试。
- ESP 使用两组 TX DMA 缓冲，在发送当前页时组装和校验下一页。
- 实验开关默认关闭、仅 RAM 中保存；读取、GBC 和原烧录配方不启用此路径。

## 验证进度

### 功能与构建

代码首版 `1aade1b`；精简选通与串口实际任务入口 `78a97a9`。
状态选通第一版把协商位加入 SPI 异步复位网络，实际 seed42 路由失败，
18 个冲突未解决，没有打包/刷入。第二版从该异步控制网络移除协商位，
重新执行 prelogic、MCU、af_quartus.tcl（内含 af_ip.tcl）、Supra 和 buildbatch。

本机原 PIO Python 中无 platformio 模块，使用独立 `.tmp-bsc2/pio-env`
安装 PlatformIO 6.1.19，调用 `python -m platformio`，不改系统 Python。
Quartus 初次因公开 QSF 未含本机 alta_sim.v 路径失败；构建时补齐本机原语库，
保存实际 QSF 后恢复仓库原文件。一次追加行缺换行导致解析失败，已纠正，
完整成功日志为 `quartus-v2.log`，不以失败阶段算构建完成。

第二版 Icarus 40/150 MHz、14 ns 假设回程预算通过：原 GBA/GB 读取、原始写、
AMD 编程、CRC/短包/超长包/取消/NOR超时；另覆盖 BSC2 信用发送、满队列
拒绝、非法操作拒绝、最短2字节载荷（8字节线包）。主机测试编译实际 ESP
writer，验证 CRC/描述符、首尾页、2字节页、DMA TX 缓冲不可被预组包覆盖、
许可不足回到 READY、启动/完成错误、状态损坏、取消与资源清理。

完整构建参数：MODE QUARTUS，FLOW ALL，seed42，FITTING timing_more，
FITTER hybrid，EFFORT highest，HOLDX default，SKEW basic，QUARTUS_SDC true。
第二版路由成功，打包前后 MCU 和 CPLD SHA256 一致。

| 实际资源 | 数值 |
| --- | --- |
| 打包逻辑 / 最终布局逻辑 | 1921 / 1920（容量2112） |
| LogicTILE | 127/132 |
| LUT / 寄存器 | 1918 / 936 |
| BRAM / PLL / 引脚 | 2/4，1/1，44/128 |
| 报告的全局信号 | 6/5（工具仍完成路由；不隐去该报告值） |
| 150MHz 系统 setup / hold | −0.833 ns / +0.348 ns |
| 40MHz SPI setup / hold | +15.340 ns / +0.603 ns |

仅作硬件测试候选，不是 STA 合格发布。自动 CS 时钟分析还报告
CS0→旧 Bacon `buf_miso_cs1[1]` 的 −3.235 ns hold；该旧逻辑未修改，
自动模型把独立 GPIO 片选当时钟。旧实测位流也有该类路径（最终 −2.696 ns）。
保留完整报告，依靠旧协议回归和长读检查其实际行为；没有增加 false-path
或改时钟掩盖它。用户约束覆盖率89.7%，不能把正 SPI 余量称为全接口已约束。

### 固件与备份

| 工件 | SHA256 |
| --- | --- |
| BSC2 CPLD（99944 B） | `dae0f8703563093eedf32dc7c8ca631e5c5b3211b668ccee4a60104d63b885c5` |
| MCU（8852 B，和原版相同） | `f0b69f07350f85aa05d663d9c37208b964b8d84fb2bd6bafc7fd8a56c2a79da4` |
| BSC2 AG32 batch（109692 B） | `4a72419c212b1bf683edc9d692d10f2efad296f24d3651f30b486ef590b3ab66` |
| ESP writeexp2 | `8ec28c577ae1815c602e01eb7fc53af51cc64e3f7d693150aac613d26316afd5` |
| ESP writeexp3（修正能力探测缓存隔离） | `2be04fbd95d0bff0a7b36a5966d3f52effbd715a1f830b72f9e9a00ab39dd94c` |
| 当前卡完整32MiB备份 | `d1e41d5f8004de024391be96535bbc8952fb30a7b04b6e9c5a666d6927492dec` |

卡备份是本轮重新导出到 `/sdcard/ROM_OUTPUT/bsc2_before_20260912.gba`，
并经过完整 file→cart 对照；没有假定旧日期备份仍等于现在卡内内容。
所有测试写回同一备份：GBA、CHIS、PSRAM、强制擦除、无补丁、全32MiB，
该镜像写入阶段没有跳过 FF 字节。串口命令调用原 `burner_start_task_ex`，
因此沿用真实 UI/HTTP 任务、原擦除/预取/写入/计时行为。
实验写入口要求上一次完整校验成功且路径就是本轮备份，拒绝无备份写入。

AG32 合并包经主机生产解析器测试和3记录逐字节 payload 对照：
option128B@0x81000000，CPLD99944B@0x800e7000，MCU8852B@0x80000000。
不要使用 Supra 的 dummy-MCU 中间 batch（其地址布局也不同）。
串口上传 TF 的 CRC32 和设备 `ag32-batch-check` 通过：3记录、108924B载荷。

### 真实烧录基线（原 AG32）

备份用时22.398s，写前全量校验18.927s。原 BSC1 编程21.005s，
界面平均1597421 B/s；擦除37.468s，整项任务59.178s；写后全量校验18.941s通过。
这与用户的约1.7M属于同一实际写入指标，不再引用上一轮纯读取吞吐。

ESP 分项：512流、32768页、219710次状态查询，READY等待9.016s、
组包/CRC2.908s、SPI事务7.948s、最终DONE等待0.484s，总底层20.924s。
READY时间包含硬件忙和轮询开销，不能全部叫可消除的软件等待。

原始证据在 `cpld_write_bsc2_20260912.jsonl` 和 `.serial.log`。

### 新协议上板

AG32 batch 已成功写入并全部校验：DP=0x2ba01477，device=0x40200001，
3记录，programmed=verified=total=108924，mcu_resumed=true，
recovery_required=false。前一轮曾出现的 MCU halt 超时本次未出现；
本轮没有改 SWD 实现或用重试掩盖失败。

新 CPLD 下 `cpld-check` 的 MCU暂停读取8×64KiB全部通过，205406us，
MCU正常恢复；32MiB备份对照18.931s通过后才启用 BSC2 写入。
系统最差 setup 具体为 `state.READ_FINISH → delay_count[0]`，−0.833ns；
保留硬件测试标识，长读/写后校验不替代 STA。

BSC2 首次真实烧录32MiB：编程20.749993s，平均1617081 B/s，
擦除37.489649s，任务58.947508s，物理编程33554432B，FF跳过0B。
写后独立全量校验18.983s通过。
对原固件基线的约1.2%差异不能视为稳定的提速结论，更没有达到4MB/s。

分项：32768包、1313次信用发送（约4.0%），284853次状态查询；
READY等待11.182s，准备2.871s（与DMA重叠），TX区间7.923s，
DONE等待0.488s，底层20.654s。
CPU组包被重叠后，大部分省出的时间变为硬件背压等待，状态轮询反而更多。
约96%的包起始快照中，另一缓冲仍占用，说明单纯减少 SPI 握手不能使本卡翻倍。
此处“硬件背压”包括 CPLD 卡带总线执行与 NOR 忙，不把两者未经测量地拆开。

### 同固件最终控制组

在同一 ESP writeexp3、同一新 AG32、同一镜像和写入参数下切回 BSC1：

| 协议 | 编程秒 | MB/s（十进制） | 擦除秒 | 任务秒 | 写后32MiB校验 |
| --- | ---: | ---: | ---: | ---: | --- |
| BSC1 | 20.991153 | 1.598504 | 37.534235 | 59.255048 | 18.966366s，通过 |
| BSC2 | 20.749993 | 1.617082 | 37.489649 | 58.947508 | 18.983019s，通过 |

两者都实际编程33554432B、FF跳过0。差值仅约1.16%，不宣称有显著提速，
没有达到4MB/s。此轮已经验证新线协议可用，否定了“仅靠这类 SPI 协议优化
就能让当前卡烧录翻倍”的预期。下一阶段需要测 CPLD 页执行、确认命令到
NOR ready 的实际时间和器件时序，再决定优化卡带端引擎；不能凭此把具体
芯片的绝对极限定死，也不能任意缩短 WR/RD 时序来追数字。

## 保存与回退

保留实验源码、生成与布线证据、本地 Git 和本轮 ROM 备份。
实验 AG32 单文件 ZIP：`release/moriburnner_bsc2_write_hwtest_20260912.zip`，
SHA256 `c5663056acf7ceb870cbab27101242da528a8b05aef95cc89d23c9a6dc4050ff`；
ZIP 仅含已逐字节检查的 `moriburnner_ag32_batch.bin`。
ESP 配套实验应用为 `release/moriburnner_bsc2_esp32_app_writeexp3.bin`，
只供已有正确分区设备写入0x20000，**不是整机16MiB发布镜像**。
固件内实验开关默认off；显式 `ag32-link cpld` 后可用
`cpld-write-experiment baseline|bsc2|off`，`cpld-write-profile` 读取分项。
模式仅在RAM，重启清除；不新增用户菜单或改默认烧录行为。

因为收益不足以替换日常固件，结束前关闭实验开关，将AG32回刷TF已有旧包
`/sdcard/ag32_cpld_posedge_20260910.bin`（已先通过设备3记录/108924B校验），
并将ESP恢复到原始备份v2.32.131。最终回退结果继续追加。

最终回退已完成：旧AG32包programmed=verified=total=108924，3记录，
mcu_resumed=true，recovery_required=false。旧CPLD上开启BSC2按预期返回
ESP_ERR_NOT_SUPPORTED，模式保持off，随后BSC1身份探测及32MiB校验通过
（19.822053s），证明可选能力失败未污染原协议可用性。
ESP原始v2.32.131镜像回刷通过esptool hash校验。
最后按用户要求仅通过串口按键，从机身烧录器→GBA→选择本轮备份→ROM校验，
原生进度和结果页可见，最终`burner done: verify finished`，界面留在结果页。
证据：`cpld_write_bsc2_20260912.ui.log` 和本目录 `ui-final.json`。
当前设备使用原固件；仓库 `example/moriburnner_ag32_batch.bin`、`build/`
和release下保留的是明确标记的实验工件，不要将其混作设备当前固件。

GB/GBC硬件没有换卡测试；其旧协议功能仿真通过。音乐/UI/擦除策略/补丁
实现未改动。三次真实烧录均逐次独立全量校验通过，卡内容保持本轮备份。
失败路由候选没有上板；没有降低时钟、写入失败后自动重试或中途切换协议。

## 用户操作偏好：串口与可见界面

用户明确要求走串口控制，以便看见机身界面与操作进度，避免 Wi-Fi 后台控制。
后续优先串口 `ui` 快照和 `key` 按键操作。必须使用诊断入口时也要显示原生
任务页，不能只在后台跑任务。此轮实际烧录全部由串口启动；唯一 Wi-Fi
连接尝试因无保存配置失败，没有通过 Wi-Fi 执行烧录。

原串口实验入口缺少跳转任务页，现已补用与HTTP入口相同的
`ui_show_burn_task_status()`；不会改变烧录实现或计时口径。
此单行可见性修正构建为writeexp4，SHA256
`f3efc2abd7fac7c34031c3ae895b8d2dc48f7ad786ab63b0a2f621a6177943cb`，
保存在 `release/moriburnner_bsc2_esp32_app_writeexp4.bin`；编译通过，未额外刷入。
BSC2烧录实测用writeexp3；最终原生界面校验用已恢复的v2.32.131。

用户随后提出双线/多线方向，接线与SDK核对见
`cpld_multilane_feasibility_20260912.md`，尚未改成DIO或更改实板接线。
