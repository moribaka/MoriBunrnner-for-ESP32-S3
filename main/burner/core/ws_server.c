#include "ws_server_internal.h"

#include "esp_attr.h"

#include "mori_system_settings.h"

#define BURN_MBC5_ROM_BANK_BYTES (16U * 1024U)
#define BURNER_GBA_SAVE_HEADER_SCAN_BYTES (64u * 1024u)
#define BURNER_GBA_ANALYSIS_HEAD_BYTES (1u * 1024u * 1024u)
#define BURNER_GBA_SAVE_SCAN_STEP_BYTES 0x1000u
#define BURNER_BATTERYLESS_MARKER "<3 from Maniac"
#define BURNER_BATTERYLESS_SEARCH_BYTES 0x2000u
#include "burner_io_config.h"
#include "burner_file_io.h"

#include "burner_tf_list_types.h"

/* SPI burn path: ESP32-S3 SPI2 master -> AG32 CPLD core (Bacon compatible). */
/* Optional fallback profiles */
/* #define BURNER_SPI_CLOCK_HZ (60 * 1000 * 1000) */
/* #define BURNER_SPI_CLOCK_HZ (20 * 1000 * 1000) */
#define BURNER_PROBE_SCAN_WINDOW_BYTES (64u * 1024u)
#define BURNER_GBA_RETAIL_ROM_WINDOW_SIZE BURN_GBA_LINEAR_ADDR_BYTES
#define BURNER_GBA_RETAIL_ROM_SECTOR_SIZE (128U * 1024U)
#define BURNER_GBA_RETAIL_ROM_BUFFER_WRITE_BYTES 0U

typedef struct {
    uint8_t region_index;
    uint8_t reserved[3];
    uint32_t addr_begin;
    uint32_t addr_end;
    uint32_t sector_size;
} burner_nor_region_cursor_t;

_Static_assert(MORI_PIN_MCU_SPI_CS < 32, "SPI CS0 must be on GPIO0-31");
_Static_assert(MORI_PIN_MCU_SPI_CS1 < 32, "SPI CS1 must be on GPIO0-31");

typedef struct {
    bool active;
    bool multi_card;
    bool erase_always;
    bool pre_erased_valid;
    burner_nor_region_cursor_t cursor;
    uint32_t range_end;
    uint32_t erased_sector_addr;
    uint32_t pre_erased_sector_addr;
} burner_gba_sector_erase_ctx_t;

#include "burner_status_types.h"

const char *s_upload_html_override = NULL;
ws_receive_cb s_receive_cb = NULL;
httpd_handle_t s_httpd = NULL;
SemaphoreHandle_t s_status_lock = NULL;
SemaphoreHandle_t s_spi_lock = NULL;
TaskHandle_t s_burn_task = NULL;
TaskHandle_t s_bacon_idle_task = NULL;
spi_device_handle_t s_mcu_spi = NULL;
bool s_mcu_spi_ready = false;
static DMA_ATTR uint8_t s_mcu_spi_tx_shadow_storage[BURNER_SPI_SHADOW_CHUNK_BYTES];
static DMA_ATTR uint8_t s_mcu_spi_rw_shadow_storage[BURNER_SPI_SHADOW_CHUNK_BYTES];
uint8_t *s_mcu_spi_tx_shadow = s_mcu_spi_tx_shadow_storage;
uint8_t *s_mcu_spi_rw_shadow = s_mcu_spi_rw_shadow_storage;
const uint32_t s_mcu_spi_clock_hz = BURNER_SPI_CLOCK_HZ;
uint32_t s_mcu_spi_actual_hz = BURNER_SPI_CLOCK_HZ;
burner_core_config_t s_burn_core_cfg = {
    .erase_core = BURNER_CORE_AFFINITY_CPU1,
    .tf_core = BURNER_CORE_AFFINITY_CPU1,
    .psram_core = BURNER_CORE_AFFINITY_CPU1,
};
uint8_t s_burn_erase_always = BURN_ERASE_ALWAYS_DEFAULT;
uint8_t s_gba_fixed_erase_window_enabled = BURN_GBA_FIXED_ERASE_WINDOW_ENABLED_DEFAULT;
uint8_t s_mbc5_power_5v_enabled = 0;
uint32_t s_bacon_power_settle_ms = BURNER_POWER_SETTLE_MS;
burner_write_path_t s_burn_write_path_default = BURNER_WRITE_PATH_DIRECT;
burner_recipe_mode_t s_burn_recipe_mode_default = BURNER_RECIPE_MODE_CHIS;
uint32_t s_burn_psram_window_mb = BURN_PSRAM_WINDOW_DEFAULT_MB;
uint32_t s_burn_mbc5_chunk_kb = BURN_MBC5_PROGRAM_CHUNK_BYTES / 1024U;
uint32_t s_burn_dump_chunk_kb = BURN_GBA_DUMP_CHUNK_BYTES / 1024U;
TickType_t s_bacon_last_active_tick = 0;
bool s_bacon_idle_powered_down = false;
burner_cart_ctx_t s_cart_ctx = {
    .prepared = false,
    .current_bank = UINT16_MAX,
    .buffer_write_bytes = 0,
    .program_buffer_write_bytes = 0,
    .sector_size = 0,
    .device_size = 0,
    .geometry = {0},
    .mbc5_id = {0},
    .gba_cmdset = BURNER_NOR_CMDSET_UNKNOWN,
    .gba_cmd_addr_mode = BURNER_GBA_CMD_ADDR_WORD,
    .gba_cmd_data_lane = BURNER_GBA_CMD_DATA_LOW,
    .gba_likely_read_only = false,
    .gba_chislink_active = false,
    .probe_cfi_ok = false,
};
static burner_gba_sector_erase_ctx_t s_gba_sector_erase_ctx = {0};

static esp_err_t burner_bacon_mbc5_read_block_program_window(uint8_t *out, size_t len, uint32_t offset);

static bool burner_is_gba_multi_card(const burner_task_param_t *job);
const char *burner_gb_mapper_name(burner_gb_mapper_t mapper);
static bool burner_ini_split_line(char *line, char **key, char **value);
static esp_err_t burner_buffer_all_ff(const uint8_t *buf, size_t len, bool *all_ff_out);

burner_status_t s_status = {
    .state = BURNER_STATE_IDLE,
    .progress = 0,
    .total_bytes = 0,
    .processed_bytes = 0,
    .rom_name = "",
    .rom_path = "",
    .message = "idle",
    .cancel_requested = false,
};
uint64_t s_burn_task_last_yield_us = 0;
static burner_gb_mapper_t s_gb_mapper_kind = BURNER_GB_MAPPER_UNKNOWN;
burner_gb_mapper_t s_gb_mapper_override_kind = BURNER_GB_MAPPER_UNKNOWN;
const char *const s_system_migrate_rel_dirs[] = {
    WEB_LANG_DIR_REL,
    WEB_ROOT_DIR_REL,
};

/* Host mission_mbc5.cs multi-cart ROM ranges (slot 1..17). */
const uint32_t s_mbc5_multi_rom_range[BURNER_MBC5_SLOT_MAX + 1u][2] = {
    {0x00000000u, 0x00000000u},
    {0x00000000u, 0x000FFFFFu},
    {0x00100000u, 0x001FFFFFu},
    {0x00200000u, 0x003FFFFFu},
    {0x00400000u, 0x005FFFFFu},
    {0x00600000u, 0x007FFFFFu},
    {0x00800000u, 0x009FFFFFu},
    {0x00A00000u, 0x00BFFFFFu},
    {0x00C00000u, 0x00DFFFFFu},
    {0x00E00000u, 0x00FFFFFFu},
    {0x01000000u, 0x011FFFFFu},
    {0x01200000u, 0x013FFFFFu},
    {0x01400000u, 0x015FFFFFu},
    {0x01600000u, 0x017FFFFFu},
    {0x01800000u, 0x019FFFFFu},
    {0x01A00000u, 0x01BFFFFFu},
    {0x01C00000u, 0x01DFFFFFu},
    {0x01E00000u, 0x01FFFFFFu},
};

/* Host mission_mbc5.cs multi-cart RAM ranges (slot 1..17). */
const uint32_t s_mbc5_multi_ram_range[BURNER_MBC5_SLOT_MAX + 1u][2] = {
    {0x00000u, 0x00000u},
    {0x00000u, 0x07FFFu},
    {0x00000u, 0x07FFFu},
    {0x08000u, 0x0FFFFu},
    {0x10000u, 0x17FFFu},
    {0x18000u, 0x1FFFFu},
    {0x20000u, 0x27FFFu},
    {0x28000u, 0x2FFFFu},
    {0x30000u, 0x37FFFu},
    {0x38000u, 0x3FFFFu},
    {0x40000u, 0x47FFFu},
    {0x48000u, 0x4FFFFu},
    {0x50000u, 0x57FFFu},
    {0x58000u, 0x5FFFFu},
    {0x60000u, 0x67FFFu},
    {0x68000u, 0x6FFFFu},
    {0x70000u, 0x77FFFu},
    {0x78000u, 0x7FFFFu},
};

const char s_base_settings_html[] =
    "<!doctype html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>...</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;max-width:900px;margin:20px auto;padding:0 14px;background:#f4f7fb;color:#172032;}"
    "h2{margin:0 0 8px;}"
    ".tip{margin:0 0 14px;color:#4f5e79;font-size:13px;}"
    ".card{background:#fff;border:1px solid #d8e0ef;border-radius:10px;padding:12px;margin-bottom:12px;}"
    ".row{display:flex;gap:8px;flex-wrap:wrap;margin:8px 0;}"
    "select{padding:8px 10px;border-radius:8px;border:1px solid #bfcbe0;background:#fff;color:#13203a;}"
    "textarea{width:100%;min-height:220px;padding:8px 10px;border-radius:8px;border:1px solid #bfcbe0;background:#fff;color:#13203a;resize:vertical;font-family:Consolas,monospace;font-size:12px;line-height:1.45;}"
    "a.btn,button{display:inline-block;padding:8px 12px;border-radius:8px;border:1px solid #bfcbe0;background:#fff;color:#13203a;text-decoration:none;cursor:pointer;}"
    "button.primary,a.btn.primary{background:#0d6efd;border-color:#0d6efd;color:#fff;}"
    "button.warn{background:#ff9f1a;border-color:#ff9f1a;color:#fff;}"
    "pre{background:#0f1726;color:#dbe7ff;border-radius:10px;padding:10px;white-space:pre-wrap;word-break:break-word;min-height:72px;}"
    "</style></head><body>"
    "<h2 id='txt_page_header'>...</h2>"
    "<p id='txt_page_tip' class='tip'>...</p>"
    "<div class='card'>"
    "<h3 id='txt_business_title'>...</h3>"
    "<div class='row'>"
    "<a id='btn_open_business' class='btn primary' href='/tf'>...</a>"
    "</div>"
    "<p id='txt_business_tip' class='tip'>...</p>"
    "</div>"
    "<div class='card'>"
    "<h3 id='txt_recovery_title'>...</h3>"
    "<p id='txt_recovery_tip' class='tip'>...</p>"
    "<div class='row'>"
    "<input id='main_file' type='file' multiple style='flex:1;min-width:240px'>"
    "<button id='btn_main_upload' class='primary'>...</button>"
    "</div>"
    "<pre id='main_upload'>...</pre>"
    "</div>"
    "<div class='card'>"
    "<h3 id='txt_system_migrate_title'>...</h3>"
    "<p id='txt_system_migrate_tip' class='tip'>...</p>"
    "<div class='row'>"
    "<a id='btn_system_migrate' class='btn primary' href='/api/system/migrate_zip' download='" SYSTEM_MIGRATE_ZIP_NAME "'>...</a>"
    "</div>"
    "</div>"
    "<div class='card'>"
    "<h3 id='txt_system_deploy_title'>...</h3>"
    "<p id='txt_system_deploy_tip' class='tip'>...</p>"
    "<div class='row'>"
    "<input id='deploy_zip_file' type='file' accept='.zip,application/zip' style='flex:1;min-width:240px'>"
    "<button id='btn_deploy_zip' class='warn'>...</button>"
    "</div>"
    "<pre id='deploy_zip_status'>...</pre>"
    "</div>"
    "<div class='card'>"
    "<h3 id='txt_fw_title'>...</h3>"
    "<p id='txt_fw_tip' class='tip'>...</p>"
    "</div>"
    "<div class='card'>"
    "<h3 id='txt_language_title'>...</h3>"
    "<p id='txt_language_tip' class='tip'>...</p>"
    "<div class='row'>"
    "<button id='btn_lang_load'>...</button>"
    "<select id='lang_select' style='flex:1;min-width:220px'></select>"
    "<button id='btn_lang_apply' class='primary'>...</button>"
    "</div>"
    "<pre id='lang_status'>...</pre>"
    "</div>"
    "<div class='card'>"
    "<h3 id='txt_usb_title'>...</h3>"
    "<p id='txt_usb_tip' class='tip'>...</p>"
    "<div class='row'>"
    "<button id='btn_enable' class='primary'>...</button>"
    "<button id='btn_disable' class='warn'>...</button>"
    "<button id='btn_refresh'>...</button>"
    "</div>"
    "<pre id='storage'>...</pre>"
    "</div>"
    "<div class='card'>"
    "<h3 id='txt_device_title'>...</h3>"
    "<div class='row'><button id='btn_dev'>...</button></div>"
    "<pre id='dev'>...</pre>"
    "</div>"
    "<div class='card'>"
    "<h3 id='txt_power_title'>...</h3>"
    "<div class='row'><button id='btn_power'>...</button></div>"
    "<pre id='power'>...</pre>"
    "</div>"
    "<script>"
    "const storageEl=document.getElementById('storage');"
    "const devEl=document.getElementById('dev');"
    "const powerEl=document.getElementById('power');"
    "const mainUploadEl=document.getElementById('main_upload');"
    "const deployZipStatusEl=document.getElementById('deploy_zip_status');"
    "const langStatusEl=document.getElementById('lang_status');"
    "const langSelectEl=document.getElementById('lang_select');"
    "const lang={};"
    "let mainUploadState='idle';"
    "let deployZipState='idle';"
    "let storageState='loading';"
    "let deviceState='loading';"
    "let powerState='loading';"
    "let langState='idle';"
    "function tr(key){"
    "const v=lang[key];"
    "return(typeof v==='string'&&v.length>0)?v:'';"
    "}"
    "function setTextByKey(id,key){"
    "const el=document.getElementById(id);"
    "if(el){el.textContent=tr(key);}"
    "}"
    "function refreshIdleTexts(){"
    "if(mainUploadState==='idle'){mainUploadEl.textContent=tr('upload_idle');}"
    "if(deployZipState==='idle'){deployZipStatusEl.textContent=tr('upload_idle')||'Idle';}"
    "if(storageState==='loading'){storageEl.textContent=tr('storage_loading');}"
    "if(deviceState==='loading'){devEl.textContent=tr('device_loading');}"
    "if(powerState==='loading'){powerEl.textContent=tr('power_loading');}"
    "if(langState==='idle'){langStatusEl.textContent=tr('language_idle');}"
    "}"
    "function applyLang(){"
    "document.title=tr('page_title');"
    "setTextByKey('txt_page_header','page_header');"
    "setTextByKey('txt_page_tip','page_tip');"
    "setTextByKey('txt_business_title','business_title');"
    "setTextByKey('btn_open_business','btn_open_business');"
    "setTextByKey('txt_business_tip','business_tip');"
    "setTextByKey('txt_recovery_title','recovery_title');"
    "setTextByKey('txt_recovery_tip','recovery_tip');"
    "setTextByKey('btn_main_upload','btn_upload_main');"
    "setTextByKey('txt_system_migrate_title','system_migrate_title');"
    "setTextByKey('txt_system_migrate_tip','system_migrate_tip');"
    "setTextByKey('btn_system_migrate','btn_system_migrate');"
    "setTextByKey('txt_system_deploy_title','system_deploy_title');"
    "setTextByKey('txt_system_deploy_tip','system_deploy_tip');"
    "setTextByKey('btn_deploy_zip','btn_system_deploy');"
    "setTextByKey('txt_fw_title','firmware_title');"
    "setTextByKey('txt_fw_tip','firmware_tip');"
    "setTextByKey('txt_language_title','language_title');"
    "setTextByKey('txt_language_tip','language_tip');"
    "setTextByKey('btn_lang_load','btn_read_lang_list');"
    "setTextByKey('btn_lang_apply','btn_apply_language');"
    "setTextByKey('txt_usb_title','usb_title');"
    "setTextByKey('txt_usb_tip','usb_tip');"
    "setTextByKey('btn_enable','btn_enable_usb');"
    "setTextByKey('btn_disable','btn_disable_usb');"
    "setTextByKey('btn_refresh','btn_refresh_storage');"
    "setTextByKey('txt_device_title','device_title');"
    "setTextByKey('btn_dev','btn_refresh_device');"
    "setTextByKey('txt_power_title','power_title');"
    "setTextByKey('btn_power','btn_refresh_power');"
    "refreshIdleTexts();"
    "}"
    "function formatBytes(value){"
    "const n=Number(value);"
    "if(!Number.isFinite(n)||n<0){return 'n/a';}"
    "const units=['B','KiB','MiB','GiB','TiB'];"
    "let v=n;"
    "let idx=0;"
    "while(v>=1024&&idx<units.length-1){v/=1024;idx++;}"
    "const digits=(idx===0||v>=100)?0:(v>=10?1:2);"
    "return v.toFixed(digits)+' '+units[idx]+' ('+Math.round(n)+' bytes)';"
    "}"
    "function boolText(v){return v?'yes / 是':'no / 否';}"
    "function powerStateText(v){"
    "if(!v||v==='unknown'){return '-';}"
    "if(v==='charging'){return 'charging / 充电中';}"
    "if(v==='discharging'){return 'discharging / 放电中';}"
    "if(v==='charge_full'){return 'full / 已满';}"
    "if(v==='discharging_light_load'){return 'light load / 轻载放电';}"
    "if(v==='no_battery_external_power'){return 'external only / 外部供电';}"
    "return v;"
    "}"
    "function powerDirectionText(v){"
    "if(!v||v==='unknown'){return '-';}"
    "if(v==='charge'){return 'charge / 充电';}"
    "if(v==='discharge'){return 'discharge / 放电';}"
    "if(v==='external'){return 'external / 外供';}"
    "return v;"
    "}"
    "function powerModeText(v){"
    "if(!v||v==='unknown'){return '-';}"
    "if(v==='charging'){return 'charging / 充电';}"
    "if(v==='charging_limited'){return 'charging limited / 限流充电';}"
    "if(v==='charge_disabled'){return 'disabled / 已禁用';}"
    "if(v==='charge_ready'){return 'ready / 待充电';}"
    "if(v==='battery_only'){return 'battery only / 电池供电';}"
    "if(v==='external_only'){return 'external only / 外部供电';}"
    "if(v==='no_battery'){return 'no battery / 无电池';}"
    "return v;"
    "}"
    "function setStorageText(obj){"
    "storageState='custom';"
    "if(!obj||typeof obj!=='object'){storageEl.textContent=String(obj);return;}"
    "const lines=[];"
    "lines.push('TF ready / TF就绪: '+boolText(!!obj.tf_ready));"
    "lines.push('USB MSC ready / USB直通就绪: '+boolText(!!obj.usb_msc_ready));"
    "lines.push('USB passthrough / USB直通启用: '+boolText(!!obj.usb_passthrough_enabled));"
    "lines.push('TF busy / TF占用: '+boolText(!!obj.tf_busy));"
    "if(obj.tf_capacity_ok){"
    "lines.push('TF total / TF总量: '+formatBytes(obj.tf_total_bytes));"
    "lines.push('TF used / TF已用: '+formatBytes(obj.tf_used_bytes));"
    "lines.push('TF free / TF剩余: '+formatBytes(obj.tf_free_bytes));"
    "}else{"
    "lines.push('TF capacity / TF容量: unavailable / 不可用');"
    "}"
    "storageEl.textContent=lines.join('\\n');"
    "}"
    "function formatPowerVoltage(value){"
    "const n=Number(value);"
    "return Number.isFinite(n)&&n>0?(Math.round(n)+' mV'):'-';"
    "}"
    "function formatPowerCurrent(value){"
    "const n=Number(value);"
    "if(!Number.isFinite(n)||n<=0){return '-';}"
    "const ma=n/10;"
    "return (Math.abs(ma-Math.round(ma))<0.05?Math.round(ma).toString():ma.toFixed(1))+' mA';"
    "}"
    "function formatPowerTemp(value,chipType){"
    "const n=Number(value);"
    "if(!Number.isFinite(n)){return '-';}"
    "if(n===0&&chipType!=='axp209'){return '-';}"
    "return (n/10).toFixed(1)+' C';"
    "}"
    "function setPowerText(obj){"
    "powerState='custom';"
    "if(!obj||typeof obj!=='object'||!obj.ok||!obj.power){powerEl.textContent=JSON.stringify(obj,null,2);return;}"
    "const p=obj.power;"
    "const lines=[];"
    "lines.push('Power chip / 电源芯片: '+(p.chip_name||'-')+' / '+(p.chip_type||'-'));"
    "lines.push('Battery voltage / 电池电压: '+formatPowerVoltage(p.battery_voltage_mv));"
    "lines.push('ACIN voltage / 外部输入电压: '+formatPowerVoltage(p.acin_voltage_mv));"
    "lines.push('VBUS voltage / USB输入电压: '+formatPowerVoltage(p.vbus_voltage_mv));"
    "lines.push('IPSOUT voltage / 系统输出电压: '+formatPowerVoltage(p.ipsout_voltage_mv));"
    "lines.push('Charge current / 充电电流: '+formatPowerCurrent(p.battery_charge_current_ma_x10));"
    "lines.push('Discharge current / 放电电流: '+formatPowerCurrent(p.battery_discharge_current_ma_x10));"
    "lines.push('Direction / 电流方向: '+powerDirectionText(p.current_direction));"
    "lines.push('Charge mode / 充电模式: '+powerModeText(p.charge_mode));"
    "lines.push('State / 当前状态: '+powerStateText(p.charge_state));"
    "lines.push('Battery present / 电池存在: '+boolText(!!p.battery_present));"
    "lines.push('VBUS present / 外部供电: '+boolText(!!p.vbus_present));"
    "lines.push('Chip temperature / 芯片温度: '+formatPowerTemp(p.internal_temp_deci_c,p.chip_type));"
    "if(p.battery_percent_valid){lines.push('Battery percent / 电量估算: '+String(p.battery_percent)+'%');}"
    "powerEl.textContent=lines.join('\\n');"
    "}"
    "function setLangOptions(files,current){"
    "langSelectEl.innerHTML='';"
    "if(!Array.isArray(files)||files.length===0){"
    "const opt=document.createElement('option');"
    "opt.value='';"
    "opt.textContent=tr('language_none');"
    "langSelectEl.appendChild(opt);"
    "return;"
    "}"
    "for(let i=0;i<files.length;i++){"
    "const name=files[i];"
    "const opt=document.createElement('option');"
    "opt.value=name;"
    "opt.textContent=name;"
    "langSelectEl.appendChild(opt);"
    "}"
    "if(typeof current==='string'&&current.length>0){langSelectEl.value=current;}"
    "if(!langSelectEl.value&&langSelectEl.options.length>0){langSelectEl.selectedIndex=0;}"
    "}"
    "async function requestJson(url,opt){"
    "const r=await fetch(url,opt);"
    "const t=await r.text();"
    "if(!r.ok){throw new Error(tr('msg_http_error_prefix')+r.status+(t?(' '+t):''));}"
    "try{return JSON.parse(t);}catch(e){throw new Error(tr('msg_invalid_json_prefix')+t);}"
    "}"
    "async function requestText(url,opt){"
    "const r=await fetch(url,opt);"
    "const t=await r.text();"
    "if(!r.ok){throw new Error(tr('msg_http_error_prefix')+r.status+(t?(' '+t):''));}"
    "return t;"
    "}"
    "async function loadLang(){"
    "try{"
    "const d=await requestJson('/api/lang');"
    "if(d&&d.strings&&typeof d.strings==='object'){Object.assign(lang,d.strings);}"
    "}catch(e){}"
    "applyLang();"
    "}"
    "async function loadLangList(){"
    "langState='loading';"
    "langStatusEl.textContent=tr('language_loading');"
    "try{"
    "const d=await requestJson('/api/lang/list');"
    "const files=(d&&Array.isArray(d.files))?d.files:[];"
    "const current=(d&&typeof d.current==='string')?d.current:'';"
    "setLangOptions(files,current);"
    "langState='custom';"
    "langStatusEl.textContent=JSON.stringify(d);"
    "}catch(e){"
    "langState='custom';"
    "langStatusEl.textContent=tr('msg_lang_list_error_prefix')+String(e);"
    "}"
    "}"
    "async function refreshStorage(){"
    "try{setStorageText(await requestJson('/api/storage/status'));}"
    "catch(e){storageState='custom';storageEl.textContent=tr('msg_storage_status_error_prefix')+String(e);}"
    "}"
    "async function setUsbMode(enable){"
    "try{"
    "setStorageText({ok:false,message:tr('msg_applying')});"
    "setStorageText(await requestJson('/api/storage/usb_msc?enable='+(enable?'1':'0'),{method:'POST'}));"
    "}catch(e){storageEl.textContent=tr('msg_set_mode_error_prefix')+String(e);}"
    "}"
    "async function loadDeviceInfo(){"
    "try{deviceState='custom';devEl.textContent=await requestText('/api/device/info');}"
    "catch(e){deviceState='custom';devEl.textContent=tr('msg_device_info_error_prefix')+String(e);}"
    "}"
    "async function loadPowerStatus(){"
    "try{setPowerText(await requestJson('/api/power/status'));}"
    "catch(e){powerState='custom';powerEl.textContent=tr('msg_power_status_error_prefix')+String(e);}"
    "}"
    "async function uploadMainHtml(){"
    "const fi=document.getElementById('main_file');"
    "const files=(fi&&fi.files)?Array.from(fi.files):[];"
    "if(files.length===0){mainUploadState='custom';mainUploadEl.textContent=tr('msg_select_main');return;}"
    "let ok=0,failed=0;"
    "const lines=[];"
    "for(let i=0;i<files.length;i++){"
    "const f=files[i];"
    "mainUploadEl.textContent=tr('msg_uploading_prefix')+' ('+(i+1)+'/'+files.length+') '+f.name+' ...';"
    "try{"
    "const r=await requestJson('/api/web/upload?name='+encodeURIComponent(f.name),{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});"
    "ok++;"
    "lines.push(tr('msg_upload_item_ok')+' '+JSON.stringify(r));"
    "}catch(e){"
    "failed++;"
    "lines.push(tr('msg_upload_item_fail')+' '+f.name+': '+String(e));"
    "}"
    "}"
    "mainUploadState='custom';"
    "if(failed>0){"
    "mainUploadEl.textContent=tr('msg_upload_failed_prefix')+' '+ok+'/'+files.length+'\\n'+lines.join('\\n');"
    "}else{"
    "mainUploadEl.textContent=tr('msg_upload_success_prefix')+' '+ok+'/'+files.length+'\\n'+lines.join('\\n');"
    "}"
    "}"
    "async function uploadSystemDeployZip(){"
    "const fi=document.getElementById('deploy_zip_file');"
    "const f=(fi&&fi.files&&fi.files[0])?fi.files[0]:null;"
    "if(!f){deployZipState='custom';deployZipStatusEl.textContent=tr('msg_select_deploy_zip');return;}"
    "deployZipStatusEl.textContent=tr('msg_deploying_prefix')+': '+f.name+' ...';"
    "try{"
    "const r=await requestJson('/api/system/deploy_zip',{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});"
    "deployZipState='custom';"
    "deployZipStatusEl.textContent=tr('msg_deploy_success_prefix')+'\\n'+JSON.stringify(r,null,2);"
    "if(fi){fi.value='';}"
    "}catch(e){"
    "deployZipState='custom';"
    "deployZipStatusEl.textContent=tr('msg_deploy_failed_prefix')+': '+String(e);"
    "}"
    "}"
    "async function applyLanguage(){"
    "const ini=(langSelectEl&&typeof langSelectEl.value==='string')?langSelectEl.value:'';"
    "if(!ini){langState='custom';langStatusEl.textContent=tr('msg_lang_select_required');return;}"
    "langState='loading';"
    "langStatusEl.textContent=tr('msg_applying');"
    "try{"
    "const d=await requestJson('/api/lang/apply?ini='+encodeURIComponent(ini),{method:'POST'});"
    "await loadLang();"
    "await loadLangList();"
    "langState='custom';"
    "langStatusEl.textContent=tr('msg_lang_apply_success_prefix')+d.language_ini;"
    "}catch(e){"
    "langState='custom';"
    "langStatusEl.textContent=tr('msg_lang_apply_error_prefix')+String(e);"
    "}"
    "}"
    "document.getElementById('btn_refresh').onclick=refreshStorage;"
    "document.getElementById('btn_enable').onclick=()=>setUsbMode(true);"
    "document.getElementById('btn_disable').onclick=()=>setUsbMode(false);"
    "document.getElementById('btn_dev').onclick=loadDeviceInfo;"
    "document.getElementById('btn_power').onclick=loadPowerStatus;"
    "document.getElementById('btn_main_upload').onclick=uploadMainHtml;"
    "document.getElementById('btn_deploy_zip').onclick=uploadSystemDeployZip;"
    "document.getElementById('btn_lang_load').onclick=loadLangList;"
    "document.getElementById('btn_lang_apply').onclick=applyLanguage;"
    "async function refreshOverview(){"
    "await refreshStorage();"
    "await loadDeviceInfo();"
    "await loadPowerStatus();"
    "}"
    "async function initPage(){"
    "await loadLang();"
    "setLangOptions([], '');"
    "refreshIdleTexts();"
    "await loadLangList();"
    "await refreshOverview();"
    "setInterval(refreshOverview,3000);"
    "}"
    "initPage();"
    "</script></body></html>";

const char *burner_state_to_str(burner_state_t state)
{
    switch (state) {
        case BURNER_STATE_IDLE:
            return "idle";
        case BURNER_STATE_RECEIVING:
            return "receiving";
        case BURNER_STATE_BURNING:
            return "burning";
        case BURNER_STATE_DONE:
            return "done";
        case BURNER_STATE_ERROR:
            return "error";
        case BURNER_STATE_CANCELLED:
            return "cancelled";
        default:
            return "unknown";
    }
}

bool burner_parse_bool_text(const char *value, bool *out)
{
    if (value == NULL || out == NULL) {
        return false;
    }

    if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
        strcasecmp(value, "on") == 0 || strcasecmp(value, "enable") == 0 ||
        strcasecmp(value, "enabled") == 0) {
        *out = true;
        return true;
    }

    if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
        strcasecmp(value, "off") == 0 || strcasecmp(value, "disable") == 0 ||
        strcasecmp(value, "disabled") == 0) {
        *out = false;
        return true;
    }

    return false;
}

bool burner_parse_size_text(const char *value, uint32_t *out_bytes)
{
    char *end = NULL;
    unsigned long parsed;
    unsigned long long bytes = 0;
    const unsigned long long kbytes = 1024ULL;
    const unsigned long long mbytes = 1024ULL * 1024ULL;

    if (value == NULL || out_bytes == NULL || value[0] == '\0') {
        return false;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value) {
        return false;
    }

    if (*end == '\0') {
        bytes = parsed;
    } else if ((end[0] == 'k' || end[0] == 'K') && end[1] == '\0') {
        bytes = (unsigned long long)parsed * kbytes;
    } else if ((end[0] == 'k' || end[0] == 'K') && (end[1] == 'b' || end[1] == 'B') && end[2] == '\0') {
        bytes = (unsigned long long)parsed * kbytes;
    } else if ((end[0] == 'm' || end[0] == 'M') && end[1] == '\0') {
        bytes = (unsigned long long)parsed * mbytes;
    } else if ((end[0] == 'm' || end[0] == 'M') && (end[1] == 'b' || end[1] == 'B') && end[2] == '\0') {
        bytes = (unsigned long long)parsed * mbytes;
    } else {
        return false;
    }

    if (bytes == 0ULL || bytes > UINT32_MAX) {
        return false;
    }

    *out_bytes = (uint32_t)bytes;
    return true;
}

bool burner_parse_u32_text(const char *value, uint32_t *out_value)
{
    char *end = NULL;
    unsigned long parsed;

    if (value == NULL || out_value == NULL || value[0] == '\0') {
        return false;
    }

    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }

    *out_value = (uint32_t)parsed;
    return true;
}

bool burner_parse_cart_mode_text(const char *text, burner_cart_mode_t *mode_out)
{
    if (mode_out == NULL) {
        return false;
    }

    if (text == NULL || text[0] == '\0' || strcasecmp(text, "mbc5") == 0) {
        *mode_out = BURNER_CART_MODE_MBC5;
        return true;
    }
    if (strcasecmp(text, "gba") == 0) {
        *mode_out = BURNER_CART_MODE_GBA;
        return true;
    }

    return false;
}

const char *burner_write_path_to_str(burner_write_path_t path)
{
    switch (path) {
    case BURNER_WRITE_PATH_PIPELINE:
        return "pipeline";
    case BURNER_WRITE_PATH_PSRAM:
        return "psram";
    case BURNER_WRITE_PATH_DIRECT:
    default:
        return "direct";
    }
}

bool burner_parse_write_path_text(const char *text, burner_write_path_t *path_out)
{
    if (path_out == NULL) {
        return false;
    }
    if (text == NULL || text[0] == '\0' || strcasecmp(text, "direct") == 0) {
        *path_out = BURNER_WRITE_PATH_DIRECT;
        return true;
    }
    if (strcasecmp(text, "psram") == 0) {
        *path_out = BURNER_WRITE_PATH_PSRAM;
        return true;
    }
    if (strcasecmp(text, "pipeline") == 0 || strcasecmp(text, "pipe") == 0) {
        *path_out = BURNER_WRITE_PATH_PIPELINE;
        return true;
    }
    return false;
}

const char *burner_recipe_mode_to_str(burner_recipe_mode_t mode)
{
    switch (mode) {
    case BURNER_RECIPE_MODE_GBX:
        return "gbx";
    case BURNER_RECIPE_MODE_CHISLINK:
        return "chislink";
    case BURNER_RECIPE_MODE_CHIS:
    default:
        return "chis";
    }
}

bool burner_parse_recipe_mode_text(const char *text, burner_recipe_mode_t *mode_out)
{
    if (mode_out == NULL) {
        return false;
    }
    if (text == NULL || text[0] == '\0' || strcasecmp(text, "chis") == 0) {
        *mode_out = BURNER_RECIPE_MODE_CHIS;
        return true;
    }
    if (strcasecmp(text, "gbx") == 0) {
        *mode_out = BURNER_RECIPE_MODE_GBX;
        return true;
    }
    if (strcasecmp(text, "chislink") == 0 ||
        strcasecmp(text, "chis_link") == 0 ||
        strcasecmp(text, "chis-link") == 0 ||
        strcasecmp(text, "chislinkpremium") == 0) {
        *mode_out = BURNER_RECIPE_MODE_CHISLINK;
        return true;
    }
    return false;
}

uint32_t burner_clamp_mbc5_program_chunk_bytes(uint32_t bytes)
{
    if (bytes < BURN_MBC5_PROGRAM_CHUNK_MIN_BYTES) {
        return BURN_MBC5_PROGRAM_CHUNK_MIN_BYTES;
    }
    if (bytes > BURN_MBC5_PROGRAM_CHUNK_MAX_BYTES) {
        return BURN_MBC5_PROGRAM_CHUNK_MAX_BYTES;
    }
    return bytes;
}

uint32_t burner_mbc5_program_chunk_kb_to_bytes(uint32_t kb)
{
    uint64_t bytes;

    if (kb == 0u) {
        return BURN_MBC5_PROGRAM_CHUNK_BYTES;
    }

    bytes = (uint64_t)kb * 1024ULL;
    if (bytes > UINT32_MAX) {
        return BURN_MBC5_PROGRAM_CHUNK_MAX_BYTES;
    }
    return burner_clamp_mbc5_program_chunk_bytes((uint32_t)bytes);
}

bool burner_is_supported_dump_chunk_bytes(uint32_t bytes)
{
    return bytes == BURN_ROM_DUMP_CHUNK_MIN_BYTES || bytes == BURN_MBC5_DUMP_CHUNK_BYTES ||
           bytes == (128U * 1024U) || bytes == BURN_ROM_DUMP_CHUNK_MAX_BYTES;
}

uint32_t burner_dump_chunk_kb_to_bytes(uint32_t kb)
{
    uint64_t bytes;

    if (kb == 0u) {
        return BURN_GBA_DUMP_CHUNK_BYTES;
    }

    bytes = (uint64_t)kb * 1024ULL;
    if (bytes > UINT32_MAX) {
        return BURN_GBA_DUMP_CHUNK_BYTES;
    }
    if (!burner_is_supported_dump_chunk_bytes((uint32_t)bytes)) {
        return BURN_GBA_DUMP_CHUNK_BYTES;
    }
    return (uint32_t)bytes;
}

uint32_t burner_dump_chunk_bytes_to_kb(uint32_t bytes)
{
    if (!burner_is_supported_dump_chunk_bytes(bytes)) {
        return BURN_GBA_DUMP_CHUNK_BYTES / 1024U;
    }
    return bytes / 1024U;
}

uint32_t burner_psram_window_mb_to_bytes(uint32_t mb)
{
    uint32_t max_mb = BURN_PSRAM_WINDOW_MAX_MB;

    if (esp_psram_is_initialized()) {
        size_t psram_size = esp_psram_get_size();

        if (psram_size >= BURN_PSRAM_WINDOW_BYTES_PER_MB) {
            max_mb = (uint32_t)(psram_size / BURN_PSRAM_WINDOW_BYTES_PER_MB);
        } else {
            max_mb = BURN_PSRAM_WINDOW_MIN_MB;
        }
        if (max_mb < BURN_PSRAM_WINDOW_MIN_MB) {
            max_mb = BURN_PSRAM_WINDOW_MIN_MB;
        } else if (max_mb > BURN_PSRAM_WINDOW_MAX_MB) {
            max_mb = BURN_PSRAM_WINDOW_MAX_MB;
        }
    }

    if (mb < BURN_PSRAM_WINDOW_MIN_MB || mb > BURN_PSRAM_WINDOW_MAX_MB) {
        mb = BURN_PSRAM_WINDOW_DEFAULT_MB;
    }
    if (mb == BURN_PSRAM_WINDOW_AUTO_MB) {
        mb = burner_psram_auto_window_mb();
    }
    if (mb > max_mb) {
        mb = max_mb;
    }
    return mb * BURN_PSRAM_WINDOW_BYTES_PER_MB;
}

uint32_t burner_psram_auto_window_mb(void)
{
    size_t free_bytes;
    size_t largest_bytes;
    size_t usable_bytes;
    uint32_t mb;

    if (!esp_psram_is_initialized()) {
        return BURN_PSRAM_WINDOW_MIN_MB;
    }

    free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    largest_bytes = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    usable_bytes = (largest_bytes < free_bytes) ? largest_bytes : free_bytes;
    if (usable_bytes > BURN_PSRAM_WINDOW_RESERVE_BYTES) {
        usable_bytes -= BURN_PSRAM_WINDOW_RESERVE_BYTES;
    }

    mb = (uint32_t)(usable_bytes / BURN_PSRAM_WINDOW_BYTES_PER_MB);
    if (mb < BURN_PSRAM_WINDOW_MIN_MB) {
        mb = BURN_PSRAM_WINDOW_MIN_MB;
    } else if (mb > BURN_PSRAM_WINDOW_MAX_MB) {
        mb = BURN_PSRAM_WINDOW_MAX_MB;
    }

    ESP_LOGI(
        BURNER_TAG,
        "PSRAM auto window: free=%u largest=%u reserve=%u -> %" PRIu32 "MB",
        (unsigned)free_bytes,
        (unsigned)largest_bytes,
        (unsigned)BURN_PSRAM_WINDOW_RESERVE_BYTES,
        mb);
    return mb;
}

uint32_t burner_psram_window_bytes_to_mb(uint32_t bytes)
{
    uint32_t mb;

    if (bytes == 0u) {
        return BURN_PSRAM_WINDOW_AUTO_MB;
    }
    if ((bytes % BURN_PSRAM_WINDOW_BYTES_PER_MB) != 0u) {
        return BURN_PSRAM_WINDOW_DEFAULT_MB;
    }

    mb = bytes / BURN_PSRAM_WINDOW_BYTES_PER_MB;
    if (mb < BURN_PSRAM_WINDOW_MIN_MB || mb > BURN_PSRAM_WINDOW_MAX_MB) {
        return BURN_PSRAM_WINDOW_DEFAULT_MB;
    }
    return mb;
}

const char *burner_core_affinity_to_str(burner_core_affinity_t affinity)
{
    switch (affinity) {
    case BURNER_CORE_AFFINITY_CPU0:
        return "cpu0";
    case BURNER_CORE_AFFINITY_CPU1:
        return "cpu1";
    case BURNER_CORE_AFFINITY_AUTO:
    default:
        return "auto";
    }
}

bool burner_parse_core_affinity_text(const char *text, burner_core_affinity_t *affinity_out)
{
    if (affinity_out == NULL) {
        return false;
    }
    if (text == NULL || text[0] == '\0' || strcasecmp(text, "auto") == 0) {
        *affinity_out = BURNER_CORE_AFFINITY_AUTO;
        return true;
    }
    if (strcasecmp(text, "cpu0") == 0 || strcmp(text, "0") == 0) {
        *affinity_out = BURNER_CORE_AFFINITY_CPU0;
        return true;
    }
    if (strcasecmp(text, "cpu1") == 0 || strcmp(text, "1") == 0) {
        *affinity_out = BURNER_CORE_AFFINITY_CPU1;
        return true;
    }
    return false;
}

BaseType_t burner_core_affinity_to_task_core_id(burner_core_affinity_t affinity)
{
    switch (affinity) {
    case BURNER_CORE_AFFINITY_CPU0:
        return 0;
    case BURNER_CORE_AFFINITY_CPU1:
        return 1;
    case BURNER_CORE_AFFINITY_AUTO:
    default:
        return tskNO_AFFINITY;
    }
}

BaseType_t burner_create_task_with_affinity(
    TaskFunction_t task_fn,
    const char *name,
    uint32_t stack_bytes,
    void *arg,
    UBaseType_t priority,
    TaskHandle_t *task_out,
    burner_core_affinity_t affinity)
{
    BaseType_t core_id = burner_core_affinity_to_task_core_id(affinity);

    if (core_id == tskNO_AFFINITY) {
        return xTaskCreate(task_fn, name, stack_bytes, arg, priority, task_out);
    }
    return xTaskCreatePinnedToCore(task_fn, name, stack_bytes, arg, priority, task_out, core_id);
}

#include "burn/burner_cart_common.c"

bool burner_sanitize_filename(const char *input, char *output, size_t output_len)
{
    size_t out_idx = 0;
    size_t i;

    if (input == NULL || output == NULL || output_len < 2) {
        return false;
    }

    for (i = 0; input[i] != '\0' && out_idx + 1 < output_len; i++) {
        unsigned char ch = (unsigned char)input[i];
        if (isalnum(ch) || ch == '_' || ch == '-' || ch == '.') {
            output[out_idx++] = (char)ch;
        } else if (ch == ' ') {
            output[out_idx++] = '_';
        }
    }

    if (out_idx == 0) {
        return false;
    }

    output[out_idx] = '\0';
    return true;
}

bool burner_validate_file_name(const char *input, char *output, size_t output_len)
{
    size_t out_idx = 0;

    if (input == NULL || output == NULL || output_len < 2) {
        return false;
    }

    for (size_t i = 0; input[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)input[i];

        if (ch < 0x20 || ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '\"' || ch == '<' || ch == '>' || ch == '|') {
            return false;
        }

        if (out_idx + 1 >= output_len) {
            return false;
        }
        output[out_idx++] = (char)ch;
    }

    if (out_idx == 0) {
        return false;
    }

    output[out_idx] = '\0';
    if (strcmp(output, ".") == 0 || strcmp(output, "..") == 0) {
        return false;
    }

    return true;
}

bool burner_wallclock_time_valid(time_t now)
{
    return now >= BURNER_VALID_WALLCLOCK_MIN;
}

bool burner_get_wallclock_time(time_t *now_out, struct tm *tm_out)
{
    time_t now = 0;
    struct tm tm_now = {0};

    time(&now);
    if (!burner_wallclock_time_valid(now)) {
        return false;
    }
    if (tm_out != NULL && localtime_r(&now, &tm_now) == NULL) {
        return false;
    }

    if (now_out != NULL) {
        *now_out = now;
    }
    if (tm_out != NULL) {
        *tm_out = tm_now;
    }
    return true;
}

bool burner_apply_current_file_mtime(const char *path, time_t *applied_time_out)
{
    struct utimbuf times = {0};
    time_t now = 0;

    if (path == NULL || path[0] == '\0') {
        return false;
    }
    if (!burner_get_wallclock_time(&now, NULL)) {
        return false;
    }

    times.actime = now;
    times.modtime = now;
    if (utime(path, &times) != 0) {
        ESP_LOGW(BURNER_TAG, "set mtime failed for %s: errno=%d", path, errno);
        return false;
    }

    if (applied_time_out != NULL) {
        *applied_time_out = now;
    }
    return true;
}

long long burner_fixup_file_mtime_for_api(const char *path, const struct stat *st)
{
    struct stat refreshed = {0};
    time_t repaired_time = 0;
    time_t current_mtime = 0;

    if (st == NULL) {
        return 0;
    }

    current_mtime = st->st_mtime;
    if (current_mtime > BURNER_SUSPECT_FILE_MTIME_MAX) {
        return (long long)current_mtime;
    }
    if (path == NULL || path[0] == '\0') {
        return (long long)current_mtime;
    }
    if (!burner_apply_current_file_mtime(path, &repaired_time)) {
        return (long long)current_mtime;
    }
    if (stat(path, &refreshed) == 0) {
        return (long long)refreshed.st_mtime;
    }
    return (long long)repaired_time;
}

#include "burn/burner_gba_analysis.c"

#include "burn/burner_gbc_analysis.c"

bool burner_build_indexed_file_name(
    const char *preferred_name,
    uint32_t index,
    char *output,
    size_t output_len)
{
    const char *dot;
    size_t stem_len;
    size_t ext_len;
    char index_buf[16] = {0};
    char ext_buf[24] = {0};
    size_t index_len = 0u;
    size_t reserve;

    if (preferred_name == NULL || preferred_name[0] == '\0' || output == NULL || output_len < 8u) {
        return false;
    }

    dot = strrchr(preferred_name, '.');
    if (dot == NULL || dot == preferred_name) {
        ext_len = 0u;
        stem_len = strlen(preferred_name);
    } else {
        ext_len = strlen(dot);
        if (ext_len + 1u > sizeof(ext_buf)) {
            return false;
        }
        memcpy(ext_buf, dot, ext_len + 1u);
        stem_len = (size_t)(dot - preferred_name);
    }

    {
        int n = snprintf(index_buf, sizeof(index_buf), "_%03" PRIu32, index);
        if (n <= 0 || n >= (int)sizeof(index_buf)) {
            return false;
        }
        index_len = (size_t)n;
    }

    reserve = index_len + ext_len + 1u;
    if (output_len <= reserve) {
        return false;
    }
    if (stem_len > output_len - reserve - 1u) {
        stem_len = output_len - reserve - 1u;
    }
    if (stem_len == 0u) {
        return false;
    }

    memcpy(output, preferred_name, stem_len);
    output[stem_len] = '\0';
    (void)snprintf(output + stem_len, output_len - stem_len, "%s%s", index_buf, ext_buf);
    return true;
}

esp_err_t burner_resolve_unique_output_path(
    const char *dir_path,
    const char *preferred_name,
    char *resolved_name,
    size_t resolved_name_len,
    char *resolved_full_path,
    size_t resolved_full_path_len)
{
    struct stat st;
    uint32_t index;

    if (dir_path == NULL || preferred_name == NULL || preferred_name[0] == '\0' || resolved_name == NULL ||
        resolved_name_len < 2u || resolved_full_path == NULL || resolved_full_path_len < 4u) {
        return ESP_ERR_INVALID_ARG;
    }

    if (snprintf(resolved_full_path, resolved_full_path_len, "%s/%s", dir_path, preferred_name) >=
        (int)resolved_full_path_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (stat(resolved_full_path, &st) != 0) {
        if (snprintf(resolved_name, resolved_name_len, "%s", preferred_name) >= (int)resolved_name_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        return ESP_OK;
    }

    for (index = 1u; index < 1000u; ++index) {
        if (!burner_build_indexed_file_name(preferred_name, index, resolved_name, resolved_name_len)) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (snprintf(resolved_full_path, resolved_full_path_len, "%s/%s", dir_path, resolved_name) >=
            (int)resolved_full_path_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (stat(resolved_full_path, &st) != 0) {
            return ESP_OK;
        }
    }

    return ESP_FAIL;
}

const char *burner_rom_dump_ext_for_mode(burner_cart_mode_t cart_mode)
{
    return (cart_mode == BURNER_CART_MODE_GBA) ? ".gba" : ".gbc";
}

bool burner_force_file_extension(
    const char *input_name,
    const char *target_ext,
    char *output,
    size_t output_len)
{
    const char *dot;
    size_t stem_len;
    size_t ext_len;

    if (input_name == NULL || input_name[0] == '\0' || target_ext == NULL || target_ext[0] != '.' || output == NULL ||
        output_len < 4u) {
        return false;
    }

    dot = strrchr(input_name, '.');
    if (dot == NULL || dot == input_name) {
        stem_len = strlen(input_name);
    } else {
        stem_len = (size_t)(dot - input_name);
    }
    ext_len = strlen(target_ext);

    if (stem_len + ext_len + 1u > output_len) {
        return false;
    }

    memcpy(output, input_name, stem_len);
    memcpy(output + stem_len, target_ext, ext_len + 1u);
    return true;
}

bool burner_hex_to_nibble(char ch, uint8_t *nibble)
{
    if (nibble == NULL) {
        return false;
    }
    if (ch >= '0' && ch <= '9') {
        *nibble = (uint8_t)(ch - '0');
        return true;
    }
    if (ch >= 'a' && ch <= 'f') {
        *nibble = (uint8_t)(10 + (ch - 'a'));
        return true;
    }
    if (ch >= 'A' && ch <= 'F') {
        *nibble = (uint8_t)(10 + (ch - 'A'));
        return true;
    }
    return false;
}

bool burner_url_decode(const char *src, char *dst, size_t dst_len)
{
    size_t si = 0;
    size_t di = 0;

    if (src == NULL || dst == NULL || dst_len < 1) {
        return false;
    }

    while (src[si] != '\0') {
        char ch = src[si];
        if (ch == '%') {
            uint8_t hi;
            uint8_t lo;
            if (src[si + 1] == '\0' || src[si + 2] == '\0') {
                return false;
            }
            if (!burner_hex_to_nibble(src[si + 1], &hi) || !burner_hex_to_nibble(src[si + 2], &lo)) {
                return false;
            }
            if (di + 1 >= dst_len) {
                return false;
            }
            dst[di++] = (char)((hi << 4) | lo);
            si += 3;
            continue;
        }

        if (ch == '+') {
            ch = ' ';
        }

        if (di + 1 >= dst_len) {
            return false;
        }
        dst[di++] = ch;
        si++;
    }

    dst[di] = '\0';
    return true;
}

bool burner_get_query_arg(
    httpd_req_t *req,
    const char *key,
    char *out,
    size_t out_len,
    bool required)
{
    char query[TF_QUERY_LEN_MAX] = {0};
    char raw[TF_QUERY_LEN_MAX] = {0};

    if (req == NULL || key == NULL || out == NULL || out_len < 1) {
        return false;
    }

    out[0] = '\0';

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return !required;
    }

    if (httpd_query_key_value(query, key, raw, sizeof(raw)) != ESP_OK) {
        return !required;
    }

    return burner_url_decode(raw, out, out_len);
}

esp_err_t burner_send_json(httpd_req_t *req, const char *json_text)
{
    if (req == NULL || json_text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    web_ws_mark_network_activity();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json_text, HTTPD_RESP_USE_STRLEN);
}

esp_err_t burner_read_request_body(
    httpd_req_t *req,
    char *out,
    size_t out_len,
    size_t *actual_len)
{
    int remaining = 0;
    int received = 0;

    if (req == NULL || out == NULL || out_len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->content_len <= 0 || req->content_len >= (int)out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    remaining = req->content_len;
    while (remaining > 0) {
        int ret = httpd_req_recv(req, out + received, remaining);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            return ESP_FAIL;
        }
        remaining -= ret;
        received += ret;
    }

    out[received] = '\0';
    if (actual_len != NULL) {
        *actual_len = (size_t)received;
    }
    return ESP_OK;
}

const char *burner_json_locate_value(const char *json, const char *key)
{
    char pattern[64];
    const char *pos = NULL;
    int n = 0;

    if (json == NULL || key == NULL) {
        return NULL;
    }

    n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pattern)) {
        return NULL;
    }

    pos = strstr(json, pattern);
    if (pos == NULL) {
        return NULL;
    }
    pos += n;

    while (*pos != '\0' && isspace((unsigned char)*pos)) {
        pos++;
    }
    if (*pos != ':') {
        return NULL;
    }
    pos++;
    while (*pos != '\0' && isspace((unsigned char)*pos)) {
        pos++;
    }

    return pos;
}

bool burner_json_get_string(const char *json, const char *key, char *out, size_t out_len)
{
    const char *pos = burner_json_locate_value(json, key);
    size_t di = 0;

    if (out == NULL || out_len < 1) {
        return false;
    }
    out[0] = '\0';

    if (pos == NULL || *pos != '\"') {
        return false;
    }
    pos++;

    while (*pos != '\0') {
        char ch = *pos++;

        if (ch == '\"') {
            out[di] = '\0';
            return true;
        }

        if (ch == '\\') {
            char esc = *pos++;
            switch (esc) {
                case '\"':
                    ch = '\"';
                    break;
                case '\\':
                    ch = '\\';
                    break;
                case '/':
                    ch = '/';
                    break;
                case 'b':
                    ch = '\b';
                    break;
                case 'f':
                    ch = '\f';
                    break;
                case 'n':
                    ch = '\n';
                    break;
                case 'r':
                    ch = '\r';
                    break;
                case 't':
                    ch = '\t';
                    break;
                case 'u': {
                    uint8_t nibble = 0;
                    bool valid = true;
                    for (int i = 0; i < 4; i++) {
                        if (pos[i] == '\0') {
                            valid = false;
                            break;
                        }
                        if (!burner_hex_to_nibble(pos[i], &nibble)) {
                            valid = false;
                            break;
                        }
                    }
                    if (!valid) {
                        return false;
                    }
                    pos += 4;
                    ch = '?';
                    break;
                }
                default:
                    return false;
            }
        }

        if (di + 1 >= out_len) {
            return false;
        }
        out[di++] = ch;
    }

    return false;
}

bool burner_json_get_bool(const char *json, const char *key, bool *out)
{
    const char *pos = burner_json_locate_value(json, key);

    if (pos == NULL || out == NULL) {
        return false;
    }

    if (strncasecmp(pos, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncasecmp(pos, "false", 5) == 0) {
        *out = false;
        return true;
    }
    if (*pos == '1') {
        *out = true;
        return true;
    }
    if (*pos == '0') {
        *out = false;
        return true;
    }
    return false;
}

bool burner_json_get_int(const char *json, const char *key, int *out)
{
    const char *pos = burner_json_locate_value(json, key);
    char num_buf[24];
    size_t di = 0;
    long parsed = 0;
    char *end = NULL;

    if (pos == NULL || out == NULL) {
        return false;
    }

    while (*pos != '\0' && isspace((unsigned char)*pos)) {
        pos++;
    }

    if (*pos == '\"') {
        pos++;
        while (*pos != '\0' && *pos != '\"') {
            if (di + 1 >= sizeof(num_buf)) {
                return false;
            }
            num_buf[di++] = *pos++;
        }
        if (*pos != '\"') {
            return false;
        }
    } else {
        while (*pos != '\0' && !isspace((unsigned char)*pos) && *pos != ',' && *pos != '}') {
            if (di + 1 >= sizeof(num_buf)) {
                return false;
            }
            num_buf[di++] = *pos++;
        }
    }

    if (di == 0) {
        return false;
    }
    num_buf[di] = '\0';

    errno = 0;
    parsed = strtol(num_buf, &end, 10);
    if (errno != 0 || end == num_buf || *end != '\0') {
        return false;
    }
    if (parsed < INT_MIN || parsed > INT_MAX) {
        return false;
    }

    *out = (int)parsed;
    return true;
}

bool burner_normalize_rel_path(
    const char *input,
    char *output,
    size_t output_len,
    bool allow_empty)
{
    size_t out_idx = 0;
    size_t seg_start = 0;
    size_t i = 0;
    const char *p = NULL;

    if (input == NULL || output == NULL || output_len < 2) {
        return false;
    }

    p = input;
    while (*p == '/') {
        p++;
    }

    if (*p == '\0') {
        if (!allow_empty) {
            return false;
        }
        output[0] = '\0';
        return true;
    }

    for (i = 0;; i++) {
        char ch = p[i];
        if (ch == '/' || ch == '\0') {
            size_t seg_len = i - seg_start;
            if (seg_len == 0 || (seg_len == 1 && p[seg_start] == '.')) {
                seg_start = i + 1;
                if (ch == '\0') {
                    break;
                }
                continue;
            }
            if (seg_len == 2 && p[seg_start] == '.' && p[seg_start + 1] == '.') {
                return false;
            }

            for (size_t j = seg_start; j < i; j++) {
                unsigned char seg_ch = (unsigned char)p[j];
                if (seg_ch < 0x20 || seg_ch == '\\' || seg_ch == ':' || seg_ch == '*' || seg_ch == '?' ||
                    seg_ch == '\"' || seg_ch == '<' || seg_ch == '>' || seg_ch == '|') {
                    return false;
                }
            }

            if (out_idx > 0) {
                if (out_idx + 1 >= output_len) {
                    return false;
                }
                output[out_idx++] = '/';
            }

            if (out_idx + seg_len + 1 >= output_len) {
                return false;
            }

            memcpy(output + out_idx, p + seg_start, seg_len);
            out_idx += seg_len;
            seg_start = i + 1;
            if (ch == '\0') {
                break;
            }
            continue;
        }
    }

    if (out_idx == 0) {
        if (!allow_empty) {
            return false;
        }
        output[0] = '\0';
        return true;
    }

    output[out_idx] = '\0';
    return true;
}

bool burner_build_full_path(const char *rel_path, char *full_path, size_t full_path_len)
{
    int n;

    if (rel_path == NULL || full_path == NULL || full_path_len < 2) {
        return false;
    }

    if (rel_path[0] == '\0') {
        n = snprintf(full_path, full_path_len, "%s", mount_point);
    } else {
        n = snprintf(full_path, full_path_len, "%s/%s", mount_point, rel_path);
    }

    return n > 0 && n < (int)full_path_len;
}

static bool burner_build_assets_full_path(const char *rel_path, char *full_path, size_t full_path_len)
{
    int n;

    if (rel_path == NULL || full_path == NULL || full_path_len < 2) {
        return false;
    }

    if (rel_path[0] == '\0') {
        n = snprintf(full_path, full_path_len, "%s", assets_mount_point);
    } else {
        n = snprintf(full_path, full_path_len, "%s/%s", assets_mount_point, rel_path);
    }

    return n > 0 && n < (int)full_path_len;
}

bool burner_json_escape(const char *src, char *dst, size_t dst_len)
{
    size_t di = 0;

    if (src == NULL || dst == NULL || dst_len < 2) {
        return false;
    }

    for (size_t i = 0; src[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)src[i];
        const char *esc = NULL;
        char esc_buf[7] = {0};

        switch (ch) {
            case '\\':
                esc = "\\\\";
                break;
            case '\"':
                esc = "\\\"";
                break;
            case '\n':
                esc = "\\n";
                break;
            case '\r':
                esc = "\\r";
                break;
            case '\t':
                esc = "\\t";
                break;
            default:
                break;
        }

        if (esc != NULL) {
            size_t esc_len = strlen(esc);
            if (di + esc_len + 1 >= dst_len) {
                return false;
            }
            memcpy(dst + di, esc, esc_len);
            di += esc_len;
            continue;
        }

        if (ch < 0x20) {
            int n = snprintf(esc_buf, sizeof(esc_buf), "\\u%04x", ch);
            if (n <= 0 || di + (size_t)n + 1 >= dst_len) {
                return false;
            }
            memcpy(dst + di, esc_buf, (size_t)n);
            di += (size_t)n;
            continue;
        }

        if (di + 2 >= dst_len) {
            return false;
        }
        dst[di++] = (char)ch;
    }

    dst[di] = '\0';
    return true;
}

static char *burner_trim_inplace(char *text)
{
    char *start = text;
    char *end = NULL;

    if (text == NULL) {
        return NULL;
    }

    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }

    if (*start == '\0') {
        return start;
    }

    end = start + strlen(start) - 1;
    while (end >= start && isspace((unsigned char)*end)) {
        *end = '\0';
        if (end == start) {
            break;
        }
        end--;
    }

    return start;
}

static bool burner_ini_split_line(char *line, char **key, char **value)
{
    char *eq = NULL;
    char *trimmed = NULL;

    if (line == NULL || key == NULL || value == NULL) {
        return false;
    }

    if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
        (unsigned char)line[2] == 0xBF) {
        memmove(line, line + 3, strlen(line + 3) + 1);
    }

    trimmed = burner_trim_inplace(line);
    if (trimmed == NULL || trimmed[0] == '\0') {
        return false;
    }
    if (trimmed[0] == '#' || trimmed[0] == ';' || trimmed[0] == '[') {
        return false;
    }

    eq = strchr(trimmed, '=');
    if (eq == NULL) {
        return false;
    }
    *eq = '\0';

    *key = burner_trim_inplace(trimmed);
    *value = burner_trim_inplace(eq + 1);
    if (*key == NULL || *value == NULL || (*key)[0] == '\0') {
        return false;
    }

    if (((*value)[0] == '\"' || (*value)[0] == '\'') && strlen(*value) >= 2) {
        char quote = (*value)[0];
        size_t len = strlen(*value);
        if ((*value)[len - 1] == quote) {
            (*value)[len - 1] = '\0';
            (*value)++;
        }
    }

    return true;
}

static bool burner_lang_ini_name_valid(const char *name)
{
    size_t len = 0;

    if (name == NULL) {
        return false;
    }

    len = strlen(name);
    if (len == 0 || len >= WEB_LANG_FILE_NAME_MAX) {
        return false;
    }
    if (name[0] == '.') {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == '.')) {
            return false;
        }
    }

    {
        const char *ext = strrchr(name, '.');
        if (ext == NULL || strcasecmp(ext, ".ini") != 0) {
            return false;
        }
    }

    return true;
}

static void burner_lang_copy(char *dst, size_t dst_len, const char *src)
{
    if (dst == NULL || dst_len == 0) {
        return;
    }
    if (src == NULL) {
        src = "";
    }
    snprintf(dst, dst_len, "%s", src);
}

static void burner_lang_set_defaults(burner_lang_pack_t *lang)
{
    if (lang == NULL) {
        return;
    }

    burner_lang_copy(lang->page_title, sizeof(lang->page_title), "MORI Base Settings");
    burner_lang_copy(
        lang->page_header,
        sizeof(lang->page_header),
        "MORI Base Settings (Firmware Built-in)");
    burner_lang_copy(
        lang->page_tip,
        sizeof(lang->page_tip),
        "This page does not depend on TF and is always available for debug/recovery.");
    burner_lang_copy(lang->business_title, sizeof(lang->business_title), "Business Web (Built-in)");
    burner_lang_copy(lang->btn_open_business, sizeof(lang->btn_open_business), "Open Business Web");
    burner_lang_copy(
        lang->business_tip,
        sizeof(lang->business_tip),
        "Default source is built-in /assets/main.html; /sdcard/.web/main.html can override it.");
    burner_lang_copy(
        lang->recovery_title,
        sizeof(lang->recovery_title),
        "Business Web Recovery");
    burner_lang_copy(
        lang->recovery_tip,
        sizeof(lang->recovery_tip),
        "Upload one or more files to /sdcard/.web/ for recovery (main.html/app.js/styles.css, etc.).");
    burner_lang_copy(lang->btn_upload_main, sizeof(lang->btn_upload_main), "Upload to .web");
    burner_lang_copy(
        lang->system_migrate_title,
        sizeof(lang->system_migrate_title),
        "System Migration ZIP");
    burner_lang_copy(
        lang->system_migrate_tip,
        sizeof(lang->system_migrate_tip),
        "Export /sdcard/.setting + /sdcard/.web in one ZIP for TF card migration and recovery.");
    burner_lang_copy(
        lang->btn_system_migrate,
        sizeof(lang->btn_system_migrate),
        "Download Migration ZIP");
    burner_lang_copy(
        lang->system_deploy_title,
        sizeof(lang->system_deploy_title),
        "System Deploy ZIP");
    burner_lang_copy(
        lang->system_deploy_tip,
        sizeof(lang->system_deploy_tip),
        "Upload a migration ZIP to deploy its contents into /sdcard/.setting and /sdcard/.web.");
    burner_lang_copy(
        lang->btn_system_deploy,
        sizeof(lang->btn_system_deploy),
        "Deploy ZIP");
    burner_lang_copy(lang->firmware_title, sizeof(lang->firmware_title), "Firmware Full Upgrade");
    burner_lang_copy(
        lang->firmware_tip,
        sizeof(lang->firmware_tip),
        "OTA upload has been removed. Please use the full-image firmware package for upgrade. Do not upload .elf.");
    burner_lang_copy(
        lang->btn_upload_firmware,
        sizeof(lang->btn_upload_firmware),
        "Upload Firmware");
    burner_lang_copy(lang->firmware_idle, sizeof(lang->firmware_idle), "Idle");
    burner_lang_copy(lang->upload_idle, sizeof(lang->upload_idle), "Idle");
    burner_lang_copy(lang->usb_title, sizeof(lang->usb_title), "TF USB Pass-Through");
    burner_lang_copy(
        lang->usb_tip,
        sizeof(lang->usb_tip),
        "Enable: PC can access TF as USB disk. Disable: ESP can access TF/web APIs. When enabled, ESP TF APIs return 503 for safety.");
    burner_lang_copy(
        lang->btn_enable_usb,
        sizeof(lang->btn_enable_usb),
        "Enable USB Pass-Through");
    burner_lang_copy(
        lang->btn_disable_usb,
        sizeof(lang->btn_disable_usb),
        "Disable USB Pass-Through");
    burner_lang_copy(
        lang->btn_refresh_storage,
        sizeof(lang->btn_refresh_storage),
        "Refresh Storage Status");
    burner_lang_copy(lang->storage_loading, sizeof(lang->storage_loading), "Loading...");
    burner_lang_copy(lang->device_title, sizeof(lang->device_title), "Device Info");
    burner_lang_copy(
        lang->btn_refresh_device,
        sizeof(lang->btn_refresh_device),
        "Refresh Device Info");
    burner_lang_copy(lang->device_loading, sizeof(lang->device_loading), "Loading...");
    burner_lang_copy(lang->power_title, sizeof(lang->power_title), "Power Status");
    burner_lang_copy(
        lang->btn_refresh_power,
        sizeof(lang->btn_refresh_power),
        "Refresh Power Status");
    burner_lang_copy(lang->power_loading, sizeof(lang->power_loading), "Loading...");
    burner_lang_copy(
        lang->msg_select_main,
        sizeof(lang->msg_select_main),
        "Please select one or more files");
    burner_lang_copy(
        lang->msg_select_deploy_zip,
        sizeof(lang->msg_select_deploy_zip),
        "Please select a ZIP file");
    burner_lang_copy(
        lang->msg_select_firmware,
        sizeof(lang->msg_select_firmware),
        "Please select a .bin firmware file");
    burner_lang_copy(
        lang->msg_deploying_prefix,
        sizeof(lang->msg_deploying_prefix),
        "Deploying");
    burner_lang_copy(
        lang->msg_deploy_success_prefix,
        sizeof(lang->msg_deploy_success_prefix),
        "Deploy success");
    burner_lang_copy(
        lang->msg_deploy_failed_prefix,
        sizeof(lang->msg_deploy_failed_prefix),
        "Deploy failed");
    burner_lang_copy(
        lang->msg_uploading_firmware_prefix,
        sizeof(lang->msg_uploading_firmware_prefix),
        "Uploading firmware");
    burner_lang_copy(
        lang->msg_firmware_success_prefix,
        sizeof(lang->msg_firmware_success_prefix),
        "Firmware upload success, rebooting");
    burner_lang_copy(lang->msg_uploading_prefix, sizeof(lang->msg_uploading_prefix), "Uploading");
    burner_lang_copy(
        lang->msg_upload_success_prefix,
        sizeof(lang->msg_upload_success_prefix),
        "Upload success");
    burner_lang_copy(
        lang->msg_upload_failed_prefix,
        sizeof(lang->msg_upload_failed_prefix),
        "Upload failed");
    burner_lang_copy(
        lang->msg_storage_status_error_prefix,
        sizeof(lang->msg_storage_status_error_prefix),
        "Storage status error: ");
    burner_lang_copy(
        lang->msg_set_mode_error_prefix,
        sizeof(lang->msg_set_mode_error_prefix),
        "Set mode error: ");
    burner_lang_copy(
        lang->msg_device_info_error_prefix,
        sizeof(lang->msg_device_info_error_prefix),
        "Device info error: ");
    burner_lang_copy(
        lang->msg_power_status_error_prefix,
        sizeof(lang->msg_power_status_error_prefix),
        "Power status error: ");
    burner_lang_copy(lang->msg_applying, sizeof(lang->msg_applying), "Applying...");
    burner_lang_copy(lang->language_title, sizeof(lang->language_title), "Language");
    burner_lang_copy(
        lang->language_tip,
        sizeof(lang->language_tip),
        "Read available language INI files, choose one, then apply.");
    burner_lang_copy(
        lang->btn_read_lang_list,
        sizeof(lang->btn_read_lang_list),
        "Read INI List");
    burner_lang_copy(
        lang->btn_apply_language,
        sizeof(lang->btn_apply_language),
        "Apply Language");
    burner_lang_copy(lang->language_idle, sizeof(lang->language_idle), "Idle");
    burner_lang_copy(lang->language_loading, sizeof(lang->language_loading), "Loading...");
    burner_lang_copy(lang->language_none, sizeof(lang->language_none), "(No language ini)");
    burner_lang_copy(lang->ip5306_title, sizeof(lang->ip5306_title), "IP5306 Config (INI)");
    burner_lang_copy(
        lang->ip5306_tip,
        sizeof(lang->ip5306_tip),
        "Edit /sdcard/.setting/ip5306.ini. Save takes effect on next boot or power init.");
    burner_lang_copy(
        lang->btn_read_ip5306_ini,
        sizeof(lang->btn_read_ip5306_ini),
        "Read IP5306 INI");
    burner_lang_copy(
        lang->btn_save_ip5306_ini,
        sizeof(lang->btn_save_ip5306_ini),
        "Save IP5306 INI");
    burner_lang_copy(lang->ip5306_idle, sizeof(lang->ip5306_idle), "Idle");
    burner_lang_copy(lang->ip5306_loading, sizeof(lang->ip5306_loading), "Loading...");
    burner_lang_copy(
        lang->msg_lang_select_required,
        sizeof(lang->msg_lang_select_required),
        "Please select a language ini");
    burner_lang_copy(
        lang->msg_lang_list_error_prefix,
        sizeof(lang->msg_lang_list_error_prefix),
        "Language list error: ");
    burner_lang_copy(
        lang->msg_lang_apply_success_prefix,
        sizeof(lang->msg_lang_apply_success_prefix),
        "Language applied: ");
    burner_lang_copy(
        lang->msg_lang_apply_error_prefix,
        sizeof(lang->msg_lang_apply_error_prefix),
        "Language apply error: ");
    burner_lang_copy(
        lang->msg_ip5306_load_ok,
        sizeof(lang->msg_ip5306_load_ok),
        "IP5306 INI loaded");
    burner_lang_copy(
        lang->msg_ip5306_load_error_prefix,
        sizeof(lang->msg_ip5306_load_error_prefix),
        "IP5306 INI load error: ");
    burner_lang_copy(
        lang->msg_ip5306_save_ok_prefix,
        sizeof(lang->msg_ip5306_save_ok_prefix),
        "IP5306 INI saved, bytes=");
    burner_lang_copy(
        lang->msg_ip5306_save_error_prefix,
        sizeof(lang->msg_ip5306_save_error_prefix),
        "IP5306 INI save error: ");
    burner_lang_copy(lang->msg_upload_item_ok, sizeof(lang->msg_upload_item_ok), "[OK]");
    burner_lang_copy(lang->msg_upload_item_fail, sizeof(lang->msg_upload_item_fail), "[FAIL]");
    burner_lang_copy(lang->msg_http_error_prefix, sizeof(lang->msg_http_error_prefix), "HTTP error: ");
    burner_lang_copy(
        lang->msg_invalid_json_prefix,
        sizeof(lang->msg_invalid_json_prefix),
        "Invalid JSON: ");
}

static void burner_lang_apply_pair(burner_lang_pack_t *lang, const char *key, const char *value)
{
    if (lang == NULL || key == NULL || value == NULL) {
        return;
    }

#define BURNER_LANG_SET_FIELD(k, f) \
    if (strcmp(key, (k)) == 0) {     \
        burner_lang_copy(lang->f, sizeof(lang->f), value); \
        return;                      \
    }

    BURNER_LANG_SET_FIELD("page_title", page_title);
    BURNER_LANG_SET_FIELD("page_header", page_header);
    BURNER_LANG_SET_FIELD("page_tip", page_tip);
    BURNER_LANG_SET_FIELD("business_title", business_title);
    BURNER_LANG_SET_FIELD("btn_open_business", btn_open_business);
    BURNER_LANG_SET_FIELD("business_tip", business_tip);
    BURNER_LANG_SET_FIELD("recovery_title", recovery_title);
    BURNER_LANG_SET_FIELD("recovery_tip", recovery_tip);
    BURNER_LANG_SET_FIELD("btn_upload_main", btn_upload_main);
    BURNER_LANG_SET_FIELD("system_migrate_title", system_migrate_title);
    BURNER_LANG_SET_FIELD("system_migrate_tip", system_migrate_tip);
    BURNER_LANG_SET_FIELD("btn_system_migrate", btn_system_migrate);
    BURNER_LANG_SET_FIELD("system_deploy_title", system_deploy_title);
    BURNER_LANG_SET_FIELD("system_deploy_tip", system_deploy_tip);
    BURNER_LANG_SET_FIELD("btn_system_deploy", btn_system_deploy);
    BURNER_LANG_SET_FIELD("firmware_title", firmware_title);
    BURNER_LANG_SET_FIELD("firmware_tip", firmware_tip);
    BURNER_LANG_SET_FIELD("btn_upload_firmware", btn_upload_firmware);
    BURNER_LANG_SET_FIELD("firmware_idle", firmware_idle);
    BURNER_LANG_SET_FIELD("upload_idle", upload_idle);
    BURNER_LANG_SET_FIELD("usb_title", usb_title);
    BURNER_LANG_SET_FIELD("usb_tip", usb_tip);
    BURNER_LANG_SET_FIELD("btn_enable_usb", btn_enable_usb);
    BURNER_LANG_SET_FIELD("btn_disable_usb", btn_disable_usb);
    BURNER_LANG_SET_FIELD("btn_refresh_storage", btn_refresh_storage);
    BURNER_LANG_SET_FIELD("storage_loading", storage_loading);
    BURNER_LANG_SET_FIELD("device_title", device_title);
    BURNER_LANG_SET_FIELD("btn_refresh_device", btn_refresh_device);
    BURNER_LANG_SET_FIELD("device_loading", device_loading);
    BURNER_LANG_SET_FIELD("power_title", power_title);
    BURNER_LANG_SET_FIELD("btn_refresh_power", btn_refresh_power);
    BURNER_LANG_SET_FIELD("power_loading", power_loading);
    BURNER_LANG_SET_FIELD("msg_select_main", msg_select_main);
    BURNER_LANG_SET_FIELD("msg_select_deploy_zip", msg_select_deploy_zip);
    BURNER_LANG_SET_FIELD("msg_select_firmware", msg_select_firmware);
    BURNER_LANG_SET_FIELD("msg_deploying_prefix", msg_deploying_prefix);
    BURNER_LANG_SET_FIELD("msg_deploy_success_prefix", msg_deploy_success_prefix);
    BURNER_LANG_SET_FIELD("msg_deploy_failed_prefix", msg_deploy_failed_prefix);
    BURNER_LANG_SET_FIELD("msg_uploading_firmware_prefix", msg_uploading_firmware_prefix);
    BURNER_LANG_SET_FIELD("msg_firmware_success_prefix", msg_firmware_success_prefix);
    BURNER_LANG_SET_FIELD("msg_uploading_prefix", msg_uploading_prefix);
    BURNER_LANG_SET_FIELD("msg_upload_success_prefix", msg_upload_success_prefix);
    BURNER_LANG_SET_FIELD("msg_upload_failed_prefix", msg_upload_failed_prefix);
    BURNER_LANG_SET_FIELD("msg_storage_status_error_prefix", msg_storage_status_error_prefix);
    BURNER_LANG_SET_FIELD("msg_set_mode_error_prefix", msg_set_mode_error_prefix);
    BURNER_LANG_SET_FIELD("msg_device_info_error_prefix", msg_device_info_error_prefix);
    BURNER_LANG_SET_FIELD("msg_power_status_error_prefix", msg_power_status_error_prefix);
    BURNER_LANG_SET_FIELD("msg_applying", msg_applying);
    BURNER_LANG_SET_FIELD("language_title", language_title);
    BURNER_LANG_SET_FIELD("language_tip", language_tip);
    BURNER_LANG_SET_FIELD("btn_read_lang_list", btn_read_lang_list);
    BURNER_LANG_SET_FIELD("btn_apply_language", btn_apply_language);
    BURNER_LANG_SET_FIELD("language_idle", language_idle);
    BURNER_LANG_SET_FIELD("language_loading", language_loading);
    BURNER_LANG_SET_FIELD("language_none", language_none);
    BURNER_LANG_SET_FIELD("ip5306_title", ip5306_title);
    BURNER_LANG_SET_FIELD("ip5306_tip", ip5306_tip);
    BURNER_LANG_SET_FIELD("btn_read_ip5306_ini", btn_read_ip5306_ini);
    BURNER_LANG_SET_FIELD("btn_save_ip5306_ini", btn_save_ip5306_ini);
    BURNER_LANG_SET_FIELD("ip5306_idle", ip5306_idle);
    BURNER_LANG_SET_FIELD("ip5306_loading", ip5306_loading);
    BURNER_LANG_SET_FIELD("msg_lang_select_required", msg_lang_select_required);
    BURNER_LANG_SET_FIELD("msg_lang_list_error_prefix", msg_lang_list_error_prefix);
    BURNER_LANG_SET_FIELD("msg_lang_apply_success_prefix", msg_lang_apply_success_prefix);
    BURNER_LANG_SET_FIELD("msg_lang_apply_error_prefix", msg_lang_apply_error_prefix);
    BURNER_LANG_SET_FIELD("msg_ip5306_load_ok", msg_ip5306_load_ok);
    BURNER_LANG_SET_FIELD("msg_ip5306_load_error_prefix", msg_ip5306_load_error_prefix);
    BURNER_LANG_SET_FIELD("msg_ip5306_save_ok_prefix", msg_ip5306_save_ok_prefix);
    BURNER_LANG_SET_FIELD("msg_ip5306_save_error_prefix", msg_ip5306_save_error_prefix);
    BURNER_LANG_SET_FIELD("load_item_ok", msg_upload_item_ok);
    BURNER_LANG_SET_FIELD("msg_upload_item_ok", msg_upload_item_ok);
    BURNER_LANG_SET_FIELD("msg_upload_item_fail", msg_upload_item_fail);
    BURNER_LANG_SET_FIELD("msg_http_error_prefix", msg_http_error_prefix);
    BURNER_LANG_SET_FIELD("msg_invalid_json_prefix", msg_invalid_json_prefix);

#undef BURNER_LANG_SET_FIELD
}

static esp_err_t burner_lang_parse_system_ini(const char *full_path, burner_lang_meta_t *meta)
{
    FILE *fp = NULL;
    char line[WEB_LANG_LINE_MAX];

    if (full_path == NULL || meta == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fp = fopen(full_path, "rb");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        bool line_truncated = (len > 0 && len == sizeof(line) - 1 && line[len - 1] != '\n');
        char *key = NULL;
        char *value = NULL;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (burner_ini_split_line(line, &key, &value)) {
            if (strcmp(key, "language_version") == 0) {
                burner_lang_copy(meta->language_version, sizeof(meta->language_version), value);
            } else if (strcmp(key, "language_ini") == 0 && burner_lang_ini_name_valid(value)) {
                burner_lang_copy(meta->language_ini, sizeof(meta->language_ini), value);
            }
        }

        if (line_truncated) {
            int ch = 0;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {
            }
        }
    }

    if (ferror(fp)) {
        fclose(fp);
        return ESP_FAIL;
    }

    fclose(fp);
    return ESP_OK;
}

static esp_err_t burner_lang_parse_pack_ini(const char *full_path, burner_lang_pack_t *lang)
{
    FILE *fp = NULL;
    char line[WEB_LANG_LINE_MAX];

    if (full_path == NULL || lang == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fp = fopen(full_path, "rb");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        bool line_truncated = (len > 0 && len == sizeof(line) - 1 && line[len - 1] != '\n');
        char *key = NULL;
        char *value = NULL;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (burner_ini_split_line(line, &key, &value)) {
            burner_lang_apply_pair(lang, key, value);
        }

        if (line_truncated) {
            int ch = 0;
            while ((ch = fgetc(fp)) != EOF && ch != '\n') {
            }
        }
    }

    if (ferror(fp)) {
        fclose(fp);
        return ESP_FAIL;
    }

    fclose(fp);
    return ESP_OK;
}

static bool burner_lang_build_rel_path(const char *ini_name, char *rel_path, size_t rel_path_len)
{
    int n = 0;

    if (ini_name == NULL || rel_path == NULL || rel_path_len < 2) {
        return false;
    }
    if (!burner_lang_ini_name_valid(ini_name)) {
        return false;
    }

    n = snprintf(rel_path, rel_path_len, WEB_LANG_DIR_REL "/%s", ini_name);
    return n > 0 && n < (int)rel_path_len;
}

static bool burner_lang_try_load_ini(const char *ini_name, burner_lang_pack_t *lang)
{
    char rel_path[TF_PATH_LEN_MAX] = {0};
    char full_path[WEB_FILE_PATH_LEN_MAX] = {0};

    if (lang == NULL || ini_name == NULL) {
        return false;
    }
    if (!burner_lang_build_rel_path(ini_name, rel_path, sizeof(rel_path))) {
        return false;
    }
    if (!burner_build_full_path(rel_path, full_path, sizeof(full_path))) {
        return false;
    }

    return burner_lang_parse_pack_ini(full_path, lang) == ESP_OK;
}

void burner_lang_load(
    burner_lang_pack_t *lang,
    burner_lang_meta_t *meta,
    bool *system_loaded,
    bool *lang_loaded)
{
    char system_path[WEB_FILE_PATH_LEN_MAX] = {0};
    bool sys_ok = false;
    bool lang_ok = false;

    if (lang == NULL || meta == NULL) {
        return;
    }

    burner_lang_set_defaults(lang);
    burner_lang_copy(meta->language_version, sizeof(meta->language_version), "1");
    burner_lang_copy(meta->language_ini, sizeof(meta->language_ini), WEB_LANG_DEFAULT_INI);

    if (card == NULL || usb_msc_tf_in_use_by_host()) {
        if (system_loaded != NULL) {
            *system_loaded = false;
        }
        if (lang_loaded != NULL) {
            *lang_loaded = false;
        }
        return;
    }

    if (burner_build_full_path(WEB_LANG_SYSTEM_INI_REL, system_path, sizeof(system_path))) {
        sys_ok = (burner_lang_parse_system_ini(system_path, meta) == ESP_OK);
    }

    if (!burner_lang_ini_name_valid(meta->language_ini)) {
        burner_lang_copy(meta->language_ini, sizeof(meta->language_ini), WEB_LANG_DEFAULT_INI);
    }

    lang_ok = burner_lang_try_load_ini(meta->language_ini, lang);
    if (!lang_ok && strcmp(meta->language_ini, WEB_LANG_DEFAULT_INI) != 0) {
        burner_lang_copy(meta->language_ini, sizeof(meta->language_ini), WEB_LANG_DEFAULT_INI);
        lang_ok = burner_lang_try_load_ini(meta->language_ini, lang);
    }
    if (!lang_ok && strcmp(meta->language_ini, WEB_LANG_FALLBACK_EN_INI) != 0) {
        burner_lang_copy(meta->language_ini, sizeof(meta->language_ini), WEB_LANG_FALLBACK_EN_INI);
        lang_ok = burner_lang_try_load_ini(meta->language_ini, lang);
    }

    if (system_loaded != NULL) {
        *system_loaded = sys_ok;
    }
    if (lang_loaded != NULL) {
        *lang_loaded = lang_ok;
    }
}

static bool burner_lang_version_valid(const char *version)
{
    if (version == NULL || version[0] == '\0') {
        return false;
    }

    for (size_t i = 0; version[i] != '\0'; i++) {
        unsigned char ch = (unsigned char)version[i];
        if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == '.')) {
            return false;
        }
    }

    return true;
}

static bool burner_lang_is_reserved_ini(const char *name)
{
    if (name == NULL) {
        return true;
    }

    if (strcasecmp(name, "mori_system.ini") == 0) {
        return true;
    }
    if (strcasecmp(name, "ip5306.ini") == 0) {
        return true;
    }
    if (strcasecmp(name, "tca9555.ini") == 0) {
        return true;
    }

    return false;
}

static bool burner_lang_is_selectable_ini(const char *name)
{
    if (!burner_lang_ini_name_valid(name)) {
        return false;
    }
    return !burner_lang_is_reserved_ini(name);
}

static void burner_lang_load_meta_defaults(burner_lang_meta_t *meta)
{
    if (meta == NULL) {
        return;
    }

    burner_lang_copy(meta->language_version, sizeof(meta->language_version), "1");
    burner_lang_copy(meta->language_ini, sizeof(meta->language_ini), WEB_LANG_DEFAULT_INI);
}

static void burner_lang_load_meta_from_system(burner_lang_meta_t *meta)
{
    char system_path[WEB_FILE_PATH_LEN_MAX] = {0};

    if (meta == NULL) {
        return;
    }

    burner_lang_load_meta_defaults(meta);
    if (card == NULL || usb_msc_tf_in_use_by_host()) {
        return;
    }

    if (burner_build_full_path(WEB_LANG_SYSTEM_INI_REL, system_path, sizeof(system_path))) {
        (void)burner_lang_parse_system_ini(system_path, meta);
    }

    if (!burner_lang_version_valid(meta->language_version)) {
        burner_lang_copy(meta->language_version, sizeof(meta->language_version), "1");
    }
    if (!burner_lang_is_selectable_ini(meta->language_ini)) {
        burner_lang_copy(meta->language_ini, sizeof(meta->language_ini), WEB_LANG_DEFAULT_INI);
    }
}

static uint8_t burner_lang_ui_language_for_ini(const char *language_ini)
{
    if (language_ini != NULL && strcmp(language_ini, WEB_LANG_FALLBACK_EN_INI) == 0) {
        return UI_LANGUAGE_EN;
    }
    if (language_ini != NULL && strcmp(language_ini, WEB_LANG_DEFAULT_INI) == 0) {
        return UI_LANGUAGE_ZH;
    }
    return ui_get_language();
}

static esp_err_t burner_lang_write_system_ini(
    const char *language_ini,
    burner_lang_meta_t *saved_meta)
{
    burner_lang_meta_t meta = {0};
    uint8_t ui_language = UI_LANGUAGE_DEFAULT;
    char setting_path[WEB_FILE_PATH_LEN_MAX] = {0};
    struct stat st = {0};
    esp_err_t err;

    if (!burner_lang_is_selectable_ini(language_ini)) {
        return ESP_ERR_INVALID_ARG;
    }

    burner_lang_load_meta_from_system(&meta);
    burner_lang_copy(meta.language_ini, sizeof(meta.language_ini), language_ini);
    ui_language = burner_lang_ui_language_for_ini(meta.language_ini);

    if (!burner_build_full_path(WEB_LANG_DIR_REL, setting_path, sizeof(setting_path))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (mkdir(setting_path, 0775) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    if (stat(setting_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return ESP_FAIL;
    }

    err = mori_save_language_settings_to_system_ini(meta.language_ini, ui_language);
    if (err != ESP_OK) {
        return err;
    }

    if (saved_meta != NULL) {
        *saved_meta = meta;
    }
    ui_set_language(ui_language);
    return ESP_OK;
}

esp_err_t burner_lang_list_handler(httpd_req_t *req)
{
    char setting_path[WEB_FILE_PATH_LEN_MAX] = {0};
    burner_lang_meta_t meta = {0};
    char esc_current[WEB_LANG_FILE_NAME_MAX * 2 + 16] = {0};
    DIR *dir = NULL;
    struct dirent *entry = NULL;
    bool first = true;
    esp_err_t send_err = ESP_OK;
    char head[256] = {0};
    int n = 0;

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }

    burner_lang_load_meta_from_system(&meta);
    if (!burner_json_escape(meta.language_ini, esc_current, sizeof(esc_current))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }
    if (!burner_build_full_path(WEB_LANG_DIR_REL, setting_path, sizeof(setting_path))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "path build failed");
    }

    dir = opendir(setting_path);
    if (dir == NULL) {
        char resp[256] = {0};
        n = snprintf(
            resp,
            sizeof(resp),
            "{\"ok\":false,\"current\":\"%s\",\"files\":[],\"message\":\"open .setting failed\"}",
            esc_current);
        if (n <= 0 || n >= (int)sizeof(resp)) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
        }
        return burner_send_json(req, resp);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    n = snprintf(head, sizeof(head), "{\"ok\":true,\"current\":\"%s\",\"files\":[", esc_current);
    if (n <= 0 || n >= (int)sizeof(head)) {
        closedir(dir);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    send_err = httpd_resp_send_chunk(req, head, n);
    while (send_err == ESP_OK && (entry = readdir(dir)) != NULL) {
        char esc_name[WEB_LANG_FILE_NAME_MAX * 2 + 16] = {0};
        char line[WEB_LANG_FILE_NAME_MAX * 2 + 24] = {0};

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!burner_lang_is_selectable_ini(entry->d_name)) {
            continue;
        }
        if (!burner_json_escape(entry->d_name, esc_name, sizeof(esc_name))) {
            continue;
        }
        n = snprintf(line, sizeof(line), "%s\"%s\"", first ? "" : ",", esc_name);
        if (n <= 0 || n >= (int)sizeof(line)) {
            continue;
        }
        send_err = httpd_resp_send_chunk(req, line, n);
        if (send_err == ESP_OK) {
            first = false;
        }
    }

    closedir(dir);
    if (send_err == ESP_OK) {
        send_err = httpd_resp_send_chunk(req, "]}", 2);
    }
    if (send_err == ESP_OK) {
        send_err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return send_err;
}

esp_err_t burner_lang_apply_handler(httpd_req_t *req)
{
    char ini_name[WEB_LANG_FILE_NAME_MAX] = {0};
    char body[WIFI_JSON_BODY_MAX] = {0};
    burner_lang_pack_t *probe = NULL;
    burner_lang_meta_t saved_meta = {0};
    char esc_version[WEB_LANG_VERSION_MAX * 2 + 16] = {0};
    char esc_ini[WEB_LANG_FILE_NAME_MAX * 2 + 16] = {0};
    char resp[256] = {0};
    esp_err_t err = ESP_OK;
    int n = 0;

    {
        esp_err_t access_err = burner_reject_if_tf_busy(req);
        if (access_err != ESP_OK) {
            return access_err;
        }
    }

    if (card == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TF card not ready");
    }

    if (!burner_get_query_arg(req, "ini", ini_name, sizeof(ini_name), false)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ini query");
    }

    if (ini_name[0] == '\0' && req->content_len > 0) {
        err = burner_read_request_body(req, body, sizeof(body), NULL);
        if (err == ESP_OK) {
            (void)burner_json_get_string(body, "ini", ini_name, sizeof(ini_name));
        }
    }

    if (ini_name[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ini");
    }
    if (!burner_lang_is_selectable_ini(ini_name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid language ini");
    }

    probe = (burner_lang_pack_t *)calloc(1, sizeof(*probe));
    if (probe == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    if (!burner_lang_try_load_ini(ini_name, probe)) {
        free(probe);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "language ini not found");
    }
    free(probe);

    err = burner_lang_write_system_ini(ini_name, &saved_meta);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write system ini failed");
    }

    if (!burner_json_escape(saved_meta.language_version, esc_version, sizeof(esc_version)) ||
        !burner_json_escape(saved_meta.language_ini, esc_ini, sizeof(esc_ini))) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    n = snprintf(
        resp,
        sizeof(resp),
        "{\"ok\":true,\"language_version\":\"%s\",\"language_ini\":\"%s\"}",
        esc_version,
        esc_ini);
    if (n <= 0 || n >= (int)sizeof(resp)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json encode failed");
    }

    return burner_send_json(req, resp);
}

esp_err_t burner_send_lang_string_chunk(
    httpd_req_t *req,
    const char *key,
    const char *value,
    bool trailing_comma)
{
    char esc_value[WEB_LANG_TEXT_MAX * 2 + 32] = {0};
    char line[WEB_LANG_TEXT_MAX * 2 + 96] = {0};
    int n = 0;

    if (req == NULL || key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (value == NULL) {
        value = "";
    }
    if (!burner_json_escape(value, esc_value, sizeof(esc_value))) {
        return ESP_ERR_INVALID_SIZE;
    }

    n = snprintf(line, sizeof(line), "\"%s\":\"%s\"%s", key, esc_value, trailing_comma ? "," : "");
    if (n <= 0 || n >= (int)sizeof(line)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return httpd_resp_send_chunk(req, line, n);
}

const char *burner_basename(const char *path)
{
    const char *slash = NULL;

    if (path == NULL || path[0] == '\0') {
        return "";
    }

    slash = strrchr(path, '/');
    if (slash == NULL) {
        return path;
    }

    return slash + 1;
}

esp_err_t burner_remove_recursive(const char *full_path)
{
    struct stat st;

    if (full_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (stat(full_path, &st) != 0) {
        return ESP_FAIL;
    }

    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(full_path);
        struct dirent *entry;

        if (dir == NULL) {
            return ESP_FAIL;
        }

        while ((entry = readdir(dir)) != NULL) {
            char child_path[TF_PATH_LEN_MAX + 64];
            int n;

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            n = snprintf(child_path, sizeof(child_path), "%s/%s", full_path, entry->d_name);
            if (n <= 0 || n >= (int)sizeof(child_path)) {
                closedir(dir);
                return ESP_ERR_INVALID_SIZE;
            }

            if (burner_remove_recursive(child_path) != ESP_OK) {
                closedir(dir);
                return ESP_FAIL;
            }
        }

        closedir(dir);
        if (rmdir(full_path) != 0) {
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    if (unlink(full_path) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t burner_mkdirs_rel(const char *rel_path)
{
    char rel_copy[TF_PATH_LEN_MAX] = {0};
    char current_rel[TF_PATH_LEN_MAX] = {0};
    char full_path[TF_PATH_LEN_MAX + 64] = {0};
    char *ctx = NULL;
    char *token = NULL;

    if (rel_path == NULL || rel_path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(rel_path) >= sizeof(rel_copy)) {
        return ESP_ERR_INVALID_SIZE;
    }

    snprintf(rel_copy, sizeof(rel_copy), "%s", rel_path);
    token = strtok_r(rel_copy, "/", &ctx);
    while (token != NULL) {
        char next_rel[TF_PATH_LEN_MAX] = {0};
        struct stat st;

        if (current_rel[0] == '\0') {
            if (snprintf(next_rel, sizeof(next_rel), "%s", token) >= (int)sizeof(next_rel)) {
                return ESP_ERR_INVALID_SIZE;
            }
        } else {
            if (snprintf(next_rel, sizeof(next_rel), "%s/%s", current_rel, token) >= (int)sizeof(next_rel)) {
                return ESP_ERR_INVALID_SIZE;
            }
        }

        snprintf(current_rel, sizeof(current_rel), "%s", next_rel);

        if (!burner_build_full_path(current_rel, full_path, sizeof(full_path))) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (mkdir(full_path, 0775) != 0) {
            if (errno != EEXIST) {
                return ESP_FAIL;
            }
            if (stat(full_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
                return ESP_FAIL;
            }
        }

        token = strtok_r(NULL, "/", &ctx);
    }

    return ESP_OK;
}

esp_err_t burner_ensure_rom_dir(void)
{
    struct stat st;
    if (stat(ROM_DIR_PATH, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }

    if (mkdir(ROM_DIR_PATH, 0775) == 0) {
        return ESP_OK;
    }

    if (errno == EEXIST) {
        return ESP_OK;
    }

    return ESP_FAIL;
}

#include "burn/burner_gbc_lowlevel.c"

#include "burn/burner_gbx_profile.c"

#include "burn/burner_gba_lowlevel.c"

#include "burn/burner_gba_gbx_lowlevel.c"

#include "burn/burner_gbc_flash_lowlevel.c"

#include "burn/burner_gba_flash_lowlevel.c"

#include "burn/burner_cart_runtime.c"

#include "burn/burner_burn_common.c"
#include "burn/burner_gbc_burn.c"
#include "burn/burner_gba_burn.c"

#include "burn/burner_cart_dispatch.c"

esp_err_t burner_reject_if_tf_busy(httpd_req_t *req)
{
    if (usb_msc_tf_in_use_by_host()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(
            req,
            "TF storage is in USB pass-through mode. Open / and disable USB pass-through first.");
    }
    return ESP_OK;
}

static const char *burner_guess_content_type(const char *path)
{
    const char *ext = NULL;

    if (path == NULL) {
        return "application/octet-stream";
    }

    ext = strrchr(path, '.');
    if (ext == NULL) {
        return "application/octet-stream";
    }

    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcasecmp(ext, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcasecmp(ext, ".js") == 0) {
        return "application/javascript; charset=utf-8";
    }
    if (strcasecmp(ext, ".json") == 0) {
        return "application/json; charset=utf-8";
    }
    if (strcasecmp(ext, ".svg") == 0) {
        return "image/svg+xml";
    }
    if (strcasecmp(ext, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(ext, ".gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(ext, ".ico") == 0) {
        return "image/x-icon";
    }
    if (strcasecmp(ext, ".txt") == 0) {
        return "text/plain; charset=utf-8";
    }
    if (strcasecmp(ext, ".woff") == 0) {
        return "font/woff";
    }
    if (strcasecmp(ext, ".woff2") == 0) {
        return "font/woff2";
    }

    return "application/octet-stream";
}

static bool burner_strip_uri_suffix(const char *uri, char *out, size_t out_len)
{
    size_t i = 0;

    if (uri == NULL || out == NULL || out_len < 2) {
        return false;
    }

    while (uri[i] != '\0' && uri[i] != '?' && uri[i] != '#') {
        if (i + 1 >= out_len) {
            return false;
        }
        out[i] = uri[i];
        i++;
    }

    out[i] = '\0';
    return true;
}

bool burner_uri_to_web_rel_path(const char *uri, char *rel_path, size_t rel_path_len)
{
    char uri_path[TF_QUERY_LEN_MAX] = {0};
    char decoded[WEB_FILE_PATH_LEN_MAX] = {0};
    char normalized[WEB_FILE_PATH_LEN_MAX] = {0};
    const char *raw_rel = NULL;
    const char *dot = NULL;

    if (uri == NULL || rel_path == NULL || rel_path_len < 2) {
        return false;
    }

    if (!burner_strip_uri_suffix(uri, uri_path, sizeof(uri_path))) {
        return false;
    }

    if (strcmp(uri_path, "/") == 0 || strcmp(uri_path, "/tf") == 0 || strcmp(uri_path, "/tf/") == 0 ||
        strcmp(uri_path, "/cart") == 0 || strcmp(uri_path, "/cart/") == 0 || strcmp(uri_path, "/burner") == 0 ||
        strcmp(uri_path, "/burner/") == 0 || strcmp(uri_path, "/settings") == 0 ||
        strcmp(uri_path, "/settings/") == 0 || strcmp(uri_path, "/smb") == 0 || strcmp(uri_path, "/smb/") == 0 ||
        strcmp(uri_path, "/wifi") == 0 || strcmp(uri_path, "/wifi/") == 0 || strcmp(uri_path, "/power") == 0 ||
        strcmp(uri_path, "/power/") == 0 || strcmp(uri_path, "/book") == 0 || strcmp(uri_path, "/book/") == 0) {
        return snprintf(rel_path, rel_path_len, "%s", WEB_MAIN_FILE_REL) < (int)rel_path_len;
    }

    if (strncmp(uri_path, "/api/", 5) == 0) {
        return false;
    }

    raw_rel = uri_path;
    while (*raw_rel == '/') {
        raw_rel++;
    }

    if (*raw_rel == '\0') {
        return snprintf(rel_path, rel_path_len, "%s", WEB_MAIN_FILE_REL) < (int)rel_path_len;
    }

    if (!burner_url_decode(raw_rel, decoded, sizeof(decoded))) {
        return false;
    }

    if (!burner_normalize_rel_path(decoded, normalized, sizeof(normalized), false)) {
        return false;
    }

    dot = strrchr(normalized, '.');
    if (dot == NULL && strstr(normalized, "/.") == NULL) {
        return snprintf(rel_path, rel_path_len, "%s", WEB_MAIN_FILE_REL) < (int)rel_path_len;
    }

    if (strcmp(normalized, WEB_ROOT_DIR_REL) == 0) {
        return snprintf(rel_path, rel_path_len, "%s", WEB_MAIN_FILE_REL) < (int)rel_path_len;
    }

    if (strncmp(normalized, WEB_ROOT_DIR_REL "/", strlen(WEB_ROOT_DIR_REL) + 1) == 0) {
        return snprintf(rel_path, rel_path_len, "%s", normalized) < (int)rel_path_len;
    }

    return snprintf(rel_path, rel_path_len, WEB_ROOT_DIR_REL "/%s", normalized) < (int)rel_path_len;
}

esp_err_t burner_send_static_file(httpd_req_t *req, const char *rel_path)
{
    char full_path[WEB_FILE_PATH_LEN_MAX] = {0};
    char assets_full_path[WEB_FILE_PATH_LEN_MAX] = {0};
    struct stat st;
    FILE *fp = NULL;
    uint8_t *buf = NULL;
    const char *content_type = NULL;
    const char *assets_rel_path = rel_path;
    bool path_ready = false;

    if (req == NULL || rel_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (card != NULL && !usb_msc_tf_in_use_by_host()) {
        if (!burner_build_full_path(rel_path, full_path, sizeof(full_path))) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "web file path too long");
        }
        if (stat(full_path, &st) == 0 && !S_ISDIR(st.st_mode)) {
            path_ready = true;
        }
    }

    if (!path_ready) {
        if (strcmp(rel_path, WEB_MAIN_FILE_REL) == 0) {
            assets_rel_path = WEB_BUILTIN_MAIN_FILE_REL;
        }
        if (!burner_build_assets_full_path(assets_rel_path, assets_full_path, sizeof(assets_full_path))) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "web file path too long");
        }
        if (stat(assets_full_path, &st) == 0 && !S_ISDIR(st.st_mode)) {
            if (snprintf(full_path, sizeof(full_path), "%s", assets_full_path) >= (int)sizeof(full_path)) {
                return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "web file path too long");
            }
            path_ready = true;
        }
    }

    if (!path_ready) {
        ESP_LOGW(
            BURNER_TAG,
            "web file not found: uri=%s rel=%s assets_rel=%s",
            req->uri,
            rel_path,
            assets_rel_path);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "web file not found");
    }

    fp = fopen(full_path, "rb");
    if (fp == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open web file failed");
    }

    buf = (uint8_t *)malloc(TF_IO_CHUNK_SIZE);
    if (buf == NULL) {
        fclose(fp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }

    content_type = burner_guess_content_type(rel_path);
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    while (true) {
        size_t read_len = fread(buf, 1, TF_IO_CHUNK_SIZE, fp);
        if (read_len == 0) {
            break;
        }
        web_ws_mark_network_activity();
        if (httpd_resp_send_chunk(req, (const char *)buf, read_len) != ESP_OK) {
            free(buf);
            fclose(fp);
            return ESP_FAIL;
        }
    }

    if (ferror(fp)) {
        free(buf);
        fclose(fp);
        return ESP_FAIL;
    }

    free(buf);
    fclose(fp);
    return httpd_resp_send_chunk(req, NULL, 0);
}
