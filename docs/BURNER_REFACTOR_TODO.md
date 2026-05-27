# Burner Refactor TODO

## Latest Progress

- Extracted SPI backend helpers into `main/burner/core/burner_spi_backend.c`.
- Scope: SPI init, CS switching, DMA shadow buffer allocation, transfer helpers, and cart power sequencing.
- Remaining in `ws_server.c`: higher-level GBC/GBA/MBC5 protocol flow and burn task execution paths.
- Split burn execution paths into `main/burner/core/burn/`.
- Added:
- `burner_burn_common.c`: shared TF/dump/erase/pipeline helpers.
- `burner_gbc_burn.c`: GBC/MBC5 ROM and RAM burn/read/verify/erase jobs.
- `burner_gba_burn.c`: GBA ROM burn/read/verify/erase jobs.
- Build verification: `ninja -C build` passed after the split.

目标：先把烧录相关代码从 `main/` 根部整理到独立目录，再逐步把 `ws_server.c` 中的烧录内核拆成可维护的小模块。

## Current Layout

- `main/burner/core`
  - `ws_server.c`
  - `ws_server.h`
  - `ws_server_internal.h`
- `main/burner/http`
  - `ws_server_httpd.c`
  - `ws_server_http_*.c/.h`
- `main/burner/db`
  - `burner_nor_db*.c/.h`

## Phase 1: 目录化整理

- [x] 新建 `main/burner/` 目录，作为烧录域代码根目录
- [x] 将现有烧录/Web 相关文件迁入 `main/burner/`
- [x] 进一步按 `core/http/db` 分层
- [x] 更新 `main/CMakeLists.txt` 的源文件路径
- [x] 调整 include 路径，保证迁移后可编译
- [x] 做一次构建验证，确认目录迁移没有引入编译错误

## Phase 2: 头文件边界收紧

- [ ] 将 `ws_server_internal.h` 拆为更小的内部头文件
- [ ] 把 HTTP 层需要的声明与烧录引擎内部声明分开
- [ ] 把烧录常量、类型、状态结构体从杂项声明中抽离
- [ ] 降低 `ui.c` / `main.c` 对烧录内部头的直接依赖

## Phase 3: 烧录引擎抽离

- [x] 从 `ws_server.c` 中抽出烧录任务启动与调度
- [x] 抽出烧录状态管理
- [x] 抽出后端初始化 / 配置读写 / 空闲掉电管理
- [ ] 抽出 SPI/电源/卡带底层访问
- [ ] 抽出 GBA 路径实现
- [ ] 抽出 MBC5 路径实现
- [ ] 抽出 RAM/SAVE 路径实现
- [ ] 抽出 TF 写入管线与 PSRAM 管线

## Phase 4: HTTP 层收口

- [ ] 保留 `ws_server_httpd.c` 只负责路由注册与 server 生命周期
- [ ] 将 `/api/burn/*`、`/api/cart/*`、`/api/tf/*`、`/api/device/*` 分组明确
- [ ] 避免 HTTP handler 直接依赖过多底层实现细节

## Phase 5: 收尾

- [ ] 删除废弃的兼容声明和重复常量
- [ ] 补充模块级注释和结构说明
- [ ] 视情况补一份源码结构文档

## 本轮范围

本轮已经完成：

- 建立 `main/burner/` 目录结构
- 迁移现有烧录/Web 文件
- 细分为 `core/http/db`
- 拆出 `burner_status.c`
- 拆出 `burner_task.c`
- 拆出 `burner_backend.c`
- 完成编译通过验证

本轮暂未开始：

- SPI/电源/卡带底层访问拆分
- GBA / MBC5 / RAM-SAVE 执行层拆分
- UI 侧重构
