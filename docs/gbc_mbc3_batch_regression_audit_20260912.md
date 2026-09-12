# MBC3 烧录失败：AG32 工件回归核对

本次按用户要求检查 GBC 烧录，全部设备操作为只读；未重新擦写卡带或 AG32。

## 当前设备证据

- ESP32 串口版本 v2.32.119；HTTP 地址 192.168.1.134。
- UI 状态 `burner error: erase flash failed`。
- HTTP 识别 mapper=MBC3，MX29GL256E/F，ID C2 7E 22 01。
- ROM 为 2 MiB《口袋妖怪 银.gbc》，SPI configured/actual 均为 40 MHz。
- 擦除进度 2/16 个 128 KiB 扇区；write_time_ms=0，processed=0。
- API 的 probe_mode=mbc5 是现有 GB/GBC 通用路径名称；probe_mapper=MBC3
  才是此次报告的 mapper，不能仅凭 mode 字符串认定误用 MBC5。

## 上轮误刷的工件

上轮明确向设备提交并成功校验的是 `/sdcard/ag32_cpld_20260910_hwtest.bin`。
本次重新从 TF 下载两份 batch，在内存解析每个 256 字节 record header 并计算
各 payload SHA256。两份 MCU 和 option 完全相同，但 CPLD 不同：

| 工件 | batch SHA256 | CPLD SHA256 |
| --- | --- | --- |
| 上轮误刷 hwtest | 4d8b994a5c42e2e2540e6d0410277d4ee6d0d19d466c337c230cd5d74aae9e1e | 6d231bb044e4d0ff62bcc5b4086903985b18c3cd4cae164f0d769a15919309ae |
| 原先通过 MBC3 回归的 posedge | 94efe7052df14813d0829e175ba520f8ee8b5c2b2c6d5a68e17c2604a43b3907 | 0058f32b2604f7f76431b326075bc852ecfd8aff85574ab5a43ea997061436d7 |

正式文件为 `/sdcard/ag32_cpld_posedge_20260910.bin`，与仓库
`example/moriburnner_ag32_batch.bin` 哈希相同。两者都是 109692 字节、三个记录，
因此仅凭格式解析通过、尺寸相同、烧后校验成功，不能证明刷的是正确版本。

此前日志声称 hwtest 与正式 BSC1 工件一致，结论错误。本次比对证明误刷造成了
CPLD 版本回退；是否完全解释当前擦除失败，仍需恢复正式 batch 后复测才能证明。
不得用身份探测通过代替读卡完整性验证，也不得把编程校验通过当成功能回归通过。

后续恢复应使用上述正式 posedge batch，先验证 MBC3 只读数据，再根据授权重测
当前 ROM 的烧录并独立校验。失败烧录可能已经擦除了卡内部分数据。
