# Retro-Go 实体卡方案试运行记录（2026-09-12）

## 当前结论

Retro-Go 仓库已经包含一套针对 MoriBurnner 的实体 GB/GBC 卡入口，位置在：

- `retro-core/main/main_gbc.c`
- `retro-core/components/gnuboy/gb_cart_backend.c`
- `retro-core/components/gnuboy/gnuboy.c`
- `launcher/main/applications.c`

启动器把 `__physical_cart__.gb` 暴露为特殊应用；Gnuboy 识别该路径后调用
`gnuboy_load_rom_cart()`，而不是从文件加载整份 ROM。

## 运行方式

1. 启动 Retro-Go，选择实体卡入口 `__physical_cart__.gb`。
2. 后端复位卡槽并读取头部，校验 Nintendo logo 和 header checksum。
3. 读取固定 bank 以及当前切换 bank；每个 16 KiB bank 写入 PSRAM 缓存。
4. Gnuboy 产生 MBC 写入时，调用 `gnuboy_cart_write_mbc()` 直接操作实体卡的
   bank 寄存器；不会在 ESP32 内伪造一个独立的 MBC 镜像。
5. 退出时关闭卡槽电源并释放 SPI、缓存和 GPIO 资源。

## 这轮试运行的范围

- 先只验证 `retro-core` 编译和现有实体卡后端的链接完整性。
- 编译通过后再在设备上刷入 Retro-Go 分区，使用已知 MBC3 卡做只读启动测试。
- 首次实机测试只确认 header、bank 0、bank 1 和连续 bank 切换；不写 SRAM、不改
  现有烧录流程、不刷 AG32。
- 实体卡后端当前固定使用纯 CPLD/BSC1 SPI 路径，未启用 AG32 MCU 实验传输。

## 2026-09-12 工具链尝试

已建立本地 Git 检查点 `a29dc05`。尝试通过 `tools/flash_retro_core.ps1` 刷写时，
当前工作区缺少可供分区刷写使用的 `partitions.bin`，且脚本调用的 Python 环境缺少
`esptool`。改用项目指定 Python 并补齐 PATH 后，`retro-core` 已重新构建成功：

- 构建时间：2026-09-12 16:31:05
- 文件大小：1,257,808 bytes
- SHA256：`274D3BB060E752E0D8AAF2A0FAEED907EAB5E077FFF95DDF5C6309285474F860`
- 应用分区检查：通过，分区大小 3 MiB，剩余 60%

本次只完成构建，没有刷写设备；实体卡方案仍未宣称实机通过。

## 设备刷写

2026-09-12 已确认 COM26 为目标 ESP32-S3（MAC `a4:cb:8f:f2:c4:c0`），将该
`retro-core.bin` 写入双系统 Retro-Go 分区 `0xAE0000`。esptool 报告：
`Hash of data verified`，写入 1,257,808 bytes，耗时 6.7 秒。重启后设备返回
`version=v2.32.119`，Retro-Go 分区正常启动；尚未插卡执行实体卡游戏验证。

## 已知限制

- 当前缓存上限为 512 个 16 KiB bank，且会预留 PSRAM 空间；大容量 MBC5 卡可能
  因缓存预算不足而拒绝启动。
- SRAM/RTC 仍通过独立读写接口，尚未完成实体卡保存数据的实机验收。
- 该后端是 Retro-Go 仓库中的既有代码，本次只做接入验证；在编译和实机回归前不
  宣称“实体卡游玩已可用”。

## 版本与回滚

MoriBurnner 现有 v2.32.119 修复已保存于本地 Git 提交；本次 Retro-Go 试运行
不修改烧录器和 AG32。若实体卡试运行失败，只回滚 Retro-Go 分区，不回滚烧录器
固件。
