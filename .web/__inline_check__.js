
        // 全局状态
        const state = {
            currentPath: '',
            currentPage: 'tf',
            burnStatus: null,
            statusInterval: null,
            burnStatusInterval: null,
            usbPassThrough: false,
            uploadXHR: null,
            firmwareUploadXHR: null,
            firmwareCancelPending: false,
            systemDeployBusy: false,
            systemDeployCancelPending: false,
            uploadCancelPending: false,
            multiUploadCancelPending: false,
            uploadStartTime: null,
            uploadStats: {
                loaded: 0,
                total: 0,
                speed: 0,
                lastTime: 0,
                lastLoaded: 0
            },
            selectedFile: null,
            selectedFiles: new Set(),
            ws: null,
            wsReconnectTimer: null,
            powerData: null,
            wifiList: [],
            multiUploads: new Map(),
            multiUploadStatsTimer: null,
            powerAutoRefresh: true,
            powerRefreshInterval: 2000,
            powerRefreshTimer: null,
            powerViewMode: 'basic',
            cartMode: 'gba',
            writePathMode: 'direct',
            psramWindowMb: 4,
            mbc5ChunkKb: 16,
            romFile: null,
            saveFile: null,
            activeBurnOperation: null,
            pendingDumpRelPath: '',
            pendingDumpName: '',
            pendingDumpAutoDownloaded: false,
            lastBurnMessage: '',
            burnCancelPending: false
        };

        // 页面渲染函数
        const pages = {
            system: renderSystemPage,
            tf: renderTFPage,
            wifi: renderWiFiPage,
            power: renderPowerPage,
            burner: renderBurnerPage,
            chip: renderChipPage,
            settings: renderSettingsPage
        };

        // 初始化
        document.addEventListener('DOMContentLoaded', () => {
            initRouter();
            loadPage(getPageFromHash());
            initWebSocket();
            startStatusPolling();
            initClock();
            
            // 测试移动端菜单按钮
            console.log('Page loaded, testing mobile menu button');
            const mobileMenuBtn = document.getElementById('mobileMenuBtn');
            if (mobileMenuBtn) {
                console.log('Mobile menu button found:', mobileMenuBtn);
                // 添加额外的事件监听器进行测试
                mobileMenuBtn.addEventListener('click', function() {
                    console.log('Mobile menu button clicked via addEventListener');
                });
                mobileMenuBtn.addEventListener('touchstart', function(e) {
                    console.log('Mobile menu button touched:', e);
                    e.preventDefault();
                    toggleMobileSidebar();
                });
            } else {
                console.error('Mobile menu button not found');
            }
            
            // 面板拖拽恢复
            document.getElementById('uploadModal').addEventListener('click', (e) => {
                if (e.target === document.getElementById('uploadModal') && 
                    document.getElementById('uploadModal').classList.contains('minimized')) {
                    restoreModal();
                }
            });
            
            document.querySelector('#uploadModal .modal').addEventListener('click', (e) => {
                if (document.getElementById('uploadModal').classList.contains('minimized') && 
                    !e.target.closest('button')) {
                    restoreModal();
                }
            });
        });

        function initClock() {
            const clockElement = document.getElementById('sidebarClock');
            if (!clockElement) return;
            
            function updateClock() {
                const now = new Date();
                const hours = String(now.getHours()).padStart(2, '0');
                const minutes = String(now.getMinutes()).padStart(2, '0');
                const seconds = String(now.getSeconds()).padStart(2, '0');
                clockElement.textContent = `${hours}:${minutes}:${seconds}`;
            }
            
            updateClock();
            setInterval(updateClock, 1000);
        }

        // WebSocket 初始化
        function initWebSocket() {
            // Current firmware build does not expose a websocket endpoint.
            // Rely on HTTP polling instead.
        }

        function handleWebSocketMessage(data) {
            switch(data.topic) {
                case 'burn':
                    updateBurnStatus(data.payload);
                    break;
                case 'power':
                    updatePowerStatus(data.payload);
                    break;
                case 'wifi':
                    updateWiFiStatus(data.payload);
                    break;
            }
        }

        async function refreshStatus() {
            try {
                const [powerReq, storageReq] = await Promise.allSettled([
                    fetch('/api/power/status'),
                    fetch('/api/storage/status')
                ]);
                
                if (powerReq.status === 'fulfilled' && powerReq.value.ok) {
                    const powerData = await powerReq.value.json();
                    state.powerData = powerData;
                    updatePowerStatus(powerData);
                }
                
                if (storageReq.status === 'fulfilled' && storageReq.value.ok) {
                    const storageData = await storageReq.value.json();
                    state.usbPassThrough = storageData.usb_passthrough_enabled;
                }
            } catch (error) {
                console.error('Status refresh failed:', error);
            }
        }

        function startStatusPolling() {
            if (state.statusInterval) {
                clearInterval(state.statusInterval);
            }
            
            refreshStatus();
            
            state.statusInterval = setInterval(() => {
                refreshStatus();
            }, 2000);
        }

        function updatePowerStatus(data) {
            state.powerData = data;
            const ip5306 = (data && data.ip5306) ? data.ip5306 : {};
            
            const batteryPercent = estimateBatteryPercent(ip5306);
            
            const sidebarBattery = document.getElementById('sidebarBattery');
            const sidebarChargeState = document.getElementById('sidebarChargeState');
            if (sidebarBattery) sidebarBattery.textContent = batteryPercent + '%';
            
            const chargeState = ip5306.charge_state || 'unknown';
            const stateMap = {
                'charging': '⚡ 充电中',
                'charge_full': '✓ 已充满',
                'discharging': '🔋 放电中',
                'discharging_light_load': '💤 轻载',
                'no_battery_external_power': '🔌 仅外部供电(无电池)',
                'unknown': '❓ 未知'
            };
            if (sidebarChargeState) sidebarChargeState.textContent = stateMap[chargeState] || chargeState;
        }

        function parseTelemetryNumber(value, fallback = 0) {
            if (typeof value === 'number' && Number.isFinite(value)) {
                return value;
            }

            if (typeof value === 'string') {
                const trimmed = value.trim();
                if (!trimmed) return fallback;

                const parsed = trimmed.startsWith('0x') || trimmed.startsWith('0X')
                    ? parseInt(trimmed, 16)
                    : Number(trimmed);

                if (Number.isFinite(parsed)) {
                    return parsed;
                }
            }

            return fallback;
        }

        function estimateBatteryPercent(ip5306) {
            if (!ip5306 || typeof ip5306 !== 'object') return 0;

            // 优先使用固件返回的 battery_level_percent。
            const reported = parseTelemetryNumber(ip5306.battery_level_percent, -1);
            if (ip5306.battery_level_percent_ok !== false && reported >= 0) {
                return Math.max(0, Math.min(100, Math.round(reported)));
            }

            // 兜底：仅在无百分比时给出确定性状态值，避免随机跳动。
            if (ip5306.charge_full_flag) return 100;
            if (ip5306.charge_state === 'charging') return 50;
            return 0;
        }

        // 路由处理
        function initRouter() {
            window.addEventListener('hashchange', () => {
                loadPage(getPageFromHash());
            });

            document.querySelectorAll('.nav-item').forEach(item => {
                item.addEventListener('click', (e) => {
                    document.querySelectorAll('.nav-item').forEach(i => i.classList.remove('active'));
                    e.currentTarget.classList.add('active');
                });
            });
        }

        function getPageFromHash() {
            const hash = window.location.hash.slice(2) || 'system';
            return pages[hash] ? hash : 'system';
        }

        function loadPage(pageName) {
            // 如果离开电源页面，停止电源页面的自动刷新
            if (state.currentPage === 'power' && pageName !== 'power') {
                stopPowerAutoRefresh();
            }

            state.currentPage = pageName;
            const render = pages[pageName];
            if (render) {
                document.getElementById('mainContent').innerHTML = '';
                render();
            }
        }

        // TF 文件管理页面（支持多选）
        function renderTFPage() {
            const container = document.getElementById('mainContent');
            container.innerHTML = `
                <div class="page-header animate-in">
                    <h1 class="page-title">📁 TF 文件管理</h1>
                    <p class="page-subtitle">管理存储卡中的文件和目录</p>
                </div>

                <div class="multi-select-bar" id="multiSelectBar">
                    <span class="multi-select-count">已选择 0 项</span>
                    <button class="btn btn-sm" onclick="downloadSelected()">⬇️ 下载</button>
                    <button class="btn btn-sm btn-danger" onclick="deleteSelected()">🗑️ 删除</button>
                    <button class="btn btn-sm" onclick="clearSelection()">✕ 取消</button>
                </div>

                <div class="card animate-in">
                    <div class="file-toolbar">
                        <button class="btn btn-primary" onclick="showMultiUploadModal()">
                            <span>⬆️</span> 上传文件
                        </button>
                        <button class="btn" onclick="showMkdirModal()">
                            <span>📂</span> 新建文件夹
                        </button>
                        <button class="btn" onclick="refreshFileList()" id="refreshBtn">
                            <span>🔄</span> 刷新
                        </button>
                    </div>

                    <div class="breadcrumb" id="breadcrumb">
                        <span class="breadcrumb-item" onclick="navigateTo('')">根目录</span>
                    </div>

                    <div class="file-list" id="fileList">
                        <div style="text-align: center; padding: 40px; color: var(--text-secondary);">
                            <div class="spinner" style="margin: 0 auto 16px;"></div>
                            加载中...
                        </div>
                    </div>
                </div>
            `;
            
            loadFileList('');
            setupTfFileListDragDrop();
        }

        function setupTfFileListDragDrop() {
            const fileList = document.getElementById('fileList');
            if (!fileList) return;

            const hasFiles = (event) => {
                const types = event && event.dataTransfer ? event.dataTransfer.types : null;
                return !!(types && Array.from(types).includes('Files'));
            };

            fileList.addEventListener('dragover', (e) => {
                if (!hasFiles(e)) return;
                e.preventDefault();
                if (e.dataTransfer) {
                    e.dataTransfer.dropEffect = 'copy';
                }
                fileList.classList.add('dragover');
            });

            fileList.addEventListener('dragleave', (e) => {
                if (fileList.contains(e.relatedTarget)) return;
                fileList.classList.remove('dragover');
            });

            fileList.addEventListener('drop', (e) => {
                if (!hasFiles(e)) return;
                e.preventDefault();
                fileList.classList.remove('dragover');
                if (e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files.length > 0) {
                    showMultiUploadModal();
                    startMultiUpload(e.dataTransfer.files);
                }
            });
        }

        // Wi-Fi 管理页面
        function renderWiFiPage() {
            const container = document.getElementById('mainContent');
            container.innerHTML = `
                <div class="page-header animate-in">
                    <h1 class="page-title">📶 Wi-Fi 管理</h1>
                    <p class="page-subtitle">配置网络连接</p>
                </div>

                <div class="card animate-in" id="wifiCurrentCard">
                    <div class="card-header">
                        <h3 class="card-title">当前连接</h3>
                        <span class="ws-indicator connecting" id="wifiStatusIndicator">
                            <span class="ws-dot"></span>获取中...
                        </span>
                    </div>
                    <div id="wifiCurrentStatus">
                        <div style="text-align: center; padding: 40px; color: var(--text-secondary);">
                            <div class="spinner" style="margin: 0 auto;"></div>
                        </div>
                    </div>
                </div>

                <div class="card animate-in">
                    <div class="card-header">
                        <h3 class="card-title">可用网络</h3>
                        <button class="btn btn-primary" onclick="scanWiFi()">
                            <span>🔍</span> 扫描
                        </button>
                    </div>
                    <div class="wifi-list" id="wifiList">
                        <div style="text-align: center; padding: 40px; color: var(--text-secondary);">
                            <div style="font-size: 48px; margin-bottom: 16px;">📡</div>
                            点击上方"扫描"按钮搜索可用网络
                        </div>
                    </div>
                </div>


            `;

            loadWiFiStatus();
        }

        // 电源状态页面
        function renderPowerPage() {
            const container = document.getElementById('mainContent');
            container.innerHTML = `
                <div class="page-header animate-in">
                    <h1 class="page-title">🔋 电源状态</h1>
                    <p class="page-subtitle">实时监控与诊断</p>
                </div>

                <div class="card animate-in" style="margin-bottom: 24px;">
                    <div style="display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; gap: 16px;">
                        <div style="display: flex; align-items: center; gap: 16px;">
                            <span style="color: var(--text-secondary);">自动刷新:</span>
                            <button class="panel-btn ${state.powerAutoRefresh ? 'active' : ''}" id="powerAutoRefreshBtn" onclick="togglePowerAutoRefresh()">
                                <span>${state.powerAutoRefresh ? '⏸️' : '▶️'}</span> ${state.powerAutoRefresh ? '开启' : '暂停'}
                            </button>
                        </div>
                        <div style="display: flex; align-items: center; gap: 12px;">
                            <span style="color: var(--text-secondary);">刷新间隔:</span>
                            <select class="input" id="powerRefreshInterval" onchange="changePowerRefreshInterval()" style="width: 120px; padding: 8px 12px;">
                                <option value="1000" ${state.powerRefreshInterval === 1000 ? 'selected' : ''}>1秒</option>
                                <option value="2000" ${state.powerRefreshInterval === 2000 ? 'selected' : ''}>2秒</option>
                                <option value="5000" ${state.powerRefreshInterval === 5000 ? 'selected' : ''}>5秒</option>
                                <option value="10000" ${state.powerRefreshInterval === 10000 ? 'selected' : ''}>10秒</option>
                            </select>
                        </div>
                    </div>
                </div>

                <div class="power-grid animate-in" id="powerGrid">
                    <div class="power-card battery">
                        <div class="status-label">电池电量 (估算)</div>
                        <div class="power-value" id="powerBattery">--%</div>
                        <div class="heap-bar">
                            <div class="heap-fill" id="powerBatteryBar" style="width: 0%"></div>
                        </div>
                    </div>
                    
                    <div class="power-card">
                        <div class="status-label">充电状态</div>
                        <div class="power-value" id="powerChargeState" style="font-size: 24px;">--</div>
                        <div style="margin-top: 8px; font-size: 13px; color: var(--text-secondary);">
                            固定电流: <span id="powerCurrent">--</span>mA
                        </div>
                    </div>
                    
                    <div class="power-card">
                        <div class="status-label">运行时间</div>
                        <div class="power-value" id="powerUptime" style="font-size: 28px;">--:--:--</div>
                        <div style="margin-top: 8px; font-size: 13px; color: var(--text-secondary);">
                            系统启动至今
                        </div>
                    </div>
                    
                    <div class="power-card">
                        <div class="status-label">内存使用</div>
                        <div class="power-value" id="powerHeap">--</div>
                        <div class="heap-bar">
                            <div class="heap-fill" id="powerHeapBar" style="width: 0%"></div>
                        </div>
                    </div>
                </div>

                <div class="card animate-in">
                    <div class="card-header">
                        <h3 class="card-title">⚡ 电源管理</h3>
                        <div style="display: flex; gap: 8px;">
                            <button class="btn btn-sm ${state.powerViewMode === 'basic' ? 'active' : ''}" id="powerBasicBtn" onclick="switchPowerView('basic')">功能</button>
                            <button class="btn btn-sm ${state.powerViewMode === 'advanced' ? 'active' : ''}" id="powerAdvancedBtn" onclick="switchPowerView('advanced')">寄存器</button>
                        </div>
                    </div>
                    <div id="powerBasicView">
                        <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(280px, 1fr)); gap: 16px; padding: 16px;">
                            <div style="background: var(--bg-tertiary); padding: 16px; border-radius: 8px; border: 1px solid var(--border-color);">
                                <div style="font-size: 14px; color: var(--text-secondary); margin-bottom: 8px;">充电状态</div>
                                <div style="font-size: 18px; font-weight: 600; color: var(--text-primary);" id="basicChargeState">--</div>
                            </div>
                            <div style="background: var(--bg-tertiary); padding: 16px; border-radius: 8px; border: 1px solid var(--border-color);">
                                <div style="font-size: 14px; color: var(--text-secondary); margin-bottom: 8px;">充电电流</div>
                                <div style="font-size: 18px; font-weight: 600; color: var(--text-primary);" id="basicChargeCurrentText">450mA (固定)</div>
                                <div style="margin-top: 8px; font-size: 12px; color: var(--text-secondary);">固件锁定，不可调</div>
                            </div>
                            <div style="background: var(--bg-tertiary); padding: 16px; border-radius: 8px; border: 1px solid var(--border-color);">
                                <div style="font-size: 14px; color: var(--text-secondary); margin-bottom: 8px;">升压模式</div>
                                <div style="font-size: 18px; font-weight: 600; color: var(--text-primary);" id="basicBoostMode">--</div>
                            </div>
                            <div style="background: var(--bg-tertiary); padding: 16px; border-radius: 8px; border: 1px solid var(--border-color);">
                                <div style="font-size: 14px; color: var(--text-secondary); margin-bottom: 8px;">低电关机</div>
                                <div style="font-size: 18px; font-weight: 600; color: var(--text-primary);" id="basicLowPowerShutdown">--</div>
                            </div>
                            <div style="background: var(--bg-tertiary); padding: 16px; border-radius: 8px; border: 1px solid var(--border-color);">
                                <div style="font-size: 14px; color: var(--text-secondary); margin-bottom: 8px;">按键使能</div>
                                <div style="font-size: 18px; font-weight: 600; color: var(--text-primary);" id="basicKeyEnable">--</div>
                            </div>
                            <div style="background: var(--bg-tertiary); padding: 16px; border-radius: 8px; border: 1px solid var(--border-color);">
                                <div style="font-size: 14px; color: var(--text-secondary); margin-bottom: 8px;">LED指示灯</div>
                                <div style="font-size: 18px; font-weight: 600; color: var(--text-primary);" id="basicLedEnable">--</div>
                            </div>
                        </div>
                    </div>
                    <div id="powerAdvancedView" style="display: none;">
                        <div style="display: grid; grid-template-columns: 1fr 1fr; gap: 16px;">
                            <div style="overflow-x: auto;">
                                <h4 style="margin-bottom: 12px; color: var(--accent-primary);">IP5306 寄存器</h4>
                                <table class="reg-table" id="ip5306Table">
                                    <thead>
                                        <tr>
                                            <th>寄存器</th>
                                            <th>原始值</th>
                                            <th>解析</th>
                                        </tr>
                                    </thead>
                                    <tbody>
                                        <tr><td colspan="3" style="text-align: center; padding: 20px;">加载中...</td></tr>
                                    </tbody>
                                </table>
                            </div>
                            <div style="overflow-x: auto;">
                                <h4 style="margin-bottom: 12px; color: var(--accent-primary);">TCA9555 IO 状态</h4>
                                <table class="reg-table" id="tca9555Table">
                                    <thead>
                                        <tr>
                                            <th>类型</th>
                                            <th>值 (HEX)</th>
                                            <th>位状态</th>
                                        </tr>
                                    </thead>
                                    <tbody>
                                        <tr><td colspan="3" style="text-align: center; padding: 20px;">加载中...</td></tr>
                                    </tbody>
                                </table>
                            </div>
                        </div>
                    </div>
                </div>
            `;

            loadPowerDetail();
            startPowerAutoRefresh();
            switchPowerView(state.powerViewMode);
        }

        // 芯片管理页面
        function renderChipPage() {
            const container = document.getElementById('mainContent');
            container.innerHTML = `
                <div class="page-header animate-in">
                    <h1 class="page-title">🔧 芯片管理</h1>
                    <p class="page-subtitle">MCU SWD 自动探测</p>
                </div>

                <div class="card animate-in">
                    <div class="card-header">
                        <h3 class="card-title">🔍 一键探测</h3>
                    </div>
                    <div style="padding: 16px; text-align: center;">
                        <p style="color: var(--text-secondary); margin-bottom: 20px;">自动尝试所有 SWD 配置组合，检测连接的 MCU 芯片</p>
                        <button class="btn btn-primary btn-lg" onclick="probeMCU()" id="probeBtn" style="padding: 16px 48px; font-size: 18px;">
                            🔍 一键探测
                        </button>
                    </div>
                </div>

                <div class="card animate-in" id="probeResultCard" style="display: none;">
                    <div class="card-header">
                        <h3 class="card-title">📊 探测结果</h3>
                    </div>
                    <div style="padding: 16px;" id="probeResultContent">
                    </div>
                </div>
            `;
        }

        async function probeMCU() {
            const btn = document.getElementById('probeBtn');
            const resultCard = document.getElementById('probeResultCard');
            const resultContent = document.getElementById('probeResultContent');
            
            btn.disabled = true;
            btn.textContent = '⏳ 探测中...';
            resultCard.style.display = 'block';
            resultContent.innerHTML = '<div style="text-align: center; padding: 20px; color: var(--text-secondary);">正在探测 MCU...</div>';

            try {
                const response = await fetch('/api/mcu/probe');
                const data = await response.json();

                if (data.ok) {
                    resultContent.innerHTML = `
                        <div style="background: rgba(0, 255, 136, 0.1); border: 1px solid var(--accent-success); border-radius: 8px; padding: 20px; margin-bottom: 16px;">
                            <div style="font-size: 24px; font-weight: 700; color: var(--accent-success); margin-bottom: 8px;">✓ 探测成功</div>
                            <div style="font-size: 32px; font-family: monospace; color: var(--text-primary);">IDCODE: ${data.idcode}</div>
                        </div>
                        <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 12px;">
                            <div style="background: var(--bg-tertiary); padding: 12px; border-radius: 8px; border: 1px solid var(--border-color);">
                                <div style="font-size: 12px; color: var(--text-secondary);">序列模式</div>
                                <div style="font-size: 16px; font-weight: 600; color: var(--text-primary);">${data.seq_used}</div>
                            </div>
                            <div style="background: var(--bg-tertiary); padding: 12px; border-radius: 8px; border: 1px solid var(--border-color);">
                                <div style="font-size: 12px; color: var(--text-secondary);">尝试次数</div>
                                <div style="font-size: 16px; font-weight: 600; color: var(--text-primary);">${data.attempt_count}</div>
                            </div>
                            <div style="background: var(--bg-tertiary); padding: 12px; border-radius: 8px; border: 1px solid var(--border-color);">
                                <div style="font-size: 12px; color: var(--text-secondary);">奇偶校验</div>
                                <div style="font-size: 16px; font-weight: 600; color: ${data.parity_ok ? 'var(--accent-success)' : 'var(--accent-secondary)'};">${data.parity_ok ? '✓ 通过' : '✕ 失败'}</div>
                            </div>
                            <div style="background: var(--bg-tertiary); padding: 12px; border-radius: 8px; border: 1px solid var(--border-color);">
                                <div style="font-size: 12px; color: var(--text-secondary);">ACK</div>
                                <div style="font-size: 16px; font-weight: 600; color: var(--text-primary);">${data.ack}</div>
                            </div>
                        </div>
                    `;
                    showToast('探测成功: ' + data.idcode, 'success');
                } else {
                    resultContent.innerHTML = `
                        <div style="background: rgba(255, 68, 68, 0.1); border: 1px solid var(--accent-secondary); border-radius: 8px; padding: 20px;">
                            <div style="font-size: 24px; font-weight: 700; color: var(--accent-secondary); margin-bottom: 8px;">✕ 探测失败</div>
                            <div style="font-size: 16px; color: var(--text-primary);">状态: ${data.status}</div>
                            <div style="margin-top: 12px; color: var(--text-secondary); font-size: 14px;">
                                请检查：SWD 连线是否正确、目标芯片是否供电、芯片是否支持 SWD 调试
                            </div>
                        </div>
                    `;
                    showToast('探测失败: ' + data.status, 'error');
                }
            } catch (error) {
                resultContent.innerHTML = `
                    <div style="background: rgba(255, 68, 68, 0.1); border: 1px solid var(--accent-secondary); border-radius: 8px; padding: 20px;">
                        <div style="font-size: 24px; font-weight: 700; color: var(--accent-secondary); margin-bottom: 8px;">✕ 网络错误</div>
                        <div style="font-size: 16px; color: var(--text-primary);">${error.message}</div>
                    </div>
                `;
                showToast('网络错误: ' + error.message, 'error');
            }

            btn.disabled = false;
            btn.textContent = '🔍 一键探测';
        }

        // 烧录中心页面
        function renderBurnerPage() {
            const container = document.getElementById('mainContent');
            container.innerHTML = `
                <div class="page-header animate-in">
                    <h1 class="page-title">🔥 烧录中心</h1>
                    <p class="page-subtitle">以 TF 文件为中心的卡带写入/导出/校验</p>
                </div>

                <div class="card animate-in">
                    <div class="card-header">
                        <h3 class="card-title">🎮 卡带模式 & 操作</h3>
                    </div>
                    <div style="padding: 16px;">
                        <div style="display: flex; gap: 12px; flex-wrap: wrap; margin-bottom: 16px;">
                            <button class="btn btn-primary" id="modeGbaBtn" onclick="setCartMode('gba')">📦 GBA 卡带</button>
                            <button class="btn" id="modeMbc5Btn" onclick="setCartMode('mbc5')">🎮 GB/GBC 卡带 (MBC5)</button>
                        </div>
                        <div style="display: flex; gap: 16px; flex-wrap: wrap; margin-bottom: 16px;">
                            <div class="setting-item" style="flex: 1; min-width: 280px;">
                                <label class="setting-label">烧录路径</label>
                                <select class="input" id="writePathModeSelect" onchange="setWritePathMode(this.value)">
                                    <option value="direct">直接: TF -> 卡带</option>
                                    <option value="psram">PSRAM: TF -> PSRAM(1~8MB流水) -> 卡带</option>
                                </select>
                                <div id="writePathModeHint" style="margin-top: 6px; font-size: 12px; color: var(--text-secondary);">
                                    两种路径都可用于 GBA / MBC5。PSRAM 模式可配 1~8MB 循环：擦除并行预取，擦除后写卡带。
                                </div>
                            </div>
                            <div class="setting-item" style="flex: 1; min-width: 220px;" id="psramWindowMbSection">
                                <label class="setting-label">PSRAM 流水窗口 (MB)</label>
                                <select class="input" id="psramWindowMbSelect" onchange="setPsramWindowMb(this.value)">
                                    <option value="1">1 MB</option>
                                    <option value="2">2 MB</option>
                                    <option value="3">3 MB</option>
                                    <option value="4" selected>4 MB</option>
                                    <option value="5">5 MB</option>
                                    <option value="6">6 MB</option>
                                    <option value="7">7 MB</option>
                                    <option value="8">8 MB</option>
                                </select>
                                <div style="margin-top: 6px; font-size: 12px; color: var(--text-secondary);">
                                    仅在 <code>write_path=psram</code> 生效。范围 1~8 MB。
                                </div>
                            </div>
                            <div class="setting-item" style="flex: 1; min-width: 220px;">
                                <label class="setting-label">MBC5 写入块大小 (KB)</label>
                                <select class="input" id="mbc5ChunkKbSelect" onchange="setMbc5ChunkKb(this.value)">
                                    <option value="4">4 KB</option>
                                    <option value="8">8 KB</option>
                                    <option value="16" selected>16 KB</option>
                                    <option value="32">32 KB</option>
                                    <option value="64">64 KB</option>
                                    <option value="128">128 KB</option>
                                </select>
                                <div style="margin-top: 6px; font-size: 12px; color: var(--text-secondary);">
                                    用于 MBC5 写入任务（direct / psram）。块越大通常吞吐越高，但稳定性可能受卡带影响。
                                </div>
                            </div>
                        </div>
                        <div style="display: flex; gap: 12px; flex-wrap: wrap; margin-bottom: 16px;">
                            <h4 style="margin: 0; width: 100%; font-size: 14px; color: var(--text-secondary);">ROM 操作</h4>
                            <button class="btn" onclick="cartReadId()">🔍 读取 ID</button>
                            <button class="btn btn-danger" id="eraseChipBtn" onclick="cartEraseChip()">🗑️ 全片擦除</button>
                            <button class="btn btn-primary" onclick="cartWriteRom()">📤 写入 ROM</button>
                            <button class="btn" onclick="cartDumpRom()">📥 导出 ROM</button>
                            <button class="btn" onclick="cartVerifyRom()">✅ 校验 ROM</button>
                            <button class="btn" onclick="goSelectTfRom()">📂 选择 GBA/GB 文件</button>
                        </div>
                        <div style="display: flex; gap: 12px; flex-wrap: wrap;">
                            <h4 style="margin: 0; width: 100%; font-size: 14px; color: var(--text-secondary);">存档操作</h4>
                            <button class="btn btn-primary" onclick="cartWriteSave()">📤 写入存档</button>
                            <button class="btn" onclick="cartDumpSave()">📥 导出存档</button>
                            <button class="btn" onclick="cartVerifySave()">✅ 校验存档</button>
                        </div>
                    </div>
                </div>

                <div style="display: flex; gap: 24px; flex-wrap: wrap;">
                    <div class="card animate-in" style="flex: 1; min-width: 300px;">
                        <div class="card-header">
                            <h3 class="card-title">📂 ROM 文件</h3>
                        </div>
                        <div style="padding: 16px;">
                            <div style="display: flex; gap: 12px; align-items: center; margin-bottom: 12px; flex-wrap: wrap;">
                                <input type="text" class="input" id="romPath" placeholder="本地 ROM（可选，用于上传到TF）..." style="flex: 1; min-width: 200px;" readonly>
                                <button class="btn" onclick="document.getElementById('romFileInput').click()">📁 浏览</button>
                                <input type="file" id="romFileInput" accept=".gba,.gb,.gbc,.bin" style="display: none;" onchange="selectRomFile(this.files[0])">
                            </div>
                            <div style="display: flex; gap: 12px; align-items: center; margin-bottom: 16px; flex-wrap: wrap;">
                                <input type="text" class="input" id="tfRomName" placeholder="未选择 TF ROM 文件（点击右侧按钮选择）" style="flex: 1; min-width: 200px;" readonly>
                                <button class="btn" onclick="goSelectTfRom()">📂 去文件浏览选择</button>
                                <button class="btn" onclick="clearTfRomSelection()">✕ 清空</button>
                            </div>
                            <div style="display: flex; gap: 16px; flex-wrap: wrap;">
                                <div class="setting-item" style="flex: 1; min-width: 150px;">
                                    <label class="setting-label">ROM 大小 (MiB)</label>
                                    <select class="input" id="romSize">
                                        <option value="1">1</option>
                                        <option value="2">2</option>
                                        <option value="4">4</option>
                                        <option value="8">8</option>
                                        <option value="16">16</option>
                                        <option value="32" selected>32</option>
                                    </select>
                                </div>
                                <div class="setting-item" style="flex: 1; min-width: 150px;" id="gbaMultiCartSection">
                                    <label class="setting-label">合卡位置</label>
                                    <select class="input" id="gbaMultiCartSelect">
                                        <option value="0">整卡</option>
                                        <option value="1">菜单 (0M)</option>
                                        <option value="2">游戏1 (4M)</option>
                                        <option value="3">游戏2 (8M)</option>
                                        <option value="4">游戏3 (12M)</option>
                                        <option value="5">游戏4 (16M)</option>
                                        <option value="6">游戏5 (20M)</option>
                                        <option value="7">游戏6 (24M)</option>
                                        <option value="8">游戏7 (28M)</option>
                                    </select>
                                </div>
                                <div class="setting-item" style="flex: 1; min-width: 150px; display: none;" id="mbc5MultiCartSection">
                                    <label class="setting-label">合卡位置</label>
                                    <select class="input" id="mbc5MultiCartSelect">
                                        <option value="0">整卡</option>
                                        <option value="1">菜单 (1M)</option>
                                        <option value="2">游戏1 (1M)</option>
                                        <option value="3">游戏2 (2M)</option>
                                        <option value="4">游戏3 (2M)</option>
                                        <option value="5">游戏4 (2M)</option>
                                        <option value="6">游戏5 (2M)</option>
                                        <option value="7">游戏6 (2M)</option>
                                        <option value="8">游戏7 (2M)</option>
                                        <option value="9">游戏8 (2M)</option>
                                    </select>
                                </div>
                            </div>
                        </div>
                    </div>

                    <div class="card animate-in" style="flex: 1; min-width: 300px;">
                        <div class="card-header">
                            <h3 class="card-title">💾 存档文件</h3>
                        </div>
                        <div style="padding: 16px;">
                            <div style="display: flex; gap: 12px; align-items: center; margin-bottom: 12px; flex-wrap: wrap;">
                                <input type="text" class="input" id="savePath" placeholder="本地存档（可选，用于上传到TF）..." style="flex: 1; min-width: 200px;" readonly>
                                <button class="btn" onclick="document.getElementById('saveFileInput').click()">📁 浏览</button>
                                <input type="file" id="saveFileInput" accept=".sav" style="display: none;" onchange="selectSaveFile(this.files[0])">
                            </div>
                            <div style="display: flex; gap: 12px; align-items: center; margin-bottom: 16px; flex-wrap: wrap;">
                                <input type="text" class="input" id="tfSaveName" placeholder="TF SAV 文件名（可选）" style="flex: 1; min-width: 200px;">
                            </div>
                            <div style="display: flex; gap: 16px; flex-wrap: wrap;">
                                <div class="setting-item" style="flex: 1; min-width: 150px;">
                                    <label class="setting-label">存档大小 (KiB)</label>
                                    <select class="input" id="saveSize">
                                        <option value="32">32</option>
                                        <option value="64">64</option>
                                        <option value="128" selected>128</option>
                                        <option value="256">256</option>
                                        <option value="512">512</option>
                                    </select>
                                </div>
                                <div class="setting-item" style="flex: 1; min-width: 150px;" id="ramTypeSection">
                                    <label class="setting-label">存档类型</label>
                                    <select class="input" id="ramType">
                                        <option value="sram">SRAM</option>
                                        <option value="fram">FRAM</option>
                                    </select>
                                </div>
                                <div class="setting-item" style="flex: 1; min-width: 150px;">
                                    <label class="setting-label">FRAM 延时 (0..255)</label>
                                    <input class="input" id="ramLatency" type="number" min="0" max="255" value="10">
                                </div>
                            </div>
                        </div>
                    </div>
                </div>

                <div class="card animate-in" id="burnStatusCard" style="display: none;">
                    <div class="card-header">
                        <h3 class="card-title">📊 烧录状态</h3>
                        <button class="btn btn-sm btn-danger" id="cancelBurnBtn" onclick="cancelBurnTask()">✕ 取消任务</button>
                    </div>
                    <div style="padding: 16px;">
                        <div class="burn-status" id="burnStatusGrid">
                            <div class="status-card">
                                <div class="status-label">当前状态</div>
                                <div class="status-value-large" id="burnState">空闲</div>
                            </div>
                            <div class="status-card">
                                <div class="status-label">ROM 文件</div>
                                <div class="status-value-large" id="burnRomName" style="font-size: 16px;">--</div>
                            </div>
                            <div class="status-card">
                                <div class="status-label">进度</div>
                                <div class="status-value-large" id="burnProgress">0%</div>
                            </div>
                            <div class="status-card">
                                <div class="status-label">当前速度</div>
                                <div class="status-value-large" id="burnSpeedCurrent">--</div>
                            </div>
                            <div class="status-card">
                                <div class="status-label">平均速度</div>
                                <div class="status-value-large" id="burnSpeedAvg">--</div>
                            </div>
                            <div class="status-card">
                                <div class="status-label">最低速度</div>
                                <div class="status-value-large" id="burnSpeedMin">--</div>
                            </div>
                            <div class="status-card">
                                <div class="status-label">最高速度</div>
                                <div class="status-value-large" id="burnSpeedMax">--</div>
                            </div>
                        </div>
                        <div class="progress-container" style="margin-top: 16px;">
                            <div class="progress-info">
                                <span id="burnMessage">等待开始...</span>
                                <span id="burnPercent">0%</span>
                            </div>
                            <div class="progress-bar">
                                <div class="progress-fill" id="burnBar" style="width: 0%"></div>
                            </div>
                        </div>
                    </div>
                </div>

                <div class="card animate-in">
                    <div class="card-header">
                        <h3 class="card-title">📝 操作日志</h3>
                        <button class="btn btn-sm" onclick="clearBurnLog()">🗑️ 清空</button>
                    </div>
                    <div style="padding: 16px;">
                        <div class="log-container" id="burnLog" style="max-height: 300px; overflow-y: auto;">
                            <div class="log-entry">
                                <span class="log-time">${formatTime(new Date())}</span>
                                <span class="log-info">就绪，请选择卡带模式</span>
                            </div>
                        </div>
                    </div>
                </div>
            `;

            state.cartMode = 'gba';
            updateCartModeUI();
            setWritePathMode(state.writePathMode || 'direct');
            setPsramWindowMb(state.psramWindowMb || 4);
            setMbc5ChunkKb(state.mbc5ChunkKb || 16);
            setupBurnerDragDrop();
        }

        function setCartMode(mode) {
            state.cartMode = mode;
            updateCartModeUI();
            addBurnLog('切换至 ' + (mode === 'gba' ? 'GBA' : 'MBC5') + ' 模式', 'info');
        }

        function getEffectiveWritePath(mode, requestedPath) {
            const normalized = (requestedPath === 'psram') ? 'psram' : 'direct';
            return normalized;
        }

        function updateWritePathControls(modeOverride) {
            const mode = modeOverride || state.cartMode || 'gba';
            const effective = getEffectiveWritePath(mode, state.writePathMode || 'direct');
            const select = document.getElementById('writePathModeSelect');
            const fileActionSelect = document.getElementById('fileActionWritePathSelect');
            const hint = document.getElementById('writePathModeHint');
            const psramSection = document.getElementById('psramWindowMbSection');
            const psramSelect = document.getElementById('psramWindowMbSelect');
            const fileActionPsramSelect = document.getElementById('fileActionPsramMbSelect');
            const psramMb = getPsramWindowMb();
            const psramEnabled = effective === 'psram';

            state.writePathMode = effective;

            [select, fileActionSelect].forEach((el) => {
                if (!el) return;
                if (el.value !== effective) {
                    el.value = effective;
                }
            });

            [psramSelect, fileActionPsramSelect].forEach((el) => {
                if (!el) return;
                if (el.value !== String(psramMb)) {
                    el.value = String(psramMb);
                }
                el.disabled = !psramEnabled;
            });
            if (psramSection) {
                psramSection.style.opacity = psramEnabled ? '1' : '0.6';
            }

            if (hint) {
                hint.textContent =
                    `两种路径都可用于 GBA / MBC5。PSRAM 模式当前按 ${psramMb}MB 循环：擦除并行预取，擦除后写卡带（可选 1~8MB）。`;
            }
        }

        function setWritePathMode(mode) {
            const normalized = (mode === 'psram') ? 'psram' : 'direct';
            const cartMode = state.cartMode || 'gba';
            const effective = getEffectiveWritePath(cartMode, normalized);
            const changed = state.writePathMode !== effective;

            state.writePathMode = effective;
            updateWritePathControls(cartMode);

            if (changed) {
                addBurnLog(
                    effective === 'psram'
                        ? `烧录路径切换为 PSRAM: ${getPsramWindowMb()}MB擦写流水`
                        : '烧录路径切换为 Direct: TF->卡带',
                    'info'
                );
            }
        }

        function setPsramWindowMb(value) {
            const allowed = [1, 2, 3, 4, 5, 6, 7, 8];
            let mb = parseInt(value, 10);
            const prev = state.psramWindowMb || 4;

            if (!Number.isFinite(mb) || !allowed.includes(mb)) {
                mb = 4;
            }
            state.psramWindowMb = mb;
            updateWritePathControls(state.cartMode || 'gba');

            if (prev !== mb) {
                addBurnLog(`PSRAM 流水窗口已设为 ${mb}MB`, 'info');
            }
        }

        function getPsramWindowMb() {
            const mb = parseInt(state.psramWindowMb, 10);
            if (!Number.isFinite(mb) || mb < 1 || mb > 8) {
                return 4;
            }
            return mb;
        }

        function setMbc5ChunkKb(value) {
            const allowed = [4, 8, 16, 32, 64, 128];
            let kb = parseInt(value, 10);
            const prev = state.mbc5ChunkKb || 16;

            if (!Number.isFinite(kb) || !allowed.includes(kb)) {
                kb = 16;
            }
            state.mbc5ChunkKb = kb;

            const select = document.getElementById('mbc5ChunkKbSelect');
            if (select && select.value !== String(kb)) {
                select.value = String(kb);
            }

            if (prev !== kb) {
                addBurnLog(`MBC5 写入块大小已设为 ${kb}KB`, 'info');
            }
        }

        function getMbc5ChunkKb() {
            const kb = parseInt(state.mbc5ChunkKb, 10);
            if (!Number.isFinite(kb) || kb <= 0) {
                return 16;
            }
            return kb;
        }

        function updateCartModeUI() {
            const gbaBtn = document.getElementById('modeGbaBtn');
            const mbc5Btn = document.getElementById('modeMbc5Btn');
            const gbaSection = document.getElementById('gbaMultiCartSection');
            const mbc5Section = document.getElementById('mbc5MultiCartSection');

            if (gbaBtn) {
                gbaBtn.classList.add('btn');
            }
            if (mbc5Btn) {
                mbc5Btn.classList.add('btn');
            }
            
            if (state.cartMode === 'gba') {
                if (gbaBtn) gbaBtn.classList.add('btn-primary');
                if (mbc5Btn) mbc5Btn.classList.remove('btn-primary');
                if (gbaSection) gbaSection.style.display = 'block';
                if (mbc5Section) mbc5Section.style.display = 'none';
            } else {
                if (mbc5Btn) mbc5Btn.classList.add('btn-primary');
                if (gbaBtn) gbaBtn.classList.remove('btn-primary');
                if (gbaSection) gbaSection.style.display = 'none';
                if (mbc5Section) mbc5Section.style.display = 'block';
            }
            updateWritePathControls(state.cartMode);
        }

        function setupBurnerDragDrop() {
            const container = document.getElementById('mainContent');
            container.addEventListener('dragover', (e) => {
                e.preventDefault();
            });
            container.addEventListener('drop', (e) => {
                e.preventDefault();
                const file = e.dataTransfer.files[0];
                if (file) {
                    const ext = file.name.split('.').pop().toLowerCase();
                    if (ext === 'sav') {
                        selectSaveFile(file);
                    } else {
                        selectRomFile(file);
                    }
                }
            });
        }

        function selectRomFile(file) {
            if (!file) return;
            document.getElementById('romPath').value = file.name;
            state.romFile = file;
            const tfRomInput = document.getElementById('tfRomName');
            if (tfRomInput && !tfRomInput.value.trim()) {
                tfRomInput.value = file.name;
            }
            
            const sizeMiB = file.size / (1024 * 1024);
            document.getElementById('romSize').value = Math.ceil(sizeMiB);
            
            const ext = file.name.split('.').pop().toLowerCase();
            if (ext === 'gba') {
                setCartMode('gba');
            } else if (ext === 'gb' || ext === 'gbc') {
                setCartMode('mbc5');
            }
            
            addBurnLog('已选择 ROM: ' + file.name + ' (' + formatSize(file.size) + ')', 'info');
        }

        function selectSaveFile(file) {
            if (!file) return;
            document.getElementById('savePath').value = file.name;
            state.saveFile = file;
            
            const sizeKiB = file.size / 1024;
            document.getElementById('saveSize').value = Math.ceil(sizeKiB);

            const tfSaveInput = document.getElementById('tfSaveName');
            if (tfSaveInput && !tfSaveInput.value.trim()) {
                tfSaveInput.value = file.name;
            }
            
            addBurnLog('已选择存档: ' + file.name + ' (' + formatSize(file.size) + ')', 'info');
        }

        function goSelectTfRom() {
            addBurnLog('跳转到 TF 文件管理，请点击 .gba/.gb/.gbc 文件后再点“烧录”', 'info');
            showToast('请在 TF 文件页选择 ROM 并点烧录', 'info');
            window.location.hash = '#/tf';
        }

        function clearTfRomSelection() {
            const tfRomInput = document.getElementById('tfRomName');
            if (tfRomInput) tfRomInput.value = '';
            state.romFile = null;
            const romPathInput = document.getElementById('romPath');
            if (romPathInput) romPathInput.value = '';
            addBurnLog('已清空当前 ROM 选择', 'info');
        }

        function getBaseName(name) {
            const raw = String(name || '').trim();
            if (!raw) return '';
            return raw.split(/[\\/]/).pop();
        }

        function getSelectedCartSlot() {
            if (state.cartMode === 'gba') {
                const el = document.getElementById('gbaMultiCartSelect');
                return el ? (el.value || '0') : '0';
            }
            const el = document.getElementById('mbc5MultiCartSelect');
            return el ? (el.value || '0') : '0';
        }

        async function listFilesFromDir(relPath) {
            const response = await apiCall(`/api/tf/list?path=${encodeURIComponent(relPath)}`);
            const data = await response.json();
            if (!response.ok || !data.ok || !Array.isArray(data.entries)) {
                return [];
            }
            return data.entries
                .filter(e => e && !e.is_dir && typeof e.name === 'string')
                .map(e => e.name);
        }

        function setDatalistOptions(id, names) {
            const list = document.getElementById(id);
            if (!list) return;
            list.innerHTML = '';
            names.forEach(name => {
                const opt = document.createElement('option');
                opt.value = name;
                list.appendChild(opt);
            });
        }

        async function refreshTfRomSuggestions() {
            try {
                const results = await Promise.allSettled([
                    listFilesFromDir('roms'),
                    listFilesFromDir('dumps')
                ]);
                const roms = results[0].status === 'fulfilled' ? results[0].value : [];
                const dumps = results[1].status === 'fulfilled' ? results[1].value : [];
                const merged = Array.from(new Set([...roms, ...dumps]))
                    .filter(name => /\.(gba|gb|gbc|bin|rom)$/i.test(name))
                    .sort((a, b) => a.localeCompare(b));
                setDatalistOptions('tfRomList', merged);
                addBurnLog('已刷新 TF ROM 列表，文件数: ' + merged.length, 'info');
            } catch (error) {
                addBurnLog('刷新 TF ROM 列表失败: ' + error.message, 'warning');
            }
        }

        async function refreshTfSaveSuggestions() {
            try {
                const results = await Promise.allSettled([
                    listFilesFromDir('roms'),
                    listFilesFromDir('dumps')
                ]);
                const roms = results[0].status === 'fulfilled' ? results[0].value : [];
                const dumps = results[1].status === 'fulfilled' ? results[1].value : [];
                const merged = Array.from(new Set([...roms, ...dumps]))
                    .filter(name => /\.sav$/i.test(name))
                    .sort((a, b) => a.localeCompare(b));
                setDatalistOptions('tfSaveList', merged);
                addBurnLog('已刷新 TF SAV 列表，文件数: ' + merged.length, 'info');
            } catch (error) {
                addBurnLog('刷新 TF SAV 列表失败: ' + error.message, 'warning');
            }
        }

        async function uploadFileToTfForBurn(file, mode) {
            state.burnStatus = {
                state: 'receiving',
                progress: 0,
                processed: 0,
                total: file ? (file.size || 0) : 0,
                message: 'upload started',
                cancel_requested: false
            };

            return new Promise((resolve, reject) => {
                const xhr = new XMLHttpRequest();

                xhr.upload.addEventListener('progress', (e) => {
                    if (e.lengthComputable) {
                        const p = Math.round((e.loaded / e.total) * 100);
                        state.burnStatus = {
                            state: 'receiving',
                            progress: p,
                            processed: e.loaded,
                            total: e.total,
                            message: 'uploading',
                            cancel_requested: state.burnCancelPending
                        };
                        updateBurnProgress(p, '上传到 TF 中...');
                    }
                });

                xhr.addEventListener('load', () => {
                    const responseText = xhr.responseText || '';
                    if (isCancelResponse(xhr.status, responseText)) {
                        state.burnStatus = {
                            ...(state.burnStatus || {}),
                            state: 'cancelled',
                            cancel_requested: false,
                            message: 'upload cancelled'
                        };
                        setBurnCancelUi(false);
                        updateBurnProgress(state.burnStatus.progress || 0, '任务已取消');
                        reject(createCancelledError('上传已取消'));
                        return;
                    }

                    try {
                        const data = JSON.parse(responseText || '{}');
                        if (xhr.status === 200 && data.ok) {
                            state.burnStatus = {
                                ...(state.burnStatus || {}),
                                state: 'done',
                                progress: 100,
                                processed: file ? (file.size || 0) : 0,
                                total: file ? (file.size || 0) : 0,
                                cancel_requested: false,
                                message: 'upload to tf complete'
                            };
                            setBurnCancelUi(false);
                            resolve(getBaseName(file.name));
                        } else {
                            reject(new Error(data.message || ('HTTP ' + xhr.status)));
                        }
                    } catch (err) {
                        reject(new Error('解析上传响应失败'));
                    }
                });

                xhr.addEventListener('error', () => {
                    setBurnCancelUi(false);
                    reject(new Error('上传网络错误'));
                });
                xhr.open('POST', `/api/upload?name=${encodeURIComponent(file.name)}&mode=${encodeURIComponent(mode)}`);
                xhr.send(file);
            });
        }

        function showUnsupportedBurnFeature(featureName) {
            const text = `${featureName} 暂未接入当前固件 API`;
            addBurnLog(text, 'warning');
            showToast(text, 'warning');
        }

        async function cartReadId() {
            const mode = state.cartMode || 'gba';

            addBurnLog('正在读取卡带 ID...', 'info');
            showBurnStatus('读取ID', '读取中...');

            try {
                const response = await apiCall('/api/cart/id?mode=' + encodeURIComponent(mode));
                const data = await response.json();
                if (!response.ok || !data.ok) {
                    throw new Error(data.message || ('HTTP ' + response.status));
                }

                addBurnLog('后端模式: ' + (data.mode || mode), 'info');
                if (data.power && typeof data.power === 'object') {
                    addBurnLog(
                        '供电状态: 5V=' + (data.power.v5 ? 'ON' : 'OFF') +
                        ', 3.3V=' + (data.power.v3 ? 'ON' : 'OFF'),
                        'info');
                }
                addBurnLog('卡带 ID: ' + (data.id || '未知'), 'success');
                addBurnLog('芯片型号: ' + (data.chip || '未知'), 'success');
                if (data.cfi_ok) {
                    addBurnLog('容量: ' + formatSize(data.device_size || 0), 'success');
                    addBurnLog(
                        '扇区/缓冲: ' +
                        String(Math.round((data.sector_size || 0) / 1024)) + ' KB / ' +
                        String(data.buffer_write || 0),
                        'info');
                } else {
                    addBurnLog('CFI 读取失败，容量信息不可用', 'warning');
                }
                showToast('ID 读取成功', 'success');
            } catch (error) {
                addBurnLog('读取失败: ' + error.message, 'error');
                showToast('ID 读取失败', 'error');
            }

            hideBurnStatus();
        }

        async function cartEraseChip() {
            const mode = state.cartMode || 'gba';
            addBurnLog('启动全片擦除任务...', 'warning');
            showBurnStatus('全片擦除', '任务启动中...');

            try {
                const { response, data } = await startTaskRequest(
                    `/api/cart/erase?mode=${encodeURIComponent(mode)}`);
                if (!response.ok || !data.ok) {
                    throw new Error(data.message || ('HTTP ' + response.status));
                }

                state.activeBurnOperation = 'erase';
                state.pendingDumpRelPath = '';
                state.pendingDumpName = '';
                state.pendingDumpAutoDownloaded = false;
                state.lastBurnMessage = '';
                addBurnLog('全片擦除任务已启动', 'success');
                startBurnStatusPolling();
            } catch (error) {
                addBurnLog('全片擦除失败: ' + error.message, 'error');
                showToast('全片擦除失败', 'error');
                hideBurnStatus();
            }
        }

        async function cartWriteRom() {
            const mode = state.cartMode || 'gba';
            const slot = getSelectedCartSlot();
            const writePath = getEffectiveWritePath(mode, state.writePathMode || 'direct');
            const psramMb = getPsramWindowMb();
            const mbc5ChunkKb = getMbc5ChunkKb();
            const tfRomInput = document.getElementById('tfRomName');
            let tfRomName = String(tfRomInput ? tfRomInput.value : '').trim();

            addBurnLog('准备烧录...', 'info');
            showBurnStatus('写入ROM', '准备中...');

            try {
                if (state.romFile &&
                    (!tfRomName || getBaseName(tfRomName) === getBaseName(state.romFile.name))) {
                    addBurnLog('检测到本地 ROM，正在先上传到 TF...', 'info');
                    tfRomName = await uploadFileToTfForBurn(state.romFile, mode);
                    if (tfRomInput) tfRomInput.value = tfRomName;
                    addBurnLog('ROM 已上传到 TF: ' + tfRomName, 'success');
                }

                if (!tfRomName) {
                    throw new Error('请填写 TF ROM 文件名，或先选择本地 ROM 文件');
                }

                let writeUrl =
                    `/api/write?name=${encodeURIComponent(tfRomName)}` +
                    `&mode=${encodeURIComponent(mode)}&slot=${encodeURIComponent(slot)}` +
                    `&write_path=${encodeURIComponent(writePath)}`;
                if (writePath === 'psram') {
                    writeUrl += `&psram_mb=${encodeURIComponent(String(psramMb))}`;
                }
                if (mode === 'mbc5') {
                    writeUrl += `&mbc5_chunk_kb=${encodeURIComponent(String(mbc5ChunkKb))}`;
                }
                const { response, data } = await startTaskRequest(writeUrl);

                if (!response.ok || !data.ok) {
                    throw new Error(data.message || ('HTTP ' + response.status));
                }
                state.activeBurnOperation = 'write';
                state.pendingDumpRelPath = '';
                state.pendingDumpName = '';
                state.pendingDumpAutoDownloaded = false;
                state.lastBurnMessage = '';
                updateBurnProgress(0, '烧录中......');
                addBurnLog('烧录中......', 'info');
                startBurnStatusPolling();
            } catch (error) {
                state.activeBurnOperation = null;
                addBurnLog('写入 ROM 失败: ' + error.message, 'error');
                showToast('写入 ROM 失败', 'error');
                hideBurnStatus();
            }
        }

        async function cartDumpRom() {
            const mode = state.cartMode || 'gba';
            const slot = getSelectedCartSlot();

            const sizeMiB = parseInt(document.getElementById('romSize').value, 10) || 32;
            const tfRomInput = document.getElementById('tfRomName');
            const baseNameRaw =
                getBaseName(tfRomInput ? tfRomInput.value : '') ||
                (state.romFile ? state.romFile.name : 'cart');
            const dumpBase = baseNameRaw.replace(/\.[^.]+$/, '') || 'cart';
            const dumpExt = mode === 'gba' ? 'gba' : 'gb';
            const dumpName = `${dumpBase}_${sizeMiB}M_dump.${dumpExt}`;
            const sizeArg = `${sizeMiB}M`;
            
            addBurnLog(
                '开始导出 ROM (' + sizeMiB + ' MiB, mode=' + mode + ', slot=' + slot + ')...',
                'info');
            showBurnStatus('导出ROM', '任务启动中...');
            
            try {
                const readResult = await startTaskRequest(
                    `/api/read?name=${encodeURIComponent(dumpName)}` +
                    `&size=${encodeURIComponent(sizeArg)}` +
                    `&mode=${encodeURIComponent(mode)}` +
                    `&slot=${encodeURIComponent(slot)}`);
                const readResponse = readResult.response;
                const data = readResult.data;
                if (!readResponse.ok || !data.ok) {
                    throw new Error(data.message || ('HTTP ' + readResponse.status));
                }

                state.activeBurnOperation = 'read';
                state.pendingDumpRelPath = (typeof data.path === 'string' && data.path.length > 0)
                    ? data.path.replace(/^\/sdcard\//, '')
                    : `ROM_OUTPUT/${dumpName}`;
                state.pendingDumpName = state.pendingDumpRelPath.split('/').pop() || dumpName;
                state.pendingDumpAutoDownloaded = false;
                state.lastBurnMessage = '';

                addBurnLog('导出任务已启动: ' + state.pendingDumpName, 'success');
                addBurnLog('任务完成后将自动下载到浏览器', 'info');
                startBurnStatusPolling();
            } catch (error) {
                addBurnLog('导出失败: ' + error.message, 'error');
                showToast('导出失败', 'error');
                state.activeBurnOperation = null;
                state.pendingDumpRelPath = '';
                state.pendingDumpName = '';
                state.pendingDumpAutoDownloaded = false;
                state.lastBurnMessage = '';
                hideBurnStatus();
            }
        }

        async function cartVerifyRom() {
            const mode = state.cartMode || 'gba';
            const slot = getSelectedCartSlot();
            const tfRomInput = document.getElementById('tfRomName');
            let tfRomName = String(tfRomInput ? tfRomInput.value : '').trim();

            addBurnLog('准备校验 ROM，模式=' + mode + ', slot=' + slot, 'info');
            showBurnStatus('校验ROM', '准备中...');

            try {
                if (state.romFile &&
                    (!tfRomName || getBaseName(tfRomName) === getBaseName(state.romFile.name))) {
                    addBurnLog('检测到本地 ROM，正在先上传到 TF...', 'info');
                    tfRomName = await uploadFileToTfForBurn(state.romFile, mode);
                    if (tfRomInput) tfRomInput.value = tfRomName;
                    addBurnLog('ROM 已上传到 TF: ' + tfRomName, 'success');
                }

                if (!tfRomName) {
                    throw new Error('请填写 TF ROM 文件名，或先选择本地 ROM 文件');
                }

                const verifyUrl =
                    `/api/verify?name=${encodeURIComponent(tfRomName)}` +
                    `&mode=${encodeURIComponent(mode)}&slot=${encodeURIComponent(slot)}`;
                const { response, data } = await startTaskRequest(verifyUrl);
                if (!response.ok || !data.ok) {
                    throw new Error(data.message || ('HTTP ' + response.status));
                }

                state.activeBurnOperation = 'verify';
                state.pendingDumpRelPath = '';
                state.pendingDumpName = '';
                state.pendingDumpAutoDownloaded = false;
                state.lastBurnMessage = '';
                addBurnLog('校验任务已启动: ' + tfRomName, 'success');
                startBurnStatusPolling();
            } catch (error) {
                addBurnLog('校验 ROM 失败: ' + error.message, 'error');
                showToast('校验 ROM 失败', 'error');
                hideBurnStatus();
            }
        }

        async function cartWriteSave() {
            const mode = state.cartMode || 'gba';
            if (mode !== 'mbc5') {
                showUnsupportedBurnFeature('存档 API 当前仅支持 GB/MBC5 模式');
                return;
            }

            const slot = getSelectedCartSlot();
            const tfSaveInput = document.getElementById('tfSaveName');
            let tfSaveName = getBaseName(tfSaveInput ? tfSaveInput.value : '');
            const ramType = (document.getElementById('ramType')?.value || 'sram').toLowerCase();
            const ramLatency = parseInt(document.getElementById('ramLatency')?.value || '10', 10);

            addBurnLog(
                `准备写入存档，slot=${slot}, ram_type=${ramType}, latency=${ramLatency}`,
                'info');
            showBurnStatus('写入存档', '准备中...');

            try {
                if (state.saveFile && (!tfSaveName || tfSaveName === getBaseName(state.saveFile.name))) {
                    addBurnLog('检测到本地存档，正在先上传到 TF...', 'info');
                    tfSaveName = await uploadFileToTfForBurn(state.saveFile, mode);
                    if (tfSaveInput) tfSaveInput.value = tfSaveName;
                    addBurnLog('存档已上传到 TF: ' + tfSaveName, 'success');
                }

                if (!tfSaveName) {
                    throw new Error('请填写 TF 存档文件名，或先选择本地存档文件');
                }

                const writeUrl =
                    `/api/ram/write?name=${encodeURIComponent(tfSaveName)}` +
                    `&slot=${encodeURIComponent(slot)}` +
                    `&ram_type=${encodeURIComponent(ramType)}` +
                    `&ram_latency=${encodeURIComponent(String(Math.max(0, Math.min(255, isNaN(ramLatency) ? 10 : ramLatency))))}`;
                const { response, data } = await startTaskRequest(writeUrl);
                if (!response.ok || !data.ok) {
                    throw new Error(data.message || ('HTTP ' + response.status));
                }

                state.activeBurnOperation = 'ram_write';
                state.pendingDumpRelPath = '';
                state.pendingDumpName = '';
                state.pendingDumpAutoDownloaded = false;
                state.lastBurnMessage = '';
                addBurnLog('存档写入任务已启动: ' + tfSaveName, 'success');
                startBurnStatusPolling();
            } catch (error) {
                addBurnLog('写入存档失败: ' + error.message, 'error');
                showToast('写入存档失败', 'error');
                hideBurnStatus();
            }
        }

        async function cartDumpSave() {
            const mode = state.cartMode || 'gba';
            if (mode !== 'mbc5') {
                showUnsupportedBurnFeature('存档 API 当前仅支持 GB/MBC5 模式');
                return;
            }

            const slot = getSelectedCartSlot();
            const sizeKiB = parseInt(document.getElementById('saveSize')?.value || '128', 10) || 128;
            const ramType = (document.getElementById('ramType')?.value || 'sram').toLowerCase();
            const ramLatency = parseInt(document.getElementById('ramLatency')?.value || '10', 10);
            const tfSaveInput = document.getElementById('tfSaveName');
            const base = getBaseName(tfSaveInput ? tfSaveInput.value : '').replace(/\.[^.]+$/, '') || 'cart_save';
            const dumpName = `${base}_${sizeKiB}K_dump.sav`;
            const sizeArg = `${sizeKiB}K`;

            addBurnLog(`开始导出存档 (${sizeKiB} KiB, slot=${slot})...`, 'info');
            showBurnStatus('导出存档', '任务启动中...');

            try {
                const readUrl =
                    `/api/ram/read?name=${encodeURIComponent(dumpName)}` +
                    `&size=${encodeURIComponent(sizeArg)}` +
                    `&slot=${encodeURIComponent(slot)}` +
                    `&ram_type=${encodeURIComponent(ramType)}` +
                    `&ram_latency=${encodeURIComponent(String(Math.max(0, Math.min(255, isNaN(ramLatency) ? 10 : ramLatency))))}`;
                const { response, data } = await startTaskRequest(readUrl);
                if (!response.ok || !data.ok) {
                    throw new Error(data.message || ('HTTP ' + response.status));
                }

                state.activeBurnOperation = 'ram_read';
                state.pendingDumpRelPath = (typeof data.path === 'string' && data.path.length > 0)
                    ? data.path.replace(/^\/sdcard\//, '')
                    : `dumps/${dumpName}`;
                state.pendingDumpName = state.pendingDumpRelPath.split('/').pop() || dumpName;
                state.pendingDumpAutoDownloaded = false;
                state.lastBurnMessage = '';
                addBurnLog('存档导出任务已启动: ' + state.pendingDumpName, 'success');
                addBurnLog('任务完成后将自动下载到浏览器', 'info');
                startBurnStatusPolling();
            } catch (error) {
                addBurnLog('导出存档失败: ' + error.message, 'error');
                showToast('导出存档失败', 'error');
                hideBurnStatus();
            }
        }

        async function cartVerifySave() {
            const mode = state.cartMode || 'gba';
            if (mode !== 'mbc5') {
                showUnsupportedBurnFeature('存档 API 当前仅支持 GB/MBC5 模式');
                return;
            }

            const slot = getSelectedCartSlot();
            const tfSaveInput = document.getElementById('tfSaveName');
            let tfSaveName = getBaseName(tfSaveInput ? tfSaveInput.value : '');
            const ramType = (document.getElementById('ramType')?.value || 'sram').toLowerCase();
            const ramLatency = parseInt(document.getElementById('ramLatency')?.value || '10', 10);

            addBurnLog('准备校验存档，slot=' + slot, 'info');
            showBurnStatus('校验存档', '准备中...');

            try {
                if (state.saveFile && (!tfSaveName || tfSaveName === getBaseName(state.saveFile.name))) {
                    addBurnLog('检测到本地存档，正在先上传到 TF...', 'info');
                    tfSaveName = await uploadFileToTfForBurn(state.saveFile, mode);
                    if (tfSaveInput) tfSaveInput.value = tfSaveName;
                    addBurnLog('存档已上传到 TF: ' + tfSaveName, 'success');
                }

                if (!tfSaveName) {
                    throw new Error('请填写 TF 存档文件名，或先选择本地存档文件');
                }

                const verifyUrl =
                    `/api/ram/verify?name=${encodeURIComponent(tfSaveName)}` +
                    `&slot=${encodeURIComponent(slot)}` +
                    `&ram_type=${encodeURIComponent(ramType)}` +
                    `&ram_latency=${encodeURIComponent(String(Math.max(0, Math.min(255, isNaN(ramLatency) ? 10 : ramLatency))))}`;
                const { response, data } = await startTaskRequest(verifyUrl);
                if (!response.ok || !data.ok) {
                    throw new Error(data.message || ('HTTP ' + response.status));
                }

                state.activeBurnOperation = 'ram_verify';
                state.pendingDumpRelPath = '';
                state.pendingDumpName = '';
                state.pendingDumpAutoDownloaded = false;
                state.lastBurnMessage = '';
                addBurnLog('存档校验任务已启动: ' + tfSaveName, 'success');
                startBurnStatusPolling();
            } catch (error) {
                addBurnLog('校验存档失败: ' + error.message, 'error');
                showToast('校验存档失败', 'error');
                hideBurnStatus();
            }
        }

        function showBurnStatus(title, status) {
            const card = document.getElementById('burnStatusCard');
            if (card) card.style.display = 'block';
            
            const stateEl = document.getElementById('burnState');
            if (stateEl) stateEl.textContent = status;
            setBurnCancelUi(false);
        }

        function hideBurnStatus() {
            const card = document.getElementById('burnStatusCard');
            if (card) card.style.display = 'none';
            setBurnCancelUi(false);
        }

        function setBurnCancelUi(isPending) {
            const btn = document.getElementById('cancelBurnBtn');
            state.burnCancelPending = !!isPending;
            if (!btn) return;

            btn.disabled = !!isPending;
            btn.textContent = isPending ? '取消中...' : '✕ 取消任务';
        }

        function isCancelResponse(status, message) {
            return status === 409 || /cancel|取消/i.test(String(message || ''));
        }

        function createCancelledError(message = '任务已取消') {
            const error = new Error(message);
            error.cancelled = true;
            return error;
        }

        function isCancelledError(error) {
            return Boolean(error && (error.cancelled || isCancelResponse(0, error.message || '')));
        }

        async function postCancelCommand() {
            const { response, data } = await startTaskRequest('/api/cancel');
            if (!response.ok || !data.ok) {
                throw new Error(data.message || ('HTTP ' + response.status));
            }
            return data;
        }

        function setUploadCancelUi(isPending) {
            const btn = document.getElementById('cancelBtn');
            state.uploadCancelPending = !!isPending;
            if (!btn) return;

            btn.disabled = !state.uploadXHR || !!isPending;
            btn.textContent = isPending ? '取消中...' : '取消上传';
        }

        function setMultiUploadCancelUi(isPending) {
            const btn = document.getElementById('cancelAllBtn');
            state.multiUploadCancelPending = !!isPending;
            if (!btn) return;

            btn.disabled = !!isPending;
            btn.textContent = isPending ? '取消中...' : '全部取消';
        }

        function setFirmwareCancelUi(isBusy, isPending) {
            const selectBtn = document.getElementById('firmwareSelectBtn');
            const cancelBtn = document.getElementById('firmwareCancelBtn');

            state.firmwareCancelPending = !!isPending;
            if (selectBtn) selectBtn.disabled = !!isBusy;
            if (!cancelBtn) return;

            cancelBtn.disabled = !isBusy || !!isPending;
            cancelBtn.textContent = isPending ? '取消中...' : '✕ 取消升级';
        }

        function setSystemDeployCancelUi(isBusy, isPending) {
            const selectBtn = document.getElementById('systemDeploySelectBtn');
            const cancelBtn = document.getElementById('systemDeployCancelBtn');

            state.systemDeployBusy = !!isBusy;
            state.systemDeployCancelPending = !!isPending;
            if (selectBtn) selectBtn.disabled = !!isBusy;
            if (!cancelBtn) return;

            cancelBtn.disabled = !isBusy || !!isPending;
            cancelBtn.textContent = isPending ? '取消中...' : '✕ 取消部署';
        }

        function updateBurnProgress(percent, message) {
            const bar = document.getElementById('burnBar');
            const percentEl = document.getElementById('burnPercent');
            const progressEl = document.getElementById('burnProgress');
            const msgEl = document.getElementById('burnMessage');
            
            if (bar) bar.style.width = percent + '%';
            if (percentEl) percentEl.textContent = percent + '%';
            if (progressEl) progressEl.textContent = percent + '%';
            if (msgEl) msgEl.textContent = message || '';
        }

        function getBurnProgressPercent(data) {
            const processed = Number(data && data.processed);
            const total = Number(data && data.total);

            if (Number.isFinite(processed) && Number.isFinite(total) && total > 0) {
                const percent = Math.round((processed * 100) / total);
                return Math.max(0, Math.min(100, percent));
            }

            const fallback = Number(data && data.progress);
            if (!Number.isFinite(fallback)) {
                return 0;
            }
            return Math.max(0, Math.min(100, Math.round(fallback)));
        }

        function getBurnRunningLogText(operation) {
            switch (operation) {
                case 'write':
                    return '烧录中......';
                case 'erase':
                    return '擦除中......';
                case 'read':
                    return '导出中......';
                case 'verify':
                    return '校验中......';
                case 'ram_write':
                    return '写入存档中......';
                case 'ram_read':
                    return '导出存档中......';
                case 'ram_verify':
                    return '校验存档中......';
                default:
                    return '处理中......';
            }
        }

        function getBurnProgressMessage(data, currentMessage) {
            if (currentMessage) {
                return currentMessage;
            }

            switch (data && data.state) {
                case 'receiving':
                    return '接收中......';
                case 'burning':
                    return getBurnRunningLogText(state.activeBurnOperation);
                case 'done':
                    return '任务完成';
                case 'cancelled':
                    return '任务已取消';
                case 'error':
                    return '任务失败';
                default:
                    return '';
            }
        }

        function getBurnPrimarySpeedLabel(operation) {
            switch (operation) {
                case 'write':
                    return 'TF->卡带';
                case 'read':
                    return '卡带->TF';
                case 'verify':
                    return '校验';
                case 'erase':
                    return '擦除';
                case 'ram_write':
                    return 'TF->存档';
                case 'ram_read':
                    return '存档->TF';
                case 'ram_verify':
                    return '存档校验';
                default:
                    return '任务';
            }
        }

        function getBurnTotalDurationText(data) {
            const taskTimeMs = Number(data && data.task_time_ms);
            if (Number.isFinite(taskTimeMs) && taskTimeMs >= 0) {
                return formatDuration(taskTimeMs / 1000);
            }

            const eraseTimeMs = Number(data && data.erase_time_ms);
            const writeTimeMs = Number(data && data.write_time_ms);
            if (Number.isFinite(eraseTimeMs) && Number.isFinite(writeTimeMs) &&
                (eraseTimeMs > 0 || writeTimeMs > 0)) {
                return formatDuration((eraseTimeMs + writeTimeMs) / 1000);
            }

            return '--';
        }

        function formatEraseSpeedText(sectorCount, eraseTimeMs) {
            if (!Number.isFinite(sectorCount) || sectorCount <= 0 ||
                !Number.isFinite(eraseTimeMs) || eraseTimeMs <= 0) {
                return '--';
            }

            const sectorsPerSecond = sectorCount / (eraseTimeMs / 1000);
            if (sectorsPerSecond >= 100) {
                return sectorsPerSecond.toFixed(0) + ' 扇区/秒';
            }
            if (sectorsPerSecond >= 10) {
                return sectorsPerSecond.toFixed(1) + ' 扇区/秒';
            }
            return sectorsPerSecond.toFixed(2) + ' 扇区/秒';
        }

        function addBurnDurationLog(data) {
            const durationText = getBurnTotalDurationText(data);
            if (durationText !== '--') {
                addBurnLog('总耗时: ' + durationText, 'info');
            }
        }

        function clearBurnLog() {
            const log = document.getElementById('burnLog');
            if (log) {
                log.innerHTML = `
                    <div class="log-entry">
                        <span class="log-time">${formatTime(new Date())}</span>
                        <span class="log-info">日志已清空</span>
                    </div>
                `;
            }
        }

        function cancelBurnTask() {
            const burnState = state.burnStatus && state.burnStatus.state;
            if (state.burnCancelPending) {
                showToast('取消请求已发送，请等待设备停止当前操作', 'warning');
                return;
            }

            if (burnState !== 'receiving' && burnState !== 'burning') {
                showToast('当前没有可取消的任务', 'warning');
                return;
            }

            showModal(
                '取消当前任务',
                `
                    <div style="line-height: 1.8; color: var(--text-primary);">
                        <div style="font-size: 16px; margin-bottom: 8px;">确定要取消当前流程吗？</div>
                        <div style="color: var(--text-secondary);">
                            确认后会向 ESP32 发送取消命令，当前烧录 / 校验 / 导出 / 擦除流程会尽快中断并停止。
                        </div>
                    </div>
                `,
                async () => {
                    closeModal();
                    setBurnCancelUi(true);
                    try {
                        const { response, data } = await startTaskRequest('/api/cancel');
                        if (!response.ok || !data.ok) {
                            throw new Error(data.message || ('HTTP ' + response.status));
                        }

                        addBurnLog('已发送取消命令，等待设备停止当前操作...', 'warning');
                        showToast('取消命令已发送', 'warning');
                        if (!state.burnStatusInterval) {
                            startBurnStatusPolling();
                        }
                    } catch (error) {
                        setBurnCancelUi(false);
                        addBurnLog('发送取消命令失败: ' + error.message, 'error');
                        showToast('取消失败: ' + error.message, 'error');
                    }
                }
            );
        }

        // 系统信息页面（首页）
        function requestUploadCancel() {
            if (state.uploadCancelPending) {
                showToast('取消请求已发送，请等待上传停止', 'warning');
                return;
            }
            if (!state.uploadXHR) {
                showToast('当前没有可取消的上传任务', 'warning');
                return;
            }

            showModal(
                '取消当前上传',
                `
                    <div style="line-height: 1.8; color: var(--text-primary);">
                        <div style="font-size: 16px; margin-bottom: 8px;">确定要取消当前上传吗？</div>
                        <div style="color: var(--text-secondary);">
                            确认后会向 ESP32 发送取消命令，当前 TF 上传会尽快停止，未完成文件会被丢弃。
                        </div>
                    </div>
                `,
                async () => {
                    closeModal();
                    setUploadCancelUi(true);
                    const eta = document.getElementById('statETA');
                    if (eta) eta.textContent = '取消中...';
                    try {
                        await postCancelCommand();
                        showToast('取消命令已发送，等待上传停止', 'warning');
                    } catch (error) {
                        setUploadCancelUi(false);
                        if (eta) eta.textContent = '计算中...';
                        showToast('取消失败: ' + error.message, 'error');
                    }
                }
            );
        }

        function requestCancelAllUploads() {
            const activeUploads = Array.from(state.multiUploads.values())
                .filter(upload => upload.xhr && upload.xhr.readyState !== 4);
            if (state.multiUploadCancelPending) {
                showToast('取消请求已发送，请等待全部上传停止', 'warning');
                return;
            }
            if (activeUploads.length === 0) {
                showToast('当前没有可取消的上传任务', 'warning');
                return;
            }

            showModal(
                '取消全部上传',
                `
                    <div style="line-height: 1.8; color: var(--text-primary);">
                        <div style="font-size: 16px; margin-bottom: 8px;">确定要取消全部上传吗？</div>
                        <div style="color: var(--text-secondary);">
                            确认后会向 ESP32 发送取消命令，当前批量上传中的文件会尽快全部停止。
                        </div>
                    </div>
                `,
                async () => {
                    closeModal();
                    setMultiUploadCancelUi(true);
                    const eta = document.getElementById('multiStatETA');
                    if (eta) eta.textContent = '取消中...';
                    try {
                        await postCancelCommand();
                        showToast('取消命令已发送，等待全部上传停止', 'warning');
                    } catch (error) {
                        setMultiUploadCancelUi(false);
                        if (eta) eta.textContent = '计算中...';
                        showToast('取消失败: ' + error.message, 'error');
                    }
                }
            );
        }

        function cancelFirmwareUpload() {
            if (state.firmwareCancelPending) {
                showToast('取消请求已发送，请等待固件上传停止', 'warning');
                return;
            }
            if (!state.firmwareUploadXHR) {
                showToast('当前没有可取消的固件升级任务', 'warning');
                return;
            }

            showModal(
                '取消固件升级',
                `
                    <div style="line-height: 1.8; color: var(--text-primary);">
                        <div style="font-size: 16px; margin-bottom: 8px;">确定要取消当前固件升级吗？</div>
                        <div style="color: var(--text-secondary);">
                            确认后会向 ESP32 发送取消命令，当前 OTA 上传会尽快停止，不会切换到新固件分区。
                        </div>
                    </div>
                `,
                async () => {
                    closeModal();
                    setFirmwareCancelUi(true, true);
                    const statusEl = document.getElementById('firmwareStatus');
                    if (statusEl) statusEl.textContent = '正在取消，请等待设备停止...';
                    try {
                        await postCancelCommand();
                        showToast('取消命令已发送，等待固件上传停止', 'warning');
                    } catch (error) {
                        setFirmwareCancelUi(true, false);
                        if (statusEl) statusEl.textContent = '取消失败: ' + error.message;
                        showToast('取消失败: ' + error.message, 'error');
                    }
                }
            );
        }

        function cancelSystemDeploy() {
            if (state.systemDeployCancelPending) {
                showToast('取消请求已发送，请等待部署停止', 'warning');
                return;
            }
            if (!state.systemDeployBusy) {
                showToast('当前没有可取消的系统部署任务', 'warning');
                return;
            }

            showModal(
                '取消系统部署',
                `
                    <div style="line-height: 1.8; color: var(--text-primary);">
                        <div style="font-size: 16px; margin-bottom: 8px;">确定要取消当前系统部署吗？</div>
                        <div style="color: var(--text-secondary);">
                            确认后会向 ESP32 发送取消命令，当前 ZIP 上传 / 解包 / 应用流程会尽快停止。
                        </div>
                    </div>
                `,
                async () => {
                    closeModal();
                    setSystemDeployCancelUi(true, true);
                    const statusEl = document.getElementById('systemDeployStatus');
                    if (statusEl) statusEl.textContent = '正在取消，请等待设备停止...';
                    try {
                        await postCancelCommand();
                        showToast('取消命令已发送，等待系统部署停止', 'warning');
                    } catch (error) {
                        setSystemDeployCancelUi(true, false);
                        if (statusEl) statusEl.textContent = '取消失败: ' + error.message;
                        showToast('取消失败: ' + error.message, 'error');
                    }
                }
            );
        }

        function renderSystemPage() {
            const container = document.getElementById('mainContent');
            container.innerHTML = `
                <div class="page-header animate-in">
                    <h1 class="page-title">💻 系统信息</h1>
                    <p class="page-subtitle">设备概览与系统状态</p>
                </div>

                <div class="power-grid animate-in" id="systemOverview">
                    <div class="power-card">
                        <div class="status-label">设备名称</div>
                        <div class="power-value" id="sysDeviceName" style="font-size: 24px;">MORI Burner</div>
                        <div style="margin-top: 8px; font-size: 13px; color: var(--text-secondary);">
                            固件版本: <span id="sysFirmware">--</span>
                        </div>
                    </div>

                    <div class="power-card battery">
                        <div class="status-label">电池电量</div>
                        <div class="power-value" id="sysBattery">--%</div>
                        <div class="heap-bar">
                            <div class="heap-fill" id="sysBatteryBar" style="width: 0%"></div>
                        </div>
                    </div>

                    <div class="power-card">
                        <div class="status-label">运行时间</div>
                        <div class="power-value" id="sysUptime" style="font-size: 28px;">--:--:--</div>
                        <div style="margin-top: 8px; font-size: 13px; color: var(--text-secondary);">
                            系统启动至今
                        </div>
                    </div>

                    <div class="power-card">
                        <div class="status-label">内存可用</div>
                        <div class="power-value" id="sysHeap">--</div>
                        <div class="heap-bar">
                            <div class="heap-fill" id="sysHeapBar" style="width: 0%"></div>
                        </div>
                    </div>
                </div>

                <div class="card animate-in">
                    <div class="card-header">
                        <h3 class="card-title">📊 系统状态</h3>
                        <button class="btn btn-sm" onclick="refreshSystemInfo()">🔄 刷新</button>
                    </div>
                    <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 16px;">
                        <div class="status-card">
                            <div class="status-label">Wi-Fi 状态</div>
                            <div class="status-value-large" id="sysWifiStatus">--</div>
                        </div>
                        <div class="status-card">
                            <div class="status-label">IP 地址</div>
                            <div class="status-value-large" id="sysIpAddress" style="font-size: 18px;">--</div>
                        </div>
                        <div class="status-card">
                            <div class="status-label">TF状态</div>
                            <div class="status-value-large" id="sysStorage">--</div>
                        </div>
                        <div class="status-card">
                            <div class="status-label">USB 直通</div>
                            <div class="status-value-large" id="sysUsbMode">--</div>
                        </div>
                    </div>
                </div>


            `;

            refreshSystemInfo();
        }

        async function refreshSystemInfo() {
            try {
                const [powerReq, storageReq, wifiReq] = await Promise.allSettled([
                    fetch('/api/power/status'),
                    fetch('/api/storage/status'),
                    fetch('/api/wifi/status')
                ]);

                if (powerReq.status === 'fulfilled' && powerReq.value.ok) {
                    const powerData = await powerReq.value.json();
                    const batteryPercent = estimateBatteryPercent(powerData.ip5306);
                    document.getElementById('sysBattery').textContent = batteryPercent + '%';
                    document.getElementById('sysBatteryBar').style.width = batteryPercent + '%';
                    document.getElementById('sysUptime').textContent = formatDuration(Math.floor(powerData.uptime_ms / 1000));

                    const freeHeap = powerData.free_heap || 0;
                    const minFreeHeap = powerData.min_free_heap || 0;
                    const totalHeap = freeHeap + (minFreeHeap * 2);
                    document.getElementById('sysHeap').textContent = formatSize(freeHeap);
                    document.getElementById('sysHeapBar').style.width = totalHeap > 0 ? (freeHeap / totalHeap * 100) + '%' : '0%';
                }

                if (storageReq.status === 'fulfilled' && storageReq.value.ok) {
                    const storageData = await storageReq.value.json();
                    const tfReady = !!(storageData.tf_ready ?? storageData.mounted);
                    document.getElementById('sysUsbMode').textContent = storageData.usb_passthrough_enabled ? '已启用' : '已禁用';
                    document.getElementById('sysUsbMode').style.color = storageData.usb_passthrough_enabled ? 'var(--accent-warning)' : 'var(--accent-success)';
                    document.getElementById('sysStorage').textContent = tfReady ? '已挂载' : '未挂载';
                    document.getElementById('sysStorage').style.color = tfReady ? 'var(--accent-success)' : 'var(--accent-danger)';
                }

                if (wifiReq.status === 'fulfilled' && wifiReq.value.ok) {
                    const wifiData = await wifiReq.value.json();
                    if (wifiData.connected) {
                        document.getElementById('sysWifiStatus').textContent = '已连接';
                        document.getElementById('sysWifiStatus').style.color = 'var(--accent-success)';
                        document.getElementById('sysIpAddress').textContent = wifiData.ip || '--';
                    } else {
                        document.getElementById('sysWifiStatus').textContent = '未连接';
                        document.getElementById('sysWifiStatus').style.color = 'var(--accent-secondary)';
                        document.getElementById('sysIpAddress').textContent = '--';
                    }
                }

                // 获取固件版本
                try {
                    const deviceRes = await fetch('/api/device/info');
                    if (deviceRes.ok) {
                        const contentType = (deviceRes.headers.get('content-type') || '').toLowerCase();
                        let firmware = '--';

                        if (contentType.includes('application/json')) {
                            const deviceData = await deviceRes.json();
                            firmware = deviceData.firmware_version || deviceData.app_version || '--';
                        } else {
                            const text = await deviceRes.text();
                            const m = text.match(/App version:\s*([^\r\n]+)/i);
                            if (m && m[1]) {
                                firmware = m[1].trim();
                            }
                        }

                        document.getElementById('sysFirmware').textContent = firmware || '--';
                    }
                } catch (e) {
                    document.getElementById('sysFirmware').textContent = '--';
                }
            } catch (error) {
                console.error('System info refresh failed:', error);
            }
        }

        // 设置页面
        function renderSettingsPage() {
            const container = document.getElementById('mainContent');
            container.innerHTML = `
                <div class="page-header animate-in">
                    <h1 class="page-title">⚙️ 设备设置</h1>
                    <p class="page-subtitle">查看设备信息和调试参数</p>
                </div>

                <div class="card animate-in">
                    <div class="card-header">
                        <h3 class="card-title">🔧 设备操作</h3>
                    </div>
                    <div style="display: flex; gap: 12px; flex-wrap: wrap; padding: 16px;">
                        <button class="btn btn-danger" onclick="restartDevice()">🔄 重启设备</button>
                    </div>
                </div>

                <div class="card animate-in">
                    <div class="card-header">
                        <h3 class="card-title">💡 屏幕亮度</h3>
                    </div>
                    <div style="padding: 16px;">
                        <div style="display: flex; align-items: center; gap: 16px;">
                            <span style="font-size: 20px;">🌙</span>
                            <input type="range" id="brightnessSlider" min="0" max="255" value="128" 
                                style="flex: 1; height: 8px; -webkit-appearance: none; background: var(--bg-tertiary); border-radius: 4px; outline: none;"
                                oninput="setBrightnessDebounced(this.value); document.getElementById('brightnessValue').textContent = this.value;">
                            <span style="font-size: 20px;">☀️</span>
                            <span id="brightnessValue" style="min-width: 50px; text-align: right; font-family: monospace;">128</span>
                        </div>
                    </div>
                </div>

                <div class="card animate-in">
                    <div class="card-header">
                        <h3 class="card-title">📦 固件升级</h3>
                    </div>
                    <div style="padding: 16px;">
                        <p style="color: var(--text-secondary); margin-bottom: 16px; font-size: 14px;">上传 moriburnner.bin 进行 OTA 升级，成功后设备会自动重启。不要上传 .elf 文件。</p>
                        <div style="display: flex; gap: 12px; align-items: center; flex-wrap: wrap;">
                            <input type="file" id="firmwareFile" accept=".bin" style="display: none;" onchange="handleFirmwareUpload(this.files[0])">
                            <button class="btn btn-primary" id="firmwareSelectBtn" onclick="document.getElementById('firmwareFile').click()">📤 选择固件文件</button>
                            <button class="btn btn-danger" id="firmwareCancelBtn" onclick="cancelFirmwareUpload()" disabled>&#x2715; &#x53D6;&#x6D88;&#x5347;&#x7EA7;</button>
                            <span id="firmwareFileName" style="color: var(--text-secondary); font-size: 14px;"></span>
                        </div>
                        <div id="firmwareProgress" class="hidden" style="margin-top: 16px;">
                            <div class="progress-info" style="margin-bottom: 8px;">
                                <span id="firmwareStatus">正在上传...</span>
                                <span id="firmwarePercent">0%</span>
                            </div>
                            <div class="progress-bar">
                                <div class="progress-fill" id="firmwareBar" style="width: 0%"></div>
                            </div>
                        </div>
                    </div>
                </div>

                <div class="card animate-in">
                    <div class="card-header">
                        <h3 class="card-title">🚚 系统迁移</h3>
                    </div>
                    <div style="padding: 16px;">
                        <p style="color: var(--text-secondary); margin-bottom: 16px; font-size: 14px;">一键打包系统目录（/sdcard/.setting + /sdcard/.web）为 ZIP，方便换 TF 卡时迁移配置和前端资源。</p>
                        <div style="display: flex; gap: 12px; align-items: center; flex-wrap: wrap;">
                            <a class="btn btn-primary" href="/api/system/migrate_zip" download="mori_system_migration.zip">⬇️ 下载迁移 ZIP</a>
                        </div>
                    </div>
                </div>

                <div class="card animate-in">
                    <div class="card-header">
                        <h3 class="card-title">📥 系统部署</h3>
                    </div>
                    <div style="padding: 16px;">
                        <p style="color: var(--text-secondary); margin-bottom: 16px; font-size: 14px;">上传迁移 ZIP 后自动部署到 /sdcard/.setting 与 /sdcard/.web，适合一键恢复到新 TF 卡。</p>
                        <div style="display: flex; gap: 12px; align-items: center; flex-wrap: wrap;">
                            <input type="file" id="systemDeployZipFile" accept=".zip,application/zip" style="display: none;" onchange="handleSystemDeployZip(this.files[0])">
                            <button class="btn btn-warning" id="systemDeploySelectBtn" onclick="document.getElementById('systemDeployZipFile').click()">📦 选择并部署 ZIP</button>
                            <button class="btn btn-danger" id="systemDeployCancelBtn" onclick="cancelSystemDeploy()" disabled>&#x2715; &#x53D6;&#x6D88;&#x90E8;&#x7F72;</button>
                            <span id="systemDeployZipName" style="color: var(--text-secondary); font-size: 14px;"></span>
                        </div>
                        <div id="systemDeployStatus" style="margin-top: 16px; padding: 12px; background: var(--bg-tertiary); border-radius: 8px; font-family: monospace; font-size: 13px;">
                            空闲
                        </div>
                    </div>
                </div>

                <div class="card animate-in">
                    <div class="card-header">
                        <h3 class="card-title">🔌 USB 直通模式</h3>
                    </div>
                    <div style="padding: 16px;">
                        <p style="color: var(--text-secondary); margin-bottom: 16px; font-size: 14px;">启用后：PC 可把 TF 当 U 盘访问；禁用后：ESP 可访问 TF/Web API。启用时 TF API 会返回 503 以保证安全。</p>
                        <div style="display: flex; gap: 12px; align-items: center; flex-wrap: wrap;">
                            <button class="btn btn-primary" id="usbEnableBtn" onclick="enableUsbPassthrough()">✓ 启用 USB 直通</button>
                            <button class="btn btn-danger" id="usbDisableBtn" onclick="disableUsbPassthrough()">✕ 禁用 USB 直通</button>
                            <button class="btn" onclick="refreshStorageStatus()">🔄 刷新状态</button>
                        </div>
                        <div id="usbStatusInfo" style="margin-top: 16px; padding: 12px; background: var(--bg-tertiary); border-radius: 8px; font-family: monospace; font-size: 13px;">
                            加载中...
                        </div>
                    </div>
                </div>

                <div class="card animate-in">
                    <div class="card-header">
                        <h3 class="card-title">⚡ Bacon SPI 频率</h3>
                    </div>
                    <div style="padding: 16px;">
                        <p style="color: var(--text-secondary); margin-bottom: 16px; font-size: 14px;">设置 ESP32 到 Bacon 的 SPI 频率。支持 20~80MHz，烧录任务运行中不可修改。</p>
                        <div style="display: flex; gap: 12px; align-items: center; flex-wrap: wrap;">
                            <label for="baconSpiMhz" style="color: var(--text-secondary); font-size: 13px;">频率 (MHz)</label>
                            <input type="number" id="baconSpiMhz" class="input" min="20" max="80" step="1" value="40" style="width: 140px;">
                            <button class="btn btn-primary" onclick="applyBaconSpiClock()">✓ 应用</button>
                            <button class="btn" onclick="refreshBaconSpiClock()">🔄 刷新</button>
                        </div>
                        <div id="baconSpiStatus" style="margin-top: 16px; padding: 12px; background: var(--bg-tertiary); border-radius: 8px; font-family: monospace; font-size: 13px;">
                            加载中...
                        </div>
                    </div>
                </div>

                <div class="card animate-in">
                    <div class="card-header">
                        <h3 class="card-title">🧠 烧录核心分配</h3>
                    </div>
                    <div style="padding: 16px;">
                        <p style="color: var(--text-secondary); margin-bottom: 16px; font-size: 14px;">配置烧录阶段任务核心：擦除、读取 TF、读写 PSRAM。可选 auto / CPU0 / CPU1。</p>
                        <div class="settings-grid">
                            <div class="setting-item">
                                <label class="setting-label" for="burnCoreErase">擦除</label>
                                <select id="burnCoreErase" class="input">
                                    <option value="auto">auto</option>
                                    <option value="cpu0">CPU0</option>
                                    <option value="cpu1">CPU1</option>
                                </select>
                                <span class="setting-desc">擦除阶段任务核心</span>
                            </div>
                            <div class="setting-item">
                                <label class="setting-label" for="burnCoreTf">读 TF</label>
                                <select id="burnCoreTf" class="input">
                                    <option value="auto">auto</option>
                                    <option value="cpu0">CPU0</option>
                                    <option value="cpu1">CPU1</option>
                                </select>
                                <span class="setting-desc">TF 读取任务核心</span>
                            </div>
                            <div class="setting-item">
                                <label class="setting-label" for="burnCorePsram">读写 PSRAM</label>
                                <select id="burnCorePsram" class="input">
                                    <option value="auto">auto</option>
                                    <option value="cpu0">CPU0</option>
                                    <option value="cpu1">CPU1</option>
                                </select>
                                <span class="setting-desc">烧录主任务/PSRAM 访问核心</span>
                            </div>
                        </div>
                        <div style="display: flex; gap: 12px; margin-top: 16px; flex-wrap: wrap;">
                            <button class="btn btn-primary" onclick="applyBurnCoreConfig()">✓ 应用</button>
                            <button class="btn" onclick="refreshBurnCoreConfig()">🔄 刷新</button>
                        </div>
                        <div id="burnCoreStatus" style="margin-top: 16px; padding: 12px; background: var(--bg-tertiary); border-radius: 8px; font-family: monospace; font-size: 13px;">
                            加载中...
                        </div>
                    </div>
                </div>

                <div class="card animate-in">
                    <div class="card-header">
                        <h3 class="card-title">🌐 语言设置</h3>
                    </div>
                    <div style="padding: 16px;">
                        <p style="color: var(--text-secondary); margin-bottom: 16px; font-size: 14px;">选择界面语言，点击应用后生效。</p>
                        <div style="display: flex; gap: 12px; align-items: center; flex-wrap: wrap;">
                            <button class="btn" onclick="loadLanguageList()">📋 读取语言列表</button>
                            <select id="languageSelect" class="input" style="width: 200px;">
                                <option value="">请先读取列表</option>
                            </select>
                            <button class="btn btn-primary" onclick="applyLanguage()">✓ 应用语言</button>
                        </div>
                        <div id="languageStatus" style="margin-top: 16px; padding: 12px; background: var(--bg-tertiary); border-radius: 8px; font-family: monospace; font-size: 13px;">
                            空闲
                        </div>
                    </div>
                </div>

                <div class="card animate-in">
                    <div class="card-header">
                        <h3 class="card-title">🔋 IP5306 配置</h3>
                    </div>
                    <div style="padding: 16px;">
                        <p style="color: var(--text-secondary); margin-bottom: 16px; font-size: 14px;">读取或保存 IP5306 电源管理芯片的 INI 配置文件。</p>
                        <div style="display: flex; gap: 12px; margin-bottom: 16px; flex-wrap: wrap;">
                            <button class="btn" onclick="loadIp5306Ini()">📖 读取配置</button>
                            <button class="btn btn-primary" onclick="saveIp5306Ini()">💾 保存配置</button>
                        </div>
                        <textarea id="ip5306IniContent" class="input" style="width: 100%; min-height: 200px; font-family: 'Consolas', monospace; font-size: 13px; resize: vertical;" placeholder="点击读取配置..."></textarea>
                        <div id="ip5306Status" style="margin-top: 12px; padding: 12px; background: var(--bg-tertiary); border-radius: 8px; font-family: monospace; font-size: 13px;">
                            空闲
                        </div>
                    </div>
                </div>

                <div class="card animate-in">
                    <div class="card-header">
                        <h3 class="card-title">📱 设备信息</h3>
                    </div>
                    <div id="deviceInfo">
                        <div style="text-align: center; padding: 40px; color: var(--text-secondary);">
                            加载中...
                        </div>
                    </div>
                </div>

                <div class="card animate-in">
                    <h3 class="card-title" style="margin-bottom: 20px;">🔧 MCU 探测设置</h3>
                    <div class="settings-grid">
                        <div class="setting-item">
                            <label class="setting-label">序列模式 (seq)</label>
                            <select class="input" id="mcuSeq">
                                <option value="auto">自动 (auto)</option>
                                <option value="std">标准 (std)</option>
                                <option value="alt">备用 (alt)</option>
                            </select>
                            <span class="setting-desc">SWD 序列生成模式</span>
                        </div>
                        <div class="setting-item">
                            <label class="setting-label">延迟 (delay)</label>
                            <input type="number" class="input" id="mcuDelay" value="8" min="0" max="100">
                            <span class="setting-desc">微秒级延迟 (0-100)</span>
                        </div>
                        <div class="setting-item">
                            <label class="setting-label">不复位 (norst)</label>
                            <select class="input" id="mcuNorst">
                                <option value="false">否</option>
                                <option value="true">是</option>
                            </select>
                            <span class="setting-desc">跳过复位序列</span>
                        </div>
                        <div class="setting-item">
                            <label class="setting-label">交换 CLK/DIO (swap)</label>
                            <select class="input" id="mcuSwap">
                                <option value="false">否</option>
                                <option value="true">是</option>
                            </select>
                            <span class="setting-desc">交换时钟和数据线</span>
                        </div>
                    </div>
                    <div style="margin-top: 24px;">
                        <button class="btn btn-primary" onclick="probeMCUWithParams()">🔍 执行探测</button>
                    </div>
                </div>
            `;

            loadDeviceInfo();
            loadBrightness();
            refreshStorageStatus();
            refreshBaconSpiClock();
            refreshBurnCoreConfig();
            setFirmwareCancelUi(Boolean(state.firmwareUploadXHR), state.firmwareCancelPending);
            setSystemDeployCancelUi(state.systemDeployBusy, state.systemDeployCancelPending);
        }

        // API 调用封装
        async function apiCall(url, options = {}) {
            try {
                const response = await fetch(url, {
                    ...options,
                    headers: {
                        ...options.headers
                    }
                });
                
                if (response.status === 503) {
                    state.usbPassThrough = true;
                    updateSystemStatus('warning', 'USB 直通模式');
                    throw new Error('USB Pass-Through 模式已开启，请先在基础设置页关闭');
                }
                
                state.usbPassThrough = false;
                return response;
            } catch (error) {
                if (error.message.includes('USB Pass-Through')) {
                    showToast(error.message, 'warning');
                } else {
                    showToast('网络错误: ' + error.message, 'error');
                }
                throw error;
            }
        }

        async function readApiPayload(response) {
            const text = await response.text();
            if (!text) {
                return { ok: response.ok, message: '' };
            }
            try {
                return JSON.parse(text);
            } catch (e) {
                return { ok: false, message: text };
            }
        }

        async function startTaskRequest(url) {
            const response = await apiCall(url, { method: 'POST' });
            const data = await readApiPayload(response);
            return { response, data };
        }

        // 文件列表加载（支持多选）
        async function loadFileList(path) {
            state.currentPath = path;
            updateBreadcrumb(path);
            state.selectedFiles.clear();
            updateMultiSelectBar();
            
            try {
                const response = await apiCall(`/api/tf/list?path=${encodeURIComponent(path)}`);
                const data = await response.json();
                
                if (data.ok) {
                    renderFileList(data.entries);
                } else {
                    throw new Error(data.message || '加载失败');
                }
            } catch (error) {
                document.getElementById('fileList').innerHTML = `
                    <div style="text-align: center; padding: 40px; color: var(--accent-secondary);">
                        加载失败: ${error.message}
                    </div>
                `;
            }

            if (state.powerViewMode === 'basic') {
                updatePowerBasicView();
            }
        }

        function renderFileList(entries) {
            const list = document.getElementById('fileList');
            
            if (entries.length === 0) {
                list.innerHTML = `
                    <div style="text-align: center; padding: 60px; color: var(--text-secondary);">
                        <div style="font-size: 48px; margin-bottom: 16px; opacity: 0.3;">📂</div>
                        空文件夹
                    </div>
                `;
                return;
            }

            entries.sort((a, b) => {
                if (a.is_dir === b.is_dir) return a.name.localeCompare(b.name);
                return a.is_dir ? -1 : 1;
            });

            list.innerHTML = entries.map(entry => `
                <div class="file-item" data-path="${entry.path}" data-is-dir="${entry.is_dir}" data-name="${entry.name}" data-size="${entry.size}" onclick="handleFileClick(this, event)">
                    <div class="file-icon">${entry.is_dir ? '📁' : getFileIcon(entry.name)}</div>
                    <div class="file-name" title="${entry.name}">${entry.name}</div>
                    <div class="file-size">${entry.is_dir ? '--' : formatSize(entry.size)}</div>
                    <div class="file-date">--</div>
                    <div class="file-actions" onclick="event.stopPropagation()">
                        ${entry.is_dir ? '' : `<button class="btn" onclick="downloadFile('${entry.path}')">⬇️</button>`}
                        <button class="btn" onclick="showRenameModal('${entry.path}')">✏️</button>
                        <button class="btn btn-danger" onclick="showDeleteModal('${entry.path}', ${entry.is_dir})">🗑️</button>
                    </div>
                </div>
            `).join('');
        }

        function handleFileClick(element, event) {
            // 如果按住 Ctrl/Cmd，切换多选
            if (event.ctrlKey || event.metaKey) {
                toggleFileSelection(element);
                return;
            }
            
            const path = element.getAttribute('data-path');
            const isDir = element.getAttribute('data-is-dir') === 'true';
            const name = element.getAttribute('data-name');
            const size = parseInt(element.getAttribute('data-size'));
            
            if (isDir) {
                navigateTo(path);
            } else {
                const ext = name.split('.').pop().toLowerCase();
                const isRom = ['gba', 'gb', 'gbc'].includes(ext);
                const isTextFile = ['ini', 'txt', 'cfg', 'conf', 'json', 'xml', 'md', 'log'].includes(ext);
                
                if (isRom) {
                    showFileActionModal(name, size, path);
                } else if (isTextFile) {
                    openTextEditor(path, name);
                } else {
                    downloadFile(path);
                }
            }
        }

        function toggleFileSelection(element) {
            const path = element.getAttribute('data-path');
            
            if (state.selectedFiles.has(path)) {
                state.selectedFiles.delete(path);
                element.classList.remove('selected');
            } else {
                state.selectedFiles.add(path);
                element.classList.add('selected');
            }
            
            updateMultiSelectBar();
        }

        function updateMultiSelectBar() {
            const bar = document.getElementById('multiSelectBar');
            const count = state.selectedFiles.size;
            
            if (count > 0) {
                bar.classList.add('active');
                bar.querySelector('.multi-select-count').textContent = `已选择 ${count} 项`;
            } else {
                bar.classList.remove('active');
            }
        }

        function clearSelection() {
            state.selectedFiles.clear();
            document.querySelectorAll('.file-item.selected').forEach(el => el.classList.remove('selected'));
            updateMultiSelectBar();
        }

        async function downloadSelected() {
            // 批量下载：逐个下载或打包（这里简单实现为逐个下载）
            const files = Array.from(state.selectedFiles);
            for (const path of files) {
                downloadFile(path);
                await new Promise(r => setTimeout(r, 500)); // 避免同时触发太多下载
            }
            clearSelection();
        }

        async function deleteSelected() {
            if (!confirm(`确定要删除选中的 ${state.selectedFiles.size} 个项目吗？`)) return;
            
            // 逐个删除
            for (const path of state.selectedFiles) {
                try {
                    await apiCall(`/api/tf/delete?path=${encodeURIComponent(path)}`, { method: 'DELETE' });
                } catch (error) {
                    showToast(`删除失败: ${path}`, 'error');
                }
            }
            
            clearSelection();
            refreshFileList();
            showToast('批量删除完成', 'success');
        }

        // Wi-Fi 功能
        async function loadWiFiStatus() {
            try {
                // 获取当前连接状态
                const response = await fetch('/api/wifi/status');
                const data = await response.json();
                
                const container = document.getElementById('wifiCurrentStatus');
                const indicator = document.getElementById('wifiStatusIndicator');
                
                if (data.connected) {
                    indicator.className = 'ws-indicator connected';
                    indicator.innerHTML = '<span class="ws-dot"></span>已连接';
                    
                    container.innerHTML = `
                        <div class="wifi-status-card">
                            <div class="wifi-icon-large">📶</div>
                            <div class="wifi-info">
                                <div class="wifi-name">${data.ssid}</div>
                                <div class="wifi-details">
                                    IP: ${data.ip} | 信号: ${data.rssi}dBm | 信道: ${data.channel}
                                </div>
                            </div>
                        </div>
                    `;
                } else {
                    indicator.className = 'ws-indicator disconnected';
                    indicator.innerHTML = '<span class="ws-dot"></span>未连接';
                    
                    container.innerHTML = `
                        <div style="text-align: center; padding: 40px; color: var(--text-secondary);">
                            <div style="font-size: 48px; margin-bottom: 16px;">📡</div>
                            未连接到 Wi-Fi 网络<br>
                            <button class="btn btn-primary" style="margin-top: 16px;" onclick="scanWiFi()">
                                扫描网络
                            </button>
                        </div>
                    `;
                }
            } catch (error) {
                document.getElementById('wifiCurrentStatus').innerHTML = `
                    <div style="text-align: center; padding: 40px; color: var(--accent-secondary);">
                        获取状态失败
                    </div>
                `;
            }
        }

        async function scanWiFi() {
            const list = document.getElementById('wifiList');
            list.innerHTML = `
                <div class="wifi-scanning">
                    <div class="spinner" style="margin: 0 auto 16px;"></div>
                    正在扫描 Wi-Fi 网络...
                </div>
            `;
            
            try {
                const response = await fetch('/api/wifi/scan');
                const data = await response.json();
                
                if (data.networks && data.networks.length > 0) {
                    renderWiFiList(data.networks);
                } else {
                    list.innerHTML = `
                        <div style="text-align: center; padding: 40px; color: var(--text-secondary);">
                            未找到可用网络
                        </div>
                    `;
                }
            } catch (error) {
                list.innerHTML = `
                    <div style="text-align: center; padding: 40px; color: var(--accent-secondary);">
                        扫描失败: ${error.message}
                    </div>
                `;
            }
        }

        function renderWiFiList(networks) {
            const list = document.getElementById('wifiList');
            
            // 去重并排序（信号强的在前）
            const uniqueNetworks = networks.filter((n, i, arr) => 
                arr.findIndex(t => t.ssid === n.ssid) === i
            ).sort((a, b) => b.rssi - a.rssi);
            
            list.innerHTML = uniqueNetworks.map(net => {
                const signalLevel = Math.min(4, Math.max(1, Math.floor((net.rssi + 90) / 20)));
                const bars = Array(4).fill(0).map((_, i) => 
                    `<div class="signal-bar ${i < signalLevel ? 'active' : ''}" style="height: ${(i+1)*5}px;"></div>`
                ).join('');
                
                return `
                    <div class="wifi-item" onclick="showWiFiConnectModal('${net.ssid}', ${net.encryption !== 0})">
                        <div class="wifi-signal">
                            <div class="signal-bars">${bars}</div>
                            <span style="font-size: 10px; color: var(--text-secondary);">${net.rssi}dBm</span>
                        </div>
                        <div class="wifi-ssid">${net.ssid}</div>
                        <div class="wifi-lock">${net.encryption !== 0 ? '🔒' : '🔓'}</div>
                        <button class="btn btn-sm wifi-connect-btn">连接</button>
                    </div>
                `;
            }).join('');
        }

        function showWiFiConnectModal(ssid, needsPassword) {
            document.getElementById('wifiConnectSsid').textContent = ssid;
            document.getElementById('wifiPassword').value = '';
            document.getElementById('wifiPassword').style.display = needsPassword ? 'block' : 'none';
            document.getElementById('wifiConnectModal').classList.add('active');
            
            if (needsPassword) {
                setTimeout(() => document.getElementById('wifiPassword').focus(), 100);
            }
        }

        function closeWifiConnectModal() {
            document.getElementById('wifiConnectModal').classList.remove('active');
        }

        async function confirmWifiConnect() {
            const ssid = document.getElementById('wifiConnectSsid').textContent;
            const password = document.getElementById('wifiPassword').value;
            const save = document.getElementById('saveWifiConfig').checked;
            
            closeWifiConnectModal();
            showToast(`正在连接 ${ssid}...`, 'info');
            
            try {
                const response = await fetch('/api/wifi/connect', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ ssid, password, save })
                });
                
                const data = await response.json();
                
                if (data.success) {
                    showToast('连接成功', 'success');
                    setTimeout(loadWiFiStatus, 2000);
                } else {
                    showToast('连接失败: ' + data.message, 'error');
                }
            } catch (error) {
                showToast('连接错误: ' + error.message, 'error');
            }
        }

        async function enableAPMode() {
            if (!confirm('确定要切换到 AP 模式吗？这将断开当前 Wi-Fi 连接。')) return;
            
            try {
                await fetch('/api/wifi/ap', { method: 'POST' });
                showToast('已启用 AP 模式', 'success');
                setTimeout(loadWiFiStatus, 2000);
            } catch (error) {
                showToast('切换失败', 'error');
            }
        }

        async function disconnectWiFi() {
            try {
                await fetch('/api/wifi/disconnect', { method: 'POST' });
                showToast('已断开连接', 'success');
                setTimeout(loadWiFiStatus, 1000);
            } catch (error) {
                showToast('断开失败', 'error');
            }
        }

        async function forgetSavedWiFi() {
            if (!confirm('确定要忘记保存的 Wi-Fi 密码吗？')) return;
            
            try {
                await fetch('/api/wifi/forget', { method: 'POST' });
                showToast('已清除保存的配置', 'success');
            } catch (error) {
                showToast('操作失败', 'error');
            }
        }

        // 电源详情
        async function loadPowerDetail() {
            if (!state.powerData) {
                await refreshStatus();
            }
            
            updatePowerDetailTables(state.powerData);
        }

        function updatePowerDetailTables(data) {
            if (!data) return;
            const ip5306 = data.ip5306 || {};
            const tca9555 = data.tca9555 || {};
            
            // 更新主显示
            const batteryPercent = estimateBatteryPercent(ip5306);
            document.getElementById('powerBattery').textContent = batteryPercent + '%';
            document.getElementById('powerBatteryBar').style.width = batteryPercent + '%';
            
            const stateMap = {
                'charging': '⚡ 充电中',
                'charge_full': '✓ 已充满',
                'discharging': '🔋 放电中',
                'discharging_light_load': '💤 轻载',
                'no_battery_external_power': '🔌 仅外部供电(无电池)',
                'unknown': '❓ 未知'
            };
            const chargeState = ip5306.charge_state || 'unknown';
            document.getElementById('powerChargeState').textContent = stateMap[chargeState] || chargeState;
            document.getElementById('powerCurrent').textContent = ip5306.charge_current_cfg_ma || '--';
            document.getElementById('powerUptime').textContent = formatDuration(Math.floor(data.uptime_ms / 1000));
            document.getElementById('powerHeap').textContent = formatSize(data.free_heap);
            document.getElementById('powerHeapBar').style.width = Math.min(100, (data.free_heap / 500000) * 100) + '%';
            
            // IP5306 寄存器表
            const ip5306Body = document.querySelector('#ip5306Table tbody');
            if (ip5306Body) {
                const regs = [
                    { name: 'SYS_CTL0', val: ip5306.sys_ctl0, desc: '系统控制0' },
                    { name: 'SYS_CTL1', val: ip5306.sys_ctl1, desc: '系统控制1' },
                    { name: 'SYS_CTL2', val: ip5306.sys_ctl2, desc: '系统控制2' },
                    { name: 'READ0', val: ip5306.read0, desc: '状态读取0' },
                    { name: 'READ1', val: ip5306.read1, desc: '状态读取1' },
                    { name: 'READ2', val: ip5306.read2, desc: '状态读取2' },
                    { name: 'READ3', val: ip5306.read3, desc: '状态读取3' },
                    { name: 'CHG_DIG_CTL0', val: ip5306.chg_dig_ctl0, desc: '充电数字控制' }
                ];
                
                ip5306Body.innerHTML = regs.map(r => `
                    <tr>
                        <td>${r.name}<br><span style="color: var(--text-secondary); font-size: 11px;">${r.desc}</span></td>
                        <td class="reg-value">0x${parseTelemetryNumber(r.val, 0).toString(16).toUpperCase().padStart(2, '0')}</td>
                        <td>${formatBits(parseTelemetryNumber(r.val, 0))}</td>
                    </tr>
                `).join('');
            }
            
            // TCA9555 表
            const tcaBody = document.querySelector('#tca9555Table tbody');
            if (tcaBody) {
                const inputVal = parseTelemetryNumber(tca9555.input ?? tca9555.inputs, 0);
                const outputVal = parseTelemetryNumber(tca9555.output ?? tca9555.outputs, 0);
                const configVal = parseTelemetryNumber(tca9555.config, 0);
                tcaBody.innerHTML = `
                    <tr>
                        <td>输入状态 (INPUT)</td>
                        <td class="reg-value">0x${inputVal.toString(16).toUpperCase().padStart(4, '0')}</td>
                        <td>${formatBits16(inputVal)}</td>
                    </tr>
                    <tr>
                        <td>输出状态 (OUTPUT)</td>
                        <td class="reg-value">0x${outputVal.toString(16).toUpperCase().padStart(4, '0')}</td>
                        <td>${formatBits16(outputVal)}</td>
                    </tr>
                    <tr>
                        <td>方向配置 (CONFIG)</td>
                        <td class="reg-value">0x${configVal.toString(16).toUpperCase().padStart(4, '0')}</td>
                        <td>${formatBits16(configVal)}</td>
                    </tr>
                `;
            }
        }

        function formatBits(val) {
            return Array(8).fill(0).map((_, i) => {
                const bit = (val >> (7-i)) & 1;
                return `<span class="bit-badge ${bit ? 'on' : ''}">${bit}</span>`;
            }).join('');
        }

        function formatBits16(val) {
            return Array(16).fill(0).map((_, i) => {
                const bit = (val >> (15-i)) & 1;
                return `<span class="bit-badge ${bit ? 'on' : ''}">${bit}</span>`;
            }).join('');
        }

        function refreshPowerDetail() {
            refreshStatus().then(() => {
                updatePowerDetailTables(state.powerData);
            });
        }

        // 电源页面自动刷新功能
        function startPowerAutoRefresh() {
            // 先清除旧的定时器
            if (state.powerRefreshTimer) {
                clearInterval(state.powerRefreshTimer);
                state.powerRefreshTimer = null;
            }

            // 立即执行一次刷新
            refreshPowerDetail();

            // 设置新的定时器
            if (state.powerAutoRefresh) {
                state.powerRefreshTimer = setInterval(() => {
                    // 只在当前页面是 power 时才刷新
                    if (state.currentPage === 'power') {
                        refreshPowerDetail();
                    } else {
                        // 不在电源页面时停止刷新
                        stopPowerAutoRefresh();
                    }
                }, state.powerRefreshInterval);
            }
        }

        function stopPowerAutoRefresh() {
            if (state.powerRefreshTimer) {
                clearInterval(state.powerRefreshTimer);
                state.powerRefreshTimer = null;
            }
        }

        function togglePowerAutoRefresh() {
            state.powerAutoRefresh = !state.powerAutoRefresh;
            const btn = document.getElementById('powerAutoRefreshBtn');
            if (btn) {
                btn.innerHTML = `<span>${state.powerAutoRefresh ? '⏸️' : '▶️'}</span> ${state.powerAutoRefresh ? '开启' : '暂停'}`;
                btn.classList.toggle('active', state.powerAutoRefresh);
            }

            if (state.powerAutoRefresh) {
                startPowerAutoRefresh();
                showToast('自动刷新已开启', 'success');
            } else {
                stopPowerAutoRefresh();
                showToast('自动刷新已暂停', 'info');
            }
        }

        function changePowerRefreshInterval() {
            const select = document.getElementById('powerRefreshInterval');
            if (select) {
                state.powerRefreshInterval = parseInt(select.value);
                // 如果正在自动刷新，重启定时器以应用新间隔
                if (state.powerAutoRefresh) {
                    startPowerAutoRefresh();
                }
                showToast(`刷新间隔已设置为 ${state.powerRefreshInterval / 1000} 秒`, 'success');
            }
        }

        function switchPowerView(mode) {
            state.powerViewMode = mode;
            const basicBtn = document.getElementById('powerBasicBtn');
            const advancedBtn = document.getElementById('powerAdvancedBtn');
            const basicView = document.getElementById('powerBasicView');
            const advancedView = document.getElementById('powerAdvancedView');

            if (mode === 'basic') {
                basicBtn.classList.add('active');
                advancedBtn.classList.remove('active');
                basicView.style.display = 'block';
                advancedView.style.display = 'none';
                updatePowerBasicView();
            } else {
                basicBtn.classList.remove('active');
                advancedBtn.classList.add('active');
                basicView.style.display = 'none';
                advancedView.style.display = 'block';
            }
        }

        function updatePowerBasicView() {
            if (!state.powerData || !state.powerData.ip5306) return;

            const ip5306 = state.powerData.ip5306;
            const chargeCurrentCfg = parseInt(ip5306.charge_current_cfg_ma, 10);
            const chargeState = ip5306.charge_state || 'unknown';
            const boostEnabled = !!(ip5306.boost_enable_cfg ?? ip5306.boost_enable);
            const lowPowerShutdown = !!(ip5306.low_power_shutdown_cfg ?? ip5306.low_power_shutdown);
            const keyEnabled = !!(ip5306.key_shutdown_enable_cfg ?? ip5306.key_enable);
            const ledEnabled = !!(ip5306.wled_toggle_cfg ?? ip5306.led_enable);
            const stateMap = {
                'charging': '充电中',
                'charge_full': '已充满',
                'discharging': '放电中',
                'discharging_light_load': '轻载放电',
                'no_battery_external_power': '仅外部供电(无电池)',
                'unknown': '未知'
            };

            document.getElementById('basicChargeState').textContent = stateMap[chargeState] || chargeState;
            {
                const currentText = document.getElementById('basicChargeCurrentText');
                if (currentText) {
                    const shownCurrent = Number.isFinite(chargeCurrentCfg) ? chargeCurrentCfg : 450;
                    currentText.textContent = `${shownCurrent}mA (固定)`;
                }
            }
            document.getElementById('basicBoostMode').textContent = boostEnabled ? '已启用' : '已禁用';
            document.getElementById('basicLowPowerShutdown').textContent = lowPowerShutdown ? '已启用' : '已禁用';
            document.getElementById('basicKeyEnable').textContent = keyEnabled ? '已启用' : '已禁用';
            document.getElementById('basicLedEnable').textContent = ledEnabled ? '已启用' : '已禁用';
        }

        // 多文件上传
        function showMultiUploadModal() {
            const modal = document.getElementById('multiUploadModal');
            const zone = document.querySelector('#multiUploadModal .upload-zone');
            modal.classList.add('active');
            modal.classList.remove('minimized');
            
            document.getElementById('multiFileInput').value = '';
            document.getElementById('multiSelectArea').classList.remove('hidden');
            document.getElementById('multiUploadList').classList.add('hidden');
            document.getElementById('multiUploadStats').classList.add('hidden');
            document.getElementById('cancelAllBtn').classList.add('hidden');
            document.getElementById('multiStatSpeed').textContent = '0 KB/s';
            document.getElementById('multiStatTime').textContent = '00:00';
            document.getElementById('multiStatETA').textContent = '计算中...';
            
            state.multiUploads.clear();
            if (state.multiUploadStatsTimer) {
                clearInterval(state.multiUploadStatsTimer);
                state.multiUploadStatsTimer = null;
            }
            setMultiUploadCancelUi(false);

            if (zone) {
                zone.ondragover = (e) => {
                    e.preventDefault();
                    zone.classList.add('dragover');
                };
                zone.ondragleave = () => zone.classList.remove('dragover');
                zone.ondrop = (e) => {
                    e.preventDefault();
                    zone.classList.remove('dragover');
                    const files = e.dataTransfer ? e.dataTransfer.files : null;
                    if (files && files.length > 0) {
                        startMultiUpload(files);
                    }
                };
            }
        }

        function closeMultiUploadModal() {
            // 检查是否有进行中的上传
            const activeUploads = Array.from(state.multiUploads.values()).filter(u => u.xhr && u.xhr.readyState !== 4);
            if (activeUploads.length > 0) {
                if (!confirm('有文件正在上传，确定要关闭吗？')) return;
                activeUploads.forEach(u => u.xhr.abort());
            }

            if (state.multiUploadStatsTimer) {
                clearInterval(state.multiUploadStatsTimer);
                state.multiUploadStatsTimer = null;
            }
            
            document.getElementById('multiUploadModal').classList.remove('active');
            refreshFileList();
        }

        function minimizeMultiUpload() {
            const modal = document.getElementById('multiUploadModal');
            if (modal.classList.contains('minimized')) {
                modal.classList.remove('minimized');
            } else {
                modal.classList.add('minimized');
                showToast('上传已最小化到右下角', 'info');
            }
        }

        function startMultiUpload(files) {
            if (!files || files.length === 0) return;
            
            document.getElementById('multiSelectArea').classList.add('hidden');
            document.getElementById('multiUploadList').classList.remove('hidden');
            document.getElementById('multiUploadStats').classList.remove('hidden');
            document.getElementById('cancelAllBtn').classList.remove('hidden');
            setMultiUploadCancelUi(false);
            
            const list = document.getElementById('multiUploadList');
            list.innerHTML = '';
            
            // 为每个文件创建上传项
            Array.from(files).forEach((file, index) => {
                const id = `upload-${Date.now()}-${index}`;
                const item = document.createElement('div');
                item.className = 'upload-file-item';
                item.id = id;
                item.innerHTML = `
                    <div class="upload-file-icon">${getFileIcon(file.name)}</div>
                    <div class="upload-file-info">
                        <div class="upload-file-name">${file.name}</div>
                        <div class="upload-file-size">${formatSize(file.size)}</div>
                    </div>
                    <div class="upload-file-status" id="${id}-status">
                        <span style="font-size: 12px; color: var(--text-secondary);">等待中...</span>
                    </div>
                `;
                list.appendChild(item);
                
                // 延迟启动上传，避免同时发送太多请求
                setTimeout(() => {
                    uploadSingleFile(id, file);
                }, index * 200);
            });
            
            // 启动总统计更新
            state.multiUploadStartTime = Date.now();
            if (state.multiUploadStatsTimer) {
                clearInterval(state.multiUploadStatsTimer);
            }
            state.multiUploadStatsTimer = setInterval(updateMultiUploadStats, 500);
            updateMultiUploadStats();
        }

        function uploadSingleFile(id, file) {
            const statusEl = document.getElementById(`${id}-status`);
            const xhr = new XMLHttpRequest();
            
            const uploadData = {
                xhr: xhr,
                stats: {
                    loaded: 0,
                    total: file.size,
                    startTime: Date.now(),
                    speed: 0
                }
            };
            state.multiUploads.set(id, uploadData);
            
            statusEl.innerHTML = `
                <div class="upload-progress-mini">
                    <div class="upload-progress-mini-fill" id="${id}-bar" style="width: 0%"></div>
                </div>
                <span style="font-size: 12px; min-width: 40px; text-align: right;" id="${id}-percent">0%</span>
            `;
            
            xhr.upload.addEventListener('progress', (e) => {
                if (e.lengthComputable) {
                    const percent = Math.round((e.loaded / e.total) * 100);
                    document.getElementById(`${id}-bar`).style.width = percent + '%';
                    document.getElementById(`${id}-percent`).textContent = percent + '%';
                    uploadData.stats.loaded = e.loaded;
                    updateMultiUploadStats();
                }
            });
            
            xhr.addEventListener('load', () => {
                const responseText = xhr.responseText || '';
                if (isCancelResponse(xhr.status, responseText)) {
                    statusEl.innerHTML = '<span style="color: var(--accent-warning);">⏹ 已取消</span>';
                } else if (xhr.status === 200) {
                    try {
                        const data = JSON.parse(responseText || '{}');
                        if (data.ok) {
                            statusEl.innerHTML = '<span style="color: var(--accent-success);">✓ 完成</span>';
                        } else {
                            statusEl.innerHTML = `<span style="color: var(--accent-secondary);">✗ ${escapeHtml(data.message || '失败')}</span>`;
                        }
                    } catch (error) {
                        statusEl.innerHTML = '<span style="color: var(--accent-success);">✓ 完成</span>';
                    }
                } else {
                    statusEl.innerHTML = '<span style="color: var(--accent-secondary);">✗ 失败</span>';
                }
                updateMultiUploadStats();
            });
            
            xhr.addEventListener('error', () => {
                statusEl.innerHTML = '<span style="color: var(--accent-secondary);">✗ 错误</span>';
                updateMultiUploadStats();
            });
            
            xhr.addEventListener('abort', () => {
                statusEl.innerHTML = '<span style="color: var(--accent-warning);">⏹ 已取消</span>';
                updateMultiUploadStats();
            });
            
            xhr.open('POST', `/api/tf/upload?dir=${encodeURIComponent(state.currentPath)}&name=${encodeURIComponent(file.name)}`);
            xhr.send(file);
        }

        function updateMultiUploadStats() {
            const uploads = Array.from(state.multiUploads.values());
            const activeUploads = uploads.filter(u => u.xhr && u.xhr.readyState !== 4);

            if (uploads.length === 0) {
                return;
            }
            
            if (activeUploads.length === 0 && uploads.length > 0) {
                // 全部完成，计算平均速度
                let totalSize = 0;
                uploads.forEach(u => {
                    totalSize += u.stats.total;
                });
                const totalTime = (Date.now() - state.multiUploadStartTime) / 1000;
                const avgSpeed = totalTime > 0 ? totalSize / totalTime : 0;
                document.getElementById('multiStatETA').textContent = '已完成';
                document.getElementById('multiStatSpeed').textContent = formatSpeed(avgSpeed);
                document.getElementById('multiStatTime').textContent = formatDuration(totalTime);
                document.getElementById('cancelAllBtn').classList.add('hidden');
                if (state.multiUploadStatsTimer) {
                    clearInterval(state.multiUploadStatsTimer);
                    state.multiUploadStatsTimer = null;
                }
                return;
            }
            
            // 计算总速度和进度
            let totalLoaded = 0;
            let totalSize = 0;
            let totalSpeed = 0;
            
            uploads.forEach(u => {
                totalLoaded += u.stats.loaded;
                totalSize += u.stats.total;
                
                // 估算速度
                const elapsed = (Date.now() - u.stats.startTime) / 1000;
                if (elapsed > 0) {
                    totalSpeed += u.stats.loaded / elapsed;
                }
            });
            
            document.getElementById('multiStatSpeed').textContent = formatSpeed(totalSpeed);
            document.getElementById('multiStatTime').textContent = formatDuration((Date.now() - state.multiUploadStartTime) / 1000);
            
            if (totalSpeed > 0 && totalLoaded < totalSize) {
                const remaining = (totalSize - totalLoaded) / totalSpeed;
                document.getElementById('multiStatETA').textContent = formatDuration(remaining);
            } else {
                document.getElementById('multiStatETA').textContent = '即将完成';
            }
            
        }

        function cancelAllUploads() {
            state.multiUploads.forEach((data, id) => {
                if (data.xhr && data.xhr.readyState !== 4) {
                    data.xhr.abort();
                }
            });
            if (state.multiUploadStatsTimer) {
                clearInterval(state.multiUploadStatsTimer);
                state.multiUploadStatsTimer = null;
            }
            setMultiUploadCancelUi(false);
            updateMultiUploadStats();
            showToast('已取消所有上传', 'warning');
        }

        function showFileActionModal(filename, size, path) {
            state.selectedFile = { filename, size, path };
            const ext = filename.split('.').pop().toLowerCase();
            const mode = ext === 'gba' ? 'gba' : 'mbc5';

            document.getElementById('fileActionName').textContent = filename;
            document.getElementById('fileActionInfo').textContent = `${formatSize(size)} • 准备烧录到卡带`;
            document.getElementById('fileActionIcon').textContent = getFileIcon(filename);
            const writePathSelect = document.getElementById('fileActionWritePathSelect');
            if (writePathSelect) {
                writePathSelect.value = getEffectiveWritePath(mode, state.writePathMode || 'direct');
            }
            const psramMbSelect = document.getElementById('fileActionPsramMbSelect');
            if (psramMbSelect) {
                psramMbSelect.value = String(getPsramWindowMb());
            }
            updateWritePathControls(mode);
            document.getElementById('fileActionModal').classList.add('active');
        }

        function closeFileActionModal() {
            document.getElementById('fileActionModal').classList.remove('active');
            state.selectedFile = null;
        }

        async function confirmBurn() {
            if (!state.selectedFile) return;

            const fileToBurn = { ...state.selectedFile };
            const writePathSelect = document.getElementById('fileActionWritePathSelect');
            const psramMbSelect = document.getElementById('fileActionPsramMbSelect');
            const requestedWritePath = (writePathSelect && writePathSelect.value === 'psram') ? 'psram' : 'direct';
            if (psramMbSelect) {
                setPsramWindowMb(psramMbSelect.value);
            }
            closeFileActionModal();
            const { filename, path } = fileToBurn;
            const ext = filename.split('.').pop().toLowerCase();
            const mode = ext === 'gba' ? 'gba' : 'mbc5';
            const selectedWritePath = getEffectiveWritePath(mode, requestedWritePath);

            window.location.hash = '#/burner';

            setTimeout(async () => {
                setCartMode(mode);
                setWritePathMode(selectedWritePath);
                const tfRomInput = document.getElementById('tfRomName');
                if (tfRomInput) tfRomInput.value = path || filename;
                state.romFile = null;
                const romPathInput = document.getElementById('romPath');
                if (romPathInput) romPathInput.value = '';

                addBurnLog('准备烧录...', 'info');

                const burnStatusCard = document.getElementById('burnStatusCard');
                if (burnStatusCard) {
                    burnStatusCard.style.display = 'block';
                }

                try {
                    await cartWriteRom();
                } catch (error) {
                    addBurnLog('烧录失败: ' + error.message, 'error');
                    showToast('烧录失败: ' + error.message, 'error');
                }
            }, 100);
        }

        async function burnROMFile(file, options = {}) {
            if (!file) return;
            const mode = options.mode || state.cartMode || 'gba';
            const slot = options.slot || getSelectedCartSlot();
            const writePath = getEffectiveWritePath(mode, state.writePathMode || 'direct');
            const psramMb = getPsramWindowMb();
            const mbc5ChunkKb = getMbc5ChunkKb();
            const autoStart = options.autoStart !== false;

            const progressDiv = document.getElementById('uploadProgress');
            const bar = document.getElementById('uploadBar');
            const percent = document.getElementById('uploadPercent');
            const status = document.getElementById('uploadStatus');
            const tfRomInput = document.getElementById('tfRomName');

            if (progressDiv) progressDiv.classList.remove('hidden');
            if (status) status.textContent = '正在上传到 TF...';
            state.burnStatus = {
                state: 'receiving',
                progress: 0,
                processed: 0,
                total: file.size || 0,
                message: 'upload started',
                cancel_requested: false
            };

            try {
                const tfName = await new Promise((resolve, reject) => {
                    const xhr = new XMLHttpRequest();
                    xhr.upload.addEventListener('progress', (e) => {
                        if (e.lengthComputable) {
                            const p = Math.round((e.loaded / e.total) * 100);
                            state.burnStatus = {
                                state: 'receiving',
                                progress: p,
                                processed: e.loaded,
                                total: e.total,
                                message: 'uploading',
                                cancel_requested: state.burnCancelPending
                            };
                            if (bar) bar.style.width = p + '%';
                            if (percent) percent.textContent = p + '%';
                            updateBurnProgress(p, '上传到 TF...');
                        }
                    });
                    xhr.addEventListener('load', () => {
                        const responseText = xhr.responseText || '';
                        if (isCancelResponse(xhr.status, responseText)) {
                            state.burnStatus = {
                                ...(state.burnStatus || {}),
                                state: 'cancelled',
                                cancel_requested: false,
                                message: 'upload cancelled'
                            };
                            setBurnCancelUi(false);
                            if (status) status.textContent = '任务已取消';
                            updateBurnProgress(state.burnStatus.progress || 0, '任务已取消');
                            reject(createCancelledError('上传已取消'));
                            return;
                        }
                        try {
                            const data = JSON.parse(responseText || '{}');
                            if (xhr.status === 200 && data.ok) {
                                resolve(getBaseName(file.name));
                            } else {
                                reject(new Error(data.message || ('HTTP ' + xhr.status)));
                            }
                        } catch (err) {
                            reject(new Error('解析上传响应失败'));
                        }
                    });
                    xhr.addEventListener('error', () => reject(new Error('上传网络错误')));
                    xhr.open('POST', `/api/upload?name=${encodeURIComponent(file.name)}&mode=${encodeURIComponent(mode)}`);
                    xhr.send(file);
                });

                if (tfRomInput) tfRomInput.value = tfName;
                if (status) status.textContent = autoStart ? '上传完成，启动烧录...' : '上传完成';
                addBurnLog('文件已上传到 TF: ' + tfName, 'success');

                if (!autoStart) {
                    return;
                }

                let writeUrl =
                    `/api/write?name=${encodeURIComponent(tfName)}` +
                    `&mode=${encodeURIComponent(mode)}&slot=${encodeURIComponent(slot)}` +
                    `&write_path=${encodeURIComponent(writePath)}`;
                if (writePath === 'psram') {
                    writeUrl += `&psram_mb=${encodeURIComponent(String(psramMb))}`;
                }
                if (mode === 'mbc5') {
                    writeUrl += `&mbc5_chunk_kb=${encodeURIComponent(String(mbc5ChunkKb))}`;
                }
                const { response: writeResponse, data: writeData } = await startTaskRequest(writeUrl);
                if (!writeResponse.ok || !writeData.ok) {
                    throw new Error(writeData.message || ('HTTP ' + writeResponse.status));
                }
                state.activeBurnOperation = 'write';
                state.pendingDumpRelPath = '';
                state.pendingDumpName = '';
                state.pendingDumpAutoDownloaded = false;
                state.lastBurnMessage = '';
                if (status) status.textContent = '烧录中......';
                updateBurnProgress(0, '烧录中......');
                addBurnLog('烧录中......', 'info');
                startBurnStatusPolling();
            } catch (error) {
                setBurnCancelUi(false);
                if (isCancelledError(error)) {
                    state.burnStatus = {
                        ...(state.burnStatus || {}),
                        state: 'cancelled',
                        cancel_requested: false,
                        message: 'upload cancelled'
                    };
                    if (status) status.textContent = '任务已取消';
                    addBurnLog('任务已取消: ' + error.message, 'warning');
                    showToast('任务已取消', 'warning');
                    return;
                }
                addBurnLog('传输错误: ' + error.message, 'error');
                if (status) status.textContent = '传输失败';
                showToast('上传/烧录失败: ' + error.message, 'error');
            }
        }

        function getFileIcon(filename) {
            const ext = filename.split('.').pop().toLowerCase();
            const icons = {
                gba: '🎮', gb: '🎮', gbc: '🎮',
                txt: '📄', md: '📝', json: '📋',
                zip: '📦', rar: '📦', '7z': '📦',
                png: '🖼️', jpg: '🖼️', gif: '🖼️'
            };
            return icons[ext] || '📄';
        }

        function updateBreadcrumb(path) {
            const breadcrumb = document.getElementById('breadcrumb');
            if (!breadcrumb) return;

            const parts = path.split('/').filter(p => p);
            const html = ['<span class="breadcrumb-item" onclick="navigateTo(\'\')">根目录</span>'];

            let currentPath = '';
            parts.forEach((part, index) => {
                currentPath += (currentPath ? '/' : '') + part;
                const isLast = index === parts.length - 1;
                html.push('<span class="breadcrumb-separator">/</span>');
                if (isLast) {
                    html.push(`<span style="color: var(--accent-primary);">${part}</span>`);
                } else {
                    html.push(`<span class="breadcrumb-item" onclick="navigateTo('${currentPath}')">${part}</span>`);
                }
            });

            breadcrumb.innerHTML = html.join('');
        }

        function navigateTo(path) {
            loadFileList(path);
        }

        function refreshFileList() {
            const btn = document.getElementById('refreshBtn');
            if (!btn) {
                loadFileList(state.currentPath);
                return;
            }

            btn.innerHTML = '<span class="spinner"></span> 刷新';
            loadFileList(state.currentPath).finally(() => {
                btn.innerHTML = '<span>🔄</span> 刷新';
            });
        }

        function showUploadModal() {
            const modal = document.getElementById('uploadModal');
            modal.classList.add('active');
            modal.classList.remove('minimized');

            document.getElementById('modalUploadInput').value = '';
            document.getElementById('uploadProgressArea').classList.add('hidden');
            document.querySelector('#uploadModal .file-select-area').classList.remove('hidden');
            document.getElementById('modalProgressBar').style.width = '0%';
            document.getElementById('uploadPercentText').textContent = '0%';
            setUploadCancelUi(false);

            const zone = document.querySelector('#uploadModal .upload-zone');
            zone.ondragover = (e) => {
                e.preventDefault();
                zone.classList.add('dragover');
            };
            zone.ondragleave = () => zone.classList.remove('dragover');
            zone.ondrop = (e) => {
                e.preventDefault();
                zone.classList.remove('dragover');
                const file = e.dataTransfer.files[0];
                if (file) startUpload(file);
            };
        }

        function closeUploadModal() {
            document.getElementById('uploadModal').classList.remove('active');
            if (state.uploadXHR) {
                cancelUpload();
            }
        }

        function minimizeModal() {
            const modal = document.getElementById('uploadModal');
            if (modal.classList.contains('minimized')) {
                restoreModal();
            } else {
                modal.classList.add('minimized');
                showToast('上传已最小化到右下角', 'info');
            }
        }

        function restoreModal() {
            document.getElementById('uploadModal').classList.remove('minimized');
        }

        function startUpload(file) {
            if (!file) return;
            if (state.uploadXHR) {
                showToast('已有上传任务正在进行', 'warning');
                return;
            }

            document.querySelector('#uploadModal .file-select-area').classList.add('hidden');
            document.getElementById('uploadProgressArea').classList.remove('hidden');

            document.getElementById('uploadFileName').textContent = file.name;
            document.getElementById('uploadSizeText').textContent = `0 MB / ${formatSize(file.size)}`;
            document.getElementById('statETA').textContent = '计算中...';

            state.uploadStartTime = Date.now();
            state.uploadStats = {
                loaded: 0,
                total: file.size,
                speed: 0,
                lastTime: Date.now(),
                lastLoaded: 0
            };
            state.uploadCancelPending = false;

            const statsInterval = setInterval(updateUploadStats, 500);
            const xhr = new XMLHttpRequest();
            state.uploadXHR = xhr;
            setUploadCancelUi(false);

            xhr.upload.addEventListener('progress', (e) => {
                if (e.lengthComputable) {
                    const percent = Math.round((e.loaded / e.total) * 100);
                    document.getElementById('modalProgressBar').style.width = percent + '%';
                    document.getElementById('uploadPercentText').textContent = percent + '%';
                    document.getElementById('uploadSizeText').textContent =
                        `${formatSize(e.loaded)} / ${formatSize(e.total)}`;

                    state.uploadStats.loaded = e.loaded;
                    state.uploadStats.total = e.total;
                }
            });

            xhr.addEventListener('load', () => {
                clearInterval(statsInterval);
                const responseText = xhr.responseText || '';
                const cancelled = isCancelResponse(xhr.status, responseText);

                state.uploadXHR = null;
                setUploadCancelUi(false);

                if (cancelled) {
                    showToast('上传已取消', 'warning');
                    resetUploadUI();
                    return;
                }

                if (xhr.status === 200) {
                    let data = null;
                    try {
                        data = JSON.parse(responseText || '{}');
                    } catch (error) {
                        data = { ok: true };
                    }
                    if (data.ok) {
                        const totalTime = (Date.now() - state.uploadStartTime) / 1000;
                        const avgSpeed = totalTime > 0 ? state.uploadStats.total / totalTime : 0;
                        document.getElementById('statETA').textContent = '完成';
                        document.getElementById('statSpeed').textContent = formatSpeed(avgSpeed);
                        showToast('上传成功', 'success');
                        setTimeout(() => {
                            closeUploadModal();
                            refreshFileList();
                        }, 1500);
                    } else {
                        showToast('上传失败: ' + data.message, 'error');
                        resetUploadUI();
                    }
                } else if (xhr.status === 503) {
                    showToast('错误: USB 直通模式已开启', 'error');
                    resetUploadUI();
                } else {
                    showToast('上传失败: HTTP ' + xhr.status, 'error');
                    resetUploadUI();
                }
            });

            xhr.addEventListener('error', () => {
                clearInterval(statsInterval);
                if (state.uploadXHR === xhr) {
                    state.uploadXHR = null;
                }
                setUploadCancelUi(false);
                showToast('上传失败: 网络错误', 'error');
                resetUploadUI();
            });

            xhr.addEventListener('abort', () => {
                clearInterval(statsInterval);
                if (state.uploadXHR === xhr) {
                    state.uploadXHR = null;
                }
                setUploadCancelUi(false);
                showToast('上传已取消', 'warning');
                resetUploadUI();
            });

            xhr.open(
                'POST',
                `/api/tf/upload?dir=${encodeURIComponent(state.currentPath)}&name=${encodeURIComponent(file.name)}`
            );
            xhr.send(file);
        }

        function updateUploadStats() {
            if (!state.uploadXHR) return;

            const now = Date.now();
            const elapsed = (now - state.uploadStartTime) / 1000;
            const loaded = state.uploadStats.loaded;
            const total = state.uploadStats.total;

            const timeDiff = (now - state.uploadStats.lastTime) / 1000;
            const loadedDiff = loaded - state.uploadStats.lastLoaded;

            if (timeDiff > 0) {
                const speed = loadedDiff / timeDiff;
                state.uploadStats.speed = speed;
                document.getElementById('statSpeed').textContent = formatSpeed(speed);

                if (speed > 0 && loaded < total) {
                    const remaining = (total - loaded) / speed;
                    document.getElementById('statETA').textContent = formatDuration(remaining);
                } else if (loaded >= total) {
                    document.getElementById('statETA').textContent = '即将完成';
                }

                state.uploadStats.lastTime = now;
                state.uploadStats.lastLoaded = loaded;
            }

            document.getElementById('statTime').textContent = formatDuration(elapsed);
        }

        function cancelUpload() {
            if (state.uploadXHR) {
                state.uploadXHR.abort();
                state.uploadXHR = null;
            }
            setUploadCancelUi(false);
        }

        function resetUploadUI() {
            document.querySelector('#uploadModal .file-select-area').classList.remove('hidden');
            document.getElementById('uploadProgressArea').classList.add('hidden');
            document.getElementById('modalProgressBar').style.width = '0%';
            document.getElementById('uploadPercentText').textContent = '0%';
            document.getElementById('statSpeed').textContent = '0 KB/s';
            document.getElementById('statTime').textContent = '00:00';
            document.getElementById('statETA').textContent = '计算中...';
            state.uploadCancelPending = false;
            setUploadCancelUi(false);
        }

        function showMkdirModal() {
            showModal('新建文件夹', `
                <input type="text" class="input" id="mkdirName" placeholder="文件夹名称" autofocus>
            `, async () => {
                const name = document.getElementById('mkdirName').value.trim();
                if (!name) {
                    showToast('请输入文件夹名称', 'warning');
                    return;
                }

                const path = state.currentPath ? `${state.currentPath}/${name}` : name;
                try {
                    const response = await apiCall(`/api/tf/mkdir?path=${encodeURIComponent(path)}`, {
                        method: 'POST'
                    });
                    const data = await response.json();
                    if (data.ok) {
                        showToast('创建成功', 'success');
                        closeModal();
                        refreshFileList();
                    } else {
                        throw new Error(data.message);
                    }
                } catch (error) {
                    showToast('创建失败: ' + error.message, 'error');
                }
            });
        }

        function showRenameModal(path) {
            const oldName = path.split('/').pop();
            showModal('重命名', `
                <input type="text" class="input" id="renameValue" value="${oldName}" autofocus>
            `, async () => {
                const newName = document.getElementById('renameValue').value.trim();
                if (!newName || newName === oldName) {
                    closeModal();
                    return;
                }

                const parentPath = path.substring(0, path.length - oldName.length - 1);
                const newPath = parentPath ? `${parentPath}/${newName}` : newName;

                try {
                    const response = await apiCall(`/api/tf/rename?from=${encodeURIComponent(path)}&to=${encodeURIComponent(newPath)}`, {
                        method: 'POST'
                    });
                    const data = await response.json();
                    if (data.ok) {
                        showToast('重命名成功', 'success');
                        closeModal();
                        refreshFileList();
                    } else {
                        throw new Error(data.message);
                    }
                } catch (error) {
                    showToast('重命名失败: ' + error.message, 'error');
                }
            });
        }

        function showDeleteModal(path, isDir) {
            const name = path.split('/').pop();
            showModal('确认删除', `
                确定要删除 ${isDir ? '文件夹' : '文件'} "<strong>${name}</strong>" 吗？${isDir ? '<br><span style="color: var(--accent-secondary);">文件夹内的所有内容将被递归删除！</span>' : ''}
            `, async () => {
                try {
                    const response = await apiCall(`/api/tf/delete?path=${encodeURIComponent(path)}`, {
                        method: 'DELETE'
                    });
                    const data = await response.json();
                    if (data.ok) {
                        showToast('删除成功', 'success');
                        closeModal();
                        refreshFileList();
                    } else {
                        throw new Error(data.message);
                    }
                } catch (error) {
                    showToast('删除失败: ' + error.message, 'error');
                }
            });
        }

        function downloadFile(path) {
            const name = path.split('/').pop();
            const a = document.createElement('a');
            a.href = `/api/tf/download?path=${encodeURIComponent(path)}`;
            a.download = name;
            a.click();
        }

        async function handleROMUpload(file) {
            if (!file) return;
            await burnROMFile(file);
        }

        function startBurnStatusPolling() {
            if (state.burnStatusInterval) clearInterval(state.burnStatusInterval);

            state.burnStatusInterval = setInterval(async () => {
                try {
                    const response = await fetch('/api/status');
                    const data = await response.json();
                    updateBurnStatus(data);

                    if (data.state === 'done' || data.state === 'error' || data.state === 'cancelled') {
                        clearInterval(state.burnStatusInterval);
                        state.burnStatusInterval = null;
                    }
                } catch (error) {
                    console.error('Status poll error:', error);
                }
            }, 500);
        }

        function updateBurnStatus(data) {
            const prevState = state.burnStatus ? state.burnStatus.state : null;
            state.burnStatus = data;
            const currentMessage = data.message || '';
            const progressPercent = getBurnProgressPercent(data);
            const progressMessage = getBurnProgressMessage(data, currentMessage);
            const primarySpeedLabel = getBurnPrimarySpeedLabel(state.activeBurnOperation);
            const speedCurrent = Number(data.speed_current_bps || 0);
            const speedAvg = Number(data.speed_avg_bps || 0);
            const speedMin = Number(data.speed_min_bps || 0);
            const speedMax = Number(data.speed_max_bps || 0);
            const eraseTimeMs = Number(data.erase_time_ms || 0);
            const writeTimeMs = Number(data.write_time_ms || 0);
            const eraseSectorCount = Number(data.erase_sector_count || 0);
            const totalDurationText = getBurnTotalDurationText(data);
            const tfToPsramSpeedCurrent = Number(data.tf_to_psram_speed_current_bps || 0);
            const tfToPsramSpeedAvg = Number(data.tf_to_psram_speed_avg_bps || 0);
            const tfToPsramSpeedMin = Number(data.tf_to_psram_speed_min_bps || 0);
            const tfToPsramSpeedMax = Number(data.tf_to_psram_speed_max_bps || 0);
            const mbc5BufferWriteOkCount = Number(data.mbc5_buffer_write_ok_count || 0);
            const mbc5BufferFallbackCount = Number(data.mbc5_buffer_fallback_count || 0);
            const mbc5BufferTotalCount = mbc5BufferWriteOkCount + mbc5BufferFallbackCount;
            const mbc5FallbackPercentText = mbc5BufferTotalCount > 0
                ? ((mbc5BufferFallbackCount * 100) / mbc5BufferTotalCount).toFixed(1) + '%'
                : '--';
            const speedCurrentText = speedCurrent > 0 ? formatSpeed(speedCurrent) : '--';
            const speedAvgText = speedAvg > 0 ? formatSpeed(speedAvg) : '--';
            const speedMinText = speedMin > 0 ? formatSpeed(speedMin) : '--';
            const speedMaxText = speedMax > 0 ? formatSpeed(speedMax) : '--';
            const tfToPsramSpeedCurrentText = tfToPsramSpeedCurrent > 0 ? formatSpeed(tfToPsramSpeedCurrent) : '--';
            const tfToPsramSpeedAvgText = tfToPsramSpeedAvg > 0 ? formatSpeed(tfToPsramSpeedAvg) : '--';
            const tfToPsramSpeedMinText = tfToPsramSpeedMin > 0 ? formatSpeed(tfToPsramSpeedMin) : '--';
            const tfToPsramSpeedMaxText = tfToPsramSpeedMax > 0 ? formatSpeed(tfToPsramSpeedMax) : '--';
            const eraseTimeText = eraseTimeMs > 0 ? formatDuration(eraseTimeMs / 1000) : '--';
            const eraseSpeedText = formatEraseSpeedText(eraseSectorCount, eraseTimeMs);
            const writeTimeText = writeTimeMs > 0 ? formatDuration(writeTimeMs / 1000) : '--';
            const cancelRequested = Boolean(data.cancel_requested);

            const stateMap = {
                idle: '空闲',
                receiving: '接收中',
                burning: '烧录中',
                done: '完成',
                error: '错误',
                cancelled: '已取消'
            };

            const stateColors = {
                idle: 'var(--text-secondary)',
                receiving: 'var(--accent-warning)',
                burning: 'var(--accent-primary)',
                done: 'var(--accent-success)',
                error: 'var(--accent-secondary)',
                cancelled: 'var(--accent-warning)'
            };

            setBurnCancelUi(cancelRequested && (data.state === 'receiving' || data.state === 'burning'));

            const statusGrid = document.getElementById('burnStatusGrid');
            if (statusGrid) {
                statusGrid.innerHTML = `
                    <div class="status-card">
                        <div class="status-label">任务概览</div>
                        <div class="status-value-large" style="font-size: 14px; line-height: 1.6; word-break: break-all;">
                            状态: <span style="color: ${stateColors[data.state] || 'inherit'}">${stateMap[data.state] || data.state}</span><br>
                            烧录文件: ${data.rom || '--'}<br>
                            已处理 / 总计: ${formatSize(data.processed || 0)} / ${formatSize(data.total || 0)}
                        </div>
                    </div>
                    <div class="status-card">
                        <div class="status-label">${primarySpeedLabel}</div>
                        <div class="status-value-large" style="font-size: 14px; line-height: 1.6;">
                            当前: ${speedCurrentText}<br>
                            平均: ${speedAvgText}<br>
                            最低: ${speedMinText}<br>
                            最高: ${speedMaxText}
                        </div>
                    </div>
                    <div class="status-card">
                        <div class="status-label">TF->PSRAM</div>
                        <div class="status-value-large" style="font-size: 14px; line-height: 1.6;">
                            当前: ${tfToPsramSpeedCurrentText}<br>
                            平均: ${tfToPsramSpeedAvgText}<br>
                            最低: ${tfToPsramSpeedMinText}<br>
                            最高: ${tfToPsramSpeedMaxText}
                        </div>
                    </div>
                    <div class="status-card">
                        <div class="status-label">耗时统计</div>
                        <div class="status-value-large" style="font-size: 14px; line-height: 1.6;">
                            总耗时: ${totalDurationText}<br>
                            擦除耗时: ${eraseTimeText}<br>
                            写入耗时: ${writeTimeText}<br>
                            擦除速度: ${eraseSpeedText}
                        </div>
                    </div>
                    ${mbc5BufferTotalCount > 0 ? `
                    <div class="status-card">
                        <div class="status-label">MBC5 缓冲写统计</div>
                        <div class="status-value-large" style="font-size: 14px; line-height: 1.5;">
                            buffer成功: ${mbc5BufferWriteOkCount}<br>
                            fallback次数: ${mbc5BufferFallbackCount}<br>
                            fallback占比: ${mbc5FallbackPercentText}
                        </div>
                    </div>
                    ` : ''}
                `;
            }

            updateBurnProgress(progressPercent, progressMessage);

            if (data.state === 'done' && prevState !== 'done') {
                if (state.activeBurnOperation === 'read' || state.activeBurnOperation === 'ram_read') {
                    const dumpName = state.pendingDumpName || data.rom || 'dump.gba';
                    addBurnLog('导出完成: ' + dumpName, 'success');
                    showToast('导出完成', 'success');

                    if (!state.pendingDumpAutoDownloaded && state.pendingDumpRelPath) {
                        state.pendingDumpAutoDownloaded = true;
                        downloadFile(state.pendingDumpRelPath);
                        addBurnLog('已自动下载: ' + state.pendingDumpRelPath, 'info');
                    }
                } else {
                    addBurnLog('烧录完成！', 'success');
                    showToast('烧录完成！', 'success');
                }
                addBurnDurationLog(data);
                state.activeBurnOperation = null;
                state.pendingDumpRelPath = '';
                state.pendingDumpName = '';
                state.pendingDumpAutoDownloaded = false;
                setBurnCancelUi(false);
            } else if (data.state === 'cancelled' && prevState !== 'cancelled') {
                addBurnLog('任务已取消: ' + (currentMessage || '已停止当前操作'), 'warning');
                addBurnDurationLog(data);
                showToast('任务已取消', 'warning');
                state.activeBurnOperation = null;
                state.pendingDumpRelPath = '';
                state.pendingDumpName = '';
                state.pendingDumpAutoDownloaded = false;
                setBurnCancelUi(false);
            } else if (data.state === 'error' && prevState !== 'error') {
                addBurnLog('任务错误: ' + currentMessage, 'error');
                addBurnDurationLog(data);
                showToast('任务失败: ' + currentMessage, 'error');
                state.activeBurnOperation = null;
                state.pendingDumpRelPath = '';
                state.pendingDumpName = '';
                state.pendingDumpAutoDownloaded = false;
                setBurnCancelUi(false);
            }

            state.lastBurnMessage = currentMessage;
        }

        function addBurnLog(message, type = 'info') {
            const log = document.getElementById('burnLog') || document.getElementById('cartLog');
            if (!log) return;

            const entry = document.createElement('div');
            entry.className = 'log-entry';
            entry.innerHTML = `
                <span class="log-time">${formatTime(new Date())}</span>
                <span class="log-${type}">${escapeHtml(message)}</span>
            `;
            log.insertBefore(entry, log.firstChild);

            while (log.children.length > 100) {
                log.removeChild(log.lastChild);
            }
        }

        async function probeMCUWithParams() {
            const params = new URLSearchParams();
            params.append('seq', document.getElementById('mcuSeq').value);
            params.append('delay', document.getElementById('mcuDelay').value);
            params.append('norst', document.getElementById('mcuNorst').value);
            params.append('swap', document.getElementById('mcuSwap').value);

            try {
                const response = await fetch(`/api/mcu/probe?${params.toString()}`);
                const data = await response.json();

                if (data.ok) {
                    renderMCUInfo(data);
                    showToast('探测成功', 'success');
                } else {
                    showToast('探测失败', 'error');
                }
            } catch (error) {
                showToast('探测错误: ' + error.message, 'error');
            }
        }

        function renderMCUInfo(data) {
            const html = `
                <div class="burn-status">
                    <div class="status-card">
                        <div class="status-label">状态</div>
                        <div class="status-value-large" style="color: ${data.status === 'ok' ? 'var(--accent-success)' : 'var(--accent-secondary)'}">
                            ${data.status.toUpperCase()}
                        </div>
                    </div>
                    <div class="status-card">
                        <div class="status-label">IDCODE</div>
                        <div class="status-value-large" style="font-family: monospace; font-size: 18px;">
                            ${data.idcode}
                        </div>
                    </div>
                    <div class="status-card">
                        <div class="status-label">ACK</div>
                        <div class="status-value-large">${data.ack}</div>
                    </div>
                    <div class="status-card">
                        <div class="status-label">校验</div>
                        <div class="status-value-large" style="color: ${data.parity_ok ? 'var(--accent-success)' : 'var(--accent-secondary)'}">
                            ${data.parity_ok ? '✓' : '✗'}
                        </div>
                    </div>
                </div>
                <div style="margin-top: 16px; padding: 16px; background: var(--bg-tertiary); border-radius: 8px; font-family: monospace; font-size: 13px; line-height: 1.6;">
                    <div>序列模式: ${data.seq_mode} (${data.seq_used})</div>
                    <div>延迟: ${data.delay_us}μs</div>
                    <div>复位: ${data.do_reset ? '是' : '否'}</div>
                    <div>交换 CLK/DIO: ${data.swap_clk_dio ? '是' : '否'}</div>
                    <div>周转周期: ${data.turnaround_cycles}</div>
                    <div>SWDIO 上拉: ${data.swdio_pull_mode}</div>
                    <div>尝试次数: ${data.attempt_count}</div>
                </div>
            `;

            const container = document.getElementById('cartInfo');
            if (container) {
                container.innerHTML = html;
            } else {
                showModal('MCU 探测结果', html);
            }
        }

        async function loadDeviceInfo() {
            const container = document.getElementById('deviceInfo');
            if (!container) return;

            container.innerHTML = '<div style="text-align: center; padding: 40px;"><div class="spinner" style="margin: 0 auto;"></div></div>';

            try {
                const response = await fetch('/api/device/info');
                const text = await response.text();

                container.innerHTML = `
                    <div style="font-family: monospace; white-space: pre-wrap; line-height: 1.8; font-size: 14px;">
                        ${escapeHtml(text)}
                    </div>
                `;
            } catch (error) {
                container.innerHTML = `<div style="color: var(--accent-secondary);">加载失败: ${error.message}</div>`;
            }
        }

        async function restartDevice() {
            if (!confirm('确定要重启设备吗？')) return;

            try {
                const response = await fetch('/api/device/restart', { method: 'POST' });
                if (response.ok) {
                    showToast('设备正在重启...', 'success');
                } else {
                    showToast('重启失败: HTTP ' + response.status, 'error');
                }
            } catch (error) {
                showToast('网络错误: ' + error.message, 'error');
            }
        }

        let brightnessDebounceTimer = null;
        
        function setBrightnessDebounced(value) {
            const brightnessValue = document.getElementById('brightnessValue');
            if (brightnessValue) brightnessValue.textContent = value;
            
            // 同步更新所有亮度滑块
            const slider = document.getElementById('brightnessSlider');
            const sidebarSlider = document.getElementById('sidebarBrightnessSlider');
            if (slider) slider.value = value;
            if (sidebarSlider) sidebarSlider.value = value;
            
            // 防抖：100ms 后发送请求
            if (brightnessDebounceTimer) {
                clearTimeout(brightnessDebounceTimer);
            }
            brightnessDebounceTimer = setTimeout(() => {
                setBrightness(value);
            }, 100);
        }

        async function setBrightness(value) {
            try {
                const response = await fetch('/api/device/brightness', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ brightness: parseInt(value) })
                });

                if (response.ok) {
                    const data = await response.json();
                    if (!data.ok) {
                        showToast('设置失败: ' + (data.message || '未知错误'), 'error');
                    }
                } else {
                    showToast('设置失败: HTTP ' + response.status, 'error');
                }
            } catch (error) {
                showToast('网络错误: ' + error.message, 'error');
            }
        }

        async function loadBrightness() {
            try {
                const response = await fetch('/api/device/brightness');
                if (response.ok) {
                    const data = await response.json();
                    if (data.ok && data.brightness !== undefined) {
                        const slider = document.getElementById('brightnessSlider');
                        const sidebarSlider = document.getElementById('sidebarBrightnessSlider');
                        const brightnessValue = document.getElementById('brightnessValue');
                        if (slider) slider.value = data.brightness;
                        if (sidebarSlider) sidebarSlider.value = data.brightness;
                        if (brightnessValue) brightnessValue.textContent = data.brightness;
                    }
                }
            } catch (error) {
                console.error('加载亮度失败:', error);
            }
        }

        async function handleFirmwareUpload(file) {
            if (!file) return;
            if (state.firmwareUploadXHR) {
                showToast('已有固件升级任务正在进行', 'warning');
                return;
            }
            
            const fileNameEl = document.getElementById('firmwareFileName');
            const progressEl = document.getElementById('firmwareProgress');
            const statusEl = document.getElementById('firmwareStatus');
            const barEl = document.getElementById('firmwareBar');
            const percentEl = document.getElementById('firmwarePercent');
            const inputEl = document.getElementById('firmwareFile');
            
            if (fileNameEl) fileNameEl.textContent = file.name;
            if (progressEl) progressEl.classList.remove('hidden');
            if (statusEl) statusEl.textContent = '正在上传...';
            if (barEl) barEl.style.width = '0%';
            if (percentEl) percentEl.textContent = '0%';
            setFirmwareCancelUi(true, false);
            
            showToast('正在上传固件: ' + file.name, 'info');
            
            try {
                const xhr = new XMLHttpRequest();
                state.firmwareUploadXHR = xhr;
                
                xhr.upload.addEventListener('progress', (e) => {
                    if (e.lengthComputable) {
                        const percent = Math.round((e.loaded / e.total) * 100);
                        if (barEl) barEl.style.width = percent + '%';
                        if (percentEl) percentEl.textContent = percent + '%';
                    }
                });
                
                xhr.addEventListener('load', () => {
                    const responseText = xhr.responseText || '';
                    const cancelled = isCancelResponse(xhr.status, responseText);

                    state.firmwareUploadXHR = null;
                    setFirmwareCancelUi(false, false);
                    if (inputEl) inputEl.value = '';

                    if (cancelled) {
                        if (statusEl) statusEl.textContent = '上传已取消';
                        showToast('固件升级已取消', 'warning');
                        return;
                    }

                    if (xhr.status === 200) {
                        try {
                            const data = JSON.parse(responseText || '{}');
                            if (data.ok) {
                                if (statusEl) statusEl.textContent = '上传成功，设备即将重启...';
                                showToast('固件上传成功，设备即将重启', 'success');
                            } else {
                                if (statusEl) statusEl.textContent = '上传失败: ' + (data.message || '未知错误');
                                showToast('固件上传失败: ' + (data.message || '未知错误'), 'error');
                            }
                        } catch (e) {
                            if (statusEl) statusEl.textContent = '上传成功，设备即将重启...';
                            showToast('固件上传成功，设备即将重启', 'success');
                        }
                    } else {
                        if (statusEl) statusEl.textContent = '上传失败: HTTP ' + xhr.status;
                        showToast('固件上传失败: HTTP ' + xhr.status, 'error');
                    }
                });
                
                xhr.addEventListener('error', () => {
                    state.firmwareUploadXHR = null;
                    setFirmwareCancelUi(false, false);
                    if (inputEl) inputEl.value = '';
                    if (statusEl) statusEl.textContent = '上传失败: 网络错误';
                    showToast('固件上传失败: 网络错误', 'error');
                });

                xhr.addEventListener('abort', () => {
                    state.firmwareUploadXHR = null;
                    setFirmwareCancelUi(false, false);
                    if (inputEl) inputEl.value = '';
                    if (statusEl) statusEl.textContent = '上传已取消';
                    showToast('固件升级已取消', 'warning');
                });
                
                xhr.open('POST', '/api/fw/upgrade');
                xhr.setRequestHeader('Content-Type', 'application/octet-stream');
                xhr.send(file);
            } catch (error) {
                state.firmwareUploadXHR = null;
                setFirmwareCancelUi(false, false);
                if (inputEl) inputEl.value = '';
                if (statusEl) statusEl.textContent = '上传失败: ' + error.message;
                showToast('固件上传失败: ' + error.message, 'error');
            }
        }

        async function handleSystemDeployZip(file) {
            if (!file) return;
            if (state.systemDeployBusy) {
                showToast('已有系统部署任务正在进行', 'warning');
                return;
            }

            const fileNameEl = document.getElementById('systemDeployZipName');
            const statusEl = document.getElementById('systemDeployStatus');
            const inputEl = document.getElementById('systemDeployZipFile');

            if (fileNameEl) fileNameEl.textContent = file.name;
            if (statusEl) statusEl.textContent = '正在部署 ZIP...';
            setSystemDeployCancelUi(true, false);

            showToast('正在部署系统 ZIP: ' + file.name, 'info');

            try {
                const response = await apiCall('/api/system/deploy_zip', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/octet-stream' },
                    body: file
                });
                const data = await readApiPayload(response);
                const msg = data.message || ('HTTP ' + response.status);
                if (response.ok && data.ok) {
                    const lines = [];
                    lines.push('部署成功');
                    lines.push('web: ' + (data.web ? 'yes' : 'no'));
                    lines.push('setting: ' + (data.setting ? 'yes' : 'no'));
                    lines.push('files: ' + (data.files ?? 0));
                    lines.push('bytes: ' + (data.bytes ?? 0));
                    if (statusEl) statusEl.textContent = lines.join('\n');
                    showToast('系统 ZIP 部署成功', 'success');
                } else if (isCancelResponse(response.status, msg)) {
                    if (statusEl) statusEl.textContent = '部署已取消';
                    showToast('系统 ZIP 部署已取消', 'warning');
                } else {
                    if (statusEl) statusEl.textContent = '部署失败: ' + msg;
                    showToast('系统 ZIP 部署失败: ' + msg, 'error');
                }
            } catch (error) {
                if (statusEl) statusEl.textContent = '部署失败: ' + error.message;
                showToast('系统 ZIP 部署失败: ' + error.message, 'error');
            } finally {
                if (inputEl) inputEl.value = '';
                setSystemDeployCancelUi(false, false);
            }
        }

        async function refreshStorageStatus() {
            const statusEl = document.getElementById('usbStatusInfo');
            if (!statusEl) return;
            
            statusEl.textContent = '加载中...';
            
            try {
                const response = await fetch('/api/storage/status');
                const data = await response.json();
                
                const lines = [];
                lines.push('TF 卡状态: ' + (data.tf_ready ? '✓ 已挂载' : '✕ 未挂载'));
                lines.push('USB 直通: ' + (data.usb_passthrough_enabled ? '✓ 已启用' : '✕ 已禁用'));
                if (data.tf_size_bytes) lines.push('TF 容量: ' + formatSize(data.tf_size_bytes));
                if (data.tf_free_bytes !== undefined) lines.push('可用空间: ' + formatSize(data.tf_free_bytes));
                
                statusEl.textContent = lines.join('\n');
                
                const enableBtn = document.getElementById('usbEnableBtn');
                const disableBtn = document.getElementById('usbDisableBtn');
                if (enableBtn && disableBtn) {
                    if (data.usb_passthrough_enabled) {
                        enableBtn.classList.remove('btn-primary');
                        enableBtn.classList.add('btn');
                        disableBtn.classList.remove('btn');
                        disableBtn.classList.add('btn-danger');
                    } else {
                        enableBtn.classList.remove('btn');
                        enableBtn.classList.add('btn-primary');
                        disableBtn.classList.remove('btn-danger');
                        disableBtn.classList.add('btn');
                    }
                }
            } catch (error) {
                statusEl.textContent = '获取状态失败: ' + error.message;
            }
        }

        async function enableUsbPassthrough() {
            try {
                const response = await fetch('/api/storage/usb_msc?enable=1', { method: 'POST' });
                const data = await response.json();
                if (data.ok) {
                    showToast('USB 直通已启用', 'success');
                } else {
                    showToast('启用失败: ' + (data.message || '未知错误'), 'error');
                }
                refreshStorageStatus();
            } catch (error) {
                showToast('启用失败: ' + error.message, 'error');
            }
        }

        async function disableUsbPassthrough() {
            try {
                const response = await fetch('/api/storage/usb_msc?enable=0', { method: 'POST' });
                const data = await response.json();
                if (data.ok) {
                    showToast('USB 直通已禁用', 'success');
                } else {
                    showToast('禁用失败: ' + (data.message || '未知错误'), 'error');
                }
                refreshStorageStatus();
            } catch (error) {
                showToast('禁用失败: ' + error.message, 'error');
            }
        }

        function formatBaconSpiStatusLines(data) {
            const configuredHz = Number(data && data.configured_hz) || 0;
            const actualHz = Number(data && data.actual_hz) || 0;
            const minHz = Number(data && data.min_hz) || 20000000;
            const maxHz = Number(data && data.max_hz) || 80000000;
            const configuredMhz = configuredHz > 0 ? (configuredHz / 1000000).toFixed(2) : '--';
            const actualMhz = actualHz > 0 ? (actualHz / 1000000).toFixed(2) : '--';
            return [
                `配置频率: ${configuredHz} Hz (${configuredMhz} MHz)`,
                `实际频率: ${actualHz} Hz (${actualMhz} MHz)`,
                `可设置范围: ${(minHz / 1000000).toFixed(0)}~${(maxHz / 1000000).toFixed(0)} MHz`
            ].join('\n');
        }

        async function refreshBaconSpiClock() {
            const statusEl = document.getElementById('baconSpiStatus');
            const inputEl = document.getElementById('baconSpiMhz');
            if (!statusEl || !inputEl) return;

            statusEl.textContent = '读取中...';
            try {
                const response = await apiCall('/api/spi/config');
                const data = await readApiPayload(response);
                if (!response.ok || !data.ok) {
                    statusEl.textContent = '读取失败: ' + (data.message || ('HTTP ' + response.status));
                    return;
                }

                if (Number.isFinite(data.configured_hz) && data.configured_hz > 0) {
                    inputEl.value = String(Math.round(Number(data.configured_hz) / 1000000));
                }
                statusEl.textContent = formatBaconSpiStatusLines(data);
            } catch (error) {
                statusEl.textContent = '读取失败: ' + error.message;
            }
        }

        async function applyBaconSpiClock() {
            const statusEl = document.getElementById('baconSpiStatus');
            const inputEl = document.getElementById('baconSpiMhz');
            if (!statusEl || !inputEl) return;

            const mhz = parseInt(String(inputEl.value || '').trim(), 10);
            if (!Number.isFinite(mhz) || mhz < 20 || mhz > 80) {
                showToast('SPI 频率必须是 20~80 MHz', 'warning');
                statusEl.textContent = '参数无效: 请输入 20~80 之间的整数 MHz';
                return;
            }

            statusEl.textContent = '应用中...';
            try {
                const response = await apiCall(`/api/spi/config?mhz=${encodeURIComponent(String(mhz))}`, { method: 'POST' });
                const data = await readApiPayload(response);
                if (!response.ok || !data.ok) {
                    const msg = data.message || ('HTTP ' + response.status);
                    statusEl.textContent = '设置失败: ' + msg;
                    showToast('SPI 频率设置失败: ' + msg, 'error');
                    return;
                }

                if (Number.isFinite(data.configured_hz) && data.configured_hz > 0) {
                    inputEl.value = String(Math.round(Number(data.configured_hz) / 1000000));
                }
                statusEl.textContent = formatBaconSpiStatusLines(data);
                showToast('SPI 频率已更新', 'success');
            } catch (error) {
                statusEl.textContent = '设置失败: ' + error.message;
                showToast('SPI 频率设置失败: ' + error.message, 'error');
            }
        }

        function formatBurnCoreStatusLines(data) {
            const erase = (data && data.erase) ? data.erase : 'auto';
            const tf = (data && data.tf) ? data.tf : 'auto';
            const psram = (data && data.psram) ? data.psram : 'auto';
            return [
                `擦除核心: ${erase}`,
                `读 TF 核心: ${tf}`,
                `读写 PSRAM 核心: ${psram}`
            ].join('\n');
        }

        async function refreshBurnCoreConfig() {
            const statusEl = document.getElementById('burnCoreStatus');
            const eraseEl = document.getElementById('burnCoreErase');
            const tfEl = document.getElementById('burnCoreTf');
            const psramEl = document.getElementById('burnCorePsram');
            if (!statusEl || !eraseEl || !tfEl || !psramEl) return;

            statusEl.textContent = '读取中...';
            try {
                const response = await apiCall('/api/burn/core_config');
                const data = await readApiPayload(response);
                if (!response.ok || !data.ok) {
                    statusEl.textContent = '读取失败: ' + (data.message || ('HTTP ' + response.status));
                    return;
                }

                eraseEl.value = (data.erase || 'auto').toLowerCase();
                tfEl.value = (data.tf || 'auto').toLowerCase();
                psramEl.value = (data.psram || 'auto').toLowerCase();
                statusEl.textContent = formatBurnCoreStatusLines(data);
            } catch (error) {
                statusEl.textContent = '读取失败: ' + error.message;
            }
        }

        async function applyBurnCoreConfig() {
            const statusEl = document.getElementById('burnCoreStatus');
            const eraseEl = document.getElementById('burnCoreErase');
            const tfEl = document.getElementById('burnCoreTf');
            const psramEl = document.getElementById('burnCorePsram');
            if (!statusEl || !eraseEl || !tfEl || !psramEl) return;

            const erase = String(eraseEl.value || 'auto').toLowerCase();
            const tf = String(tfEl.value || 'auto').toLowerCase();
            const psram = String(psramEl.value || 'auto').toLowerCase();
            statusEl.textContent = '应用中...';

            try {
                const query = new URLSearchParams();
                query.set('erase', erase);
                query.set('tf', tf);
                query.set('psram', psram);
                const response = await apiCall(`/api/burn/core_config?${query.toString()}`, { method: 'POST' });
                const data = await readApiPayload(response);
                if (!response.ok || !data.ok) {
                    const msg = data.message || ('HTTP ' + response.status);
                    statusEl.textContent = '设置失败: ' + msg;
                    showToast('核心分配设置失败: ' + msg, 'error');
                    return;
                }

                eraseEl.value = (data.erase || 'auto').toLowerCase();
                tfEl.value = (data.tf || 'auto').toLowerCase();
                psramEl.value = (data.psram || 'auto').toLowerCase();
                statusEl.textContent = formatBurnCoreStatusLines(data);
                showToast('烧录核心分配已更新', 'success');
            } catch (error) {
                statusEl.textContent = '设置失败: ' + error.message;
                showToast('核心分配设置失败: ' + error.message, 'error');
            }
        }

        async function loadLanguageList() {
            const statusEl = document.getElementById('languageStatus');
            const selectEl = document.getElementById('languageSelect');
            
            if (statusEl) statusEl.textContent = '加载中...';
            
            try {
                const response = await fetch('/api/lang/list');
                const data = await response.json();
                
                if (data.files && data.files.length > 0) {
                    if (selectEl) {
                        selectEl.innerHTML = '';
                        data.files.forEach(file => {
                            const option = document.createElement('option');
                            option.value = file;
                            option.textContent = file;
                            if (file === data.current) option.selected = true;
                            selectEl.appendChild(option);
                        });
                    }
                    if (statusEl) statusEl.textContent = '已加载 ' + data.files.length + ' 个语言文件\n当前: ' + (data.current || '无');
                } else {
                    if (selectEl) {
                        selectEl.innerHTML = '<option value="">无可用语言</option>';
                    }
                    if (statusEl) statusEl.textContent = '无可用语言文件';
                }
            } catch (error) {
                if (statusEl) statusEl.textContent = '加载失败: ' + error.message;
                showToast('加载语言列表失败', 'error');
            }
        }

        async function applyLanguage() {
            const statusEl = document.getElementById('languageStatus');
            const selectEl = document.getElementById('languageSelect');
            
            if (!selectEl || !selectEl.value) {
                showToast('请先选择一个语言', 'warning');
                return;
            }
            
            if (statusEl) statusEl.textContent = '正在应用...';
            
            try {
                const response = await fetch('/api/lang/apply?ini=' + encodeURIComponent(selectEl.value), { method: 'POST' });
                const data = await response.json();
                
                if (data.ok) {
                    if (statusEl) statusEl.textContent = '语言已应用: ' + data.language_ini;
                    showToast('语言设置成功', 'success');
                } else {
                    if (statusEl) statusEl.textContent = '应用失败: ' + (data.message || '未知错误');
                    showToast('语言应用失败', 'error');
                }
            } catch (error) {
                if (statusEl) statusEl.textContent = '应用失败: ' + error.message;
                showToast('语言应用失败: ' + error.message, 'error');
            }
        }

        async function loadIp5306Ini() {
            const statusEl = document.getElementById('ip5306Status');
            const contentEl = document.getElementById('ip5306IniContent');
            
            if (statusEl) statusEl.textContent = '加载中...';
            
            try {
                const response = await fetch('/api/ip5306/ini');
                const text = await response.text();
                
                if (contentEl) contentEl.value = text;
                if (statusEl) statusEl.textContent = '配置已加载';
                showToast('IP5306 配置已加载', 'success');
            } catch (error) {
                if (statusEl) statusEl.textContent = '加载失败: ' + error.message;
                showToast('加载 IP5306 配置失败', 'error');
            }
        }

        async function saveIp5306Ini() {
            const statusEl = document.getElementById('ip5306Status');
            const contentEl = document.getElementById('ip5306IniContent');
            
            if (!contentEl) return;
            
            if (statusEl) statusEl.textContent = '保存中...';
            
            try {
                const response = await fetch('/api/ip5306/ini', {
                    method: 'POST',
                    headers: { 'Content-Type': 'text/plain; charset=utf-8' },
                    body: contentEl.value
                });
                const data = await response.json();
                
                if (data.ok) {
                    if (statusEl) statusEl.textContent = '配置已保存';
                    showToast('IP5306 配置已保存', 'success');
                } else {
                    if (statusEl) statusEl.textContent = '保存失败: ' + (data.message || '未知错误');
                    showToast('保存 IP5306 配置失败', 'error');
                }
            } catch (error) {
                if (statusEl) statusEl.textContent = '保存失败: ' + error.message;
                showToast('保存 IP5306 配置失败: ' + error.message, 'error');
            }
        }

        function updateSystemStatus(status, text) {
            const dot = document.getElementById('sysStatusDot');
            const label = document.getElementById('sysStatusText');
            if (dot && label) {
                label.textContent = text;
                dot.className = 'status-dot';
                switch (status) {
                    case 'ready':
                        dot.style.background = 'var(--accent-success)';
                        dot.style.boxShadow = '0 0 10px var(--accent-success)';
                        break;
                    case 'busy':
                        dot.style.background = 'var(--accent-primary)';
                        dot.style.boxShadow = '0 0 10px var(--accent-primary)';
                        break;
                    case 'warning':
                        dot.classList.add('warning');
                        break;
                    case 'error':
                        dot.classList.add('error');
                        break;
                }
            }
        }

        function showModal(title, body, onConfirm) {
            document.getElementById('modalTitle').textContent = title;
            document.getElementById('modalBody').innerHTML = body;

            const confirmBtn = document.getElementById('modalConfirm');
            if (onConfirm) {
                confirmBtn.onclick = onConfirm;
                confirmBtn.style.display = 'inline-flex';
            } else {
                confirmBtn.style.display = 'none';
                confirmBtn.onclick = null;
            }

            document.getElementById('modal').classList.add('active');
        }

        function closeModal() {
            document.getElementById('modal').classList.remove('active');
        }

        function updateWiFiStatus() {
            if (state.currentPage === 'wifi') {
                loadWiFiStatus();
            }
        }
        
        // 工具函数
        function formatSize(bytes) {
            if (bytes === 0) return '0 B';
            const k = 1024;
            const sizes = ['B', 'KB', 'MB', 'GB'];
            const i = Math.floor(Math.log(bytes) / Math.log(k));
            return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
        }

        function formatTime(date) {
            return date.toTimeString().split(' ')[0];
        }

        function formatDuration(seconds) {
            if (!isFinite(seconds) || seconds < 0) return '--:--:--';
            const hrs = Math.floor(seconds / 3600);
            const mins = Math.floor((seconds % 3600) / 60);
            const secs = Math.floor(seconds % 60);
            return `${hrs.toString().padStart(2, '0')}:${mins.toString().padStart(2, '0')}:${secs.toString().padStart(2, '0')}`;
        }

        function formatSpeed(bytesPerSecond) {
            if (bytesPerSecond === 0) return '0 KB/s';
            if (bytesPerSecond < 1024) return bytesPerSecond.toFixed(0) + ' B/s';
            if (bytesPerSecond < 1024 * 1024) return (bytesPerSecond / 1024).toFixed(1) + ' KB/s';
            return (bytesPerSecond / (1024 * 1024)).toFixed(2) + ' MB/s';
        }

        function escapeHtml(text) {
            const div = document.createElement('div');
            div.textContent = text;
            return div.innerHTML;
        }

        function showToast(message, type = 'info') {
            const container = document.getElementById('toastContainer');
            const toast = document.createElement('div');
            toast.className = `toast ${type}`;
            
            const icons = {
                success: '✓',
                error: '✕',
                warning: '⚠',
                info: 'ℹ'
            };
            
            toast.innerHTML = `
                <span style="font-size: 20px;">${icons[type]}</span>
                <span>${escapeHtml(message)}</span>
            `;
            
            container.appendChild(toast);
            
            setTimeout(() => {
                toast.style.opacity = '0';
                toast.style.transform = 'translateX(100%)';
                setTimeout(() => toast.remove(), 300);
            }, 3000);
        }

        // 文本编辑器功能
        let currentEditingFile = null;

        async function openTextEditor(path, name) {
            currentEditingFile = { path, name };
            document.getElementById('textEditTitle').textContent = `编辑: ${name}`;
            document.getElementById('textEditContent').value = '加载中...';
            document.getElementById('textEditModal').classList.add('active');
            
            try {
                const response = await fetch(`/api/tf/download?path=${encodeURIComponent(path)}`);
                const content = await response.text();
                document.getElementById('textEditContent').value = content;
            } catch (error) {
                showToast('加载文件失败: ' + error.message, 'error');
                closeTextEditModal();
            }
        }

        function closeTextEditModal() {
            document.getElementById('textEditModal').classList.remove('active');
            currentEditingFile = null;
        }

        async function saveTextFile() {
            if (!currentEditingFile) return;
            
            const content = document.getElementById('textEditContent').value;
            const { path, name } = currentEditingFile;
            
            try {
                const response = await fetch(`/api/tf/upload?dir=${encodeURIComponent(state.currentPath)}&name=${encodeURIComponent(name)}`, {
                    method: 'POST',
                    headers: { 'Content-Type': 'text/plain; charset=utf-8' },
                    body: content
                });
                
                const data = await response.json();
                if (data.ok) {
                    showToast('保存成功', 'success');
                    closeTextEditModal();
                    refreshFileList();
                } else {
                    showToast('保存失败: ' + (data.message || '未知错误'), 'error');
                }
            } catch (error) {
                showToast('保存失败: ' + error.message, 'error');
            }
        }

        // 点击模态框外部关闭
        document.getElementById('modal').addEventListener('click', (e) => {
            if (e.target === document.getElementById('modal')) {
                closeModal();
            }
        });

        document.getElementById('fileActionModal').addEventListener('click', (e) => {
            if (e.target === document.getElementById('fileActionModal')) {
                closeFileActionModal();
            }
        });

        document.getElementById('wifiConnectModal').addEventListener('click', (e) => {
            if (e.target === document.getElementById('wifiConnectModal')) {
                closeWifiConnectModal();
            }
        });

        document.getElementById('textEditModal').addEventListener('click', (e) => {
            if (e.target === document.getElementById('textEditModal')) {
                closeTextEditModal();
            }
        });

        // 移动端侧边栏控制
        function toggleMobileSidebar() {
            console.log('toggleMobileSidebar called');
            const sidebar = document.getElementById('sidebar');
            const overlay = document.getElementById('mobileOverlay');
            const menuBtn = document.getElementById('mobileMenuBtn');

            if (!sidebar || !overlay || !menuBtn) {
                console.error('Elements not found:', { sidebar, overlay, menuBtn });
                return;
            }

            console.log('Before toggle:', {
                sidebarOpen: sidebar.classList.contains('open'),
                overlayActive: overlay.classList.contains('active'),
                menuBtnActive: menuBtn.classList.contains('active')
            });

            sidebar.classList.toggle('open');
            overlay.classList.toggle('active');
            menuBtn.classList.toggle('active');

            // 禁止背景滚动
            if (sidebar.classList.contains('open')) {
                document.body.style.overflow = 'hidden';
            } else {
                document.body.style.overflow = '';
            }

            console.log('After toggle:', {
                sidebarOpen: sidebar.classList.contains('open'),
                overlayActive: overlay.classList.contains('active'),
                menuBtnActive: menuBtn.classList.contains('active')
            });
        }

        function closeMobileSidebar() {
            console.log('closeMobileSidebar called');
            const sidebar = document.getElementById('sidebar');
            const overlay = document.getElementById('mobileOverlay');
            const menuBtn = document.getElementById('mobileMenuBtn');

            if (!sidebar || !overlay || !menuBtn) {
                console.error('Elements not found:', { sidebar, overlay, menuBtn });
                return;
            }

            sidebar.classList.remove('open');
            overlay.classList.remove('active');
            menuBtn.classList.remove('active');
            document.body.style.overflow = '';

            console.log('Sidebar closed');
        }

        // 点击导航项后自动关闭侧边栏（移动端）
        document.querySelectorAll('.nav-item').forEach(item => {
            item.addEventListener('click', () => {
                if (window.innerWidth <= 1024) {
                    closeMobileSidebar();
                }
            });
        });

        // 窗口大小改变时重置侧边栏状态
        window.addEventListener('resize', () => {
            if (window.innerWidth > 1024) {
                closeMobileSidebar();
            }
        });

    
