# AG32 更新偶发启动失败：流程审计

用户反馈：刚开始就失败。审计对象为当前已刷的
`v2.32-106-g981613b-dirty`（源码实现提交 `5a107e2`）。
本轮未改固件、未擦写 AG32 或卡带。

## 结论

启动及失败退出流程有确定的缺口，但当前没有复现用户的首发错误。
不能把内存竞争、SWD 连线或某个错误分支直接当成已确认的根因。

### 1. SWD 接管中途失败没有恢复一致状态

`main/burner/core/burner_spi_backend.c:657` 的 `burner_spi_enter_swd_mode`
先设 `s_swd_mode=true` 并删除 SPI device。若随后 `spi_bus_free` 失败，
只把 `s_mcu_spi_ready` 清零并释放锁，没有恢复模式，也没有保留总线仍分配的信息。

调用方 `main/ag32_batch_programmer.c:418` 仅在 enter 成功后设置
`spi_mode_entered=true`，因此该错误路径不会调用 leave。
此时普通 SPI 初始化会被 `s_swd_mode` 拒绝，后续退出 SWD 时还可能对仍存在的
SPI bus 再初始化。需要将接管作为完整状态转换，精确回滚或明确进入待恢复状态。
本轮没有人为制造 SPI 驱动释放失败。

### 2. 擦除前失败会留下暂停的 MCU

`main/ag32_batch_programmer.c:431` 暂停 MCU 后，还会读取 device ID 和解锁 flash。
这些步骤失败会进入 `out`；`main/ag32_batch_programmer.c:489` 只关闭 SWD 会话，
并在 `destructive_started=false` 时恢复 SPI。

`mcu_debug_session_end` 只释放 GPIO 和互斥锁，不恢复 MCU 运行。
因此这一分支可留下 MCU 暂停、SPI 已恢复、recovery_required=false 的不一致状态。
它是错误后的状态缺陷，不证明它导致最初的连接失败，也不意味着纯 CPLD 必然失效。
修正需要区分未暂停、已暂停但未修改、已开始擦写三种退出条件。

### 3. AG32 入口仍临时创建两个内部栈任务

`main/ui_ag32_update.inc:69` 创建 6144 字节的 UI 工作任务，
它又在 `main/ag32_batch_programmer.c:583` 创建 8192 字节的烧录任务。
两者在交接时可能同时存活，都依赖动态内部内存分配。

前一轮共用静态栈的修复只涉及卡带文件预览和启动，没有覆盖这里。
当前测得内部最大连续块 31744 字节，没有观察到本轮 AG32 分配失败，
因此本项是明确的设计风险而非已复现原因。
该入口也没有记录创建失败时的空闲/最大连续块或任务栈余量。

### 4. 原始错误和失败阶段被抹掉

- `flash_erase_operation` / `program_option_record` / `program_flash_record`
  的清理调用复用同一 error 缓冲。首次错误后若清理也失败，文字被覆盖，
  但函数返回值仍是首次错误，出现代码与文字不对应。
- device ID 读取失败被统一改成 `ESP_ERR_INVALID_RESPONSE` 和 unexpected ID，
  无法从终态区别 SWD 读取错误与真正芯片不匹配。
- `verify_record` 把 SWD 读取失败与内容不一致都报告为 CRC 校验失败。
- 作业结束时把 `phase` 一律改成 `failed`，原 connect/erase/program/verify 阶段丢失。

修正应保存首次错误、失败阶段、地址及底层 ACK/错误码，单独报告清理失败。
不能用再次尝试、降低频率或忽略报错替代流程修正。

## 实机与工件检查

- 板：COM26，ESP32-S3，SPI 40 MHz。
- 接入时 AG32 作业为 idle，没有可读取的上次失败报告。
- 连续 20 次 `ag32-probe` 均成功：DP `0x2ba01477`、device `0x40200001`。
  命令包含连接、halt、读取 device ID、resume 和 SPI 恢复，不擦写 flash。
- 之后 `cpld-info` 成功，block_bytes=1024。
- TF 文件 `/sdcard/ag32_cpld_posedge_20260910.bin` 校验成功，3 records、108924 字节载荷。
  从 TF 下载核对为 109692 字节，SHA256：
  `94EFE7052DF14813D0829E175BA520F8EE8B5C2B2C6D5A68E17C2604A43B3907`。
- 现有 AG32 batch parser 主机测试通过。

建议下一轮先修完整的任务/接管/退出生命周期及错误记录，再做早期失败注入、
重复启动和实机批量刷写验证。20 次连接通过不能替代烧录流程回归，
也不能排除低概率的首次连接故障。
