module bacon (
  inout       [23:16] A,
  inout       [15:0] AD,
  output tri         CART_DIR_A,
  output tri         CART_DIR_AD,
  output tri0        CS1_N,
  output tri0        CS2_N,
  input              ESP32_SPI2_CS_N,
  output tri0        ESP32_SPI2_MISO,
  input              ESP32_SPI2_MOSI,
  input              ESP32_SPI_CS1,
  inout              IRQ,
  output tri0        LED_ACT,
  output tri0        LED_READY,
  inout              MCU_SPI_CLK,
  inout              PHI,
  output tri0        RD_N,
  output tri0        V3V3_CTRL,
  output tri0        V5V_CTRL,
  output tri0        WR_N,
  input              sys_clock,
  input              bus_clock,
  input              resetn,
  input              stop,
  input       [1:0]  mem_ahb_htrans,
  input              mem_ahb_hready,
  input              mem_ahb_hwrite,
  input       [31:0] mem_ahb_haddr,
  input       [2:0]  mem_ahb_hsize,
  input       [2:0]  mem_ahb_hburst,
  input       [31:0] mem_ahb_hwdata,
  output tri1        mem_ahb_hreadyout,
  output tri0        mem_ahb_hresp,
  output tri0 [31:0] mem_ahb_hrdata,
  output tri0        slave_ahb_hsel,
  output tri1        slave_ahb_hready,
  input              slave_ahb_hreadyout,
  output tri0 [1:0]  slave_ahb_htrans,
  output tri0 [2:0]  slave_ahb_hsize,
  output tri0 [2:0]  slave_ahb_hburst,
  output tri0        slave_ahb_hwrite,
  output tri0 [31:0] slave_ahb_haddr,
  output tri0 [31:0] slave_ahb_hwdata,
  input              slave_ahb_hresp,
  input       [31:0] slave_ahb_hrdata,
  output tri0 [3:0]  ext_dma_DMACBREQ,
  output tri0 [3:0]  ext_dma_DMACLBREQ,
  output tri0 [3:0]  ext_dma_DMACSREQ,
  output tri0 [3:0]  ext_dma_DMACLSREQ,
  input       [3:0]  ext_dma_DMACCLR,
  input       [3:0]  ext_dma_DMACTC,
  output tri0 [3:0]  local_int
);

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

// Route both SPI CS signals from ESP32 into legacy core.
bacon_legacy_core core_inst (
  .spi_cs0    (ESP32_SPI2_CS_N),
  .spi_cs1    (ESP32_SPI_CS1),
  .spi_sck    (MCU_SPI_CLK),
  .sys_clock  (sys_clock),
  .resetn     (resetn),
  .spi_mosi   (ESP32_SPI2_MOSI),
  .spi_miso   (core_spi_miso),
  .led0       (core_led_act),
  .led1       (core_led_ready),
  .pwr_3v     (core_3v3),
  .pwr_5v     (core_5v),
  .cart_phi   (core_phi),
  .cart_nWR   (core_wr_n),
  .cart_nRD   (core_rd_n),
  .cart_cs1   (core_cs1_n),
  .cart_cs2   (core_cs2_n),
  .cart_req   (IRQ),
  .cart_dir_a (core_dir_a),
  .cart_dir_ad(core_dir_ad),
  .cart_ad    (AD),
  .cart_a     (A)
);

assign ESP32_SPI2_MISO = core_spi_miso;
assign LED_ACT = core_led_act;
assign LED_READY = core_led_ready;
assign V3V3_CTRL = core_3v3;
assign V5V_CTRL = core_5v;
assign PHI = core_phi;
assign WR_N = core_wr_n;
assign RD_N = core_rd_n;
assign CS1_N = core_cs1_n;
assign CS2_N = core_cs2_n;
assign CART_DIR_A = core_dir_a;
assign CART_DIR_AD = core_dir_ad;

assign mem_ahb_hreadyout = 1'b1;
assign slave_ahb_hready  = 1'b1;

endmodule

module bacon_legacy_core(
  input spi_cs0,
  input spi_cs1,
  input spi_sck,
  input sys_clock,
  input resetn,
  input spi_mosi,
  output reg spi_miso,
  output reg led0,
  output reg led1,
  output pwr_3v,
  output pwr_5v,
  output cart_phi,
  output cart_nWR,
  output cart_nRD,
  output cart_cs1,
  inout cart_cs2,
  input cart_req,
  output tri cart_dir_a,
  output tri cart_dir_ad,
  inout [15:0] cart_ad,
  inout [23:16] cart_a
);

reg        cart_a_oe    = 1'd0;
reg        cart_ad_oe   = 1'd0;
reg        cart_cs2_oe  = 1'd1;
reg        cart_cs1_out = 1'd1;
reg        cart_cs2_out = 1'd1;
reg        cart_nRD_out = 1'd1;
reg        cart_nWR_out = 1'd1;
reg [15:0] cart_ad_out  = 16'd0;
reg  [7:0] cart_a_out   = 8'd0;

reg        ad_incr = 1'd0;

reg        cart_a_oe_shadow  = 1'd0;
reg        cart_ad_oe_shadow = 1'd0;
reg        cart_cs1_shadow   = 1'd1;
reg        cart_cs2_shadow   = 1'd1;
reg        cart_nRD_shadow   = 1'd1;
reg        cart_nWR_shadow   = 1'd1;

reg [1:0] phi_div = 2'd0;

reg pwr_5v_out = 1'd0;
reg pwr_3v_out = 1'd1;
assign pwr_3v = pwr_3v_out;
assign pwr_5v = pwr_5v_out;

wire spi_cs;
assign spi_cs = spi_cs0 & spi_cs1;

reg [4:0] bit_cnt           = 5'd0;
reg [4:0] bit_cnt_threshold = 5'd31;

always @(posedge spi_cs or posedge spi_sck) begin
  if (spi_cs) begin
    bit_cnt <= 5'd0;
  end
  else begin
    if (bit_cnt >= bit_cnt_threshold) begin
      bit_cnt <= 5'd0;
    end
    else begin
      bit_cnt <= bit_cnt + 5'd1;
    end
  end
end

reg [1:0] batch_size = 2'd0;
always @(*) begin
  case (batch_size)
    2'd0: bit_cnt_threshold = 5'd7;
    2'd1: bit_cnt_threshold = 5'd15;
    2'd2: bit_cnt_threshold = 5'd23;
    2'd3: bit_cnt_threshold = 5'd31;
  endcase
end

reg [23:0] buf_miso_cs0;
always @(posedge spi_sck) begin
  if (bit_cnt == 5'd7) begin
    buf_miso_cs0 <= {cart_ad[7:0], cart_ad[15:8], cart_a[23:16]};
  end
  else begin
    buf_miso_cs0 <= {buf_miso_cs0[22:0], 1'd0};
  end
end

reg [7:0] buf_miso_cs1;
always @(posedge spi_cs1 or posedge spi_sck) begin
  if (spi_cs1) begin
    buf_miso_cs1 <= {6'd0, cart_cs2, cart_req};
  end
  else begin
    buf_miso_cs1 <= {buf_miso_cs1[6:0], 1'd0};
  end
end

reg [23:0] buf_miso_cs2;
always @(posedge spi_sck) begin
  if (bit_cnt == 5'd7) begin
    buf_miso_cs2 <= {cart_a[23:16], cart_ad[7:0], cart_ad[15:8]};
  end
  else begin
    buf_miso_cs2 <= {buf_miso_cs2[22:0], 1'd0};
  end
end

reg  [31:0] buf_mosi = 8'd0;
wire [31:0] buf_mosi_current;

assign buf_mosi_current = {buf_mosi[30:0], spi_mosi};
always @(posedge spi_sck) begin
  buf_mosi <= buf_mosi_current;
end

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
        phi_div     <= buf_mosi_current[1:0];
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

// The original design used an oscillator IP not available in this flow.
// Keep phi-div behavior with a divided system clock replacement.
localparam [3:0] OSC_DIV_HALF = 4'd12;
reg osc = 1'b0;
reg [3:0] osc_div = 4'd0;
reg [1:0] cnt_osc = 2'd0;

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

always @(*) begin
  case ({spi_cs1, spi_cs0})
    2'b10: spi_miso   = buf_miso_cs0[23];
    2'b01: spi_miso   = buf_miso_cs1[7];
    2'b00: spi_miso   = buf_miso_cs2[23];
    default: spi_miso = 1'b0;
  endcase
end

assign cart_nWR = cart_nWR_out;
assign cart_nRD = cart_nRD_out;
assign cart_cs1 = cart_cs1_out;
assign cart_cs2 = cart_cs2_oe ? cart_cs2_out : 1'hz;
// Keep bus direction fully controlled by SPI protocol dir bits.
// DIR semantics for level shifter:
// 1 -> MCU drives cart, 0 -> cart drives MCU.
wire cart_bus_active;
assign cart_bus_active = (~cart_cs1_out) | (cart_cs2_oe & ~cart_cs2_out);

assign cart_ad = (cart_bus_active & cart_ad_oe) ? cart_ad_out : 16'hzzzz;
assign cart_a  = (cart_bus_active & cart_a_oe)  ? cart_a_out  : 8'hzz;

// Inactive bus -> high-Z DIR (both sides isolated by level shifter).
assign cart_dir_a  = cart_bus_active ? cart_a_oe  : 1'bz;
assign cart_dir_ad = cart_bus_active ? cart_ad_oe : 1'bz;

always @(posedge spi_cs0 or posedge cart_nRD_out) begin
  if (spi_cs0) begin
    led0 <= 1'd0;
  end
  else begin
    led0 <= 1'd1;
  end
end

always @(posedge spi_cs0 or posedge cart_nWR_out) begin
  if (spi_cs0) begin
    led1 <= 1'd0;
  end
  else begin
    led1 <= 1'd1;
  end
end

endmodule
