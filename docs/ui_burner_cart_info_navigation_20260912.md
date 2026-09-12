# 烧录页卡带信息导航

GBA/GBC 烧录页是左右双面板布局，左/右面板位置可由 `panel` 键互换。信息面板按行枚举
（`ui_burn_info_enumerate`），绘制和光标索引共用同一份列表，任何探测/分析状态下的行数
都一致。

本次只改 UI 导航和显示，不改变 mapper 探测、MBC3/MBC5 烧录流程、擦除时序或烧录协议。
芯片型号来自现有 CFI/ID 数据库；若数据库无法区分同系列后缀，页面显示数据库已有的通用
型号，不猜测 N/P/S 后缀。

## 当前行为

- 方向键：`LEFT`/`RIGHT` 按面板实际位置把光标移到左/右面板（由 `s_burner_info_left`
  决定方向）；`UP`/`DOWN` 在获得焦点的面板内移动光标，超过一屏（可见 10 行）自动滚动。
- 左侧摘要顺序：类型、D1/D0、Save、Size、SRAM patch、`CFI:`，然后 File，最后是底部的
  NOR 区块（空行 + 分隔线/`NOR INFO` 标题 + 型号行 + 容量行）。
- `CFI:` 独立成行（`ok`/`fallback`）。容量/扇区/Count/NOR ID/ID 不再逐行铺开，全部折叠
  进 NOR 区块。
- NOR 区块格式：空行、`NOR INFO` 标题、识别出的型号（或 `未知NOR`）、容量（如 `128MB`，
  取自 CFI 的 `probe_device_size`）。
- 空行（`UI_BURN_INFO_ROW_GAP`）、`NOR INFO` 标题（`UI_BURN_INFO_ROW_HEADER`）和容量行
  （`UI_BURN_INFO_ROW_STATIC`）都不可选，`UP`/`DOWN` 直接跳过；只有型号行是可选行。
- 型号行取 ID 数据库里的型号（`ui_probe_nor_model`）；数据库识别不出时显示 `未知NOR`
  （英文界面 `Unknown NOR`），不再回退到 `CFI AMD 32MB` 之类的通用名。
- 焦点在左侧且选中型号行时按 `A`，在**同一块左侧面板内**展开 NOR 详情（面板标题变为
  `> NOR`），右侧操作列表保持不变；`B` 返回摘要并把光标放回型号行。
- NOR 详情条目：NOR chip、NOR family、Cmdset、NOR ID（全 8/4 字节）、Manuf ID、
  Device ID、Capacity、Sector、Count、Buffer（CFI 缓存写入字节）、CFI（ok/fallback）。
- 详情列表只读：`A` 不触发任何操作，焦点在信息面板时也不会误触发右侧被遮住的操作光标。
- 串口 `ui` 额外暴露 `burn_info_focus`/`burn_info_selected`/`burn_info_count`/
  `burn_info_nor_detail`/`nor_model`/`nor_id`/`nor_device_size`/`nor_sector_size`/
  `nor_buffer_bytes`，便于无屏验证。

## 实机验证（COM26，MAC `a4:cb:8f:f2:c4:c0`）

- 卡带原始 ID：`89 00 7E 22 28 22 01 22`；CFI 报告 128 MiB、128 KiB 扇区、1024 字节
  CFI 写缓冲。
- 摘要 `burn_info_count=10`；`down` 索引 1,2,3,4,5,**8**,8… —— 索引 6（空行）、7
  （`NOR INFO`）和 9（容量行）都被跳过，光标停在型号行。
- 型号行按 `A`：`burn_info_nor_detail=1`、`burn_info_count=11`、`burn_info_selected=0`，
  右侧 `selected`/`selection`（`ROM: 选择文件`）保持不变。
- 详情内 `down`×12 夹紧到 10，`up`×14 夹紧到 0；详情内 `A` 无动作；`B`→回到摘要，
  `burn_info_selected=8`（型号行），右侧光标不变。
- `right`→`burn_info_focus=0`，`left`→`1`；`panel` 互换面板后方向键仍按物理位置切换。

## NOR 数据库与本次识别结果

- 有 NOR 库：`main/burner/db/burner_nor_db_data.c`（GBA 8 字节 ID 表 + MBC5 4 字节 ID 表 +
  共用 family 元数据），查询逻辑在 `burner_nor_db_lookup.c`（GBA 按 8 字节掩码精确匹配）。
- GBA 读到的 8 字节 ID 结构：`厂商 00 7E 22 <容量码> 22 01 22`。厂商 `0x01`=AMD/Spansion、
  `0x89`=Intel/Numonyx/Micron、`0xC2`=Macronix；容量码 `0x21`=16MB、`0x22`=32MB、
  `0x23`=64MB、`0x28`=128MB、`0x48`=256MB。
- JS28 与 MT28 是同一颗 die 的两个品牌料号（Intel/Numonyx 的 JS28F 与 Micron 的 MT28EW），
  ID 完全一样，库里本来就写成别名（例如 32MB 档 `JS28F256 / MT28EW256ABA`）。**能分辨的
  只有容量码，同一容量下的 JS28/MT28 无法区分，也不需要区分。**
- 本卡 ID `89 00 7E 22 28 22 01 22` = 厂商 0x89 + 容量码 0x28 = 128 MiB / 1 Gbit，属于
  `MT28EW01G` 级别。原来库里只有 0x89+0x22（32 MiB），所以显示 `未知NOR`。
- 已补库：新增 family `s_family_mt28ew01g`（名 `MT28EW01GABA`，profile `AMD_AAA_AA`，
  cmdset AMD，128 MiB / 128 KiB / GBA buffer 1024，与同厂商 `s_family_js28f256` 一致）+
  GBA ID 条目 `89 00 7E 22 28 22 01 22`。补库后 `nor_model=MT28EW01GABA`。
- 补库不影响写入时序：flags 仍是 `BURNER_NOR_FLAG_NONE`（与未命中时的 0 相同）；AMD 运行时
  在 CFI 可用时优先采用探测结果（`burner_gba_apply_amd_runtime_from_probe_or_id`），
  family 几何也与本卡 CFI 报告一致（128 MiB / 128 KiB / 1024 B）。
- 暂未加 MBC5（4 字节 `89 7E 28 01`）条目：手头是 GBA 卡，GB 8 位总线下的
  buffer/flag 未在实机验证，不猜。

## 历史

- 第一版：左键回到简要信息、右键展开详细 NOR 信息，按左右键切换且没有光标。
- 第二版：左面板支持逐项光标；`ui_px_invert_rect()` 曾画在文字之前导致整行纯白，
  改为画在文字之后恢复白底黑字反色。
- 第三版：NOR 相关行折叠为一行，详情改为在左侧面板内展开，不再占用右侧操作列表。
- 第四版：NOR 区块移到摘要底部，加分隔线与不可选的 `NOR INFO` 标题行；`CFI:` 独立成行。
- 第五版：`NOR INFO` 上方再加一个空行；补充 NOR 库排查用的串口字段。
- 第六版：型号行下面加只读容量行（如 `128MB`）。
- 第七版（当前）：补 NOR 库 `MT28EW01GABA`（0x89 + 容量码 0x28，128 MiB），实机识别通过。

## 构建与刷写

- 当前：`idf.py '-DPROJECT_VER=v2.32.131' build`，`build/moriburnner.bin` 2840976 字节，
  SHA256 `110EC67A5B6A98692B2B2B7A39550D5C9FC0541216B5404FEBC3221ADB46573E`，写入 `0x20000`，
  esptool 报告 `Hash of data verified`。
- 更早版本：v2.32.122（2838896）、v2.32.123/124（2839968）、v2.32.125（2839936）、
  v2.32.126（2840448）、v2.32.127（2840608）、v2.32.128（2840640）、v2.32.129（2840784）、
  v2.32.130（2840880）。
- 测试中一度从首页“Retro-Go”项切到了 launcher，已用 `build/ota_data_initial.bin`
  （全 `0xFF`）写回 `0xF000` 恢复到 factory 主程序。
