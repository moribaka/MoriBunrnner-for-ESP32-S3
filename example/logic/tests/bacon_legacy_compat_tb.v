`timescale 1ns/1ps

module bacon_legacy_compat_tb;
    reg sys_clock = 1'b0;
    reg resetn = 1'b0;
    reg spi_cs0 = 1'b1;
    reg spi_cs1 = 1'b1;
    reg spi_sck = 1'b0;
    reg spi_mosi = 1'b0;
    wire spi_clk_wire = spi_sck;
    wire power_3v;
    wire power_5v;
    tri [7:0] cart_a;
    tri [15:0] cart_ad;

    bacon dut (
        .A(cart_a),
        .AD(cart_ad),
        .V3V3_CTRL(power_3v),
        .V5V_CTRL(power_5v),
        .ESP32_SPI2_CS_N(spi_cs0),
        .ESP32_SPI2_MOSI(spi_mosi),
        .ESP32_SPI_CS1(spi_cs1),
        .MCU_SPI_CLK(spi_clk_wire),
        .sys_clock(sys_clock),
        .bus_clock(sys_clock),
        .resetn(resetn),
        .stop(1'b0),
        .mem_ahb_htrans(2'b00),
        .mem_ahb_hready(1'b1),
        .mem_ahb_hwrite(1'b0),
        .mem_ahb_haddr(32'd0),
        .mem_ahb_hsize(3'b010),
        .mem_ahb_hburst(3'b000),
        .mem_ahb_hwdata(32'd0),
        .slave_ahb_hreadyout(1'b1),
        .slave_ahb_hresp(1'b0),
        .slave_ahb_hrdata(32'd0),
        .ext_dma_DMACCLR(4'd0),
        .ext_dma_DMACTC(4'd0)
    );

    always #3.333 sys_clock = ~sys_clock;

    task spi_bit(input value);
        begin
            spi_mosi = value;
            #12.5 spi_sck = 1'b1;
            #12.5 spi_sck = 1'b0;
        end
    endtask

    task legacy_power_byte(input [7:0] value);
        integer bit_index;
        begin
            spi_cs0 = 1'b1;
            spi_cs1 = 1'b0;
            for (bit_index = 7; bit_index >= 0; bit_index = bit_index - 1)
                spi_bit(value[bit_index]);
            spi_cs1 = 1'b1;
            #100;
        end
    endtask

    task mode_bytes(input [63:0] value);
        integer bit_index;
        begin
            spi_cs0 = 1'b1;
            spi_cs1 = 1'b1;
            for (bit_index = 63; bit_index >= 0; bit_index = bit_index - 1)
                spi_bit(value[bit_index]);
            #100;
        end
    endtask

    initial begin
        #30 resetn = 1'b1;
        #50;

        legacy_power_byte(8'h54);
        if (!power_3v || power_5v)
            $fatal(1, "legacy 3V power command failed");

        mode_bytes(64'h4d4f5249324d4355);
        if (!dut.mcu_mode || !power_3v || power_5v)
            $fatal(1, "MCU entry changed legacy power state");

        mode_bytes(64'h4d4f5249324c4547);
        if (dut.mcu_mode || !power_3v || power_5v)
            $fatal(1, "legacy exit did not restore prior state");

        legacy_power_byte(8'h04);
        if (power_3v || power_5v)
            $fatal(1, "legacy command failed after MCU exit");

        $display("bacon legacy compatibility test passed");
        $finish;
    end
endmodule
