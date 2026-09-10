/* Pure hardware cartridge streaming. No MCU/AHB data-path dependency.
 * One descriptor per stream, two 1024-byte buffers, page CRC before WR.
 * Port A of each inferred RAM is shared by the sys-clock producer/consumer;
 * port B supplies the native SPI serializer. Ownership prevents collisions.
 */
module bacon_cpld_stream #(
    parameter PROGRAM_TIMEOUT_CYCLES = 150000000
)(
    input clk, resetn, enable, abort_request,
    input cs0, cs1, sck, mosi,
    output miso, safe_to_exit,
    input [7:0] cart_a_in,
    input [15:0] cart_ad_in,
    output reg [7:0] cart_a,
    output reg [15:0] cart_ad,
    output reg a_oe, ad_oe, cart_cs, cart_rd, cart_wr
);
    localparam IDLE=0, START=1, ADDR_SETUP=2, ADDR_HOLD=3,
        READ_TURN=4, READ_LOW=5, READ_HIGH=6, READ_FINISH=7,
        WRITE_LOAD=8, WRITE_SETUP=9, WRITE_LOW=10, WRITE_HIGH=11,
        CYCLE_END=12, DISPATCH=13, POLL_CHECK=14;
    reg [3:0] state;
    reg [4:0] delay_count;
    reg configured, done;
    reg [8:0] faults;
    wire [7:0] error_code = faults[8] ? 8'd9 : faults[7] ? 8'd8 :
        faults[6] ? 8'd7 : faults[5] ? 8'd6 : faults[4] ? 8'd5 :
        faults[3] ? 8'd4 : faults[2] ? 8'd3 : faults[1] ? 8'd2 :
        faults[0] ? 8'd1 : 8'd0;
    reg byte_bus, reading, programming;
    wire [1:0] stride = byte_bus ? 2'd1 : 2'd2;
    reg [25:0] completed_bytes, receive_remaining;
    reg remaining_nonzero, more_after_chunk, last_in_cycle;
    reg [24:0] address;
    reg [9:0] receive_offset;

    reg [1:0] valid;
    reg producer, consumer;
    reg [10:0] length0, length1;
    wire [10:0] consumer_length = consumer ? length1 : length0;
    reg [10:0] transfer_length;
    reg [10:0] byte_index;
    reg [8:0] ram_read_index;
    reg ram_we0, ram_we1;
    reg [8:0] ram_write_index;
    localparam RAM_ZERO=0, RAM_RX=1, RAM_READ=2, RAM_CRC_LOW=3, RAM_CRC_HIGH=4;
    reg [2:0] ram_source;
    reg ram_byte_pair;
    wire [15:0] ram_data;
    wire [8:0] ram_address0 = ram_we0 ? ram_write_index : ram_read_index;
    wire [8:0] ram_address1 = ram_we1 ? ram_write_index : ram_read_index;
    (* ramstyle = "M9K, no_rw_check" *) reg [15:0] ram0 [0:511];
    (* ramstyle = "M9K, no_rw_check" *) reg [15:0] ram1 [0:511];
    reg [15:0] ram_q0, ram_q1, spi_q0, spi_q1;
    wire [15:0] ram_q = consumer ? ram_q1 : ram_q0;
    always @(posedge clk) begin
        if (ram_we0) ram0[ram_address0] <= ram_data;
        ram_q0 <= ram0[ram_address0];
        if (ram_we1) ram1[ram_address1] <= ram_data;
        ram_q1 <= ram1[ram_address1];
    end

    /* Native RX holds a complete byte while a toggle crosses to sys clock. */
    wire rx_csn = !enable || cs0 || !cs1;
    wire tx_csn = !enable || !cs0 || cs1;
    wire status_csn = !enable || cs0 || cs1;
    reg [2:0] rx_bit;
    reg [7:0] rx_shift, rx_byte;
    reg rx_toggle;
    always @(posedge sck or posedge rx_csn or negedge resetn)
        if (!resetn || rx_csn) rx_bit <= 0;
        else rx_bit <= rx_bit + 3'd1;
    always @(posedge sck or negedge resetn) begin
        if (!resetn) begin rx_shift<=0; rx_byte<=0; rx_toggle<=0; end
        else if (!rx_csn) begin
            rx_shift <= {rx_shift[6:0],mosi};
            if (rx_bit == 7) begin
                rx_byte <= {rx_shift[6:0],mosi};
                rx_toggle <= !rx_toggle;
            end
        end
    end
    reg [2:0] rx_sync, sck_sync;
    reg [3:0] rx_cs_sync, tx_cs_sync;
    reg [2:0] status_cs_sync;
    wire rx_event = rx_sync[2] != rx_sync[1];
    wire sck_rise = sck_sync[2:1] == 2'b01;
    wire rx_end = !rx_cs_sync[3] && rx_cs_sync[2];
    wire tx_end = !tx_cs_sync[3] && tx_cs_sync[2];
    reg [2:0] rx_partial_bits;
    reg [13:0] tx_wire_bits;
    reg [10:0] rx_index;
    wire [13:0] rx_wire_bits = {rx_index,rx_partial_bits};
    reg descriptor_bad;
    reg [31:0] received_crc;
    reg [7:0] rx_low;
    reg [9:0] page_mask;
    reg [10:0] rx_limit, rx_length;
    reg [10:0] rx_padded, rx_packet_bytes;
    reg [1:0] arm_phase;
    reg rx_armed;
    reg rx_end_pending, rx_nonempty, rx_crc_ok, rx_size_ok;
    reg descriptor_invalid;
    reg rx_header_byte, rx_descriptor_crc, rx_payload_byte, rx_payload_last;
    reg rx_crc_byte, rx_overlong;
    // rx_index changes once per SPI byte (30 system clocks). Decode it
    // between bytes, outside the CRC/RAM write-enable paths.
    always @(posedge clk) begin
        rx_header_byte <= rx_index < 16;
        rx_descriptor_crc <= rx_index < 20;
        rx_payload_byte <= rx_index < rx_length;
        rx_payload_last <= rx_index == rx_length - 11'd1;
        rx_crc_byte <= rx_index >= rx_padded && rx_index < rx_packet_bytes;
        rx_overlong <= rx_index >= rx_packet_bytes;
    end

    /* CRC is serialized at 150MHz: 8 cycles per RX byte, 16 per ROM word.
     * This avoids a deep parallel CRC network in the 40MHz SPI domain. */
    reg [31:0] crc;
    reg [4:0] crc_bits;
    reg crc_rx_pending, crc_read_pending, crc_reload_pending;
    wire [31:0] crc_next = (crc >> 1) ^ (crc[0] ? 32'hedb88320 : 32'd0);
    reg [1:0] finish_phase;
    wire [9:0] read_crc_index = ((transfer_length+11'd3)&11'h7fc)>>1;

    /* Response: 4 dummy bytes, padded payload, then CRC32. */
    reg [3:0] tx_bit;
    reg [9:0] tx_word;
    reg [15:0] tx_shift;
    wire [10:0] tx_length = consumer ? length1 : length0;
    reg [9:0] tx_padded_words, tx_packet_words;
    // Buffer metadata settles before TX_READY can be polled and a packet
    // shifted. Keep rounding and the framing comparison in separate stages.
    always @(posedge clk) begin
        tx_padded_words <= ((tx_length + 11'd3) & 11'h7fc) >> 1;
        tx_packet_words <= tx_padded_words + 10'd4;
    end
    wire [8:0] spi_address = tx_word >= 2 ? tx_word - 10'd1 : 9'd0;
    // Match the proven Bacon return path: the master samples the current bit
    // on its rising edge; the slave then presents the next bit. This leaves a
    // full SCK period for the board round trip, rather than half a period.
    always @(posedge sck) begin
        spi_q0 <= ram0[spi_address];
        spi_q1 <= ram1[spi_address];
    end
    wire [15:0] spi_q = consumer ? spi_q1 : spi_q0;
    always @(posedge sck or posedge tx_csn or negedge resetn) begin
        if (!resetn || tx_csn) begin tx_bit<=0; tx_word<=0; tx_shift<=0; end
        else begin
            tx_bit <= tx_bit + 4'd1;
            tx_shift <= {tx_shift[14:0],1'b0};
            if (tx_bit == 15) begin
                tx_word <= tx_word + 10'd1;
                if (!reading || !valid[consumer]) tx_shift <= 0;
                else if (tx_word >= 1 && tx_word < 3 + tx_padded_words)
                    tx_shift <= {spi_q[7:0],spi_q[15:8]};
                else tx_shift <= 0;
            end
        end
    end

    /* Status is frozen before its dummy word ends. No data FIFO clocking. */
    wire [7:0] flags = {1'b0, state != IDLE,
        configured && reading && valid[consumer],
        configured && !reading && rx_armed && !valid[producer],
        error_code != 0, done, configured, enable};
    reg [95:0] status_snapshot;
    always @(posedge clk)
        if (status_cs_sync[2]) status_snapshot <= {32'hbace0100 | flags,
            completed_bytes[7:0],completed_bytes[15:8],completed_bytes[23:16],6'd0,completed_bytes[25:24],
            error_code,24'd0};
    reg [2:0] status_bit;
    reg [3:0] status_byte;
    reg [7:0] status_shift;
    always @(posedge sck or posedge status_csn or negedge resetn) begin
        if (!resetn || status_csn) begin status_bit<=0; status_byte<=0; status_shift<=0; end
        else begin
            status_bit <= status_bit + 3'd1;
            status_shift <= {status_shift[6:0],1'b0};
            if (status_bit == 7) begin
                status_byte <= status_byte + 4'd1;
                case (status_byte)
                    3: status_shift <= 8'hba;
                    4: status_shift <= 8'hce;
                    5: status_shift <= 8'h01;
                    6: status_shift <= status_snapshot[71:64];
                    7: status_shift <= status_snapshot[63:56];
                    8: status_shift <= status_snapshot[55:48];
                    9: status_shift <= status_snapshot[47:40];
                    10: status_shift <= status_snapshot[39:32];
                    11: status_shift <= status_snapshot[31:24];
                    default: status_shift <= 0;
                endcase
            end
        end
    end
    assign miso = !status_csn ? status_shift[7] : tx_shift[15];
    assign safe_to_exit = state == IDLE && cart_wr && cart_rd;

    reg [2:0] command_step;
    reg [23:0] cycle_address;
    reg [8:0] cycle_value; // 512 GBA words encode as count 511.
    reg [15:0] last_value, observed_value;
    reg cycle_read, cycle_payload;
    reg [10:0] cycle_length;
    localparam WATCHDOG_BITS = $clog2(PROGRAM_TIMEOUT_CYCLES + 1);
    reg [WATCHDOG_BITS-1:0] watchdog;
    reg poll_match;
    reg [15:0] payload_word;
    always @(posedge clk) payload_word <= ram_q;
    wire [15:0] payload_value = byte_bus ?
        (byte_index[0] ? {8'd0,payload_word[15:8]} : {8'd0,payload_word[7:0]}) : payload_word;
    wire [15:0] incoming_bus = byte_bus ? {8'd0,cart_a_in} : cart_ad_in;
    reg failed;
    always @(posedge clk or negedge resetn)
        if (!resetn) failed<=0;
        else if (!enable) failed<=0;
        else failed <= (|faults) || abort_request;
    assign ram_data = ram_source == RAM_RX ?
        (ram_byte_pair ? {rx_byte,rx_low} : {8'd0,rx_byte}) :
        ram_source == RAM_READ ?
        (byte_bus ? (ram_byte_pair ? {observed_value[7:0],rx_low} : {8'd0,observed_value[7:0]}) : observed_value) :
        ram_source == RAM_CRC_LOW ? ~crc[15:0] :
        ram_source == RAM_CRC_HIGH ? ~crc[31:16] : 16'd0;
    reg [25:0] range_end;
    reg [24:0] next_address;
    wire [23:0] last_cycle_address = (byte_bus ? next_address[23:0] : next_address[24:1]) - 24'd1;
    wire [10:0] advance_length = reading ? consumer_length : transfer_length;
    wire [25:0] next_completed = completed_bytes + advance_length;
    reg [8:0] program_count;
    // These operands settle well before a page completes or a bus pulse ends.
    // Publish the already computed decision with the matching cursor update.
    always @(posedge clk) begin
        more_after_chunk <= receive_remaining > (reading ? transfer_length : rx_length);
        last_in_cycle <= byte_index + stride >= cycle_length;
        program_count <= byte_bus ? transfer_length-11'd1 : (transfer_length>>1)-11'd1;
    end

    // Input strobes are registered by the bus/parser state machines. RX data
    // remains stable until the next SPI byte; observed_value holds the sampled
    // cartridge word after RD rises. CRC therefore has one shallow arbiter.
    wire [15:0] crc_operand = crc_rx_pending ? {8'd0,rx_byte} : observed_value;
    always @(posedge clk or negedge resetn) begin
        if (!resetn) begin crc<=32'hffffffff; crc_bits<=0; end
        else if (!enable || crc_reload_pending) begin crc<=32'hffffffff; crc_bits<=0; end
        else if (crc_rx_pending || crc_read_pending) begin
            crc<=crc ^ crc_operand; crc_bits<=crc_rx_pending || byte_bus ? 8 : 16;
        end else if (crc_bits != 0) begin
            crc<=crc_next; crc_bits<=crc_bits-5'd1;
        end
    end

    always @(posedge clk or negedge resetn) begin
        if (!resetn) begin
            rx_sync<=0; sck_sync<=0; rx_cs_sync<=15; tx_cs_sync<=15; status_cs_sync<=7;
        end else begin
            rx_sync <= {rx_sync[1:0],rx_toggle};
            sck_sync <= {sck_sync[1:0],sck};
            rx_cs_sync <= {rx_cs_sync[2:0],rx_csn};
            tx_cs_sync <= {tx_cs_sync[2:0],tx_csn};
            status_cs_sync <= {status_cs_sync[1:0],status_csn};
        end
    end

    always @(posedge clk or negedge resetn) begin
        if (!resetn) begin
            configured<=0; done<=0; faults<=0; byte_bus<=0; reading<=0; programming<=0;
            remaining_nonzero<=0;
            completed_bytes<=0; receive_remaining<=0;
            receive_offset<=0; address<=0; valid<=0; producer<=0; consumer<=0;
            length0<=0; length1<=0;
            transfer_length<=0; byte_index<=0; ram_read_index<=0;
            ram_we0<=0; ram_we1<=0; ram_write_index<=0; ram_source<=RAM_ZERO; ram_byte_pair<=0;
            rx_partial_bits<=0; tx_wire_bits<=0; rx_index<=0; descriptor_bad<=0; range_end<=0;
            received_crc<=0; rx_low<=0; finish_phase<=0;
            crc_rx_pending<=0; crc_read_pending<=0; crc_reload_pending<=0;
            page_mask<=0; rx_limit<=0; rx_length<=0; rx_padded<=0; rx_packet_bytes<=0;
            arm_phase<=0; rx_armed<=0; next_address<=0;
            rx_end_pending<=0; rx_nonempty<=0; rx_crc_ok<=0; rx_size_ok<=0; descriptor_invalid<=0;
            state<=IDLE; delay_count<=0; command_step<=0; cycle_address<=0;
            cycle_value<=0; last_value<=0; observed_value<=0;
            cycle_read<=0; cycle_payload<=0; cycle_length<=0; watchdog<=0; poll_match<=0;
            cart_a<=0; cart_ad<=0; a_oe<=0; ad_oe<=0; cart_cs<=1; cart_rd<=1; cart_wr<=1;
        end else begin
            ram_we0<=0; ram_we1<=0;
            crc_rx_pending<=0; crc_read_pending<=0; crc_reload_pending<=0;
            if (!enable) begin
                configured<=0; done<=0; faults<=0; valid<=0; producer<=0; consumer<=0;
                remaining_nonzero<=0;
                rx_partial_bits<=0; tx_wire_bits<=0; rx_index<=0; received_crc<=0;
                state<=IDLE;
                completed_bytes<=0; watchdog<=0; descriptor_bad<=0;
                rx_armed<=0; arm_phase<=0; rx_end_pending<=0;
                cart_cs<=1; cart_rd<=1; cart_wr<=1; a_oe<=0; ad_oe<=0;
            end else begin
                range_end <= {1'b0,address} + receive_remaining;
                descriptor_invalid <= descriptor_bad || receive_remaining == 0 ||
                    (!byte_bus && (address[0] || receive_remaining[0] || range_end > 26'h2000000)) ||
                    (byte_bus && (address[24:16] != 0 || range_end > 65536)) ||
                    (programming && (page_mask == 0 || (byte_bus && page_mask[9:8] != 0) ||
                        (page_mask & (page_mask+10'd1)) != 0));
                rx_end_pending <= rx_end;
                if (!configured || reading || failed) begin rx_armed<=0; arm_phase<=0; end
                else if (!rx_armed && !rx_end_pending && !valid[producer] && remaining_nonzero && rx_index == 0) begin
                    case (arm_phase)
                        0: begin
                            rx_limit <= programming ? {1'b0,(~receive_offset & page_mask)} + 11'd1 : 11'd1024;
                            arm_phase<=1;
                        end
                        1: begin
                            rx_length <= receive_remaining[25:11] != 0 ? rx_limit :
                                (receive_remaining[10:0] < rx_limit ? receive_remaining[10:0] : rx_limit);
                            arm_phase<=2;
                        end
                        2: begin
                            rx_padded <= ({1'b0,rx_length}+11'd3)&11'h7fc;
                            rx_packet_bytes <= ({1'b0,rx_length}+11'd7)&11'h7fc;
                            arm_phase<=3;
                        end
                        3: begin rx_armed<=1; arm_phase<=0; end
                    endcase
                end
                if (configured && !failed && state == IDLE && !remaining_nonzero && valid == 0) done<=1;
                if (!rx_cs_sync[2] && sck_rise)
                    rx_partial_bits <= rx_partial_bits + 3'd1;
                if (!tx_cs_sync[2] && sck_rise && tx_wire_bits != 16383)
                    tx_wire_bits <= tx_wire_bits + 14'd1;

                if (rx_event && !failed) begin
                    rx_index <= rx_index + 11'd1;
                    if (!configured) begin
                        if (rx_header_byte) begin
                            case (rx_index[3:0])
                                0: if (rx_byte != 8'h42) descriptor_bad<=1;
                                1: if (rx_byte != 8'h53) descriptor_bad<=1;
                                2: if (rx_byte != 8'h43) descriptor_bad<=1;
                                3: if (rx_byte != 8'h31) descriptor_bad<=1;
                                4: begin
                                    byte_bus<=rx_byte[2];
                                    reading<=rx_byte == 1 || rx_byte == 4;
                                    programming<=rx_byte == 3 || rx_byte == 6;
                                    if (rx_byte < 1 || rx_byte > 6) descriptor_bad<=1;
                                end
                                5: if (rx_byte != 0) descriptor_bad<=1;
                                6: page_mask[7:0]<=rx_byte;
                                7: begin
                                    page_mask[9:8]<=rx_byte[1:0];
                                    if (rx_byte > 3) descriptor_bad<=1;
                                end
                                8: address[7:0]<=rx_byte;
                                9: address[15:8]<=rx_byte;
                                10: address[23:16]<=rx_byte;
                                11: begin
                                    address[24]<=rx_byte[0];
                                    if (rx_byte > 1) descriptor_bad<=1;
                                end
                                12: receive_remaining[7:0]<=rx_byte;
                                13: receive_remaining[15:8]<=rx_byte;
                                14: receive_remaining[23:16]<=rx_byte;
                                15: begin
                                    receive_remaining[25:24]<=rx_byte[1:0];
                                    if (rx_byte > 2 || (rx_byte == 2 && receive_remaining[23:0] != 0))
                                        descriptor_bad<=1;
                                end
                            endcase
                            if (crc_bits != 0) faults[0]<=1;
                            crc_rx_pending<=1;
                        end else if (rx_descriptor_crc) received_crc<={rx_byte,received_crc[31:8]};
                        else faults[1]<=1;
                    end else if (reading || !rx_armed || valid[producer]) faults[2]<=1;
                    else if (rx_payload_byte) begin
                        if (crc_bits != 0) faults[0]<=1;
                        crc_rx_pending<=1;
                        if (!rx_index[0]) rx_low<=rx_byte;
                        if (rx_index[0] || rx_payload_last) begin
                            ram_write_index<=rx_index[9:1];
                            ram_source<=RAM_RX; ram_byte_pair<=rx_index[0];
                            if (producer) ram_we1<=1; else ram_we0<=1;
                        end
                    end else if (rx_crc_byte)
                        received_crc<={rx_byte,received_crc[31:8]};
                    else if (rx_overlong) faults[1]<=1;
                end
                // Freeze checks at CS end, then publish the buffer on the
                // next cycle. The 32-bit CRC tree does not drive RAM ownership
                // or the wide remaining-byte counter in the same cycle.
                if (rx_end_pending) begin
                    if (rx_nonempty && !failed) begin
                        if (!configured) begin
                            if (!rx_size_ok || !rx_crc_ok)
                                faults[3]<=1;
                            else if (descriptor_invalid) faults[4]<=1;
                            else begin
                                if (!programming) page_mask<=1023;
                                receive_offset<=address[9:0];
                                configured<=1; done<=0;
                                remaining_nonzero<=1;
                            end
                        end else if (!rx_size_ok || !rx_crc_ok) faults[5]<=1;
                        else if (!reading && !valid[producer] && remaining_nonzero) begin
                            if (producer) length1<=rx_length; else length0<=rx_length;
                            valid[producer]<=1; producer<=!producer;
                            receive_remaining<=receive_remaining-rx_length;
                            remaining_nonzero<=more_after_chunk;
                            receive_offset<=receive_offset+rx_length[9:0];
                        end
                    end
                end
                if (rx_end) begin
                    rx_nonempty <= rx_wire_bits != 0;
                    rx_crc_ok <= received_crc == ~crc;
                    rx_size_ok <= configured ? rx_wire_bits == {rx_packet_bytes,3'b000} : rx_wire_bits == 160;
                    rx_partial_bits<=0; rx_index<=0; received_crc<=0;
                    if (rx_wire_bits != 0) rx_armed<=0;
                    if (!configured || !reading) crc_reload_pending<=1;
                end
                if (tx_end) begin
                    if (tx_wire_bits != 0 && !failed) begin
                        if (!configured || !reading || !valid[consumer] ||
                            tx_wire_bits != {tx_packet_words,4'b0000}) faults[6]<=1;
                        else begin
                            valid[consumer]<=0; consumer<=!consumer;
                            completed_bytes<=next_completed;
                        end
                    end
                    tx_wire_bits<=0;
                end

                if (delay_count != 0) delay_count<=delay_count-6'd1;
                if (state != IDLE && !reading) begin
                    watchdog<=watchdog+1'b1;
                    if (watchdog >= PROGRAM_TIMEOUT_CYCLES) faults[7]<=1;
                end
                case (state)
                    IDLE: begin
                        cart_cs<=1; cart_rd<=1; cart_wr<=1; a_oe<=0; ad_oe<=0;
                        watchdog<=0;
                        if (configured && !done && !failed) begin
                            if (reading && remaining_nonzero && !valid[producer]) begin
                                transfer_length<=receive_remaining > 1020 ? 11'd1020 : receive_remaining[10:0];
                                cycle_length<=receive_remaining > 1020 ? 11'd1020 : receive_remaining[10:0];
                                cycle_address<=byte_bus ? {8'd0,address[15:0]} : address[24:1];
                                cycle_read<=1; cycle_payload<=1; byte_index<=0;
                                crc_reload_pending<=1;
                                state<=START;
                            end else if (!reading && valid[consumer]) begin
                                transfer_length<=consumer_length; command_step<=0; poll_match<=0;
                                ram_read_index<=0; state<=DISPATCH;
                            end
                        end
                    end
                    DISPATCH: begin
                        if (failed) state<=IDLE;
                        else begin
                            if (!programming || command_step == 0) next_address<=address+transfer_length;
                            byte_index<=0; cycle_read<=0; cycle_payload<=0;
                            cycle_length<=stride; cycle_address<=byte_bus ? {8'd0,address[15:0]} : address[24:1];
                            if (!programming) begin cycle_payload<=1; cycle_length<=transfer_length; end
                            else case (command_step)
                                0: begin cycle_address<=byte_bus ? 24'haaa : 24'h555; cycle_value<=16'haa; end
                                1: begin cycle_address<=byte_bus ? 24'h555 : 24'h2aa; cycle_value<=16'h55; end
                                2: cycle_value<=16'h25;
                                3: cycle_value<=program_count;
                                4: begin cycle_payload<=1; cycle_length<=transfer_length; ram_read_index<=0; end
                                5: cycle_value<=16'h29;
                                6: begin
                                    cycle_read<=1;
                                    cycle_address<=last_cycle_address;
                                end
                                default: state<=IDLE;
                            endcase
                            state<=START;
                        end
                    end
                    START: begin
                        if (failed) state<=IDLE;
                        else begin
                            if (reading) next_address<=address+transfer_length;
                            cart_cs<=1; cart_rd<=1; cart_wr<=1;
                            cart_ad<=cycle_address[15:0];
                            cart_a<=cycle_address[23:16];
                            ad_oe<=1; a_oe<=!byte_bus || !cycle_read;
                            delay_count<=7; state<=ADDR_SETUP;
                        end
                    end
                    ADDR_SETUP: if (delay_count == 0) begin
                        cart_cs<=0; delay_count<=7; state<=ADDR_HOLD;
                    end
                    ADDR_HOLD: if (delay_count == 0) begin
                        if (cycle_read) begin
                            if (!byte_bus) ad_oe<=0;
                            delay_count<=7; state<=READ_TURN;
                        end else state<=WRITE_LOAD;
                    end
                    READ_TURN: if (delay_count == 0) begin
                        if (failed) state<=CYCLE_END;
                        else begin cart_rd<=0; delay_count<=23; state<=READ_LOW; end
                    end
                    READ_LOW: if (delay_count == 0) begin
                        cart_rd<=1; observed_value<=incoming_bus;
                        if (reading && !failed) begin
                            ram_write_index<=byte_index[9:1];
                            if (byte_bus) begin
                                if (!byte_index[0]) rx_low<=cart_a_in;
                                ram_source<=RAM_READ; ram_byte_pair<=byte_index[0];
                                if (byte_index[0] || last_in_cycle)
                                    if (producer) ram_we1<=1; else ram_we0<=1;
                            end else begin
                                ram_source<=RAM_READ;
                                if (producer) ram_we1<=1; else ram_we0<=1;
                            end
                            if (crc_bits != 0) faults[0]<=1;
                            crc_read_pending<=1;
                        end
                        delay_count<=7; state<=READ_HIGH;
                    end
                    READ_HIGH: if (delay_count == 0 && (!reading || crc_bits == 0)) begin
                        if (failed || !reading || last_in_cycle) state<=CYCLE_END;
                        else begin
                            byte_index<=byte_index+stride;
                            if (byte_bus) cart_ad<=cart_ad+16'd1;
                            delay_count<=7; state<=READ_TURN;
                        end
                    end
                    WRITE_LOAD: begin
                        if (failed) state<=CYCLE_END;
                        else begin
                            if (byte_bus) begin
                                cart_a<=cycle_payload ? payload_value[7:0] : cycle_value[7:0];
                            end else cart_ad<=cycle_payload ? payload_value : cycle_value;
                            if (cycle_payload) last_value<=payload_value;
                            delay_count<=7; state<=WRITE_SETUP;
                        end
                    end
                    WRITE_SETUP: if (delay_count == 0) begin
                        if (failed) state<=CYCLE_END;
                        else begin
                            cart_wr<=0; delay_count<=7; state<=WRITE_LOW;
                            ram_read_index<=(byte_index+stride)>>1;
                        end
                    end
                    WRITE_LOW: if (delay_count == 0) begin
                        cart_wr<=1; delay_count<=7; state<=WRITE_HIGH;
                    end
                    WRITE_HIGH: if (delay_count == 0) begin
                        if (failed || last_in_cycle) state<=CYCLE_END;
                        else begin
                            byte_index<=byte_index+stride;
                            if (byte_bus) cart_ad<=cart_ad+16'd1;
                            state<=WRITE_LOAD;
                        end
                    end
                    CYCLE_END: begin
                        cart_cs<=1; cart_rd<=1; cart_wr<=1;
                        if (failed) state<=IDLE;
                        else if (reading) begin finish_phase<=0; state<=READ_FINISH; end
                        else if (programming && command_step != 6) begin
                            command_step<=command_step+3'd1; state<=DISPATCH;
                        end else if (programming) state<=POLL_CHECK;
                        else begin
                            valid[consumer]<=0; consumer<=!consumer;
                            completed_bytes<=next_completed;
                            address<=next_address;
                            state<=IDLE;
                        end
                    end
                    // READ_HIGH already waits for CRC completion before
                    // CYCLE_END, so finishing never needs another CRC gate.
                    READ_FINISH: begin
                        case (finish_phase)
                            0: begin
                                if (transfer_length[1:0] == 1 || transfer_length[1:0] == 2) begin
                                    ram_write_index<=read_crc_index-10'd1; ram_source<=RAM_ZERO;
                                    if (producer) ram_we1<=1; else ram_we0<=1;
                                end
                                finish_phase<=1;
                            end
                            1: begin
                                ram_write_index<=read_crc_index; ram_source<=RAM_CRC_LOW;
                                if (producer) ram_we1<=1; else ram_we0<=1;
                                finish_phase<=2;
                            end
                            2: begin
                                ram_write_index<=read_crc_index+10'd1; ram_source<=RAM_CRC_HIGH;
                                if (producer) ram_we1<=1; else ram_we0<=1;
                                finish_phase<=3;
                            end
                            3: begin
                                if (producer) length1<=transfer_length; else length0<=transfer_length;
                                valid[producer]<=1; producer<=!producer;
                                receive_remaining<=receive_remaining-transfer_length;
                                remaining_nonzero<=more_after_chunk;
                                address<=next_address; state<=IDLE;
                            end
                        endcase
                    end
                    POLL_CHECK: begin
                        if (observed_value == last_value && poll_match) begin
                            valid[consumer]<=0; consumer<=!consumer;
                            completed_bytes<=next_completed;
                            address<=next_address;
                            state<=IDLE;
                        end else begin
                            poll_match<=observed_value == last_value;
                            state<=DISPATCH;
                        end
                    end
                    default: begin faults[8]<=1; state<=IDLE; end
                endcase
            end
        end
    end
endmodule
