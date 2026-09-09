`timescale 1ns/1ps

module bacon_mode_guard_tb;
    reg sys_clock = 1'b0;
    reg resetn = 1'b0;
    reg spi_cs0 = 1'b1;
    reg spi_cs1 = 1'b1;
    reg spi_sck = 1'b0;
    reg spi_mosi = 1'b0;
    wire mcu_mode;
    wire status_miso;

    bacon_mode_guard dut (
        .sys_clock(sys_clock),
        .resetn(resetn),
        .spi_cs0(spi_cs0),
        .spi_cs1(spi_cs1),
        .spi_sck(spi_sck),
        .spi_mosi(spi_mosi),
        .response_ready(1'b1),
        .request_busy(1'b0),
        .protocol_error(1'b0),
        .force_legacy(1'b0),
        .mcu_mode(mcu_mode),
        .status_miso(status_miso)
    );

    always #3.333 sys_clock = ~sys_clock;

    task spi_bit(input value);
        begin
            spi_mosi = value;
            #12.5 spi_sck = 1'b1;
            #12.5 spi_sck = 1'b0;
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

    task read_status(output [31:0] value);
        integer bit_index;
        begin
            value = 32'd0;
            spi_cs0 = 1'b0;
            spi_cs1 = 1'b0;
            #20;
            for (bit_index = 31; bit_index >= 0; bit_index = bit_index - 1) begin
                spi_mosi = 1'b0;
                #12.5 spi_sck = 1'b1;
                value[bit_index] = status_miso;
                #12.5 spi_sck = 1'b0;
            end
            spi_cs0 = 1'b1;
            spi_cs1 = 1'b1;
            #30;
        end
    endtask

    reg [31:0] status;
    initial begin
        #30 resetn = 1'b1;
        #30;
        if (mcu_mode !== 1'b0) $fatal(1, "MCU mode must default off");

        mode_bytes(64'h4d4f5249324d4354);
        if (mcu_mode !== 1'b0) $fatal(1, "near-match entered MCU mode");

        mode_bytes(64'h4d4f5249324d4355);
        if (mcu_mode !== 1'b1) $fatal(1, "entry magic did not enter MCU mode");

        read_status(status);
        if (status !== 32'ha7320103)
            $fatal(1, "status mismatch: %08x", status);

        mode_bytes(64'h4d4f5249324c4547);
        if (mcu_mode !== 1'b0) $fatal(1, "exit magic did not restore legacy mode");

        $display("bacon mode guard test passed");
        $finish;
    end
endmodule
