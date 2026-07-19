`timescale 1ns/1ps

module tb_thd_uart_v2;
    reg clk=0;
    reg rst_n=0;
    reg uart_rx=1;
    wire uart_tx;
    wire activity_toggle, valid_frame_toggle, commit_toggle;
    wire [31:0] fundamental_ftw;
    wire signed [13:0] dc_offset;
    wire [69:0] amplitude_flat;
    wire [159:0] phase_flat;
    wire clear_phase, output_enable;
    reg [7:0] frame_payload [0:63];
    integer i;

    thd_uart_v2 #(.CLK_HZ(1_000_000),.BAUD(100_000)) dut (
        .clk(clk),.rst_n(rst_n),.uart_rx(uart_rx),.uart_tx(uart_tx),
        .activity_toggle(activity_toggle),.valid_frame_toggle(valid_frame_toggle),
        .commit_toggle(commit_toggle),.fundamental_ftw(fundamental_ftw),
        .dc_offset(dc_offset),.amplitude_flat(amplitude_flat),
        .phase_flat(phase_flat),.clear_phase(clear_phase),
        .output_enable(output_enable)
    );
    always #500 clk=~clk;

    function [15:0] crc_next;
        input [15:0] crc_in;
        input [7:0] data_in;
        integer k;
        reg [15:0] c;
        begin
            c=crc_in^{data_in,8'h00};
            for (k=0;k<8;k=k+1)
                c=c[15]?((c<<1)^16'h1021):(c<<1);
            crc_next=c;
        end
    endfunction

    task send_byte;
        input [7:0] value;
        integer bit_index;
        begin
            uart_rx=0; #10000;
            for (bit_index=0;bit_index<8;bit_index=bit_index+1) begin
                uart_rx=value[bit_index]; #10000;
            end
            uart_rx=1; #10000;
        end
    endtask

    task send_frame;
        input [7:0] command;
        input [15:0] sequence;
        input [7:0] length;
        input corrupt_crc;
        reg [15:0] c;
        begin
            send_byte(8'hD3); send_byte(8'h91);
            c=16'hFFFF;
            send_byte(8'h02); c=crc_next(c,8'h02);
            send_byte(8'h10); c=crc_next(c,8'h10);
            send_byte(8'h02); c=crc_next(c,8'h02);
            send_byte(command); c=crc_next(c,command);
            send_byte(8'h01); c=crc_next(c,8'h01);
            send_byte(sequence[7:0]); c=crc_next(c,sequence[7:0]);
            send_byte(sequence[15:8]); c=crc_next(c,sequence[15:8]);
            send_byte(length); c=crc_next(c,length);
            send_byte(8'h00); c=crc_next(c,8'h00);
            for (i=0;i<length;i=i+1) begin
                send_byte(frame_payload[i]); c=crc_next(c,frame_payload[i]);
            end
            send_byte(c[7:0] ^ (corrupt_crc?8'h01:8'h00));
            send_byte(c[15:8]); send_byte(8'h91); send_byte(8'hD3);
        end
    endtask

    task set_entry;
        input integer index;
        input [13:0] amplitude;
        input [31:0] phase;
        integer b;
        begin
            b=10+index*6;
            frame_payload[b]=amplitude[7:0];
            frame_payload[b+1]={2'b00,amplitude[13:8]};
            frame_payload[b+2]=phase[7:0];
            frame_payload[b+3]=phase[15:8];
            frame_payload[b+4]=phase[23:16];
            frame_payload[b+5]=phase[31:24];
        end
    endtask

    initial begin
        #5000 rst_n=1;
        frame_payload[0]=8'h34; frame_payload[1]=8'h12;
        frame_payload[2]=8'h29; frame_payload[3]=8'h5C;
        frame_payload[4]=8'h8F; frame_payload[5]=8'h02; // 42,949,673
        frame_payload[6]=0; frame_payload[7]=0;
        frame_payload[8]=5; frame_payload[9]=0;
        set_entry(0,14'd4096,32'd0);
        set_entry(1,14'd1024,32'h1000_0000);
        set_entry(2,14'd512,32'h2000_0000);
        set_entry(3,14'd256,32'h3000_0000);
        set_entry(4,14'd128,32'h4000_0000);
        send_frame(8'h30,16'h0101,8'd40,1'b0);
        #2500000;
        if (commit_toggle!==1'b0) begin $display("FAIL: STAGE changed output"); $fatal; end

        frame_payload[0]=8'h34; frame_payload[1]=8'h12;
        frame_payload[2]=8'h03; frame_payload[3]=8'h00;
        send_frame(8'h31,16'h0102,8'd4,1'b0);
        #200000;
        if (commit_toggle!==1'b1 || fundamental_ftw!==32'd42_949_673 ||
            amplitude_flat[13:0]!==14'd4096 || !clear_phase || !output_enable) begin
            $display("FAIL: valid COMMIT was not decoded"); $fatal;
        end

        #2500000;
        frame_payload[0]=8'h34; frame_payload[1]=8'h12;
        frame_payload[2]=8'h03; frame_payload[3]=8'h00;
        send_frame(8'h31,16'h0103,8'd4,1'b1);
        #200000;
        if (commit_toggle!==1'b1) begin
            $display("FAIL: bad CRC changed output"); $fatal;
        end

        // Revision-1 protocol fixes dc_offset_code at zero.
        frame_payload[0]=8'h78; frame_payload[1]=8'h56;
        frame_payload[2]=8'h29; frame_payload[3]=8'h5C;
        frame_payload[4]=8'h8F; frame_payload[5]=8'h02;
        frame_payload[6]=8'h01; frame_payload[7]=8'h00;
        frame_payload[8]=5; frame_payload[9]=0;
        set_entry(0,14'd4096,32'd0);
        set_entry(1,14'd1024,32'h1000_0000);
        set_entry(2,14'd512,32'h2000_0000);
        set_entry(3,14'd256,32'h3000_0000);
        set_entry(4,14'd128,32'h4000_0000);
        send_frame(8'h30,16'h0201,8'd40,1'b0);
        #2500000;
        frame_payload[0]=8'h78; frame_payload[1]=8'h56;
        frame_payload[2]=8'h03; frame_payload[3]=8'h00;
        send_frame(8'h31,16'h0202,8'd4,1'b0);
        #200000;
        if (commit_toggle!==1'b1) begin
            $display("FAIL: nonzero offset was accepted"); $fatal;
        end

        // The fundamental amplitude must be nonzero.
        frame_payload[0]=8'h79; frame_payload[1]=8'h56;
        frame_payload[2]=8'h29; frame_payload[3]=8'h5C;
        frame_payload[4]=8'h8F; frame_payload[5]=8'h02;
        frame_payload[6]=0; frame_payload[7]=0;
        frame_payload[8]=5; frame_payload[9]=0;
        set_entry(0,14'd0,32'd0);
        set_entry(1,14'd1024,32'h1000_0000);
        set_entry(2,14'd512,32'h2000_0000);
        set_entry(3,14'd256,32'h3000_0000);
        set_entry(4,14'd128,32'h4000_0000);
        send_frame(8'h30,16'h0211,8'd40,1'b0);
        #2500000;
        frame_payload[0]=8'h79; frame_payload[1]=8'h56;
        frame_payload[2]=8'h03; frame_payload[3]=8'h00;
        send_frame(8'h31,16'h0212,8'd4,1'b0);
        #200000;
        if (commit_toggle!==1'b1) begin
            $display("FAIL: zero fundamental amplitude was accepted"); $fatal;
        end

        // The sum of all amplitudes must not exceed 6552.
        frame_payload[0]=8'h7A; frame_payload[1]=8'h56;
        frame_payload[2]=8'h29; frame_payload[3]=8'h5C;
        frame_payload[4]=8'h8F; frame_payload[5]=8'h02;
        frame_payload[6]=0; frame_payload[7]=0;
        frame_payload[8]=5; frame_payload[9]=0;
        set_entry(0,14'd6000,32'd0);
        set_entry(1,14'd553,32'h1000_0000);
        set_entry(2,14'd0,32'h2000_0000);
        set_entry(3,14'd0,32'h3000_0000);
        set_entry(4,14'd0,32'h4000_0000);
        send_frame(8'h30,16'h0221,8'd40,1'b0);
        #2500000;
        frame_payload[0]=8'h7A; frame_payload[1]=8'h56;
        frame_payload[2]=8'h03; frame_payload[3]=8'h00;
        send_frame(8'h31,16'h0222,8'd4,1'b0);
        #200000;
        if (commit_toggle!==1'b1) begin
            $display("FAIL: amplitude sum above 6552 was accepted"); $fatal;
        end

        // Same SRC/CMD/SEQ with a different transaction is a new request,
        // not a cached duplicate response.
        frame_payload[0]=8'h11; frame_payload[1]=8'h11;
        frame_payload[2]=8'h40; frame_payload[3]=8'h42;
        frame_payload[4]=8'h0F; frame_payload[5]=8'h00; // 1,000,000
        frame_payload[6]=0; frame_payload[7]=0;
        frame_payload[8]=5; frame_payload[9]=0;
        set_entry(0,14'd4000,32'd0);
        set_entry(1,14'd1000,32'h1000_0000);
        set_entry(2,14'd500,32'h2000_0000);
        set_entry(3,14'd250,32'h3000_0000);
        set_entry(4,14'd125,32'h4000_0000);
        send_frame(8'h30,16'h0301,8'd40,1'b0);
        #2500000;

        frame_payload[0]=8'h22; frame_payload[1]=8'h22;
        frame_payload[2]=8'h80; frame_payload[3]=8'h84;
        frame_payload[4]=8'h1E; frame_payload[5]=8'h00; // 2,000,000
        send_frame(8'h30,16'h0301,8'd40,1'b0);
        #2500000;
        frame_payload[0]=8'h22; frame_payload[1]=8'h22;
        frame_payload[2]=8'h03; frame_payload[3]=8'h00;
        send_frame(8'h31,16'h0302,8'd4,1'b0);
        #200000;
        if (commit_toggle!==1'b0 || fundamental_ftw!==32'd2_000_000) begin
            $display("FAIL: transaction-aware duplicate handling failed"); $fatal;
        end

        $display("PASS: UART V2 MD validation, CRC, STAGE/COMMIT and transaction handling");
        $finish;
    end
endmodule
