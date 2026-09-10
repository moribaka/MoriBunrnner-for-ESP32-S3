`timescale 1ns/1ps
module bacon_cpld_stream_tb;
    reg clk=0, resetn=1, enable=0, abort_request=0;
    always #3.333 clk=~clk;
    reg cs0=1, cs1=1, sck=0, mosi=0;
    wire miso, safe_to_exit;
    wire [7:0] a;
    wire [15:0] ad;
    wire ao, ado, cs, rd, wr;
    reg [7:0] a_in;
    reg [15:0] ad_in;
    bacon_cpld_stream #(.PROGRAM_TIMEOUT_CYCLES(150000)) dut(
        .clk(clk),.resetn(resetn),.enable(enable),.abort_request(abort_request),
        .cs0(cs0),.cs1(cs1),.sck(sck),.mosi(mosi),.miso(miso),.safe_to_exit(safe_to_exit),
        .cart_a_in(a_in),.cart_ad_in(ad_in),.cart_a(a),.cart_ad(ad),
        .a_oe(ao),.ad_oe(ado),.cart_cs(cs),.cart_rd(rd),.cart_wr(wr));

    reg [15:0] memory[0:8191];
    reg [15:0] pending[0:511];
    integer latched_address=0, writes=0, reads=0, program_stage=0;
    integer program_base=0, program_units=0, program_index=0;
    integer programmed_pages=0;
    reg byte_mode=0, raw_mode=0, busy=0, overlap_seen=0, stuck_busy=0;
    time busy_until=0, wr_fell=0, rd_fell=0;
    integer j;

    always @(negedge cs) if (enable && ado)
        latched_address = {a,ad};
    always @(*) begin
        a_in = ad[0] ? memory[ad>>1][15:8] : memory[ad>>1][7:0];
        ad_in = memory[latched_address & 8191];
        if (busy) begin
            ad_in = pending[program_units-1] ^ 16'h0080;
            a_in = pending[program_units-1][7:0] ^ 8'h80;
        end
    end
    always @(posedge rd) if (enable && !cs) begin
        if ($time-rd_fell < 150) $fatal(1,"RD pulse too short");
        latched_address=latched_address+1; reads=reads+1;
    end
    always @(negedge rd) rd_fell=$time;
    always @(negedge wr) wr_fell=$time;
    integer write_address;
    reg [15:0] write_data;
    always @(posedge wr) if (enable && !cs) begin
        if ($time-wr_fell < 50) $fatal(1,"WR pulse truncated");
        writes=writes+1;
        write_address=byte_mode ? ad : latched_address;
        write_data=byte_mode ? {8'd0,a} : ad;
        if (busy) $fatal(1,"write before NOR became ready");
        if (raw_mode) begin
            if (byte_mode) begin
                if (write_address[0]) memory[write_address>>1][15:8]=write_data[7:0];
                else memory[write_address>>1][7:0]=write_data[7:0];
            end else memory[write_address & 8191]=write_data;
        end else begin
            case (program_stage)
                0: begin
                    if (write_address != (byte_mode ? 'haaa : 'h555) || write_data != 'haa)
                        $fatal(1,"bad unlock AA @%x =%x",write_address,write_data);
                    program_stage=1;
                end
                1: begin
                    if (write_address != (byte_mode ? 'h555 : 'h2aa) || write_data != 'h55)
                        $fatal(1,"bad unlock 55");
                    program_stage=2;
                end
                2: begin
                    if (write_data != 'h25) $fatal(1,"missing buffer-load command");
                    program_base=write_address; program_stage=3;
                end
                3: begin
                    if (write_address != program_base || write_data > 511)
                        $fatal(1,"invalid NOR count");
                    program_units=write_data+1; program_index=0; program_stage=4;
                end
                4: begin
                    if (write_address != program_base+program_index)
                        $fatal(1,"payload address mismatch %x expected %x",write_address,program_base+program_index);
                    pending[program_index]=write_data; program_index=program_index+1;
                    if (program_index == program_units) program_stage=5;
                end
                5: begin
                    if (write_address != program_base || write_data != 'h29)
                        $fatal(1,"invalid buffer confirmation");
                    busy=1; busy_until=$time+200000; program_stage=0;
                end
            endcase
        end
        latched_address=latched_address+1;
    end
    always @(negedge clk) begin
        if (busy && dut.valid == 3) overlap_seen=1;
        if (busy && !stuck_busy && $time >= busy_until) begin
            for (j=0;j<program_units;j=j+1) begin
                if (byte_mode) begin
                    if ((program_base+j)%2) memory[(program_base+j)/2][15:8]=pending[j][7:0];
                    else memory[(program_base+j)/2][7:0]=pending[j][7:0];
                end else memory[program_base+j]=pending[j];
            end
            busy=0; programmed_pages=programmed_pages+1;
        end
    end

    reg [7:0] bytes[0:8191];
    reg [7:0] received[0:8191];
    function [7:0] pattern(input integer index);
        pattern=(index*73) ^ (index>>3) ^ 'hb3;
    endfunction
    function [31:0] crc_bytes(input integer n);
        reg [31:0] c;
        integer k,b;
        begin
            c=32'hffffffff;
            for(k=0;k<n;k=k+1) begin
                c=c^bytes[k];
                for(b=0;b<8;b=b+1) c=(c>>1)^((c&1)?32'hedb88320:0);
            end
            crc_bytes=~c;
        end
    endfunction
    task put32(input integer offset,input [31:0] value);
        begin
            bytes[offset]=value; bytes[offset+1]=value>>8;
            bytes[offset+2]=value>>16; bytes[offset+3]=value>>24;
        end
    endtask
    task transfer_byte(input [7:0] tx,output [7:0] rx);
        integer b;
        begin
            for(b=7;b>=0;b=b-1) begin
                mosi=tx[b]; #12.5 sck=1; #2 rx[b]=miso; #10.5 sck=0;
            end
        end
    endtask
    reg [7:0] ignored;
    task send_packet(input integer n);
        integer k;
        begin
            cs0=0; #100; cs1=1; #100;
            for(k=0;k<n;k=k+1) transfer_byte(bytes[k],ignored);
            cs0=1; #200;
        end
    endtask
    reg [7:0] flags;
    reg [31:0] completed, error;
    task status;
        integer k;
        begin
            cs0=0; #100 cs1=0; #100;
            for(k=0;k<16;k=k+1) transfer_byte(8'h0f,received[k]);
            cs0=1; #100 cs1=1; #200;
            if ({received[4],received[5],received[6]} !== 24'hbace01)
                $fatal(1,"status prefix mismatch %02x%02x%02x",received[4],received[5],received[6]);
            flags=received[7];
            completed={received[11],received[10],received[9],received[8]};
            error={received[15],received[14],received[13],received[12]};
        end
    endtask
    task wait_flag(input [7:0] mask);
        integer tries;
        begin : wait_loop
            for(tries=0;tries<2000;tries=tries+1) begin
                status();
                if(error) $fatal(1,"transport error=%0d state=%0d rx_bits=%0d rx_index=%0d",error,dut.state,dut.rx_wire_bits,dut.rx_index);
                if(flags&mask) disable wait_loop;
            end
            $fatal(1,"timed out flags=%02x mask=%02x",flags,mask);
        end
    endtask
    task begin_stream(input [7:0] op,input integer addr,n,page,input corrupt);
        integer k;
        begin
            enable=0; #200; enable=1; #200;
            byte_mode=op>=4; raw_mode=op==2 || op==5;
            status();
            for(k=0;k<20;k=k+1) bytes[k]=0;
            put32(0,32'h31435342); bytes[4]=op;
            bytes[6]=page ? page-1 : 0; bytes[7]=page ? (page-1)>>8 : 0;
            put32(8,addr); put32(12,n); put32(16,crc_bytes(16)^corrupt);
            send_packet(20);
            if(!corrupt) wait_flag(2);
        end
    endtask
    task receive_block(input integer address,n);
        integer k,padded;
        reg [31:0] c;
        begin
            wait_flag(32); padded=(n+3)&~3;
            cs1=0; #100;
            for(k=0;k<padded+8;k=k+1) transfer_byte(0,received[k]);
            cs1=1; #200;
            for(k=0;k<n;k=k+1) begin
                if(received[k+4] !== pattern(address+k))
                    $fatal(1,"read mismatch offset %0d got %02x expected %02x",address+k,received[k+4],pattern(address+k));
                bytes[k]=received[k+4];
            end
            c={received[padded+7],received[padded+6],received[padded+5],received[padded+4]};
            if(c !== crc_bytes(n)) $fatal(1,"read CRC mismatch %08x expected %08x",c,crc_bytes(n));
        end
    endtask
    task payload(input integer offset,n,input corrupt);
        integer k,padded;
        begin
            wait_flag(16); padded=(n+3)&~3;
            for(k=0;k<padded;k=k+1) bytes[k]=k<n?pattern(offset+k):0;
            put32(padded,crc_bytes(n)^corrupt); send_packet(padded+4);
        end
    endtask
    integer i, count_before, offset,n;
    initial begin
        #1 resetn=0; #30 resetn=1;
        for(i=0;i<8192;i=i+1) memory[i]={pattern(2*i+1),pattern(2*i)};
        #100;
        begin_stream(1,0,4096,0,0);
        for(i=0;i<4;i=i+1) begin
            receive_block(i*1020,1020);
            #731;
        end
        receive_block(4080,16);
        wait_flag(4);
        if(completed!=4096) $fatal(1,"read completed count mismatch");

        begin_stream(4,100,1777,0,0);
        receive_block(100,1020); receive_block(1120,757); wait_flag(4);
        if(completed!=1777) $fatal(1,"GB odd read count mismatch");

        begin_stream(5,100,1773,0,0);
        payload(8000,1024,0); payload(9024,749,0); wait_flag(4);
        for(i=0;i<1773;i=i+1)
            if (((memory[(100+i)/2]>>(((100+i)%2)*8))&255) !== pattern(8000+i))
                $fatal(1,"GB raw write mismatch %0d",i);

        for(i=0;i<4096;i=i+1) memory[i]=16'hffff;
        program_stage=0; overlap_seen=0;
        begin_stream(3,128,3072,1024,0);
        offset=0;
        while(offset<3072) begin
            n=1024-((128+offset)%1024); if(n>3072-offset)n=3072-offset;
            payload(offset,n,0); offset=offset+n;
        end
        wait_flag(4);
        if(completed!=3072 || !overlap_seen) $fatal(1,"no receive/program overlap");
        for(i=0;i<1536;i=i+1)
            if(memory[64+i] !== {pattern(i*2+1),pattern(i*2)}) $fatal(1,"NOR data mismatch word %0d",i);

        program_stage=0;
        begin_stream(6,128,517,256,0);
        payload(2000,128,0); payload(2128,256,0); payload(2384,133,0); wait_flag(4);
        for(i=0;i<517;i=i+1)
            if (((memory[(128+i)/2]>>(((128+i)%2)*8))&255) !== pattern(2000+i))
                $fatal(1,"GB NOR data mismatch byte %0d",i);

        count_before=writes;
        begin_stream(3,0,512,512,1); status();
        if(!error || writes!=count_before) $fatal(1,"bad descriptor touched WR");
        begin_stream(3,0,512,512,0); payload(0,512,1); status();
        if(!error || writes!=count_before) $fatal(1,"bad payload touched WR");

        begin_stream(3,0,512,512,0);
        bytes[0]=1; send_packet(1); status();
        if(!error || writes!=count_before) $fatal(1,"short payload touched WR");

        begin_stream(3,0,512,512,0);
        for(i=0;i<512;i=i+1) bytes[i]=pattern(i);
        put32(512,crc_bytes(512)); put32(516,0); send_packet(520); status();
        if(!error || writes!=count_before) $fatal(1,"overlong payload touched WR");

        begin_stream(3,0,512,512,0); stuck_busy=1; payload(0,512,0);
        begin : await_timeout
            for(i=0;i<1000;i=i+1) begin
                status(); if(error) disable await_timeout;
            end
        end
        if(error!=8) $fatal(1,"NOR timeout did not terminate stream");
        busy=0; stuck_busy=0;

        begin_stream(1,0,512,0,0); wait_flag(32);
        cs1=0; #100; repeat(6) transfer_byte(0,ignored); cs1=1; #200; status();
        if(error!=7) $fatal(1,"short read not rejected");

        begin_stream(2,0,512,0,0); payload(0,512,0);
        wait(!wr); #10 abort_request=1;
        wait(safe_to_exit); enable=0; #100; abort_request=0;
        if(!wr || !rd || !cs) $fatal(1,"abort did not release bus");

        $display("CPLD stream passed: GBA/GB reads, raw writes, AMD pages, CRC, overlap, short frames, abort");
        $finish;
    end
    initial begin #20000000; $fatal(1,"test watchdog"); end
endmodule
