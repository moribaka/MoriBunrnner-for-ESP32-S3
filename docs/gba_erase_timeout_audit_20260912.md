# GBA 擦除偶发停滞审计（2026-09-12）

## 串口证据

本次用户操作实际是 GBA《地球冒险3》PSRAM 烧录，不是 GBC/MBC3。识别阶段全部
通过：CFI AMD、128 MiB NOR、128 KiB 扇区、1024 字节缓冲、40 MHz SPI、D0/D1
正常、4 次 paired polling 通过。

烧录在首个 4 MiB 窗口擦除阶段停滞，之后按设计的 80 秒预算超时：

```text
GBA erase timeout flash=0x003C0000 bank=0 sa_word=0x1E0000
read=0xF1CE multi=1 timeout=79977ms ppb_lock=ok(0x0418) sector_ppb=ok(0x0D00)
CHIS summary: err=ESP_ERR_TIMEOUT total=80028ms bytes=0/33554432 erase=80005ms
burn_task result: err=ESP_ERR_TIMEOUT ... msg=erase gba flash failed
```

没有发生 panic、看门狗重启或编程阶段；`program=0`，卡带数据写入尚未开始。
擦除超时后任务正常释放 CPU 性能锁并返回错误页。

## 当前判断

这是 NOR 在指定扇区没有在预算内完成擦除/状态轮询，不是 UI 文案或计时统计错误。
`ppb_lock` 和 `sector_ppb` 均已读取成功，说明保护位读取路径工作；仅凭这一次不能
区分芯片内部擦除异常、卡带供电/接触瞬态或该扇区已有异常状态。GBC/MBC3 的失败需
单独抓取同样的擦除状态，不能用这次 GBA 日志替代。

本次未修改擦除算法、频率或电压，也未自动重试；失败后没有继续写卡。
