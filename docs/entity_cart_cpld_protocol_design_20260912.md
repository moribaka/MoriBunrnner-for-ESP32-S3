# 实体卡连续读协议与 CPLD 设计

## 审查结论

现有 `F:/dev/esp32/Bacon/AGM/rtl/top.v` 是逐字节总线控制器：SPI 的 CS0/CS1
组合直接决定地址、数据和读写控制，`batch_size` 只有 1/2/3/4 字节。它没有
命令 FIFO、长度寄存器或连续读状态，因此不能仅修改 Retro-Go 就获得新协议。

## 新通道边界

新增实体卡流状态机必须独立于 Bacon 编程状态机：

- `CART_OPEN`：锁定卡槽、电压和 mapper；
- `CART_ROM_BANK`：发送一次 MBC bank 选择；
- `CART_READ`：接收 offset/length，进入连续读；
- `CART_READ_DATA`：从返回 FIFO 连续移出有效数据；
- `CART_CLOSE`：停止流并释放卡槽。

Bacon 原有 CS0/CS1 逐字节路径、40 MHz 设置和写入时序保持不变。新通道使用
独立 opcode/状态寄存器，禁止在一笔事务中自动切换旧路径。

## CPLD 模块拆分

计划新增 `entity_cart_stream.v`，由以下部分组成：

1. SPI 命令接收器：识别 magic、版本、opcode、request_id、bank、offset、length；
2. bank 控制器：按 MBC3/MBC5 生成卡带寄存器写序列；
3. 连续读引擎：固定地址窗口，驱动 `nRD`，每个卡带字节写入返回 FIFO；
4. 返回 FIFO：至少 2 KiB，允许 ESP32 连续 DMA 读取；
5. CRC/状态返回：帧尾给出 request_id、长度、CRC32 和错误码；
6. 仲裁器：实体卡流会话和 Bacon 烧录会话互斥，互相不改变对方寄存器。

## 当前状态

ESP32 侧 `shared/entity_cart_protocol.h` 已固定 ABI。审查发现板载 BSC1 CPLD
扩展已经原生支持 `GB_READ`，因此不需要另刷一套 CPLD bitstream；Retro-Go 已
改用现有 BSC1 连续读通道，保留旧 Bacon 作为 MBC 写入和兼容路径。

Retro-Go 提交 `d96b798`，产物 SHA256：
`50B862429580ED0CB9A0BFB1CE6CFC06C71702E3830181809178A436526A7481`。
已刷入 COM26 的 Retro-Go 分区 `0xAE0000`，esptool 报告 `Hash of data verified`。
CPLD 未重刷，因为当前 BSC1 bitstream 已包含 GB_READ 状态机；仍需插卡完成实机
bank/帧率验证。

## 验收门槛

- RTL 仿真覆盖 MBC3/MBC5 bank、FIFO 满/空、任意长度和 CS 边界；
- 完整 Quartus + Supra flow，确认 bitstream 与 batch 输入匹配；
- ESP32 端连续读与旧路径逐字节 hash 一致；
- 真实游戏启动后记录 bank miss、帧时间和有效吞吐；
- Bacon GBA/GBC 烧录回归保持通过。

## 刷写后观察

COM26 已确认仍运行 `retro-core` 分区，串口 debug 输出稳定在约 109-110 FPS
（当前输出未标明实体卡游戏场景，因此这只是运行态证据，不等同于实体卡验收）。
下一步需要在设备上实际进入 `__physical_cart__.gb`，记录 `gb-cart` 的 bank
读取和 CRC 结果，再与旧路径的帧率比较。

## 崩溃修复尝试

实体卡入口崩溃后，Retro-Go BSC1 进入流程与 MoriBurnner 已验证驱动对比发现少了
`MORI2LEG`、旧路径释放字节 `0x0F`、再进入 `MORI2CPL` 的复位序列。Retro-Go
已补齐该序列并重新刷写，提交为 `5d45b49`，固件 SHA256：
`FDE5461C55E56DEFE8204AA2E0AD1529C0D668D55376F2ACFEFD18228D1E07E2`。
刷写地址仍为 `0xAE0000`，esptool 已报告 `Hash of data verified`。

## 载入卡住修复

实机反馈显示 BSC1 失败后仍在启动阶段等待，实体卡界面卡在载入。因此将
`CART_USE_BSC1` 暂设为 0，保留 BSC1 实现但恢复旧 GB 读取作为默认启动路径，
避免未确认 CPLD bitstream 时阻塞游戏启动。新固件已刷写并校验：

- Retro-Go 提交：`d673ece`
- SHA256：`580C095EDB9F6283444992BE9841ED586463CDC47CF5AAB075A3D5C4DA125849`

随后实机仍卡在载入，故将 Retro-Go 实体卡后端完整恢复到原始已知版本
`c6cb61e`，撤掉本轮 BSC1 接入代码后重新构建刷写。恢复提交为 `17c4223`，
固件 SHA256：`463EC79D202AC38231EF5EB464DDF54EAA6587EE59E6F2AD09CEA55E0A6C65AC`，
刷写校验通过。新协议实验暂不留在运行固件中。

## 旧路径缓存优化

恢复原始后端后抓到的实机日志显示 FPS 约 38，且 bank 1/35/58/127 高频重复加载。
原因是 Gnuboy 为模拟两个物理 ROM 窗口而回收所有 switch bank，导致每次切换都重新
搬运 16 KiB。提交 `eb8039c` 改为保留已加载 bank 的 PSRAM 指针，命中时直接复用，
不改变卡带协议或 MBC 写入。重新刷写后的实机串口稳定在 60-61 FPS，BUSY 约 34%。
固件 SHA256：`D25ABBAA199D8E3778685CE1D3427D791D49EE0035C18DDC10B2660EBB6397A1`。

只读核对显示当前仓库 batch 为 `94EFE7052DF14813D0829E175BA520F8EE8B5C2B2C6D5A68E17C2604A43B3907`，
文档记录的 CPLD payload hash 为
`0058F32B2604F7F76431B326075BC852ECFD8AFF85574AB5A43EA997061436D7`。
这证明仓库里有已路由的 BSC1 工件，但还不能证明当前板上实际烧的是同一 payload；
需要回到 MoriBurnner 分区执行 `cpld-info` 才能完成板上身份核对。
