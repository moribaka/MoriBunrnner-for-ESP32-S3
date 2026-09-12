# Retro-Go 实体卡游玩方案（备选）

状态：设计备忘录，尚未实现，尚未完成实机验证。

本文记录一种让 ESP32-S3 运行 Retro-Go/Gnuboy，并把 GBC/GB 实体卡作为 ROM
后端的方案。它是现有文件 ROM 模拟器和烧录流程之外的备选路径，不改变当前
烧录协议、Bacon/CPLD 路径或 AG32 固件。

## 目标与边界

- GBC/GB 游戏代码从实体卡读取，首次访问后缓存到 PSRAM。
- MBC 操作由模拟器产生，直接写到外部卡槽；不在 ESP32 中模拟一个假的 MBC。
- 默认使用纯 CPLD/BSC1 读卡路径，AG32 MCU 传输只作为以后单独评估的实验路径。
- 先支持 ROM 读取，再扩展 SRAM 和 RTC；不把读错、超时或校验失败的数据静默填成
  `00`/`FF`。
- 现有 Retro-Go 的文件 ROM后端保持不变，实体卡作为新增 cartridge backend。

## 后端模型

建议增加三种后端模式：

| 模式 | 用途 | 数据来源 |
| --- | --- | --- |
| `FILE` | 现有模拟器行为 | LittleFS/SD/网络文件 |
| `CART_CACHE` | 默认实体卡模式 | 卡带按 16 KiB bank 读取，缓存到 PSRAM |
| `CART_STREAM` | 调试/低内存模式 | 未命中的 bank 直接从卡读取 |

`CART_CACHE` 是默认方案：MBC3 的 2 MiB 地址空间可以完整放入 PSRAM；MBC5
使用按 16 KiB bank 的 LRU 缓存，避免为大容量卡预留整段内存。缓存项至少包含
mapper、bank 编号、有效长度、数据和 CRC32。缓存命中只访问 PSRAM，不重新切卡。

## 运行流程

1. `CART_OPEN` 复位卡槽、读取头部并执行 mapper 探测，返回 mapper、容量、电压和
   可用功能。
2. 模拟器启动固定区 `0x0000-0x3FFF`。后端装载 bank 0，并验证 CRC。
3. CPU 访问 `0x4000-0x7FFF` 时，后端把地址转换成逻辑 bank 请求。若 bank 在
   PSRAM，直接返回；否则先选择 bank、等待卡带稳定、连续读取 16 KiB，再放入缓存。
4. 模拟器写入 MBC 寄存器时，只更新后端的 mapper 状态并立即发送对应卡带命令。
   MBC 切换不是由 ESP32 内部 ROM 镜像模拟。
5. SRAM/RTC 后续通过独立队列实现，不能阻塞 ROM 读请求；写回必须由显式提交或
   退出游戏时触发，并带 CRC/忙状态。
6. `CART_CLOSE` 停止未完成事务，释放缓存和卡槽所有权，恢复普通文件 ROM 模式。

## MBC 规则

- MBC3：只写 `0x2000` 的 7 位 ROM bank 寄存器；bank 0 映射到 bank 1；
  当前标准实现的可寻址容量上限为 2 MiB。
- MBC5：低 ROM bank 位写 `0x2000`，高位写 `0x3000`；两次写入必须按顺序完成，
  再发起读事务。
- 后端必须以探测结果为准。用户选择的 mapper 与探测结果冲突时，在启动游戏前
  报错并停止，不得继续读卡。
- 不把 MBC3 当作 MBC5 操作；特别是不能对 MBC3 发送 MBC5 的 `0x3000` 写入，
  否则会覆盖同一 bank 寄存器并造成地址别名。

## ESP32 与卡槽协议

建议由一个独占的 cartridge service 管理 CPLD/BSC1：

```text
CART_OPEN        -> mapper/capacity/voltage/status
CART_ROM_BANK    -> mapper-specific bank select + ready
CART_READ        -> bank, offset, length, request id
CART_READ_DATA   -> request id, length, payload, CRC32, status
CART_SRAM_READ   -> reserved for SRAM/RTC extension
CART_SRAM_WRITE  -> reserved for SRAM/RTC extension
CART_CLOSE       -> release slot
```

实际总线传输应保持连续：bank 选择只发送一次，随后连续搬运整段数据，不为每个
小包重复携带地址和长度。服务层按请求 ID 配对响应，返回明确的超时、CRC 和 mapper
错误；任何错误都让模拟器停在可见错误状态，由上层决定重试或退出。

推荐错误码：

- `CART_ERR_MAPPER_MISMATCH`
- `CART_ERR_BANK_READ_TIMEOUT`
- `CART_ERR_CACHE_CRC`
- `CART_ERR_VOLTAGE_UNAVAILABLE`
- `CART_ERR_UNSUPPORTED_MAPPER`

## 延迟和预取

首次 bank miss 的延迟由卡带切换、CPLD 建立时间和 16 KiB 读取决定；这部分不能
伪装成命中。为了减少游戏运行中的停顿，后端可以在当前 bank 读取完成后预取下一个
顺序 bank，但预取必须让位于 CPU 当前读请求，且不能改变 MBC 状态。

启动游戏前可选择：

- `完整缓存`：先把可寻址 ROM 全部读入 PSRAM，启动后延迟最稳定；
- `按需缓存`：启动更快，首次访问每个新 bank 可能卡顿；
- `调试直读`：不缓存，只用于抓取时序和协议问题。

## 与现有系统的关系

- 不复用烧录任务的擦除、编程和校验流程；实体卡游玩是只读优先的独立服务。
- 不在播放过程中偷偷切换卡槽所有权。进入实体卡游戏前应停止音乐和其它占用
  卡槽的任务，退出后由用户显式恢复。
- 不自动在 CPLD、AG32、旧 Bacon 协议之间切换；协议选择在 `CART_OPEN` 时固定，
  失败就报告原因。
- 当前没有把该方案接入 UI，也没有修改 Retro-Go 源码；本文件仅作为后续实现依据。

## 实现顺序

1. 在 Retro-Go 中抽象 cartridge backend，并先实现 `FILE` 兼容适配层。
2. 在 ESP32 侧实现 `CART_OPEN`、mapper 探测和单 bank 读取，完成 CRC 验证。
3. 加入 `CART_CACHE` 的 PSRAM 分配、命中/未命中统计和顺序预取。
4. 用已知 MBC3、MBC5 ROM 做全量 hash 对比，再测试真实游戏的 bank 切换。
5. 最后扩展 SRAM/RTC 和 UI 入口；在此之前不宣称实体卡游玩可用。

