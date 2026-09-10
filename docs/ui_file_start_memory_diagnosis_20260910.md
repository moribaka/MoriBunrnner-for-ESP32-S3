# 选文件后启动任务失败：实机诊断

日期：2026-09-10。ESP32 COM26，版本 `v2.32-103-g8eedd43-dirty`。
本轮仅诊断，没有修改或刷写 ESP32/AG32 固件，没有擦写卡带。

## 已确认的失败原因

接入时机身保留的错误为 `启动失败: create start task failed`，
后台 `burn_running=false`。该错误发生在创建 `ui_file_start` 时，
早于文件烧录后端及卡带写入。

在烧录器内重新选择 `LK_MULTIMENU_L556.gba`（134217728 字节），
随即选择共用启动函数的“ROM: 校验”，复现相同错误。
过程中没有退出烧录器、重启设备或等待再次息屏。

| 时刻 | 内部空闲字节 | 内部最大连续块 | 结果 |
| --- | ---: | ---: | --- |
| 选文件前 | 80983 | 31744 | 尚未启动 |
| 选文件后后台扫描期间 | 39551 | 15360 | 扫描占用内部内存 |
| 创建启动任务失败时 | 39323 | 15360 | 申请 16384 字节任务栈失败 |
| 扫描结束 | 80983 | 31744 | 内存恢复 |
| 原文件再次启动校验 | 80983（启动前） | 31744（启动前） | 启动任务创建成功，进入卡带准备阶段 |

关键串口原文：

```text
W (3515198) ui: LCD file action task create failed: ret=-1 stack=16384 internal_free=39323 internal_largest=15360 psram_free=7521964 psram_largest=7208960
W (3515202) ui: LCD file action start failed (校验ROM): create start task failed
I (3558035) ui: LCD start file action: action=校验ROM path=LK_MULTIMENU_L556.gba mode=gba write_path=direct
I (3558049) ui: LCD file action task stack free min=11548 bytes
```

源码对应关系：

- `main/burner/ui/ui_burner_tasks.inc` 中 `ui_select_file_for_burner_locked`
  为每次 GBA 文件选择创建 `ui_patch_probe`，使用内部内存任务栈。
- `main/ui.c` 定义该栈为 `UI_BURN_PROBE_TASK_STACK_SIZE`，大小 24 KiB；
  文件启动栈 `UI_FILE_START_TASK_STACK_SIZE` 为 16 KiB。
- `ui_patch_analysis_task` 调用 `burner_gba_rom_has_sram_patch_target`。
  该函数可能扫描整个 ROM，直到找到补丁标识或到达文件末尾。
- `ui_start_file_action_async` 在扫描仍运行时可以创建第二个内部栈任务，
  没有与 `s_gba_patch_analysis_active` 协调。PSRAM 空闲不能满足内部栈分配。
- 再次选文件会把扫描活动标记清零再启动新任务；旧扫描没有取消、代次校验或等待机制。
  这是源码上的另一处任务生命周期缺口，本轮未做多扫描并发实测。

息屏本身只调整背光；卡带另有独立的 60 秒空闲断电机制。
本轮已证实上述内存竞争足以造成用户设备保留的启动错误，
没有证明息屏造成了内存泄漏，也不能把退回重进当成根治。

建议修正任务生命周期和内存预算：扫描与启动由明确的作业状态协调，
处理旧扫描结束及结果归属，减少扫描栈的不必要占用；
内部栈大小需结合实际最坏调用路径验证，不能直接改成 PSRAM 栈或盲目缩栈。
不需要降频、切协议或失败后重试。

## 只读校验的后续识卡异常

启动任务成功不代表校验成功。本轮后续卡带准备仍失败：

```text
GBA CFI signature mismatch: [010]=0010 [011]=0011 [012]=0012
GBA ROM larger than flash: rom=134217728 flash=33554432
burn_task result: err=ESP_ERR_INVALID_SIZE state=error processed=0/134217728 msg=gba cart prepare failed
```

随后执行“重新分析”，同样读到 `00 00 01 00 0E 00 0F 00`，
未取得有效 CFI，被现有代码归类为只读 ROM。
因此不能将该后续异常解释成启动任务仍未恢复，也没有证据确定其物理原因。
当前板上插卡情况未核实；没有更改协议/频率来规避该现象。
最终无任务运行，界面停在烧录器的 ROM 选择行。SPI 始终 40 MHz。
