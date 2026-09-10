# GBC 5V 切换故障排查

硬件是单一共用卡槽，不是两个物理卡槽。固件 v2.32.110，ESP32-S3
MAC a4:cb:8f:f2:c4:c0，AXP209，SPI 保持 40 MHz。此次排查没有修改固件。

## USB 供电、空槽实测

用户确认拔卡后，先保存 5V 设置，再通过 `/api/cart/id?mode=mbc5&recipe_mode=chis`
调用实际供电与 ID 探测。首次成功执行 5V 供电命令，之后执行 10 轮 3.3V/5V 交替探测。
共 11 次 5V 上电，没有串口中断、ESP32 重启、panic、brownout 或看门狗记录。
20 次交替探测期间 uptime 从 1001956 ms 连续增加至 1012806 ms。

空卡槽返回 `500 cart id read failed`，串口 ID 为 00 00 00 00；这是预期的无卡结果，
不是死机。之后 HTTP 电源状态继续响应。USB 与电池均连接，电池约 4.166V。
轮次结束采样 VBUS 4.600–4.882V、IPSOUT 4.723–4.767V。
这些是 AXP209 的离散 ADC 样本，不是示波器波形，不能据此排除瞬态浪涌或解释历史关机。

特别注意：现有探测函数结束时会恢复 3.3V，所以上述试验覆盖短时间 5V 上电与返回，
不等于持续带载 5V 测试；探测后的 PMIC 样本也不能标成“5V 持续负载电压”。
没有卡槽电压 ADC，本记录的 5V 表示发出并执行控制命令，不是万用表确认的卡槽实测电压。

证据：`gbc_5v_empty_20260910.serial.log`、`gbc_5v_empty_20260910.jsonl`、
`gbc_5v_cycles_20260910.jsonl`。USB 测试后配置恢复 `gbc_voltage=3v3`。

## 源码发现

- 菜单和 HTTP 设置都只更新并保存 `s_mbc5_power_5v_enabled`，设置动作本身不切换供电。
- `burner_bacon_mbc5_prepare_power` 先关两路，等待 100 ms，再开启选定电源并等待 100 ms。
- `example/logic/bacon.v` 对供电位有互斥解码，正常命令不会逻辑上同时使能两路。
- HTTP 探测 `ws_server_http_maintenance.c` 和机身分析 `ui_burner_tasks.inc` 均无条件调用
  `burner_bacon_restore_3v3_power`，使 5V 设置与分析结束后的实际供电状态不一致。
- 此恢复函数直接从当前电压切到 3.3V，没有像正常上电路径一样先关闭两路并等待。
  CPLD 输出互斥不能证明外部功率开关不存在瞬态重叠；需硬件拓扑及波形确认，不能凭此认定根因。
- AXP209 初始化会清 IRQ 状态，而当前遥测没有输出故障 IRQ 锁存；历史保护动作不能由当前
  `power_status`/`charge_status` 反推出。

当前原生菜单导航另发现分析后 GBC 操作数量为 10，最后一项“烧录方式”，映射中的下一项
“设置”未计入。导航测试未进入电压设置页，没有误触写入；日志为 `gbc_5v_native_20260910.serial.log`。

## 纯电池、空槽实测

用户拔掉 USB 后确认机器仍开着。HTTP 遥测确认 VBUS=0、`vbus_present=false`、
`battery_present=true`、`charge_mode=battery_only`，然后重复 10 轮 3.3V/5V 交替 ID 探测。

- 共 20 次探测，包括 10 次 5V 上电，全部 HTTP 正常返回无卡错误，并可立即读取电源状态。
- uptime 从 1293571 ms 连续增加至 1305733 ms，无关机、重启或 HTTP 失联。
- 每轮确认 VBUS 未出现，避免把 USB 接回后的测量算成纯电池试验。
- 探测后电池 4.128–4.134V，IPSOUT 4.110–4.123V，电池放电电流 128.5–157.5mA。
- 与 USB 试验一样，探测后会自动恢复 3.3V；上述电流不是 5V 卡槽负载电流或启动峰值。
- USB 已断开，本轮没有串口记录，结论依据连续 HTTP 响应和单调 uptime，不能声称捕获了复位日志。
- 结束后已保存 `gbc_voltage=3v3`，100 ms 上电等待和 40 MHz SPI 保持原值。

原始记录：`gbc_5v_battery_cycles_20260910.jsonl`。

## 结论边界

目前没有复现用户报告的关机，不能判定为 AXP209 浪涌保护，也不能宣布 5V 已正常。
现有卡是 MX29GL256E/F（3V Flash）；未确认卡上稳压拓扑前，不使用该卡做 5V 带载实验。
USB 与纯电池空槽均未复现；下一步需要确认支持 5V 的卡带负载，并记录上电瞬态与复位原因。
