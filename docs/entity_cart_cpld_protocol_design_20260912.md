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

ESP32 侧 `shared/entity_cart_protocol.h` 已固定 ABI；Retro-Go 仍使用旧 CPLD/BSC1
访问路径。CPLD 新模块尚未接入顶层、尚未完成 Quartus/Supra 路由、尚未刷写，
因此当前设备继续使用已恢复的旧版实体卡固件。

## 验收门槛

- RTL 仿真覆盖 MBC3/MBC5 bank、FIFO 满/空、任意长度和 CS 边界；
- 完整 Quartus + Supra flow，确认 bitstream 与 batch 输入匹配；
- ESP32 端连续读与旧路径逐字节 hash 一致；
- 真实游戏启动后记录 bank miss、帧时间和有效吞吐；
- Bacon GBA/GBC 烧录回归保持通过。

