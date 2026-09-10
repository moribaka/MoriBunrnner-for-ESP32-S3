# ESP32 完整刷机包与 AG32 刷机说明

本套件来自 MoriBurnner 提交 f34383c。ESP32 主程序和五个 Retro-Go 应用均重新构建；
AG32 使用已经完成实机读写校验的 BSC1 CPLD batch，SHA256 为
94EFE7052DF14813D0829E175BA520F8EE8B5C2B2C6D5A68E17C2604A43B3907。
源码工作区的 dirty 标记来自保留的用户文档批注。

## 1. 刷 ESP32

解压 ESP32 完整包，使用数据线连接电脑。
双击 `flash_full.cmd`，输入实际串口号，例如 `COM26`；
也可以在解压目录的终端执行：

```bat
flash_full.cmd COM26
```

包内附带 Espressif 官方 Windows x64 esptool 5.4.0，无需先安装 Python。
脚本会写入主程序、网页资源和全部 Retro-Go 固件分区。
看到 hash 校验成功和 `Flash completed successfully` 后，ESP32 自动重启。

如果使用图形刷机工具，选择 ESP32-S3，将 `FULL.bin` 烧到 **0x0**，
参数为 **DIO / 80 MHz / 16 MB**。
也可执行 `flash_merged.cmd COM26` 写入该完整镜像。
`FULL.bin`、`fullflash-single.bin` 和长文件名 merged.bin 内容相同，选一个即可。

`flash_full.cmd` 写全部固件分区，保留未列出的 NVS 配置区域；
合并镜像写满整个 16 MB，会覆盖 ESP32 片内配置区域。
TF 卡上的 ROM 和文件不包含在 ESP32 片内镜像中。
完整镜像用于串口刷写；网页 ESP32 OTA 升级选择的是单独 `moriburnner.bin`。

| 分区/内容 | 地址 |
| --- | --- |
| Bootloader | 0x000000 |
| 分区表 | 0x008000 |
| OTA 初始数据 | 0x00F000 |
| MoriBurnner 主程序 | 0x020000 |
| 网页等资源 | 0x820000 |
| Retro-Go launcher | 0x9A0000 |
| retro-core | 0xAE0000 |
| gwenesis | 0xC20000 |
| prboom-go | 0xD60000 |
| fmsx | 0xEA0000 |

## 2. 在 ESP32 网页 UI 中刷 AG32

**网页 UI 已有入口；机身屏幕菜单目前没有独立的 AG32 Batch 更新选项。**

1. 先让 ESP32 启动，插好 TF 卡，关闭“USB 直通模式”。电脑与 ESP32 接入同一网络。
2. 浏览器访问 ESP32 的 IP，打开左下角 **设备设置**，找到 **AG32 Batch 更新**。
   也可以直接访问 `http://设备IP/#/settings`。
   本板上一轮使用 `http://192.168.1.134/#/settings`，实际地址以当前分配为准。
3. 解压 AG32 包，点击 **选择 Batch**，选择 `moriburnner_ag32_batch.bin`。
4. 网页自动上传并校验。当前包应显示 **3 个记录、108924 字节有效载荷**。
5. 确认更新，等待 **AG32 Batch 更新完成**。ESP32 通过已接好的板载 SWD
   自动刷写 AG32 MCU 和 CPLD 配置，通常约几分钟。期间保持供电并等待校验结束。

AG32 文件选择的是 Batch 更新卡片，不能当作 ESP32 的 OTA 主程序上传。
完成后将“AG32 链路”设为 **AUTO**，写入流程可使用 **PSRAM**。
SPI 固定 40 MHz；AG32 系统固定 150 MHz。

## 3. 旧 TF 卡看不到更新入口时

固件内置最新网页，但 TF 卡上的 `/.web/main.html` 优先于内置网页。
把 ESP32 包中的 `TF_UPDATE/web/main.html` 复制到 TF 卡的 `/.web/main.html`，
然后在浏览器按 Ctrl+F5 刷新。仅更新这份网页即可，无需替换整个配置目录。

## 4. 包与验证记录

ESP32 包包含 10 份分区镜像、16 MB 合并镜像、Windows 刷机工具、脚本、
网页更新文件和 SHA256SUMS.txt。AG32 ZIP 中只有一份合并 batch。

已检查全部分区容量、镜像位置、合并内容、ZIP 内容及 hash。
AG32 保留硬件测试版标识：系统 setup -1.122 ns，实机 GBA 读写已通过，
不等同于全部 STA 时序合格。相关测试见源码项目的
`docs/cpld_protocol_results_20260910.md`。

esptool 官方来源：
https://github.com/espressif/esptool/releases/tag/v5.4.0

官方 Windows 发布 ZIP 的 SHA256：
b7f6b9dd301a210b31f4829118c909c84aae23107f9ca1fdc14ccf4d7384be2e

包中保留了工具的 LICENSE 和 README。
