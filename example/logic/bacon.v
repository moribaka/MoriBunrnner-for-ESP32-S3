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
    wire mcu_mode;
    wire mcu_status_miso;
    wire mcu_data_miso;
    wire mcu_response_ready;
    wire mcu_request_busy;
    wire mcu_protocol_error;
    wire mcu_force_legacy;
    wire [7:0] mcu_cart_a_out;
    wire [15:0] mcu_cart_ad_out;
    wire mcu_cart_a_oe;
    wire mcu_cart_ad_oe;
    wire mcu_cart_cs1;
    wire mcu_cart_cs2;
    wire mcu_cart_rd;
    wire mcu_cart_wr;
    wire mcu_power_3v;
    wire mcu_power_5v;
    wire mcu_phi;
    wire legacy_spi_cs0 = mcu_mode ? 1'b1 : ESP32_SPI2_CS_N;
    wire legacy_spi_cs1 = mcu_mode ? 1'b1 : ESP32_SPI_CS1;

    bacon_mode_guard mode_guard (
        .sys_clock    (sys_clock),
        .resetn       (resetn),
        .spi_cs0      (ESP32_SPI2_CS_N),
        .spi_cs1      (ESP32_SPI_CS1),
        .spi_sck      (MCU_SPI_CLK),
        .spi_mosi     (ESP32_SPI2_MOSI),
        .response_ready(mcu_response_ready),
        .request_busy (mcu_request_busy),
        .protocol_error(mcu_protocol_error),
        .force_legacy(mcu_force_legacy),
        .mcu_mode     (mcu_mode),
        .status_miso  (mcu_status_miso)
    );

    bacon_mcu_transport mcu_transport (
        .sys_clock(sys_clock),
        .resetn(resetn),
        .mcu_mode(mcu_mode),
        .spi_cs0(ESP32_SPI2_CS_N),
        .spi_cs1(ESP32_SPI_CS1),
        .spi_sck(MCU_SPI_CLK),
        .spi_mosi(ESP32_SPI2_MOSI),
        .spi_miso(mcu_data_miso),
        .response_ready(mcu_response_ready),
        .request_busy(mcu_request_busy),
        .protocol_error(mcu_protocol_error),
        .force_legacy(mcu_force_legacy),
        .legacy_power_3v(core_3v3),
        .legacy_power_5v(core_5v),
        .cart_a_in(A),
        .cart_ad_in(AD),
        .cart_a_out(mcu_cart_a_out),
        .cart_ad_out(mcu_cart_ad_out),
        .cart_a_oe(mcu_cart_a_oe),
        .cart_ad_oe(mcu_cart_ad_oe),
        .cart_cs1(mcu_cart_cs1),
        .cart_cs2(mcu_cart_cs2),
        .cart_rd(mcu_cart_rd),
        .cart_wr(mcu_cart_wr),
        .power_3v(mcu_power_3v),
        .power_5v(mcu_power_5v),
        .cart_phi(mcu_phi),
        .mem_ahb_htrans(mem_ahb_htrans),
        .mem_ahb_hready(mem_ahb_hready),
        .mem_ahb_hwrite(mem_ahb_hwrite),
        .mem_ahb_haddr(mem_ahb_haddr),
        .mem_ahb_hwdata(mem_ahb_hwdata),
        .mem_ahb_hreadyout(mem_ahb_hreadyout),
        .mem_ahb_hresp(mem_ahb_hresp),
        .mem_ahb_hrdata(mem_ahb_hrdata),
        .slave_ahb_hreadyout(slave_ahb_hreadyout),
        .slave_ahb_htrans(slave_ahb_htrans),
        .slave_ahb_hsize(slave_ahb_hsize),
        .slave_ahb_hburst(slave_ahb_hburst),
        .slave_ahb_hwrite(slave_ahb_hwrite),
        .slave_ahb_haddr(slave_ahb_haddr),
        .slave_ahb_hwdata(slave_ahb_hwdata),
        .slave_ahb_hresp(slave_ahb_hresp),
        .slave_ahb_hrdata(slave_ahb_hrdata)
    );

    bacon_legacy_core core_inst (
        .spi_cs0     (legacy_spi_cs0),
        .spi_cs1     (legacy_spi_cs1),
        .spi_sck     (MCU_SPI_CLK),
        .spi_mosi    (ESP32_SPI2_MOSI),
        .spi_miso    (core_spi_miso),
        .interface_enable(!mcu_mode),
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

    assign ESP32_SPI2_MISO = mcu_mode ?
        ((!ESP32_SPI2_CS_N && !ESP32_SPI_CS1) ? mcu_status_miso : mcu_data_miso) :
        core_spi_miso;
    assign LED_ACT         = core_led_act;
    assign LED_READY       = core_led_ready;
    assign V3V3_CTRL       = mcu_mode ? mcu_power_3v : core_3v3;
    assign V5V_CTRL        = mcu_mode ? mcu_power_5v : core_5v;
    assign PHI             = mcu_mode ? mcu_phi : core_phi;
    assign WR_N            = mcu_mode ? mcu_cart_wr : core_wr_n;
    assign RD_N            = mcu_mode ? mcu_cart_rd : core_rd_n;
    assign CS1_N           = mcu_mode ? mcu_cart_cs1 : core_cs1_n;
    assign CS2_N           = mcu_mode ? mcu_cart_cs2 : core_cs2_n;
    assign CART_DIR_A      = mcu_mode ? mcu_cart_a_oe : core_dir_a;
    assign CART_DIR_AD     = mcu_mode ? mcu_cart_ad_oe : core_dir_ad;
    assign A               = (mcu_mode && mcu_cart_a_oe) ? mcu_cart_a_out : 8'hzz;
    assign AD              = (mcu_mode && mcu_cart_ad_oe) ? mcu_cart_ad_out : 16'hzzzz;

    assign slave_ahb_hsel   = slave_ahb_htrans[1];
    assign slave_ahb_hready  = 1'b1;

endmodule

module bacon_mode_guard(
    input  sys_clock,
    input  resetn,
    input  spi_cs0,
    input  spi_cs1,
    input  spi_sck,
    input  spi_mosi,
    input  response_ready,
    input  request_busy,
    input  protocol_error,
    input  force_legacy,
    output reg mcu_mode,
    output status_miso
);
    localparam [63:0] ENTER_MAGIC = 64'h4d4f5249324d4355; // MORI2MCU
    localparam [63:0] EXIT_MAGIC  = 64'h4d4f5249324c4547; // MORI2LEG

    reg [2:0] sck_sync;
    reg [2:0] mosi_sync;
    reg [2:0] idle_sync;
    reg [63:0] mode_shift;
    reg [6:0] mode_bit_count;
    reg [5:0] status_bit_count;

    wire sampled_sck_rise = !sck_sync[2] && sck_sync[1];
    wire mode_channel_idle = idle_sync[2];
    wire status_selected = !spi_cs0 && !spi_cs1;
    wire [31:0] status_word = {
        8'ha7,
        8'h32,
        8'h01,
        4'b0000,
        protocol_error,
        request_busy,
        response_ready,
        mcu_mode
    };

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn) begin
            sck_sync <= 3'b000;
            mosi_sync <= 3'b000;
            idle_sync <= 3'b000;
        end else begin
            sck_sync <= {sck_sync[1:0], spi_sck};
            mosi_sync <= {mosi_sync[1:0], spi_mosi};
            idle_sync <= {idle_sync[1:0], spi_cs0 && spi_cs1};
        end
    end

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn) begin
            mcu_mode <= 1'b0;
            mode_shift <= 64'd0;
            mode_bit_count <= 7'd0;
        end else if (force_legacy) begin
            mcu_mode <= 1'b0;
            mode_shift <= 64'd0;
            mode_bit_count <= 7'd0;
        end else if (!mode_channel_idle) begin
            mode_shift <= 64'd0;
            mode_bit_count <= 7'd0;
        end else if (sampled_sck_rise) begin
            mode_shift <= {mode_shift[62:0], mosi_sync[2]};
            if (mode_bit_count == 7'd63) begin
                mode_bit_count <= 7'd0;
                if ({mode_shift[62:0], mosi_sync[2]} == ENTER_MAGIC)
                    mcu_mode <= 1'b1;
                else if ({mode_shift[62:0], mosi_sync[2]} == EXIT_MAGIC)
                    mcu_mode <= 1'b0;
            end else begin
                mode_bit_count <= mode_bit_count + 7'd1;
            end
        end
    end

    always @(posedge spi_sck or negedge resetn or negedge status_selected) begin
        if (!resetn || !status_selected)
            status_bit_count <= 6'd0;
        else if (status_bit_count == 6'd31)
            status_bit_count <= 6'd0;
        else
            status_bit_count <= status_bit_count + 6'd1;
    end

    assign status_miso = status_word[31 - status_bit_count];
endmodule

module bacon_mcu_transport(
    input sys_clock,
    input resetn,
    input mcu_mode,
    input spi_cs0,
    input spi_cs1,
    input spi_sck,
    input spi_mosi,
    output spi_miso,
    output response_ready,
    output request_busy,
    output protocol_error,
    output reg force_legacy,
    input legacy_power_3v,
    input legacy_power_5v,
    input [7:0] cart_a_in,
    input [15:0] cart_ad_in,
    output [7:0] cart_a_out,
    output [15:0] cart_ad_out,
    output cart_a_oe,
    output cart_ad_oe,
    output cart_cs1,
    output cart_cs2,
    output cart_rd,
    output cart_wr,
    output power_3v,
    output power_5v,
    output cart_phi,
    input [1:0] mem_ahb_htrans,
    input mem_ahb_hready,
    input mem_ahb_hwrite,
    input [31:0] mem_ahb_haddr,
    input [31:0] mem_ahb_hwdata,
    output mem_ahb_hreadyout,
    output mem_ahb_hresp,
    output reg [31:0] mem_ahb_hrdata,
    input slave_ahb_hreadyout,
    output [1:0] slave_ahb_htrans,
    output [2:0] slave_ahb_hsize,
    output [2:0] slave_ahb_hburst,
    output slave_ahb_hwrite,
    output [31:0] slave_ahb_haddr,
    output [31:0] slave_ahb_hwdata,
    input slave_ahb_hresp,
    input [31:0] slave_ahb_hrdata
);
    localparam [31:0] REG_CTRL       = 32'h6002_0000;
    localparam [31:0] REG_RX_ADDR    = 32'h6002_0004;
    localparam [31:0] REG_RX_LIMIT   = 32'h6002_0008;
    localparam [31:0] REG_RX_WORDS   = 32'h6002_000c;
    localparam [31:0] REG_TX_ADDR    = 32'h6002_0010;
    localparam [31:0] REG_TX_WORDS   = 32'h6002_0014;
    localparam [31:0] REG_IDENTITY   = 32'h6002_0018;
    localparam [31:0] REG_MODE       = 32'h6002_001c;
    localparam [31:0] REG_CART_OUT   = 32'h6002_0100;
    localparam [31:0] REG_CART_CTRL  = 32'h6002_0104;
    localparam [31:0] REG_CART_IN    = 32'h6002_0108;
    localparam [31:0] MODE_EXIT_KEY  = 32'h3147_454c; // "LEG1"

    reg ahb_hreadyout_reg;
    reg ahb_write_reg;
    reg [10:0] ahb_register_select_reg;
    reg [31:0] rx_base_address;
    reg [31:0] rx_address;
    reg [12:0] rx_limit_words;
    reg [12:0] rx_word_count;
    reg rx_armed;
    reg rx_done;
    reg rx_end_seen;
    reg [31:0] tx_base_address;
    reg [31:0] tx_address;
    reg [12:0] tx_word_count;
    reg [12:0] tx_fetched_words;
    reg tx_published;
    reg tx_consumed;
    reg tx_retry_reset;
    reg mcu_busy;
    reg transport_error;
    reg [23:0] cart_output_reg;
    reg [9:0] cart_control_reg;
    reg [7:0] phi_counter;
    reg [2:0] cs0_sync;
    reg [2:0] cs1_sync;
    reg [2:0] sck_sync;
    reg rx_selected_d;
    reg tx_selected_d;
    reg [5:0] tx_bit_count;
    reg [12:0] tx_sent_words;
    reg [5:0] rx_bit_count;
    reg [12:0] rx_wire_words;

    wire rx_selected = mcu_mode && !cs0_sync[2] && cs1_sync[2];
    wire tx_selected = mcu_mode && cs0_sync[2] && !cs1_sync[2];
    wire sck_rise = !sck_sync[2] && sck_sync[1];
    wire stream_rx_valid;
    wire stream_rx_addr_inc;
    wire stream_tx_ready;
    wire stream_tx_addr_inc;
    wire stream_idle;
    wire stream_rx_enable = mcu_mode && rx_armed &&
        (rx_selected || rx_selected_d || rx_end_seen) &&
        (rx_word_count < rx_limit_words);
    wire stream_tx_enable = mcu_mode && tx_published && !tx_retry_reset;
    wire stream_tx_fetch_enable = stream_tx_enable &&
        (tx_fetched_words < tx_word_count);

    assign mem_ahb_hreadyout = ahb_hreadyout_reg;
    assign mem_ahb_hresp = 1'b0;
    assign response_ready = tx_published;
    assign request_busy = mcu_busy || rx_done;
    assign protocol_error = transport_error;
    assign cart_a_out = cart_output_reg[23:16];
    assign cart_ad_out = cart_output_reg[15:0];
    assign cart_wr = cart_control_reg[0];
    assign cart_rd = cart_control_reg[1];
    assign cart_cs1 = cart_control_reg[2];
    assign cart_cs2 = cart_control_reg[3];
    assign cart_ad_oe = cart_control_reg[4];
    assign cart_a_oe = cart_control_reg[5];
    assign power_3v = cart_control_reg[6];
    assign power_5v = cart_control_reg[7];
    assign cart_phi = (cart_control_reg[9:8] == 2'd0) ? 1'b0 :
                      (cart_control_reg[9:8] == 2'd1) ? phi_counter[7] :
                      (cart_control_reg[9:8] == 2'd2) ? phi_counter[6] :
                                                       phi_counter[5];

    bacon_spi_ahb_stream stream (
        .sys_clock(sys_clock),
        .resetn(resetn),
        .csn(spi_cs0 && spi_cs1),
        .sck(spi_sck),
        .mosi(spi_mosi),
        .miso(spi_miso),
        .rx_en(stream_rx_enable),
        .rx_valid(stream_rx_valid),
        .rx_addr(rx_address),
        .rx_addr_inc(stream_rx_addr_inc),
        .tx_en(stream_tx_enable),
        .tx_fetch_en(stream_tx_fetch_enable),
        .tx_ready(stream_tx_ready),
        .tx_addr(tx_address),
        .tx_addr_inc(stream_tx_addr_inc),
        .idle(stream_idle),
        .ahb_hreadyout(slave_ahb_hreadyout),
        .ahb_htrans(slave_ahb_htrans),
        .ahb_hsize(slave_ahb_hsize),
        .ahb_hburst(slave_ahb_hburst),
        .ahb_hwrite(slave_ahb_hwrite),
        .ahb_haddr(slave_ahb_haddr),
        .ahb_hwdata(slave_ahb_hwdata),
        .ahb_hresp(slave_ahb_hresp),
        .ahb_hrdata(slave_ahb_hrdata)
    );

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn) begin
            cs0_sync <= 3'b111;
            cs1_sync <= 3'b111;
            sck_sync <= 3'b000;
            phi_counter <= 8'd0;
        end else begin
            cs0_sync <= {cs0_sync[1:0], spi_cs0};
            cs1_sync <= {cs1_sync[1:0], spi_cs1};
            sck_sync <= {sck_sync[1:0], spi_sck};
            phi_counter <= phi_counter + 8'd1;
        end
    end

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn) begin
            ahb_hreadyout_reg <= 1'b1;
            ahb_write_reg <= 1'b0;
            ahb_register_select_reg <= 11'd0;
            rx_base_address <= 32'h2000_0000;
            rx_address <= 32'h2000_0000;
            rx_limit_words <= 13'd0;
            rx_word_count <= 13'd0;
            rx_armed <= 1'b0;
            rx_done <= 1'b0;
            rx_end_seen <= 1'b0;
            tx_base_address <= 32'h2000_0000;
            tx_address <= 32'h2000_0000;
            tx_word_count <= 13'd0;
            tx_fetched_words <= 13'd0;
            tx_published <= 1'b0;
            tx_consumed <= 1'b0;
            tx_retry_reset <= 1'b0;
            mcu_busy <= 1'b0;
            transport_error <= 1'b0;
            force_legacy <= 1'b0;
            cart_output_reg <= 24'd0;
            cart_control_reg <= 10'b00_0100_1111;
            rx_selected_d <= 1'b0;
            tx_selected_d <= 1'b0;
            tx_bit_count <= 6'd0;
            tx_sent_words <= 13'd0;
            rx_bit_count <= 6'd0;
            rx_wire_words <= 13'd0;
        end else begin
            force_legacy <= 1'b0;
            tx_retry_reset <= 1'b0;
            rx_selected_d <= rx_selected;
            tx_selected_d <= tx_selected;

            if (!mcu_mode)
                cart_control_reg[7:6] <= {legacy_power_5v, legacy_power_3v};

            if (stream_rx_addr_inc) begin
                rx_address <= rx_address + 32'd4;
                rx_word_count <= rx_word_count + 13'd1;
            end
            if (!rx_selected) begin
                rx_bit_count <= 6'd0;
                if (!rx_selected_d) rx_wire_words <= 13'd0;
            end else if (sck_rise) begin
                if (rx_bit_count == 6'd31) begin
                    rx_bit_count <= 6'd0;
                    rx_wire_words <= rx_wire_words + 13'd1;
                end else begin
                    rx_bit_count <= rx_bit_count + 6'd1;
                end
            end
            if (rx_selected_d && !rx_selected) begin
                rx_end_seen <= 1'b1;
                if (rx_bit_count != 6'd0)
                    transport_error <= 1'b1;
            end
            if (rx_end_seen && stream_idle) begin
                rx_done <= 1'b1;
                rx_armed <= 1'b0;
                rx_end_seen <= 1'b0;
                if (rx_wire_words > rx_limit_words)
                    transport_error <= 1'b1;
            end

            if (stream_tx_addr_inc) begin
                tx_address <= tx_address + 32'd4;
                tx_fetched_words <= tx_fetched_words + 13'd1;
            end
            if (!tx_selected) begin
                tx_bit_count <= 6'd0;
                tx_sent_words <= 13'd0;
            end else if (sck_rise) begin
                if (tx_bit_count == 6'd31) begin
                    tx_bit_count <= 6'd0;
                    tx_sent_words <= tx_sent_words + 13'd1;
                end else begin
                    tx_bit_count <= tx_bit_count + 6'd1;
                end
            end
            if (tx_selected_d && !tx_selected) begin
                if (tx_sent_words >= tx_word_count) begin
                    tx_published <= 1'b0;
                    tx_consumed <= 1'b1;
                end else begin
                    tx_address <= tx_base_address;
                    tx_fetched_words <= 13'd0;
                    tx_retry_reset <= 1'b1;
                end
            end

            if (mem_ahb_hready && ahb_hreadyout_reg && mem_ahb_htrans[1]) begin
                ahb_hreadyout_reg <= 1'b0;
                ahb_write_reg <= mem_ahb_hwrite;
                ahb_register_select_reg <= 11'd0;
                if (mem_ahb_haddr[31:12] == 20'h60020) begin
                    case (mem_ahb_haddr[11:2])
                        REG_CTRL[11:2]:      ahb_register_select_reg <= 11'b00000000001;
                        REG_RX_ADDR[11:2]:   ahb_register_select_reg <= 11'b00000000010;
                        REG_RX_LIMIT[11:2]:  ahb_register_select_reg <= 11'b00000000100;
                        REG_RX_WORDS[11:2]:  ahb_register_select_reg <= 11'b00000001000;
                        REG_TX_ADDR[11:2]:   ahb_register_select_reg <= 11'b00000010000;
                        REG_TX_WORDS[11:2]:  ahb_register_select_reg <= 11'b00000100000;
                        REG_IDENTITY[11:2]:  ahb_register_select_reg <= 11'b00001000000;
                        REG_MODE[11:2]:      ahb_register_select_reg <= 11'b00010000000;
                        REG_CART_OUT[11:2]:  ahb_register_select_reg <= 11'b00100000000;
                        REG_CART_CTRL[11:2]: ahb_register_select_reg <= 11'b01000000000;
                        REG_CART_IN[11:2]:   ahb_register_select_reg <= 11'b10000000000;
                        default: begin end
                    endcase
                end
            end else if (!ahb_hreadyout_reg) begin
                ahb_hreadyout_reg <= 1'b1;
            end

            if (!ahb_hreadyout_reg && ahb_write_reg) begin
                    case (1'b1)
                        ahb_register_select_reg[0]: begin
                            if (mem_ahb_hwdata[0]) begin
                                rx_address <= rx_base_address;
                                rx_word_count <= 13'd0;
                                rx_wire_words <= 13'd0;
                                rx_bit_count <= 6'd0;
                                rx_armed <= 1'b1;
                                rx_done <= 1'b0;
                                rx_end_seen <= 1'b0;
                                transport_error <= 1'b0;
                            end
                            if (mem_ahb_hwdata[1]) rx_done <= 1'b0;
                            if (mem_ahb_hwdata[2]) begin
                                tx_address <= tx_base_address;
                                tx_fetched_words <= 13'd0;
                                tx_published <= 1'b1;
                                tx_consumed <= 1'b0;
                            end
                            if (mem_ahb_hwdata[3]) begin
                                tx_published <= 1'b0;
                                tx_consumed <= 1'b0;
                            end
                            if (mem_ahb_hwdata[4]) mcu_busy <= 1'b1;
                            if (mem_ahb_hwdata[5]) mcu_busy <= 1'b0;
                            if (mem_ahb_hwdata[6]) transport_error <= 1'b1;
                            if (mem_ahb_hwdata[7]) transport_error <= 1'b0;
                        end
                        ahb_register_select_reg[1]: begin
                            rx_base_address <= mem_ahb_hwdata;
                            rx_address <= mem_ahb_hwdata;
                        end
                        ahb_register_select_reg[2]: rx_limit_words <= mem_ahb_hwdata[12:0];
                        ahb_register_select_reg[4]: begin
                            tx_base_address <= mem_ahb_hwdata;
                            tx_address <= mem_ahb_hwdata;
                        end
                        ahb_register_select_reg[5]: tx_word_count <= mem_ahb_hwdata[12:0];
                        ahb_register_select_reg[7]: if (mem_ahb_hwdata == MODE_EXIT_KEY)
                            force_legacy <= 1'b1;
                        ahb_register_select_reg[8]: cart_output_reg <= mem_ahb_hwdata[23:0];
                        ahb_register_select_reg[9]: cart_control_reg <= mem_ahb_hwdata[9:0];
                        default: begin end
                    endcase
            end
        end
    end

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn) begin
            mem_ahb_hrdata <= 32'd0;
        end else if (!ahb_hreadyout_reg && !ahb_write_reg) begin
            case (1'b1)
                ahb_register_select_reg[0]: mem_ahb_hrdata <= {
                    20'd0, tx_consumed, transport_error, mcu_busy, tx_published,
                    rx_done, rx_armed, mcu_mode, 5'd0};
                ahb_register_select_reg[1]: mem_ahb_hrdata <= rx_base_address;
                ahb_register_select_reg[2]: mem_ahb_hrdata <= {19'd0, rx_limit_words};
                ahb_register_select_reg[3]: mem_ahb_hrdata <= {19'd0, rx_word_count};
                ahb_register_select_reg[4]: mem_ahb_hrdata <= tx_base_address;
                ahb_register_select_reg[5]: mem_ahb_hrdata <= {19'd0, tx_word_count};
                ahb_register_select_reg[6]: mem_ahb_hrdata <= 32'h3155_434d; // "MCU1"
                ahb_register_select_reg[7]: mem_ahb_hrdata <= {31'd0, mcu_mode};
                ahb_register_select_reg[8]: mem_ahb_hrdata <= {8'd0, cart_output_reg};
                ahb_register_select_reg[9]: mem_ahb_hrdata <= {22'd0, cart_control_reg};
                ahb_register_select_reg[10]: mem_ahb_hrdata <= {8'd0, cart_a_in, cart_ad_in};
                default: mem_ahb_hrdata <= 32'hdead_beef;
            endcase
        end
    end
endmodule

module bacon_spi_ahb_stream(
    input sys_clock,
    input resetn,
    input csn,
    input sck,
    input mosi,
    output miso,
    input rx_en,
    output rx_valid,
    input [31:0] rx_addr,
    output rx_addr_inc,
    input tx_en,
    input tx_fetch_en,
    output tx_ready,
    input [31:0] tx_addr,
    output tx_addr_inc,
    output idle,
    input ahb_hreadyout,
    output reg [1:0] ahb_htrans,
    output [2:0] ahb_hsize,
    output [2:0] ahb_hburst,
    output reg ahb_hwrite,
    output reg [31:0] ahb_haddr,
    output reg [31:0] ahb_hwdata,
    input ahb_hresp,
    input [31:0] ahb_hrdata
);
    reg ahb_data_phase;
    wire [31:0] rx_data;
    reg [31:0] tx_data;
    wire rx_ready;
    reg tx_valid;

    assign ahb_hsize = 3'b010;
    assign ahb_hburst = 3'b000;
    assign rx_addr_inc = rx_en && rx_valid && ahb_data_phase &&
        ahb_hreadyout && ahb_hwrite;
    assign tx_addr_inc = tx_fetch_en && tx_ready && ahb_data_phase &&
        ahb_hreadyout && !ahb_hwrite;
    assign rx_ready = rx_addr_inc;
    assign idle = ahb_htrans == 2'b00 && !ahb_data_phase && !rx_valid;

    bacon_spi_stream_core core (
        .sys_clock(sys_clock),
        .resetn(resetn),
        .csn(csn),
        .sck(sck),
        .mosi(mosi),
        .miso(miso),
        .rx_en(rx_en),
        .rx_ready(rx_ready),
        .rx_valid(rx_valid),
        .rx_data(rx_data),
        .tx_en(tx_en),
        .tx_ready(tx_ready),
        .tx_valid(tx_valid),
        .tx_data(tx_data)
    );

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn)
            ahb_data_phase <= 1'b0;
        else if (ahb_htrans == 2'b10)
            ahb_data_phase <= 1'b1;
        else if (ahb_hreadyout)
            ahb_data_phase <= 1'b0;
    end

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn) begin
            ahb_htrans <= 2'b00;
            ahb_hwrite <= 1'b0;
            ahb_haddr <= 32'd0;
            ahb_hwdata <= 32'd0;
        end else if (ahb_htrans == 2'b10) begin
            ahb_htrans <= 2'b00;
        end else if (rx_valid && rx_en && !ahb_data_phase && ahb_hreadyout) begin
            ahb_htrans <= 2'b10;
            ahb_hwrite <= 1'b1;
            ahb_haddr <= rx_addr;
            ahb_hwdata <= rx_data;
        end else if (tx_ready && tx_fetch_en && !ahb_data_phase && ahb_hreadyout) begin
            ahb_htrans <= 2'b10;
            ahb_hwrite <= 1'b0;
            ahb_haddr <= tx_addr;
        end
    end

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn)
            tx_data <= 32'd0;
        else if (tx_addr_inc)
            tx_data <= ahb_hrdata;
    end

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn)
            tx_valid <= 1'b0;
        else
            tx_valid <= tx_addr_inc;
    end
endmodule

module bacon_spi_stream_core(
    input sys_clock,
    input resetn,
    input csn,
    input sck,
    input mosi,
    output miso,
    input rx_en,
    input rx_ready,
    output reg rx_valid,
    output reg [31:0] rx_data,
    input tx_en,
    output reg tx_ready,
    input tx_valid,
    input [31:0] tx_data
);
    reg [2:0] csn_reg;
    reg [2:0] sck_reg;
    reg [2:0] mosi_reg;
    reg [4:0] sck_count;
    reg rx_word_ready;
    reg [31:0] rx_word;
    reg [7:0] rx_shift;
    reg [31:0] tx_word;
    reg [7:0] tx_shift;

    wire sck_rise = !sck_reg[2] && sck_reg[1];

    assign miso = tx_shift[7];

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn) begin
            csn_reg <= 3'b111;
            sck_reg <= 3'b000;
            mosi_reg <= 3'b000;
        end else begin
            csn_reg <= {csn_reg[1:0], csn};
            sck_reg <= {sck_reg[1:0], sck};
            mosi_reg <= {mosi_reg[1:0], mosi};
        end
    end

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn)
            sck_count <= 5'd0;
        else if (csn_reg[2])
            sck_count <= 5'd0;
        else if (sck_rise)
            sck_count <= sck_count + 5'd1;
    end

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn) begin
            rx_word <= 32'd0;
            rx_shift <= 8'd0;
            rx_word_ready <= 1'b0;
        end else begin
            rx_word_ready <= 1'b0;
            if (!csn_reg[2] && sck_rise) begin
                rx_shift <= {rx_shift[6:0], mosi_reg[2]};
                if (sck_count[2:0] == 3'd7)
                    rx_word <= {{rx_shift[6:0], mosi_reg[2]}, rx_word[31:8]};
                if (sck_count == 5'd31)
                    rx_word_ready <= 1'b1;
            end
        end
    end

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn)
            rx_data <= 32'd0;
        else if (rx_word_ready)
            rx_data <= rx_word;
    end

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn)
            rx_valid <= 1'b0;
        else if (rx_en && rx_word_ready)
            rx_valid <= 1'b1;
        else if (!rx_en || rx_ready)
            rx_valid <= 1'b0;
    end

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn)
            tx_ready <= 1'b1;
        else if (tx_en && tx_valid)
            tx_ready <= 1'b0;
        else if (!tx_en || (!csn_reg[2] && sck_rise && sck_count == 5'd0))
            tx_ready <= 1'b1;
    end

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn)
            tx_shift <= 8'd0;
        else if (!tx_en)
            tx_shift <= 8'd0;
        else if (csn_reg[2] || (!csn_reg[2] && sck_rise && sck_count[2:0] == 3'd7))
            tx_shift <= tx_word[7:0];
        else if (!csn_reg[2] && sck_rise)
            tx_shift <= {tx_shift[6:0], tx_shift[7]};
    end

    always @(posedge sys_clock or negedge resetn) begin
        if (!resetn)
            tx_word <= 32'd0;
        else if (csn_reg[2] || (!csn_reg[2] && sck_rise && sck_count == 5'd30))
            tx_word <= tx_data;
        else if (!csn_reg[2] && sck_rise && sck_count[2:0] == 3'd0)
            tx_word <= {tx_word[7:0], tx_word[31:8]};
    end
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
    input  interface_enable,

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

    assign cart_ad     = (interface_enable && cart_ad_oe) ? cart_ad_out : 16'hzzzz;
    assign cart_a      = (interface_enable && cart_a_oe)  ? cart_a_out  : 8'hzz;
    assign cart_dir_a  = interface_enable && cart_a_oe;
    assign cart_dir_ad = interface_enable && cart_ad_oe;

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
