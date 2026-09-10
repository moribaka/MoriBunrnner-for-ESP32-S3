# 原生文件启动任务的内存竞争修复

## 修改

对应 `ui_file_start_memory_diagnosis_20260910.md` 的实机故障：
GBA 文件选择创建 24 KiB 内部栈，随即烧录再申请 16 KiB 连续内部栈，
当时最大连续块仅 15360 字节，报 `create start task failed`。

现在由 `ui_file_worker` 复用一个静态分配的 16 KiB 内部栈，执行 ROM 预览
和文件启动。文件选择不再创建扫描任务，启动烧录也不再申请任务栈。
内部栈保留 TF/SPIFFS/cache 调用要求，没有改用 PSRAM 栈或缩栈试错。

- 模型锁保护最新扫描请求、选择代次及单个启动请求。
- 再次选 ROM 替换待扫描文件；旧扫描在每个 32 KiB 块前检查取消并正常关闭文件。
- 启动请求优先，取消预览，交给现有烧录/校验/补丁导出后端。
- 只有当前代次且成功完成的扫描可以更新补丁选项；取消或读取失败不冒充“不需要补丁”。
- 启动活动标记由准备/执行流程管理，旧任务终态和状态文案不能清除新请求的活动标记。
- 串口 `ui` 增加预览代次、活动/完成/可用状态、工作任务栈余量及息屏状态，便于实测。

没有修改 Bacon、CPLD、AG32 固件、SPI 频率或卡带电源协议。

## 验证

ESP32-S3 COM26，MAC `a4:cb:8f:f2:c4:c0`。当前 HTTP 为 `192.168.1.134`。
SPI configured/actual 始终 40000000 Hz。

- ESP-IDF 构建通过；双系统的全部 10 个烧录区段通过容量检查。
- 编译运行 `tests/patch_host/test_patch.c` 通过，包括块边界标识、取消后重新扫描、
  文件错误，以及既有 SRAM/WAITCNT/免电补丁生成与导出验证。
- ESP app 已通过 esptool 写入 `0x20000`，收到 `Hash of data verified`。

机身路径回归使用 `tests/ui_device/test_file_worker.py`，串口原始记录为
`ui_file_worker_20260910.serial.log`：

| 场景 | 实测结果 |
| --- | --- |
| 选 128 MiB ROM 后立即校验，连续 3 次 | 启动成功；交接 50 / 83 / 79 ms |
| 烧录器内等待息屏，唤醒后选 ROM 立即校验 | 启动成功；交接 53 ms |
| 扫描期间反复换文件 | 旧代次取消，新代次完成；没有累积扫描任务 |
| Kirby 无目标预览、黄金太阳有目标预览 | 分别得到 available=false / true |
| 真实 32 MiB 原生 UI 烧录 | 擦除 256 个 128 KiB 扇区，写入全部 33554432 字节 |
| 写后全部 32 MiB 校验 | done，33554432/33554432，最后一字节匹配 |
| 原生 UI SRAM + WAITCNT + 免电补丁导出 | done，9699328 字节，与既有已验证导出逐字节哈希一致 |

交接时间由主机发送按键到收到 `LCD start file action` 日志计时，含串口/UI调度，
不是 SPI 访问延迟。最初无有效卡带时的校验仍报告识卡错误；这些只算启动验证，
不算完整校验成功。用户确认插卡后，下述完整读写及校验通过。

新共享工作任务最低栈余量为 10464 字节；连续启动回到空闲时内部空闲为
82507 字节、最大连续块为 31744 字节（该次启动周期的测量）。

## 卡带数据与实测耗时

卡带 ID `89 00 7E 22 28 22 01 22`，CFI AMD 128 MiB，128 KiB 扇区，1024 字节缓冲。
先以原 Bacon 读取前 32 MiB 到 `/sdcard/ROM_OUTPUT/ui_worker_before_20260910.gba`，
再以 CPLD 连续协议完整比较备份，通过后才擦写。原生 UI 写回相同备份，原数据保留。

| 项目 | 设备时间 |
| --- | ---: |
| 原 Bacon 读卡并写 TF 备份 | 113.282 s（包含 TF 写文件） |
| 写前 CPLD 全量比较 | 23.219 s |
| 原生 UI PSRAM 烧录任务，含擦除 | 59.405 s |
| 其中 NOR 擦除 / 编程 | 38.540 s / 19.924 s |
| 写后全量校验 | 23.455 s |

原始结构化结果：`ui_file_worker_read32_20260910.jsonl`、
`ui_file_worker_verify32_20260910.jsonl`、`ui_file_worker_native_burn32_20260910.json`、
`ui_file_worker_after_write32_20260910.jsonl`，相应串口记录一并保留。
擦除/TF/编程可能重叠，不能将分项机械相加当成端到端时间。

原 Bacon 的 256 次 MCU 暂停读取及 CPLD 的 8×64 KiB MCU 暂停比较也通过，MCU 已恢复。
另一次 HTTP `/api/cart/id` 请求发生超时，之后观察到新启动周期，未取得其原因证据；
不将它记作识卡通过。上述备份、原生 UI 烧录、全量校验和导出在之后的周期完成。

补丁导出：`黄金太阳1 开启的封印.patched-4.gba` 与此前的 `.patched-3.gba`
均为 9699328 字节，SHA256 均为
`B2471C930BFAE1A1228D85B5CECEFE6E98C8C9000D5D3272CC6EF2BE38875BF6`。
结果见 `ui_file_worker_export_20260910.json`。没有覆盖原 ROM 或原导出。

结束时恢复 `ag32_link=auto`、`pipeline_erase=smart`、100 ms 上电等待；
UI 补丁选择恢复关闭，回到主菜单，无后台烧录或扫描。

## 固件交付

包编号 `v2.32.106`，固件内版本字符串 `v2.32-106-g981613b-dirty`。
`release/MoriBurnner_v2.32.106.zip` 仅含两个 BIN：

- `ESP32_FULL_v2.32.106.bin`：16777216 字节，完整 16 MiB 双系统镜像。
- `AG32_BATCH_v2.32.106.bin`：109692 字节，沿用已验证 MCU+CPLD batch。

完整镜像的所有 10 个区段均与实际构建/固定 Retro-Go 工件一致；
ZIP 解压后的两个条目也与来源 SHA256 一致。

| 工件 | SHA256 |
| --- | --- |
| 已刷 ESP app | 7CB92852870693B8978ED4D8432A01F77D1B769DFCE24FB4791048FE156CEF6D |
| ESP 完整镜像 | 19F34C46F9A817921C70C93EB7865926E3C97C47CE5C2C00A4C8C711DF8C6697 |
| AG32 batch | 94EFE7052DF14813D0829E175BA520F8EE8B5C2B2C6D5A68E17C2604A43B3907 |
| 单 ZIP | B30DF52B36DB32899AFAB45E8E778DFD02EC3787C6B07ECBF89A4874118C9976 |

重复执行机身测试前，先读取 `ui` 与 `status` 确认当前页面、卡带和任务状态。
`burn` 用例要求显式的 `--write-rom`、`--url` 及预先验证过的卡带备份，
不能将纯启动用例中的“不匹配”当成真实烧录校验通过。
