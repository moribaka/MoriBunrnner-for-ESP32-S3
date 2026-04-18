# MORI Burner 业务 API 文档（已按代码同步）

基准代码：`main/ws_server.c`

## 1. 业务页面入口

- `GET /tf` -> `/sdcard/.web/main.html`
- `GET /cart` -> `/sdcard/.web/main.html`
- `GET /burner` -> `/sdcard/.web/main.html`
- `GET /settings` -> `/sdcard/.web/main.html`
- `GET /*` -> `/sdcard/.web/<asset>`（非 `/api/*`）

说明：
- `/` 与 `/sys` 是固件内置基础设置页。
- `PSRAM Bench` 页面与接口已下线（不再提供 `/psram`、`/api/psram/bench`）。
- `IP5306 INI` 配置接口已下线（不再提供 `/api/ip5306/ini`）。

## 2. ROM 写入流程（按模式独立实现）

写入采用分块流水，`mode=mbc5` 与 `mode=gba` 走独立任务链路：
- 电源与探测：
  - MBC5：上电全关 -> 5V on，探测 ID/CFI，按需擦除
  - GBA：5V off -> 3V3 on，探测 ID/CFI，按需擦除
- 写入路径：
  - `write_path=direct`：`TF -> RAM小块 -> 卡带`
    - MBC5 按 4KB 分包编程
    - GBA 按 64KB 分包编程
  - `write_path=psram`：`TF -> PSRAM(N MB流水) -> 卡带`
    - `N = psram_mb`（1..8，默认 4）
    - 以 `N MB` 为粒度循环：擦除当前窗口时并行预取 TF 到 PSRAM
    - 当前窗口擦除完成后，从 PSRAM 分包写入卡带，再进入下一个窗口
- 写入统计：
  - `/api/status` 提供卡带写入速度（平均/最低/最高已剔除前 1s 预热）
  - `write_path=psram` 时额外提供 `TF->PSRAM` 独立速度统计
  - 提供擦除耗时、写入耗时

## 3. ROM 读取流程（按模式独立实现）

读取现在固定为直写 TF：
- `mode=mbc5`: 卡带 -> RAM 小块 -> 直接写入目标 TF 文件
- `mode=gba`: 卡带 -> RAM 小块 -> 直接写入目标 TF 文件
- 不再经过 PSRAM staging
- 不再生成 ROM dump 临时碎片并做后期合并

当前已实现任务：
- ROM 校验任务：`/api/verify`
- RAM 写入任务：`/api/ram/write`
- RAM 导出任务：`/api/ram/read`
- RAM 校验任务：`/api/ram/verify`

## 4. 烧录/读取 API

## 4.1 查询状态

- 方法：`GET`
- 路径：`/api/status`
- 返回示例：

```json
{
  "state": "burning",
  "progress": 62,
  "processed": 131072,
  "total": 2097152,
  "speed_current_bps": 812345,
  "speed_avg_bps": 768901,
  "speed_min_bps": 640123,
  "speed_max_bps": 845210,
  "speed_warmup_ms": 1000,
  "tf_to_psram_speed_current_bps": 1310720,
  "tf_to_psram_speed_avg_bps": 1245184,
  "tf_to_psram_speed_min_bps": 1180000,
  "tf_to_psram_speed_max_bps": 1382000,
  "erase_time_ms": 12345,
  "write_time_ms": 6789,
  "spi_configured_hz": 40000000,
  "spi_actual_hz": 40000000,
  "cart_auto_sleep_ms": 5000,
  "cart_sleeping": false,
  "cancel_requested": false,
  "rom": "demo.gba",
  "message": "ram->cart programmed"
}
```

字段：
- `state`: `idle|receiving|burning|done|error|cancelled`
- `progress`: 百分比
- `processed`: 已处理字节
- `total`: 总字节
- `speed_current_bps`: 当前速度（B/s）
- `speed_avg_bps`: 平均速度（B/s，剔除写入开始前 `speed_warmup_ms` 的预热时间）
- `speed_min_bps`: 最低速度（B/s，剔除预热时间）
- `speed_max_bps`: 最高速度（B/s，剔除预热时间）
- `speed_warmup_ms`: 速度统计预热剔除窗口（当前固定 `1000`）
- `cancel_requested`: 是否已经收到取消请求
- `tf_to_psram_speed_current_bps`: `TF -> PSRAM` 当前速度（B/s，仅 `write_path=psram` 有效）
- `tf_to_psram_speed_avg_bps`: `TF -> PSRAM` 平均速度（B/s，仅 `write_path=psram` 有效）
- `tf_to_psram_speed_min_bps`: `TF -> PSRAM` 最低速度（B/s，仅 `write_path=psram` 有效）
- `tf_to_psram_speed_max_bps`: `TF -> PSRAM` 最高速度（B/s，仅 `write_path=psram` 有效）
- `dump_read_speed_current_bps`: direct dump 时 `卡带 -> RAM` 当前速度（B/s）
- `dump_read_speed_avg_bps`: direct dump 时 `卡带 -> RAM` 平均速度（B/s）
- `dump_read_speed_min_bps`: direct dump 时 `卡带 -> RAM` 最低速度（B/s）
- `dump_read_speed_max_bps`: direct dump 时 `卡带 -> RAM` 最高速度（B/s）
- `dump_write_speed_current_bps`: direct dump 时 `RAM -> TF` 当前速度（B/s）
- `dump_write_speed_avg_bps`: direct dump 时 `RAM -> TF` 平均速度（B/s）
- `dump_write_speed_min_bps`: direct dump 时 `RAM -> TF` 最低速度（B/s）
- `dump_write_speed_max_bps`: direct dump 时 `RAM -> TF` 最高速度（B/s）
- `erase_time_ms`: 擦除耗时（毫秒）
- `write_time_ms`: 写入耗时（毫秒）
- `dump_read_time_ms`: direct dump 时 `卡带 -> RAM` 累计耗时（毫秒）
- `dump_write_time_ms`: direct dump 时 `RAM -> TF` 累计耗时（毫秒）
- `spi_configured_hz`: 当前配置的 Bacon SPI 频率（Hz）
- `spi_actual_hz`: 驱动实际生效的 SPI 频率（Hz）
- `cart_auto_sleep_ms`: 卡带自动休眠阈值（毫秒）
- `cart_sleeping`: 卡带是否已进入自动休眠（供电关闭）
- `rom`: 当前任务文件名
- `message`: 阶段信息

## 4.2 取消当前流程

- 方法：`POST`
- 路径：`/api/cancel`
- 说明：对当前运行中的接收 / 烧录 / 导出 / 校验 / 擦除流程发出取消请求，ESP32 会在安全检查点尽快中断当前操作
- 返回示例：

```json
{"ok":true,"state":"burning","message":"cancel requested"}
```

## 4.2 上传 ROM 到 TF（浏览器本地文件 -> TF）

- 方法：`POST`
- 路径：`/api/upload?name=<file_name>[&mode=mbc5|gba]`
- 说明：
  - 上传目标目录：`/sdcard/roms`
  - `mode=gba` 且文件为奇数字节时，固件会自动补齐 1 字节
  - 上传过程状态会反映到 `/api/status`（`state=receiving`）

成功返回示例：

```json
{"ok":true,"mode":"gba","message":"upload complete","path":"/sdcard/roms/demo.gba","size":33554432}
```

## 4.3 启动 ROM 写入（从 TF 文件）

- 方法：`POST`
- 路径：`/api/write?name=<file_name>[&mode=mbc5|gba][&slot=...][&write_path=direct|psram][&psram_mb=1..8]`
- 说明：`name` 为 TF 中已有 ROM 文件（支持相对路径）
- `mode=mbc5`:
  - `slot=0..17`，与上位机 MBC5 多卡地址表一致（`0` 表示整卡）
- `mode=gba`:
  - `slot=0` 整卡
  - `slot=1` 合卡菜单
  - `slot>=2` 基址按上位机规则：`(8 + 4*(slot-2)) MB`
- `write_path`:
  - `direct`：直接 `TF -> 卡带`
  - `psram`：`TF -> PSRAM(N MB流水) -> 卡带`，按窗口循环执行“擦除+预取+写入”
- `psram_mb`（可选）:
  - 范围：`1..8`（单位 MB）
  - 默认：`4`
  - 仅 `write_path=psram` 时生效

成功返回：

```json
{"ok":true,"mode":"mbc5","write_path":"psram","psram_mb":4,"message":"burn started"}
```

## 4.4 从卡带读取 ROM 到 TF

- 方法：`POST`
- 路径：`/api/read?name=<file_name>&size=<bytes|KB|MB>[&mode=mbc5|gba][&slot=...]`
- `size` 示例：`33554432`、`32K`、`32M`、`32MB`
- `mode=mbc5`: MBC5 读取链路
- `mode=gba`: GBA 读取链路（64KB 分包 + 尾部裁剪）
- ROM 读取固定为直接写目标 TF 文件，不走碎片合并
- `read_path` 参数仅为兼容保留；即使传入也会按 direct 处理
- 导出结果：`/sdcard/ROM_OUTPUT/<file_name>`
- If target file exists, firmware auto-appends timestamp and numeric suffix to avoid overwrite
- 不再使用 ROM dump 临时碎片目录

成功返回示例：

```json
{"ok":true,"mode":"gba","read_path":"direct","message":"dump started","path":"/sdcard/ROM_OUTPUT/cart32m.gba","size":33554432}
```

## 4.5 ROM 校验

- 方法：`POST`
- 路径：`/api/verify?name=<file_name>[&mode=mbc5|gba][&slot=...]`
- 说明：
  - 使用 TF 已有文件（优先 `/sdcard/roms`，其次 `/sdcard/dumps`）
  - 启动异步校验任务，逐块对比 ROM
  - `mode=gba` 时按上位机逻辑处理：
    - 奇数字节文件补齐一字节 0x00
    - 读取范围超出 flash 尾部自动裁剪

## 4.6 RAM 写入

- 方法：`POST`
- 路径：`/api/ram/write?name=<file_name>[&slot=0..17][&ram_type=sram|fram][&ram_latency=0..255]`
- 说明：使用 TF 已有存档文件写入卡带 RAM（`ram_type=fram` 对齐上位机 FRAM 分支）

## 4.7 RAM 导出

- 方法：`POST`
- 路径：`/api/ram/read?name=<file_name>&size=<bytes|KB|MB>[&slot=0..17][&ram_type=sram|fram][&ram_latency=0..255]`
- 说明：将卡带 RAM 导出到 TF 文件

## 4.8 RAM 校验

- 方法：`POST`
- 路径：`/api/ram/verify?name=<file_name>[&slot=0..17][&ram_type=sram|fram][&ram_latency=0..255]`
- 说明：使用 TF 存档文件校验卡带 RAM
- 默认：`ram_type=sram`；FRAM 默认延时参数 `10`

## 4.9 整片擦除（MBC5 / GBA）

- 方法：`POST`
- 路径：`/api/cart/erase[?mode=mbc5|gba]`
- 说明：
  - `mode=mbc5`：触发 MBC5 整片擦除（`mission_eraseChip_mbc5`）
  - `mode=gba`：触发 GBA 整片擦除（`mission_eraseChip`，包含 S70GL02 双 die 处理）

## 4.10 读取卡带 ID（GBA/MBC5）

- 方法：`GET`
- 路径：`/api/cart/id[?mode=gba|mbc5]`
- 说明：
  - `mode=gba`：读取前上电顺序 5V off -> 3V3 on
  - `mode=mbc5`：读取前上电顺序 all off -> 5V on

返回示例：

```json
{
  "ok": true,
  "mode": "gba",
  "power": {"v5": false, "v3": true},
  "id": "01 00 7E 22 22 22 01 22",
  "chip": "S29GL256",
  "cfi_ok": true,
  "device_size": 33554432,
  "sector_size": 131072,
  "buffer_write": 256
}
```

## 4.11 调试读取卡带 ID + 内容样本

- 方法：`GET`
- 路径：`/api/cart/id_debug[?mode=gba|mbc5][&sample_addr=<uint32>][&sample_len=1..64]`
- 说明：
  - 新增独立调试接口，不影响原有 `/api/cart/id`
  - 在读取 ID / CFI 后，继续按普通 ROM 读路径读取一段内容样本，方便排查“返回值像 ROM 头”“命令模式没进去”等问题
  - 默认：`sample_addr=0`、`sample_len=32`
  - `sample_len` 最大 `64`
  - 即使内容样本读取失败，ID 主结果仍会返回；此时 `sample_ok=false`，并通过 `sample_error` 给出原因

返回示例：

```json
{
  "ok": true,
  "mode": "gba",
  "power": {"v5": false, "v3": true},
  "id": "01 00 7E 22 22 22 01 22",
  "chip": "S29GL256",
  "cmd_mode": "word",
  "cmd_data_lane": "low",
  "id_looks_like_rom_header": false,
  "cfi_ok": true,
  "device_size": 33554432,
  "sector_size": 131072,
  "buffer_write": 256,
  "sample_ok": true,
  "sample_addr": 0,
  "sample_len": 32,
  "sample_hex": "00 B5 00 48 00 47 00 00 ...",
  "sample_error": ""
}
```

## 5. TF 文件管理 API

- `GET /api/tf/list?path=<relative_path>`
- `POST /api/tf/upload?dir=<relative_dir>&name=<file_name>`
- `GET /api/tf/download?path=<relative_file_path>`
- `DELETE /api/tf/delete?path=<relative_path>`
- `POST /api/tf/rename?from=<relative_path>&to=<relative_path>`
- `POST /api/tf/mkdir?path=<relative_dir_path>`

## 6. Bacon 协议封包解析（ROM 写/读/ReadID）

本节按上位机源码整理：
- `H:\dev\esp32\Bacon\叫花培根 鸡血上位机源码\ChisFlashBurner\CartAdapter_bacon.cs`
- `H:\dev\esp32\Bacon\叫花培根 鸡血上位机源码\ChisFlashBurner\mission_gba.cs`

### 6.1 SPI 侧通道选择

- ROM/GBA 主通道使用 `spi_cs=0`（`CH347SPI_SetChipSelect(..., 0x0100, ...)`）
- 电源命令使用 `spi_cs=1`
- MBC5 的 optionByte2 流模式常用 `spi_cs=2`

在当前 ESP 侧实现中，`spi_cs=0` 对应 Bacon 逻辑 `{spi_cs1, spi_cs0}=10` 的 optionByte0 分支。

### 6.2 optionByte0 位定义

`optionByte0 = [7:6]batch_size [5]dir_a [4]dir_ad [3]cs2 [2]cs1 [1]rd [0]wr`

- `batch_size`: `0/1/2/3` 分别表示后续有效载荷是 `1/2/3/4` 字节窗口
- `dir_a`: 地址总线方向（1 表示主机侧驱动）
- `dir_ad`: AD 总线方向（1 写、0 读）
- `cs2/cs1/rd/wr`: 卡带侧控制线电平位（高低由 bit 值直接决定）

常用控制字节（上位机固定写法）：
- `0xFF` = `batch=3, ao=1, ado=1, cs2=1, cs1=1, rd=1, wr=1`（占用总线+带地址）
- `0x3B` = 建立基地址（`cs1` 拉低）
- `0xBB` = 发送 16-bit 数据窗口
- `0x3A` = `wr` 脉冲低
- `0x3F` = 释放总线
- `0x29` = 读阶段前导（`dir_ad=0, rd=0`）
- `0xAB` = 读 16-bit 数据窗口

### 6.3 ROM 写命令包构成

上位机函数：`bacon_romWrite(UInt32 wordAddr, byte[] buf)`

#### 6.3.1 通用长度

- 写 `N` 个 word（每 word=2 字节）：
- 包长 = `4 + 1 + 5*N + 1`

构成：
1. 4 字节地址头：`[opt=0xFF, addrL, addrH, addrU]`
2. 1 字节基地址建立：`0x3B`
3. 每个 word 5 字节：
   - `[0]=0xBB`
   - `[1]=dataL`
   - `[2]=dataH`
   - `[3]=0x3A`（wr 拉低）
   - `[4]=0x3B`（wr 拉高）
4. 1 字节释放：`0x3F`

#### 6.3.2 单 word 写（最常用）

固定 11 字节：

```text
[0]  0xFF
[1]  wordAddr[7:0]
[2]  wordAddr[15:8]
[3]  wordAddr[23:16]
[4]  0x3B
[5]  0xBB
[6]  data[7:0]
[7]  data[15:8]
[8]  0x3A
[9]  0x3B
[10] 0x3F
```

示例：`0x555 <- 0x00AA`

```text
FF 55 05 00 3B BB AA 00 3A 3B 3F
```

### 6.4 ROM 读命令包构成

上位机函数：`bacon_romRead(UInt32 addr_byte, ref byte[] buf)`

注意：函数入参是 byte 地址，内部先转 word 地址：`addr_word = addr_byte >> 1`

#### 6.4.1 通用长度

- 读 `N` 个 word：
- 包长 = `4 + 1 + 4*N + 1`

构成：
1. 4 字节地址头：`[opt=0xFF, addrWordL, addrWordH, addrWordU]`
2. 1 字节基地址建立：`0x3B`
3. 每个 word 4 字节：
   - `[0]=0x29`（转输入、rd 拉低）
   - `[1]=0xAB`（读 2 字节窗口）
   - `[2]=0x00`（占位，回填 dataL）
   - `[3]=0x00`（占位，回填 dataH）
4. 1 字节释放：`0x3F`

#### 6.4.2 单 word 读（最常用）

固定 10 字节：

```text
[0]  0xFF
[1]  wordAddr[7:0]
[2]  wordAddr[15:8]
[3]  wordAddr[23:16]
[4]  0x3B
[5]  0x29
[6]  0xAB
[7]  0x00  <- 回包后 dataL
[8]  0x00  <- 回包后 dataH
[9]  0x3F
```

### 6.5 ReadID 的实际事务序列

上位机函数：`bacon_romReadID(ref byte[] id)`

步骤：
1. `write(0x555, 0xAA)`
2. `write(0x2AA, 0x55)`
3. `write(0x555, 0x90)`（进入 Autoselect）
4. `read(0x00<<1)`、`read(0x01<<1)`、`read(0x0E<<1)`、`read(0x0F<<1)`
5. `write(0x000, 0xF0)`（软件复位退出）

ID 输出顺序：
- `id[0..1]` 来自 word `0x000`
- `id[2..3]` 来自 word `0x001`
- `id[4..5]` 来自 word `0x00E`
- `id[6..7]` 来自 word `0x00F`

### 6.6 与当前固件对拍要点

- 地址/数据均按小端字节顺序发送（`L -> H -> U` / `dataL -> dataH`）
- 单 word 写应是严格 11 字节，单 word 读应是严格 10 字节
- `ReadID` 是多次独立 SPI 事务，不是一个大事务
- 对拍逻辑分析仪时可直接搜索关键模板：
  - 写：`FF .. .. .. 3B BB .. .. 3A 3B 3F`
  - 读：`FF .. .. .. 3B 29 AB 00 00 3F`

### 6.7 Bacon 时钟边沿解析（上升沿/下降沿）

本节回答“Bacon 是按上升沿还是下降沿发/收包”。

#### 6.7.1 SPI 模式约定

- 上位机 CH347 配置是 `SPI Mode0`、`MSB first`（见 `utility.cs`）
- ESP32 侧 SPI 也配置为 `mode = 0`（见 `ws_server.c`）

#### 6.7.2 `bacon.v` 的边沿行为

按 `example/logic/bacon.v`（`bacon_legacy_core`）：

1. `bit_cnt` 计数在 `posedge spi_sck` 推进，在 `spi_cs` 拉高时清零  
2. MOSI 移位寄存器 `buf_mosi` 在 `posedge spi_sck` 移位采样  
3. 协议字段解析（`case {spi_cs1, spi_cs0}`）在 `posedge spi_sck` 执行  
4. `cart_*_shadow`（cs/rd/wr/dir/address/data 的 shadow）在 `posedge spi_sck` 更新  
5. 真正输出到卡带引脚的 `cart_cs1_out/cart_cs2_out/cart_nRD_out/cart_nWR_out/cart_a_oe/cart_ad_oe` 在 `negedge spi_sck` 提交  
6. MISO 由组合逻辑从 `buf_miso_cs0/cs1/cs2` 最高位选择输出；对应的 `buf_miso_*` 在 `posedge spi_sck` 装载/移位

结论：
- **收包解析主边沿是上升沿（posedge）**
- **对卡带控制线生效主边沿是下降沿（negedge）**

#### 6.7.3 单 bit 时序直观理解

在 `spi_cs0` 有效期间，一个 bit 周期可理解为：

1. 上升沿：Bacon 采样 MOSI，更新内部状态机/影子寄存器  
2. 下降沿：把影子寄存器提交到卡带控制线（RD/WR/CS/DIR/地址/数据）  
3. 下一个上升沿：继续采样下一 bit

因此协议是否成功，不仅取决于“字节值是否对”，还取决于：
- CS 维持窗口是否完整（中途抬高会清计数）
- `batch_size` 对齐是否正确
- RD/WR 脉冲是否在正确 bit 窗口内出现

### 6.8 GBA 写时序（当前 ESP 实现）

本节描述 `mode=gba` 下 ROM 编程的实际时序，分为：
- A. 单 word 编程（无 buffer-write）
- B. buffer-write 编程（有写缓冲能力）

#### 6.8.1 A. 单 word 编程时序

使用条件：
- `buffer_write_bytes < 2`

Flash 命令序列：
1. `0x555 <- 0xAA`
2. `0x2AA <- 0x55`
3. `0x555 <- 0xA0`
4. `PA <- PD`（目标地址写入数据）
5. 轮询 `PA`，直到读回 `PD`

其中每一次 `write(wordAddr, value)` 都是固定 11 字节包：

```text
FF addrL addrH addrU 3B BB dataL dataH 3A 3B 3F
```

关键控制语义：
- `BB + dataL + dataH`：把 16-bit 数据放上 AD
- `3A -> 3B`：形成一次 `WR` 低脉冲
- `3F`：释放总线

#### 6.8.2 B. buffer-write 编程时序

使用条件：
- `buffer_write_bytes >= 2`

Flash 命令序列：
1. `AA@unlock0`
2. `55@unlock1`
3. `25@PA`（进入写缓冲）
4. `WC@PA`（写入 word 数减 1）
5. 连续 `PD` 数据写入（按 word）
6. `29@PA`（确认提交）
7. 轮询最后一个 word 地址，直到读回最后一个 word 数据

说明：
- 当前实现默认 GBA 用 word 地址模式：`unlock0=0x555`、`unlock1=0x2AA`
- 每个数据 word 仍通过 `... BB dataL dataH 3A 3B ...` 子序列形成写脉冲
- 支持按 `buffer_write_bytes`、SPI 最大包长切分批次

#### 6.8.3 写入的线级边沿关系

结合 `bacon.v`：
- `posedge spi_sck`：采样 MOSI + 更新 shadow
- `negedge spi_sck`：把 shadow 提交到实际 `WR/RD/CS/DIR/ADDR/DATA`

所以“写成功”依赖两层一致性：
1. 包字节序列正确（命令语义）
2. `WR` 脉冲所在窗口正确（边沿提交语义）

### 6.9 GBA 读时序（当前 ESP 实现）

本节分成两种读路径：
- A. 命令态读（ID/CFI/轮询）：单 word 10 字节
- B. 数据流读（ROM dump）：分段流式读

#### 6.9.1 A. 命令态单 word 读（ID/CFI）

单次读包固定 10 字节：

```text
FF addrL addrH addrU 3B 29 AB 00 00 3F
```

语义：
- `29`：`dir_ad=0`（AD 设为输入）并拉低 `RD`
- `AB 00 00`：触发 16-bit 读窗口
- 回包后从字节 `[7],[8]` 提取 `dataL,dataH`
- `3F`：释放总线

该路径用于：
- `ReadID` 内的 `0x000/0x001/0x00E/0x00F` 读取
- CFI 关键字读取
- 编程后轮询（`wait_u16`）

#### 6.9.2 B. 数据流读（批量 ROM 读取）

批量读取采用“建立基地址 + 连续 read slot”模式：
1. `FF + addr_word(3B) + 3B` 建立地址窗口
2. `29` 进入读相位
3. 循环发送 `AB 00 00`（每个 slot 读 1 word）
4. 结束时发送 `3F` 释放总线

该模式用于大块 ROM 导出，减少每 word 独立事务开销。

#### 6.9.3 读的线级边沿关系

与写相同：
- `posedge spi_sck`：协议解析与移位
- `negedge spi_sck`：控制线提交

对读而言，重点是：
- `dir_ad` 必须先切到输入
- `RD` 低窗口必须覆盖数据采样窗口
- `CS` 不能在中途抬高，否则 bit 计数会重置

## 7. 业务侧常用设备 API

- `GET /api/device/info`
- `POST /api/device/restart`
- `GET /api/device/brightness`
- `POST /api/device/brightness`（JSON: `{"brightness":0..255}`）
- `GET /api/spi/config`
- `POST /api/spi/config?mhz=<20..80>`（或 `hz=<20000000..80000000>`）
- `GET /api/burn/core_config`
- `POST /api/burn/core_config?erase=auto|cpu0|cpu1&tf=auto|cpu0|cpu1&psram=auto|cpu0|cpu1`
- `GET /api/power/status`
- `POST /api/power/charge_current`（固定返回 `403`）
- `GET /api/storage/status`
- `POST /api/storage/usb_msc?enable=1|0`
- `GET /api/mcu/probe?...`

## 8. Wi-Fi / 语言 API

Wi-Fi：
- `GET /api/wifi/status`
- `GET /api/wifi/scan`
- `POST /api/wifi/connect`（JSON: `{"ssid":"...","password":"...","save":true|false}`）
- `POST /api/wifi/ap`
- `POST /api/wifi/disconnect`
- `POST /api/wifi/forget`

语言：
- `GET /api/lang`
- `GET /api/lang/list`
- `POST /api/lang/apply?ini=<file.ini>` 或 JSON `{"ini":"file.ini"}`

## 9. Web 资源和 OTA API

- `POST /api/web/main_html[?name=main.html]`
- `POST /api/web/upload?name=<file_name>`
- `POST /api/fw/upgrade`
- `GET /api/system/migrate_zip`
  - 打包导出系统迁移 ZIP（包含 `/sdcard/.setting` 与 `/sdcard/.web`）
  - 下载文件名：`mori_system_migration.zip`
- `POST /api/system/deploy_zip`
  - 请求体：ZIP 二进制流（`application/octet-stream` 或 `application/zip`）
  - 部署目标：`/sdcard/.setting` 与 `/sdcard/.web`
  - 当前限制：仅支持 `store`（不压缩）ZIP，不支持加密/deflate

## 10. USB 直通互斥

当 TF 被 USB Host 占用时，涉及 TF 访问的接口会返回 `503 Service Unavailable`。
典型受影响接口：
- 页面与静态资源：`/tf`、`/cart`、`/burner`、`/settings`、`/*`
- `/api/upload`、`/api/write`、`/api/read`、`/api/verify`
- `/api/ram/write`、`/api/ram/read`、`/api/ram/verify`
- `/api/tf/*`
- `/api/web/*`
- `/api/lang/list`、`/api/lang/apply`

## 11. 路径安全规则

- 拒绝 `..` 路径穿越
- 拒绝 FAT 非法字符
- 删除根路径被禁止
