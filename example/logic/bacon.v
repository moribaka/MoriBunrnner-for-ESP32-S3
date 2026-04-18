/*
 * bacon.v
 * 当前工程使用的卡带接口控制模块
 *
 * 说明：
 * 1. 外层 bacon 为平台适配层，负责把 ESP32/SoC 引脚映射到 legacy core
 * 2. 内层 bacon_legacy_core 保留 AGM 原版 top.v 的 SPI 协议主体
 * 3. 为方便与 AGM 原版逐段对照，差异实现统一前置并用分割线标注
 */
module bacon(
    /* 卡带总线信号 */
    inout       [23:16] A,
    inout       [15:0]  AD,
    output tri          CART_DIR_A,
    output tri          CART_DIR_AD,
    output tri0         CS1_N,
    output tri0         CS2_N,
    inout               IRQ,
    output tri0         LED_ACT,
    output tri0         LED_READY,
    inout               PHI,
    output tri0         RD_N,
    output tri0         V3V3_CTRL,
    output tri0         V5V_CTRL,
    output tri0         WR_N,

    /* ESP32 SPI 接口 */
    input               ESP32_SPI2_CS_N,
    output tri0         ESP32_SPI2_MISO,
    input               ESP32_SPI2_MOSI,
    input               ESP32_SPI_CS1,
    inout               MCU_SPI_CLK,

    /* 平台时钟与控制信号 */
    input               sys_clock,
    input               bus_clock,
    input               resetn,
    input               stop,

    /* 平台 AHB 接口（当前版本仅保留外形兼容） */
    input       [1:0]   mem_ahb_htrans,
    input               mem_ahb_hready,
    input               mem_ahb_hwrite,
    input       [31:0]  mem_ahb_haddr,
    input       [2:0]   mem_ahb_hsize,
    input       [2:0]   mem_ahb_hburst,
    input       [31:0]  mem_ahb_hwdata,
    output tri1         mem_ahb_hreadyout,
    output tri0         mem_ahb_hresp,
    output tri0 [31:0]  mem_ahb_hrdata,
    output tri0         slave_ahb_hsel,
    output tri1         slave_ahb_hready,
    input               slave_ahb_hreadyout,
    output tri0 [1:0]   slave_ahb_htrans,
    output tri0 [2:0]   slave_ahb_hsize,
    output tri0 [2:0]   slave_ahb_hburst,
    output tri0         slave_ahb_hwrite,
    output tri0 [31:0]  slave_ahb_haddr,
    output tri0 [31:0]  slave_ahb_hwdata,
    input               slave_ahb_hresp,
    input       [31:0]  slave_ahb_hrdata,

    /* 平台 DMA/中断接口（当前版本仅保留外形兼容） */
    output tri0 [3:0]   ext_dma_DMACBREQ,
    output tri0 [3:0]   ext_dma_DMACLBREQ,
    output tri0 [3:0]   ext_dma_DMACSREQ,
    output tri0 [3:0]   ext_dma_DMACLSREQ,
    input       [3:0]   ext_dma_DMACCLR,
    input       [3:0]   ext_dma_DMACTC,
    output tri0 [3:0]   local_int
);

    /* ====================================================================== */
    /* 与 AGM top.v 不同的部分：顶层包装适配                                  */
    /* - AGM 原版直接暴露 SPI/卡带接口                                         */
    /* - 当前版本增加平台封装，复用 legacy core                                */
    /* - AHB/DMA 相关端口仅保留平台兼容外形                                    */
    /* ====================================================================== */

    wire core_spi_miso;
    wire core_led_act;
    wire core_led_ready;
    wire core_3v3;
    wire core_5v;
    wire core_phi;
    wire core_wr_n;
    wire core_rd_n;
    wire core_cs1_n;
    wire core_cs2_n;
    wire core_dir_a;
    wire core_dir_ad;

    bacon_legacy_core core_inst (
        .spi_cs0     (ESP32_SPI2_CS_N),
        .spi_cs1     (ESP32_SPI_CS1),
        .spi_sck     (MCU_SPI_CLK),
        .spi_mosi    (ESP32_SPI2_MOSI),
        .spi_miso    (core_spi_miso),
        .led0        (core_led_act),
        .led1        (core_led_ready),
        .pwr_3v      (core_3v3),
        .pwr_5v      (core_5v),
        .cart_phi    (core_phi),
        .cart_nWR    (core_wr_n),
        .cart_nRD    (core_rd_n),
        .cart_cs1    (core_cs1_n),
        .cart_cs2    (core_cs2_n),
        .cart_req    (IRQ),
        .cart_ad     (AD),
        .cart_a      (A),
        .sys_clock   (sys_clock),
        .resetn      (resetn),
        .cart_dir_a  (core_dir_a),
        .cart_dir_ad (core_dir_ad)
    );

    assign ESP32_SPI2_MISO = core_spi_miso;
    assign LED_ACT         = core_led_act;
    assign LED_READY       = core_led_ready;
    assign V3V3_CTRL       = core_3v3;
    assign V5V_CTRL        = core_5v;
    assign PHI             = core_phi;
    assign WR_N            = core_wr_n;
    assign RD_N            = core_rd_n;
    assign CS1_N           = core_cs1_n;
    assign CS2_N           = core_cs2_n;
    assign CART_DIR_A      = core_dir_a;
    assign CART_DIR_AD     = core_dir_ad;

    assign mem_ahb_hreadyout = 1'b1;
    assign slave_ahb_hready  = 1'b1;

endmodule

/*
 * bacon_legacy_core
 * 由 AGM 原版 top.v 迁移而来。
 *
 * 为方便对照 AGM 原版：
 * 1. 与 AGM 不同的实现统一前置，并用分割线标出
 * 2. 其余 SPI 协议主体尽量保持与 top.v 相同的中文注释和摆放顺序
 */
module bacon_legacy_core(
    /* SPI 接口信号 */
    input  spi_cs0,
    input  spi_cs1,
    input  spi_sck,
    input  spi_mosi,
    output reg spi_miso,

    /* 指示灯信号 */
    output reg led0 = 1'd1,
    output reg led1 = 1'd1,

    /* 电源控制信号 */
    output pwr_3v,
    output pwr_5v,

    /* 卡带总线信号 */
    output cart_phi,
    output cart_nWR,
    output cart_nRD,
    output cart_cs1,
    inout  cart_cs2,
    input  cart_req,
    inout  [15:0] cart_ad,
    inout  [23:16] cart_a,

    /* 与 AGM top.v 不同的新增信号 */
    input  sys_clock,
    input  resetn,
    output tri cart_dir_a,
    output tri cart_dir_ad
);

    /*
     * SPI 通信协议定义
     *
     * spi_cs0         mosi                          miso
     *    ________________________________________________________
     *   |
     *  B| byte0    [7:6] batch_size  // 批处理大小
     *  A|          5:dir_a         4:dir_ad         dummy
     *  T|          3:cs2  2:cs1  1:rd  0:wr  // 控制信号
     *  C| byte1    ad_out[7:0]                   ad_in[7:0]
     *  H| byte2    ad_out[15:8]                  ad_in[15:8]
     *   | byte3    a_out[7:0]                    a_in[7:0]
     *   |________________________________________________________
     *
     *
     * spi_cs1         mosi                          miso
     *     byte0    6:pwr_en  5:pwr_5  4:pwr_3    1:cs2  0:req
     *              2:dir_cs2   [1:0]phi_div  // 时钟分频
     *
     *
     * spi_cs0+1         mosi                          miso
     *    ________________________________________________________
     *   |
     *  B| byte0    [7:6] batch_size
     *  A|          5:dir_a         4:dir_ad         dummy
     *  T|          3:ad_incr  2:cs1  1:rd  0:wr  // 地址自增
     *  C| byte1    a_out[7:0]                    a_in[7:0]
     *  H| byte2    ad_out[7:0]                   ad_in[7:0]
     *   | byte3    ad_out[15:8]                  ad_in[15:8]
     *   |________________________________________________________
     */

    /* ====================================================================== */
    /* 与 AGM top.v 不同的部分                                                */
    /* 1. 新增 cart_dir_a/cart_dir_ad，用于电平转换方向控制                   */
    /* 2. DIR 与 A/AD 的 OE 直接对应，便于保持与 AGM 原版总线时序一致         */
    /* 3. 使用 sys_clock 分频替代 AGM 内部振荡器                              */
    /* 4. LED 改为“空闲 1 秒后 led1 呼吸灯”的状态逻辑                         */
    /* ====================================================================== */

    localparam [3:0]  OSC_DIV_HALF         = 4'd12;
    localparam [22:0] LED_IDLE_DELAY_COUNT = 23'd5769231;
    localparam [15:0] LED_BREATHE_STEP_DIV = 16'd20000;

    reg        osc     = 1'b0;
    reg [3:0]  osc_div = 4'd0;
    reg [1:0]  cnt_osc = 2'd0;

    /* 卡带总线输出使能信号 */
    reg        cart_a_oe    = 1'd0;
    reg        cart_ad_oe   = 1'd0;
    reg        cart_cs2_oe  = 1'd1;

    /* 卡带总线控制信号 */
    reg        cart_cs1_out = 1'd1;
    reg        cart_cs2_out = 1'd1;
    reg        cart_nRD_out = 1'd1;
    reg        cart_nWR_out = 1'd1;

    /* 卡带总线数据信号 */
    reg [15:0] cart_ad_out  = 16'd0;
    reg [7:0]  cart_a_out   = 8'd0;

    /* 地址自增标志 */
    reg        ad_incr = 1'd0;

    /* 控制信号影子寄存器（用于同步更新） */
    reg        cart_a_oe_shadow  = 1'd0;
    reg        cart_ad_oe_shadow = 1'd0;
    reg        cart_cs1_shadow   = 1'd1;
    reg        cart_cs2_shadow   = 1'd1;
    reg        cart_nRD_shadow   = 1'd1;
    reg        cart_nWR_shadow   = 1'd1;

    /* 时钟分频控制 */
    reg [1:0]  phi_div = 2'd0;

    /* 电源控制 */
    reg        pwr_5v_out = 1'd0;
    reg        pwr_3v_out = 1'd1;

    /* LED 状态控制 */
    wire       spi_active;
    wire       led1_breathe_on;
    reg [22:0] led_idle_count = 23'd0;
    reg        led_idle_ready = 1'b0;
    reg [7:0]  led1_pwm_counter = 8'd0;
    reg [7:0]  led1_breathe_level = 8'd0;
    reg        led1_breathe_dir = 1'b0;
    reg [15:0] led1_breathe_step_count = 16'd0;

    /* 位计数器（用于 SPI 通信） */
    wire       spi_cs;
    reg [4:0]  bit_cnt           = 5'd0;
    reg [4:0]  bit_cnt_threshold = 5'd31;

    /* 批处理大小设置 */
    reg [1:0]  batch_size = 2'd0;

    /* MISO（主入从出）数据缓冲区 */
    reg [23:0] buf_miso_cs0;
    reg [7:0]  buf_miso_cs1;
    reg [23:0] buf_miso_cs2;

    /* MOSI（主出从入）数据缓冲区 */
    reg [31:0] buf_mosi = 8'd0;
    wire [31:0] buf_mosi_current;

    /* ====================================================================== */
    /* 与 AGM top.v 不同的实现：方向引脚输出                                  */
    /* DIR 语义：1 -> MCU 驱动卡带；0 -> 卡带驱动 MCU                         */
    /* A/AD 总线本身保持与 AGM 原版一致，直接由 OE 控制                       */
    /* ====================================================================== */

    assign cart_ad     = cart_ad_oe ? cart_ad_out : 16'hzzzz;
    assign cart_a      = cart_a_oe  ? cart_a_out  : 8'hzz;
    assign cart_dir_a  = cart_a_oe;
    assign cart_dir_ad = cart_ad_oe;

    /* ====================================================================== */
    /* 与 AGM top.v 不同的实现：卡带时钟生成                                  */
    /* AGM 原版使用内部振荡器 IP                                               */
    /* 当前版本改为使用 sys_clock 分频，保持 phi_div 控制语义                  */
    /* ====================================================================== */

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn) begin
            osc_div <= 4'd0;
            osc <= 1'b0;
        end
        else if (osc_div == OSC_DIV_HALF) begin
            osc_div <= 4'd0;
            osc <= ~osc;
        end
        else begin
            osc_div <= osc_div + 4'd1;
        end
    end

    always @(posedge osc or negedge resetn) begin
        if (!resetn) begin
            cnt_osc <= 2'd0;
        end
        else begin
            cnt_osc <= cnt_osc + 2'd1;
        end
    end

    assign cart_phi = (phi_div == 2'd0) ? 1'd0 :
                      (phi_div == 2'd1) ? cnt_osc[1] :
                      (phi_div == 2'd2) ? cnt_osc[0] : osc;

    /* ====================================================================== */
    /* 与 AGM top.v 不同的实现：LED 状态控制                                  */
    /* 无动作满 1 秒后，led1 进入 PWM 呼吸灯                                  */
    /* ====================================================================== */

    assign spi_cs           = spi_cs0 & spi_cs1;
    assign spi_active       = !spi_cs0 || !spi_cs1;
    assign buf_mosi_current = {buf_mosi[30:0], spi_mosi};
    assign led1_breathe_on  = led1_pwm_counter < led1_breathe_level;

    always @(posedge osc or negedge resetn) begin
        if (!resetn) begin
            led_idle_count          <= 23'd0;
            led_idle_ready          <= 1'b0;
            led1_pwm_counter        <= 8'd0;
            led1_breathe_level      <= 8'd0;
            led1_breathe_dir        <= 1'b0;
            led1_breathe_step_count <= 16'd0;
        end
        else begin
            led1_pwm_counter <= led1_pwm_counter + 8'd1;

            if (spi_active) begin
                led_idle_count          <= 23'd0;
                led_idle_ready          <= 1'b0;
                led1_breathe_level      <= 8'd0;
                led1_breathe_dir        <= 1'b0;
                led1_breathe_step_count <= 16'd0;
            end
            else if (!led_idle_ready) begin
                if (led_idle_count < (LED_IDLE_DELAY_COUNT - 23'd1)) begin
                    led_idle_count <= led_idle_count + 23'd1;
                end
                else begin
                    led_idle_ready          <= 1'b1;
                    led1_breathe_level      <= 8'd1;
                    led1_breathe_dir        <= 1'b0;
                    led1_breathe_step_count <= 16'd0;
                end
            end
            else begin
                if (led1_breathe_step_count < (LED_BREATHE_STEP_DIV - 16'd1)) begin
                    led1_breathe_step_count <= led1_breathe_step_count + 16'd1;
                end
                else begin
                    led1_breathe_step_count <= 16'd0;

                    if (!led1_breathe_dir) begin
                        if (led1_breathe_level == 8'hff) begin
                            led1_breathe_dir   <= 1'b1;
                            led1_breathe_level <= 8'hfe;
                        end
                        else begin
                            led1_breathe_level <= led1_breathe_level + 8'd1;
                        end
                    end
                    else begin
                        if (led1_breathe_level == 8'd0) begin
                            led1_breathe_dir   <= 1'b0;
                            led1_breathe_level <= 8'd1;
                        end
                        else begin
                            led1_breathe_level <= led1_breathe_level - 8'd1;
                        end
                    end
                end
            end
        end
    end

    /* LED 控制逻辑 */
    always @(*) begin
        if (spi_active) begin
            led0 = 1'd0;
            led1 = 1'd1;
        end
        else if (led_idle_ready) begin
            led0 = 1'd1;
            led1 = led1_breathe_on ? 1'd0 : 1'd1;
        end
        else begin
            led0 = 1'd1;
            led1 = 1'd1;
        end
    end

    /* 位计数器逻辑 */
    always @(posedge spi_cs or posedge spi_sck) begin
        if (spi_cs) begin
            bit_cnt <= 5'd0;
        end
        else begin
            if (bit_cnt >= bit_cnt_threshold)
                bit_cnt <= 5'd0;
            else
                bit_cnt <= bit_cnt + 5'd1;
        end
    end

    always @(*) begin
        case (batch_size)
            2'd0: bit_cnt_threshold = 5'd7;
            2'd1: bit_cnt_threshold = 5'd15;
            2'd2: bit_cnt_threshold = 5'd23;
            2'd3: bit_cnt_threshold = 5'd31;
        endcase
    end


    /* ====================================================================== */
    /* SPI 接口逻辑                                                            */
    /* ====================================================================== */


    /* MISO（主入从出）数据处理 */

    /* CS0 选择时的 MISO 缓冲区 */
    always @(posedge spi_sck) begin
        if (bit_cnt == 5'd7) begin
            buf_miso_cs0 <= {cart_ad[7:0], cart_ad[15:8], cart_a[23:16]};
        end
        else begin
            buf_miso_cs0 <= {buf_miso_cs0[22:0], 1'd0};
        end
    end

    /* CS1 选择时的 MISO 缓冲区 */
    always @(posedge spi_cs1 or posedge spi_sck) begin
        if (spi_cs1) begin
            buf_miso_cs1 <= {6'd0, cart_cs2, cart_req};
        end
        else begin
            buf_miso_cs1 <= {buf_miso_cs1[6:0], 1'd0};
        end
    end

    /* CS0+CS1 选择时的 MISO 缓冲区 */
    always @(posedge spi_sck) begin
        if (bit_cnt == 5'd7) begin
            buf_miso_cs2 <= {cart_a[23:16], cart_ad[7:0], cart_ad[15:8]};
        end
        else begin
            buf_miso_cs2 <= {buf_miso_cs2[22:0], 1'd0};
        end
    end


    /* MOSI（主出从入）数据处理 */

    always @(posedge spi_sck) begin
        buf_mosi <= buf_mosi_current;
    end

    /* MOSI 数据解析 */
    always @(posedge spi_sck) begin
        case ({spi_cs1, spi_cs0})
            2'b10: begin
                if (bit_cnt == 5'd1) begin
                    batch_size <= buf_mosi_current[1:0];
                end

                if (bit_cnt == 5'd7) begin
                    cart_a_oe_shadow  <= buf_mosi_current[5];
                    cart_ad_oe_shadow <= buf_mosi_current[4];
                    cart_cs2_shadow   <= buf_mosi_current[3];
                    cart_cs1_shadow   <= buf_mosi_current[2];
                    cart_nRD_shadow   <= buf_mosi_current[1];
                    cart_nWR_shadow   <= buf_mosi_current[0];
                end

                if (bit_cnt == 5'd23) begin
                    cart_ad_out <= {buf_mosi_current[7:0], buf_mosi_current[15:8]};
                end

                if (bit_cnt == 5'd31) begin
                    cart_a_out <= buf_mosi_current[7:0];
                end
            end

            2'b01: begin
                if (bit_cnt == 5'd7) begin
                    pwr_5v_out <= (buf_mosi_current[6] & buf_mosi_current[5] & !buf_mosi_current[4]) ? 1'd1 : 1'd0;
                    pwr_3v_out <= (buf_mosi_current[6] & !buf_mosi_current[5] & buf_mosi_current[4]) ? 1'd1 : 1'd0;
                    cart_cs2_oe <= buf_mosi_current[2];
                    phi_div <= buf_mosi_current[1:0];
                end
            end

            2'b00: begin
                if (bit_cnt == 5'd1) begin
                    batch_size <= buf_mosi_current[1:0];
                end

                if (bit_cnt == 5'd4) begin
                    ad_incr <= buf_mosi_current[0];
                end

                if (bit_cnt == 5'd7) begin
                    cart_a_oe_shadow  <= buf_mosi_current[5];
                    cart_ad_oe_shadow <= buf_mosi_current[4];
                    cart_cs1_shadow   <= buf_mosi_current[2];
                    cart_nRD_shadow   <= buf_mosi_current[1];
                    cart_nWR_shadow   <= buf_mosi_current[0];
                end

                if (bit_cnt == 5'd15) begin
                    cart_a_out <= buf_mosi_current[7:0];
                end

                if (bit_cnt == 5'd31) begin
                    cart_ad_out <= {buf_mosi_current[7:0], buf_mosi_current[15:8]};
                end
                else begin
                    if ((bit_cnt == bit_cnt_threshold) && ad_incr) begin
                        cart_ad_out <= cart_ad_out + 16'd1;
                    end
                end
            end

            default: begin
            end
        endcase
    end


    /* 卡带总线控制线更新 */
    always @(posedge spi_cs0 or negedge spi_sck) begin
        if (spi_cs0) begin
            cart_a_oe    <= cart_a_oe_shadow;
            cart_ad_oe   <= cart_ad_oe_shadow;
            cart_cs2_out <= cart_cs2_shadow;
            cart_cs1_out <= cart_cs1_shadow;
            cart_nRD_out <= cart_nRD_shadow;
            cart_nWR_out <= cart_nWR_shadow;
        end
        else begin
            if (bit_cnt == 5'd0) begin
                cart_a_oe    <= cart_a_oe_shadow;
                cart_ad_oe   <= cart_ad_oe_shadow;
                cart_cs2_out <= cart_cs2_shadow;
                cart_cs1_out <= cart_cs1_shadow;
                cart_nRD_out <= cart_nRD_shadow;
                cart_nWR_out <= cart_nWR_shadow;
            end
        end
    end


    /* ====================================================================== */
    /* LED 状态控制                                                            */
    /* 当前版本与 AGM 原版不同，已前置到上方差异分段                           */
    /* ====================================================================== */


    /* 卡带时钟生成 */
    /* 当前版本与 AGM 原版不同，已前置到上方差异分段 */


    /* SPI MISO 输出选择 */
    always @(*) begin
        case ({spi_cs1, spi_cs0})
            2'b10: spi_miso   = buf_miso_cs0[23];
            2'b01: spi_miso   = buf_miso_cs1[7];
            2'b00: spi_miso   = buf_miso_cs2[23];
            default: spi_miso = 1'b0;
        endcase
    end

    /* 电源输出赋值 */
    assign pwr_3v = pwr_3v_out;
    assign pwr_5v = pwr_5v_out;

    /* 卡带控制线赋值 */
    assign cart_nWR = cart_nWR_out;
    assign cart_nRD = cart_nRD_out;
    assign cart_cs1 = cart_cs1_out;
    assign cart_cs2 = cart_cs2_oe ? cart_cs2_out : 1'hz;

    /* 卡带总线赋值（三态） */
    /* 当前版本与 AGM 原版不同的部分仅剩 DIR 引脚输出，已前置到对应分段 */

endmodule
