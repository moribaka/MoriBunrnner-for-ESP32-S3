`timescale 1ns/1ps

module bacon_mcu_transport_tb;
    reg sys_clock = 1'b0;
    reg resetn = 1'b0;
    reg mcu_mode = 1'b0;
    reg spi_cs0 = 1'b1;
    reg spi_cs1 = 1'b1;
    reg spi_sck = 1'b0;
    reg spi_mosi = 1'b0;
    wire spi_miso;
    wire response_ready;
    wire request_busy;
    wire protocol_error;
    wire force_legacy;
    reg legacy_power_3v = 1'b0;
    reg legacy_power_5v = 1'b1;
    reg [7:0] cart_a_in = 8'h5a;
    reg [15:0] cart_ad_in = 16'ha55a;
    wire [7:0] cart_a_out;
    wire [15:0] cart_ad_out;
    wire cart_a_oe;
    wire cart_ad_oe;
    wire cart_cs1;
    wire cart_cs2;
    wire cart_rd;
    wire cart_wr;
    wire power_3v;
    wire power_5v;
    wire cart_phi;

    reg [1:0] mem_htrans = 2'b00;
    reg mem_hready = 1'b1;
    reg mem_hwrite = 1'b0;
    reg [31:0] mem_haddr = 32'd0;
    reg [31:0] mem_hwdata = 32'd0;
    wire mem_hreadyout;
    wire mem_hresp;
    wire [31:0] mem_hrdata;

    reg slave_hreadyout = 1'b1;
    wire [1:0] slave_htrans;
    wire [2:0] slave_hsize;
    wire [2:0] slave_hburst;
    wire slave_hwrite;
    wire [31:0] slave_haddr;
    wire [31:0] slave_hwdata;
    reg slave_hresp = 1'b0;
    reg [31:0] slave_hrdata;

    reg [31:0] sram [0:4095];
    reg memory_data_phase = 0;
    reg memory_write = 0;
    reg [31:0] memory_address = 0;
    integer memory_wait = 0;
    reg force_legacy_seen = 1'b0;
    integer i;

    bacon_mcu_transport dut (
        .sys_clock(sys_clock), .resetn(resetn), .mcu_mode(mcu_mode),
        .spi_cs0(spi_cs0), .spi_cs1(spi_cs1), .spi_sck(spi_sck),
        .spi_mosi(spi_mosi), .spi_miso(spi_miso),
        .response_ready(response_ready), .request_busy(request_busy),
        .protocol_error(protocol_error), .force_legacy(force_legacy),
        .legacy_power_3v(legacy_power_3v), .legacy_power_5v(legacy_power_5v),
        .cart_a_in(cart_a_in), .cart_ad_in(cart_ad_in),
        .cart_a_out(cart_a_out), .cart_ad_out(cart_ad_out),
        .cart_a_oe(cart_a_oe), .cart_ad_oe(cart_ad_oe),
        .cart_cs1(cart_cs1), .cart_cs2(cart_cs2),
        .cart_rd(cart_rd), .cart_wr(cart_wr),
        .power_3v(power_3v), .power_5v(power_5v), .cart_phi(cart_phi),
        .mem_ahb_htrans(mem_htrans), .mem_ahb_hready(mem_hready),
        .mem_ahb_hwrite(mem_hwrite), .mem_ahb_haddr(mem_haddr),
        .mem_ahb_hwdata(mem_hwdata), .mem_ahb_hreadyout(mem_hreadyout),
        .mem_ahb_hresp(mem_hresp), .mem_ahb_hrdata(mem_hrdata),
        .slave_ahb_hreadyout(slave_hreadyout),
        .slave_ahb_htrans(slave_htrans), .slave_ahb_hsize(slave_hsize),
        .slave_ahb_hburst(slave_hburst), .slave_ahb_hwrite(slave_hwrite),
        .slave_ahb_haddr(slave_haddr), .slave_ahb_hwdata(slave_hwdata),
        .slave_ahb_hresp(slave_hresp), .slave_ahb_hrdata(slave_hrdata)
    );

    always #3.333 sys_clock = ~sys_clock;

    always @(*) begin
        if (memory_address >= 32'h2000_0000 && memory_address < 32'h2000_4000)
            slave_hrdata = sram[(memory_address - 32'h2000_0000) >> 2];
        else
            slave_hrdata = 32'hdead_beef;
    end

    always @(posedge sys_clock) begin
        if (force_legacy) force_legacy_seen <= 1'b1;
        if (memory_wait != 0) begin
            memory_wait <= memory_wait - 1;
            if (memory_wait == 1) slave_hreadyout <= 1;
        end
        if (slave_hreadyout) begin
            if (memory_data_phase && memory_write)
                sram[(memory_address - 32'h2000_0000) >> 2] <= slave_hwdata;
            memory_data_phase <= slave_htrans[1];
            if (slave_htrans[1]) begin
                memory_address <= slave_haddr;
                memory_write <= slave_hwrite;
                memory_wait <= 3;
                slave_hreadyout <= 0;
            end
        end
    end

    task ahb_write(input [31:0] address, input [31:0] value);
        begin
            @(negedge sys_clock);
            mem_haddr = address;
            mem_hwrite = 1'b1;
            mem_htrans = 2'b10;
            @(negedge sys_clock);
            mem_htrans = 2'b00;
            mem_hwdata = value;
            while (!mem_hreadyout) @(negedge sys_clock);
            @(negedge sys_clock);
            mem_hwrite = 1'b0;
            mem_hwdata = 32'd0;
        end
    endtask

    task ahb_read(input [31:0] address, output [31:0] value);
        begin
            @(negedge sys_clock);
            mem_haddr = address;
            mem_hwrite = 1'b0;
            mem_htrans = 2'b10;
            @(negedge sys_clock);
            mem_htrans = 2'b00;
            while (!mem_hreadyout) @(negedge sys_clock);
            value = mem_hrdata;
        end
    endtask

    task spi_send_byte(input [7:0] value);
        integer bit_index;
        begin
            for (bit_index = 7; bit_index >= 0; bit_index = bit_index - 1) begin
                spi_mosi = value[bit_index];
                #12.5 spi_sck = 1'b1;
                #12.5 spi_sck = 1'b0;
            end
        end
    endtask

    task spi_recv_byte(output [7:0] value);
        integer bit_index;
        begin
            value = 8'd0;
            for (bit_index = 7; bit_index >= 0; bit_index = bit_index - 1) begin
                spi_mosi = 1'b0;
                #12.5 spi_sck = 1'b1;
                #2;
                value[bit_index] = spi_miso;
                #10.5 spi_sck = 1'b0;
            end
        end
    endtask

    reg [7:0] rx_bytes [0:11];
    reg [7:0] retry_bytes [0:11];
    reg [31:0] ahb_value;
    reg [7:0] bulk_byte;
    reg [31:0] expected_word;
    initial begin
        #1 resetn = 1;
        #1 resetn = 0;
        for (i = 0; i < 64; i = i + 1) sram[i] = 32'd0;
        #30 resetn = 1'b1;
        #50;
        if (power_3v || !power_5v)
            $fatal(1, "legacy 5V state was not mirrored before mode switch");
        mcu_mode = 1'b1;
        #50;
        if (power_3v || !power_5v)
            $fatal(1, "mode switch changed cartridge power rail");
        ahb_read(32'h6002_0018, ahb_value);
        if (ahb_value !== 32'h3155_434d)
            $fatal(1, "AHB identity read mismatch: %08x", ahb_value);
        ahb_read(32'h6003_0018, ahb_value);
        if (ahb_value !== 32'hdead_beef)
            $fatal(1, "AHB address window mismatch: %08x", ahb_value);

        ahb_write(32'h6002_0004, 32'h2000_0000);
        ahb_write(32'h6002_0008, 32'd3);
        ahb_write(32'h6002_0000, 32'h0000_0001);

        // ESP32 writes the two GPIO CS pins separately. Clockless transient
        // selections must not become empty RX requests or interrupted TX.
        spi_cs0 = 0;
        #200 spi_cs1 = 0;
        #30;
        repeat (4) spi_send_byte(8'h07);
        spi_cs0 = 1;
        #200 spi_cs1 = 1;
        #200;
        if (dut.rx_done || !dut.rx_armed || protocol_error)
            $fatal(1, "CS skew created an empty request");

        spi_cs0 = 1'b0;
        spi_cs1 = 1'b1;
        #30;
        spi_send_byte(8'h11);
        spi_send_byte(8'h22);
        spi_send_byte(8'h33);
        spi_send_byte(8'h44);
        spi_send_byte(8'h55);
        spi_send_byte(8'h66);
        spi_send_byte(8'h77);
        spi_send_byte(8'h88);
        spi_send_byte(8'h99);
        spi_send_byte(8'haa);
        spi_send_byte(8'hbb);
        spi_send_byte(8'hcc);
        spi_cs0 = 1'b1;
        #200;

        if (!dut.rx_done || dut.rx_word_count != 13'd3)
            $fatal(1, "request completion mismatch words=%0d", dut.rx_word_count);
        if (sram[0] !== 32'h44332211 || sram[1] !== 32'h88776655 ||
            sram[2] !== 32'hccbbaa99)
            $fatal(1, "request byte order mismatch %08x %08x %08x",
                   sram[0], sram[1], sram[2]);

        sram[8] = 32'h00000000;
        sram[9] = 32'h04030201;
        sram[10] = 32'h08070605;
        ahb_write(32'h6002_0010, 32'h2000_0020);
        ahb_write(32'h6002_0014, 32'd3);
        ahb_write(32'h6002_0000, 32'h0000_0004);
        wait(response_ready);
        #100;
        // Polls between publication and reading must not consume TX words.
        spi_cs0 = 0;
        #200 spi_cs1 = 0;
        #30;
        repeat (8) spi_send_byte(8'h07);
        spi_cs0 = 1;
        #200 spi_cs1 = 1;
        #100;

        spi_cs0 = 1'b1;
        spi_cs1 = 1'b0;
        #30;
        for (i = 0; i < 12; i = i + 1) spi_recv_byte(rx_bytes[i]);
        spi_cs1 = 1'b1;
        #200;

        if (rx_bytes[4] !== 8'h01 || rx_bytes[5] !== 8'h02 ||
            rx_bytes[6] !== 8'h03 || rx_bytes[7] !== 8'h04 ||
            rx_bytes[8] !== 8'h05 || rx_bytes[9] !== 8'h06 ||
            rx_bytes[10] !== 8'h07 || rx_bytes[11] !== 8'h08)
            $fatal(1, "response byte order mismatch: %02x %02x %02x %02x | %02x %02x %02x %02x | %02x %02x %02x %02x",
                   rx_bytes[0], rx_bytes[1], rx_bytes[2], rx_bytes[3],
                   rx_bytes[4], rx_bytes[5], rx_bytes[6], rx_bytes[7],
                   rx_bytes[8], rx_bytes[9], rx_bytes[10], rx_bytes[11]);
        if (response_ready || !dut.tx_consumed)
            $fatal(1, "response consumption mismatch");

        sram[8] = 32'h00000000;
        sram[9] = 32'h14131211;
        sram[10] = 32'h18171615;
        ahb_write(32'h6002_0010, 32'h2000_0020);
        ahb_write(32'h6002_0014, 32'd3);
        ahb_write(32'h6002_0000, 32'h0000_0004);
        #100;

        spi_cs0 = 1'b1;
        spi_cs1 = 1'b0;
        #30;
        for (i = 0; i < 6; i = i + 1) spi_recv_byte(retry_bytes[i]);
        spi_cs1 = 1'b1;
        #200;
        if (!response_ready || dut.tx_consumed)
            $fatal(1, "partial response must remain available");

        spi_cs1 = 1'b0;
        #30;
        for (i = 0; i < 12; i = i + 1) spi_recv_byte(retry_bytes[i]);
        spi_cs1 = 1'b1;
        #200;
        if (retry_bytes[4] !== 8'h11 || retry_bytes[5] !== 8'h12 ||
            retry_bytes[6] !== 8'h13 || retry_bytes[7] !== 8'h14 ||
            retry_bytes[8] !== 8'h15 || retry_bytes[9] !== 8'h16 ||
            retry_bytes[10] !== 8'h17 || retry_bytes[11] !== 8'h18)
            $fatal(1, "partial response retry did not restart from base");
        if (response_ready || !dut.tx_consumed)
            $fatal(1, "retried response consumption mismatch");

        // 8192 bytes repeatedly wrap both FIFO slots. Alternate last bits
        // and sample after the edge so an early word replacement is visible.
        for (i = 0; i < 2048; i = i + 1)
            sram[1024+i] = (32'h9e3779b9 * i) ^ 32'h8147ab33;
        ahb_write(32'h6002_0010, 32'h2000_1000);
        ahb_write(32'h6002_0014, 32'd2048);
        ahb_write(32'h6002_0000, 32'h0000_0004);
        wait(response_ready);
        spi_cs1 = 0;
        #30;
        for (i = 0; i < 8192; i = i + 1) begin
            spi_recv_byte(bulk_byte);
            expected_word = sram[1024+i/4] >> (8*(i%4));
            if (bulk_byte !== expected_word[7:0])
                $fatal(1, "bulk response mismatch at %0d: %02x != %02x", i, bulk_byte, expected_word[7:0]);
        end
        spi_cs1 = 1;
        #200;
        if (response_ready || !dut.tx_consumed)
            $fatal(1, "bulk response did not complete");

        ahb_write(32'h6002_0000, 32'h0000_0002);
        ahb_write(32'h6002_0000, 32'h0000_0080);
        ahb_write(32'h6002_0004, 32'h2000_0000);
        ahb_write(32'h6002_0008, 32'd3);
        ahb_write(32'h6002_0000, 32'h0000_0001);
        spi_cs0 = 1'b0;
        spi_cs1 = 1'b1;
        #30;
        spi_send_byte(8'hde);
        spi_mosi = 1'b1;
        #12.5 spi_sck = 1'b1;
        #12.5 spi_sck = 1'b0;
        spi_cs0 = 1'b1;
        #200;
        if (!protocol_error)
            $fatal(1, "partial request word was not rejected");

        ahb_write(32'h6002_0100, 32'h005a_a55a);
        ahb_write(32'h6002_0104, 32'h0000_00f1);
        if (cart_a_out !== 8'h5a || cart_ad_out !== 16'ha55a ||
            !cart_a_oe || !cart_ad_oe || !power_3v || !power_5v ||
            cart_rd || !cart_wr)
            $fatal(1, "cart control register mismatch");

        ahb_write(32'h6002_001c, 32'h3147_454c);
        if (!force_legacy_seen) $fatal(1, "AHB legacy exit pulse missing");

        $display("bacon MCU transport test passed");
        $finish;
    end
    initial begin
        #3000000;
        $fatal(1, "transport test timed out");
    end
endmodule
