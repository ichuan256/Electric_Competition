`timescale 1ns/1ps

module thd_uart_v2 #(
    parameter integer CLK_HZ = 50_000_000,
    parameter integer BAUD   = 115_200
) (
    input  wire         clk,
    input  wire         rst_n,
    input  wire         uart_rx,
    output wire         uart_tx,
    output reg          activity_toggle,
    output reg          valid_frame_toggle,
    output reg          commit_toggle,
    output reg [31:0]   fundamental_ftw,
    output reg signed [13:0] dc_offset,
    output reg [69:0]   amplitude_flat,
    output reg [159:0]  phase_flat,
    output reg          clear_phase,
    output reg          output_enable
);
    localparam [7:0] VERSION=8'h02, NODE_BLUE=8'h02, NODE_FPGA=8'h10;
    localparam [7:0] CMD_STAGE=8'h30, CMD_COMMIT=8'h31, CMD_ACK=8'h7F;
    localparam [7:0] FLAG_RESPONSE=8'h02, FLAG_ERROR=8'h08;
    localparam [7:0] ST_OK=8'h00, ST_BAD_VERSION=8'h01, ST_BAD_LENGTH=8'h02;
    localparam [7:0] ST_BAD_CRC=8'h03, ST_BAD_TARGET=8'h04;
    localparam [7:0] ST_BAD_COMMAND=8'h05, ST_BAD_FIELD=8'h06;
    localparam [7:0] ST_OUT_OF_RANGE=8'h07, ST_NOT_STAGED=8'h09;
    localparam integer MAX_PAYLOAD=64;
    localparam integer RX_TIMEOUT_CYCLES=CLK_HZ/50;

    localparam [3:0] R_D3=0, R_91=1, R_VER=2, R_DST=3, R_SRC=4,
                     R_CMD=5, R_FLAGS=6, R_SEQL=7, R_SEQH=8,
                     R_LENL=9, R_LENH=10, R_PAYLOAD=11,
                     R_CRCL=12, R_CRCH=13, R_EOF91=14, R_EOFD3=15;

    wire rx_valid;
    wire [7:0] rx_byte;
    wire tx_busy;
    reg tx_start;
    reg [7:0] tx_byte;
    uart_rx_byte #(.CLK_HZ(CLK_HZ),.BAUD(BAUD)) u_rx (
        .clk(clk),.rst_n(rst_n),.rx(uart_rx),.valid(rx_valid),.data(rx_byte));
    uart_tx_byte #(.CLK_HZ(CLK_HZ),.BAUD(BAUD)) u_tx (
        .clk(clk),.rst_n(rst_n),.start(tx_start),.data(tx_byte),
        .tx(uart_tx),.busy(tx_busy));

    function [15:0] crc16_next;
        input [15:0] crc_in;
        input [7:0] data_in;
        integer k;
        reg [15:0] c;
        begin
            c=crc_in^{data_in,8'h00};
            for (k=0;k<8;k=k+1)
                c=c[15] ? ((c<<1)^16'h1021) : (c<<1);
            crc16_next=c;
        end
    endfunction

    function [15:0] response_crc16;
        input [7:0] dst;
        input [7:0] flags;
        input [15:0] seq;
        input [7:0] request_cmd;
        input [7:0] status;
        input [15:0] detail;
        input [15:0] transaction;
        input [15:0] applied_mask;
        reg [15:0] c;
        begin
            c=16'hFFFF;
            c=crc16_next(c,VERSION); c=crc16_next(c,dst);
            c=crc16_next(c,NODE_FPGA); c=crc16_next(c,CMD_ACK);
            c=crc16_next(c,flags); c=crc16_next(c,seq[7:0]);
            c=crc16_next(c,seq[15:8]); c=crc16_next(c,8'd8);
            c=crc16_next(c,8'd0); c=crc16_next(c,request_cmd);
            c=crc16_next(c,status); c=crc16_next(c,detail[7:0]);
            c=crc16_next(c,detail[15:8]); c=crc16_next(c,transaction[7:0]);
            c=crc16_next(c,transaction[15:8]);
            c=crc16_next(c,applied_mask[7:0]);
            c=crc16_next(c,applied_mask[15:8]);
            response_crc16=c;
        end
    endfunction

    reg [3:0] rx_state;
    reg [7:0] req_version, req_dst, req_src, req_cmd, req_flags;
    reg [15:0] req_seq, payload_len, rx_crc, received_crc;
    reg [7:0] len_low;
    reg [6:0] payload_index;
    reg [7:0] payload [0:MAX_PAYLOAD-1];
    reg crc_ok;
    reg [19:0] rx_timeout_count;

    reg staged_valid;
    reg [15:0] staged_transaction;
    reg [31:0] staged_ftw;
    reg signed [13:0] staged_offset;
    reg [69:0] staged_amplitude;
    reg [159:0] staged_phase;

    reg response_pending;
    reg [4:0] response_index;
    reg [1:0] response_tx_state;
    reg [7:0] response_dst, response_flags, response_request_cmd, response_status;
    reg [15:0] response_seq, response_detail, response_transaction, response_mask;
    wire [15:0] response_crc = response_crc16(
        response_dst,response_flags,response_seq,response_request_cmd,
        response_status,response_detail,response_transaction,response_mask);

    reg last_response_valid;
    reg [7:0] last_src, last_cmd, last_flags, last_status;
    reg [15:0] last_seq, last_detail, last_transaction, last_mask;

    integer i;
    integer base;
    integer amplitude_sum;
    integer offset_absolute;
    reg fields_valid;
    reg [15:0] transaction_temp;
    reg [31:0] ftw_temp;
    reg signed [15:0] offset_temp;
    reg [69:0] amplitude_temp;
    reg [159:0] phase_temp;
    reg [13:0] amplitude_entry;
    reg [31:0] phase_entry;
    reg [15:0] commit_flags_temp;

    task prepare_response;
        input [7:0] request_cmd_i;
        input [7:0] status_i;
        input [15:0] detail_i;
        input [15:0] transaction_i;
        input [15:0] mask_i;
        begin
            response_dst<=req_src;
            response_flags<=FLAG_RESPONSE|((status_i==ST_OK)?8'h00:FLAG_ERROR);
            response_seq<=req_seq;
            response_request_cmd<=request_cmd_i;
            response_status<=status_i;
            response_detail<=detail_i;
            response_transaction<=transaction_i;
            response_mask<=mask_i;
            response_pending<=1'b1;
            response_index<=5'd0;
            last_response_valid<=1'b1;
            last_src<=req_src; last_cmd<=req_cmd; last_seq<=req_seq;
            last_flags<=FLAG_RESPONSE|((status_i==ST_OK)?8'h00:FLAG_ERROR);
            last_status<=status_i; last_detail<=detail_i;
            last_transaction<=transaction_i; last_mask<=mask_i;
        end
    endtask

    always @* begin
        case (response_index)
            0:tx_byte=8'hD3;  1:tx_byte=8'h91;  2:tx_byte=VERSION;
            3:tx_byte=response_dst; 4:tx_byte=NODE_FPGA; 5:tx_byte=CMD_ACK;
            6:tx_byte=response_flags; 7:tx_byte=response_seq[7:0];
            8:tx_byte=response_seq[15:8]; 9:tx_byte=8'd8; 10:tx_byte=8'd0;
            11:tx_byte=response_request_cmd; 12:tx_byte=response_status;
            13:tx_byte=response_detail[7:0]; 14:tx_byte=response_detail[15:8];
            15:tx_byte=response_transaction[7:0];
            16:tx_byte=response_transaction[15:8];
            17:tx_byte=response_mask[7:0]; 18:tx_byte=response_mask[15:8];
            19:tx_byte=response_crc[7:0]; 20:tx_byte=response_crc[15:8];
            21:tx_byte=8'h91; default:tx_byte=8'hD3;
        endcase
    end

    // Keep payload RAM free of asynchronous reset so Vivado can infer memory.
    always @(posedge clk) begin
        if (rx_valid && rx_state==R_PAYLOAD)
            payload[payload_index] <= rx_byte;
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rx_state<=R_D3; req_version<=0; req_dst<=0; req_src<=0;
            req_cmd<=0; req_flags<=0; req_seq<=0; payload_len<=0;
            len_low<=0; payload_index<=0; rx_crc<=16'hFFFF;
            received_crc<=0; crc_ok<=0; rx_timeout_count<=0;
            activity_toggle<=0; valid_frame_toggle<=0; commit_toggle<=0;
            fundamental_ftw<=0; dc_offset<=0; amplitude_flat<=0; phase_flat<=0;
            clear_phase<=1; output_enable<=0;
            staged_valid<=0; staged_transaction<=0; staged_ftw<=0;
            staged_offset<=0; staged_amplitude<=0; staged_phase<=0;
            response_pending<=0; response_index<=0; response_tx_state<=0;
            response_dst<=0; response_flags<=0; response_seq<=0;
            response_request_cmd<=0; response_status<=0; response_detail<=0;
            response_transaction<=0; response_mask<=0; tx_start<=0;
            last_response_valid<=0; last_src<=0; last_cmd<=0; last_flags<=0;
            last_status<=0; last_seq<=0; last_detail<=0;
            last_transaction<=0; last_mask<=0;
        end else begin
            tx_start<=1'b0;

            if (rx_valid) begin
                activity_toggle<=~activity_toggle;
                rx_timeout_count<=0;
                case (rx_state)
                    R_D3: if (rx_byte==8'hD3) rx_state<=R_91;
                    R_91: begin
                        if (rx_byte==8'h91) begin rx_state<=R_VER; rx_crc<=16'hFFFF; end
                        else if (rx_byte!=8'hD3) rx_state<=R_D3;
                    end
                    R_VER: begin req_version<=rx_byte; rx_crc<=crc16_next(rx_crc,rx_byte); rx_state<=R_DST; end
                    R_DST: begin req_dst<=rx_byte; rx_crc<=crc16_next(rx_crc,rx_byte); rx_state<=R_SRC; end
                    R_SRC: begin req_src<=rx_byte; rx_crc<=crc16_next(rx_crc,rx_byte); rx_state<=R_CMD; end
                    R_CMD: begin req_cmd<=rx_byte; rx_crc<=crc16_next(rx_crc,rx_byte); rx_state<=R_FLAGS; end
                    R_FLAGS: begin req_flags<=rx_byte; rx_crc<=crc16_next(rx_crc,rx_byte); rx_state<=R_SEQL; end
                    R_SEQL: begin req_seq[7:0]<=rx_byte; rx_crc<=crc16_next(rx_crc,rx_byte); rx_state<=R_SEQH; end
                    R_SEQH: begin req_seq[15:8]<=rx_byte; rx_crc<=crc16_next(rx_crc,rx_byte); rx_state<=R_LENL; end
                    R_LENL: begin len_low<=rx_byte; rx_crc<=crc16_next(rx_crc,rx_byte); rx_state<=R_LENH; end
                    R_LENH: begin
                        payload_len<={rx_byte,len_low};
                        rx_crc<=crc16_next(rx_crc,rx_byte);
                        payload_index<=0;
                        if (rx_byte!=0 || len_low>MAX_PAYLOAD) rx_state<=R_D3;
                        else if (len_low==0) rx_state<=R_CRCL;
                        else rx_state<=R_PAYLOAD;
                    end
                    R_PAYLOAD: begin
                        rx_crc<=crc16_next(rx_crc,rx_byte);
                        if (payload_index+1'b1==payload_len) rx_state<=R_CRCL;
                        else payload_index<=payload_index+1'b1;
                    end
                    R_CRCL: begin received_crc[7:0]<=rx_byte; rx_state<=R_CRCH; end
                    R_CRCH: begin
                        received_crc[15:8]<=rx_byte;
                        crc_ok<=({rx_byte,received_crc[7:0]}==rx_crc);
                        rx_state<=R_EOF91;
                    end
                    R_EOF91: begin
                        if (rx_byte==8'h91) rx_state<=R_EOFD3;
                        else rx_state<=R_D3;
                    end
                    default: begin
                        rx_state<=R_D3;
                        if (rx_byte==8'hD3) begin
                            if ((req_src==last_src)&&(req_cmd==last_cmd)&&
                                (req_seq==last_seq)&&last_response_valid&&crc_ok&&
                                (payload_len>=2)&&
                                ({payload[1],payload[0]}==last_transaction)) begin
                                response_dst<=req_src; response_flags<=last_flags;
                                response_seq<=req_seq; response_request_cmd<=req_cmd;
                                response_status<=last_status; response_detail<=last_detail;
                                response_transaction<=last_transaction;
                                response_mask<=last_mask; response_pending<=1;
                                response_index<=0; valid_frame_toggle<=~valid_frame_toggle;
                            end else if (!crc_ok) begin
                                prepare_response(req_cmd,ST_BAD_CRC,0,0,0);
                            end else if (req_version!=VERSION) begin
                                prepare_response(req_cmd,ST_BAD_VERSION,0,0,0);
                            end else if ((req_dst!=NODE_FPGA)||(req_src!=NODE_BLUE)) begin
                                prepare_response(req_cmd,ST_BAD_TARGET,0,0,0);
                            end else if ((req_flags&8'hEE)!=0) begin
                                prepare_response(req_cmd,ST_BAD_FIELD,0,0,0);
                            end else begin
                                valid_frame_toggle<=~valid_frame_toggle;
                                if (req_cmd==CMD_STAGE) begin
                                    fields_valid=1'b1;
                                    transaction_temp={payload[1],payload[0]};
                                    ftw_temp={payload[5],payload[4],payload[3],payload[2]};
                                    offset_temp=$signed({payload[7],payload[6]});
                                    amplitude_temp=0; phase_temp=0; amplitude_sum=0;
                                    offset_absolute=(offset_temp<0)?-offset_temp:offset_temp;
                                    if (payload_len!=40 || payload[8]!=8'd5 || payload[9]!=8'd0)
                                        fields_valid=1'b0;
                                    // Stage3 protocol revision 1 fixes the offset at zero.
                                    if (ftw_temp==0 || offset_temp!=16'sd0)
                                        fields_valid=1'b0;
                                    for (i=0;i<5;i=i+1) begin
                                        base=10+i*6;
                                        amplitude_entry={payload[base+1][5:0],payload[base]};
                                        phase_entry={payload[base+5],payload[base+4],payload[base+3],payload[base+2]};
                                        if (payload[base+1][7:6]!=0 ||
                                            amplitude_entry>14'd8191)
                                            fields_valid=1'b0;
                                        if (i==0 &&
                                            (phase_entry!=0 || amplitude_entry==0))
                                            fields_valid=1'b0;
                                        amplitude_temp[i*14 +: 14]=amplitude_entry;
                                        phase_temp[i*32 +: 32]=phase_entry;
                                        amplitude_sum=amplitude_sum+amplitude_entry;
                                    end
                                    if ((amplitude_sum+offset_absolute)>6552)
                                        fields_valid=1'b0;
                                    if (fields_valid) begin
                                        staged_transaction<=transaction_temp;
                                        staged_ftw<=ftw_temp;
                                        staged_offset<=offset_temp[13:0];
                                        staged_amplitude<=amplitude_temp;
                                        staged_phase<=phase_temp;
                                        staged_valid<=1'b1;
                                        prepare_response(req_cmd,ST_OK,0,transaction_temp,0);
                                    end else begin
                                        prepare_response(req_cmd,
                                            (payload_len==40)?ST_OUT_OF_RANGE:ST_BAD_LENGTH,
                                            0,transaction_temp,0);
                                    end
                                end else if (req_cmd==CMD_COMMIT) begin
                                    transaction_temp={payload[1],payload[0]};
                                    commit_flags_temp={payload[3],payload[2]};
                                    if (payload_len!=4)
                                        prepare_response(req_cmd,ST_BAD_LENGTH,0,transaction_temp,0);
                                    else if ((commit_flags_temp&16'hFFFC)!=0)
                                        prepare_response(req_cmd,ST_BAD_FIELD,0,transaction_temp,0);
                                    else if (!staged_valid || transaction_temp!=staged_transaction)
                                        prepare_response(req_cmd,ST_NOT_STAGED,0,transaction_temp,0);
                                    else begin
                                        fundamental_ftw<=staged_ftw;
                                        dc_offset<=staged_offset;
                                        amplitude_flat<=staged_amplitude;
                                        phase_flat<=staged_phase;
                                        clear_phase<=commit_flags_temp[0];
                                        output_enable<=commit_flags_temp[1];
                                        commit_toggle<=~commit_toggle;
                                        staged_valid<=1'b0;
                                        prepare_response(req_cmd,ST_OK,0,transaction_temp,16'h0001);
                                    end
                                end else begin
                                    prepare_response(req_cmd,ST_BAD_COMMAND,0,0,0);
                                end
                            end
                        end
                    end
                endcase
            end else if (rx_state!=R_D3 && rx_state!=R_91) begin
                if (rx_timeout_count==RX_TIMEOUT_CYCLES-1) begin
                    rx_state<=R_D3;
                    rx_timeout_count<=0;
                end else rx_timeout_count<=rx_timeout_count+1'b1;
            end

            case (response_tx_state)
                2'd0: if (response_pending && !tx_busy) begin
                    tx_start<=1'b1;
                    response_tx_state<=2'd1;
                end
                2'd1: if (tx_busy) response_tx_state<=2'd2;
                default: if (!tx_busy) begin
                    response_tx_state<=2'd0;
                    if (response_index==5'd22) begin
                        response_pending<=1'b0;
                        response_index<=0;
                    end else response_index<=response_index+1'b1;
                end
            endcase
        end
    end
endmodule

module uart_rx_byte #(
    parameter integer CLK_HZ=50_000_000,
    parameter integer BAUD=115_200
) (
    input wire clk, input wire rst_n, input wire rx,
    output reg valid, output reg [7:0] data
);
    localparam integer DIV=(CLK_HZ+BAUD/2)/BAUD;
    (* ASYNC_REG = "TRUE" *) reg [1:0] rx_sync;
    reg busy;
    reg [15:0] count;
    reg [3:0] bit_number;
    reg [7:0] shift;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rx_sync<=2'b11; busy<=0; count<=0; bit_number<=0;
            shift<=0; valid<=0; data<=0;
        end else begin
            rx_sync<={rx_sync[0],rx};
            valid<=1'b0;
            if (!busy && rx_sync[1] && !rx_sync[0]) begin
                busy<=1'b1; count<=DIV+DIV/2-1; bit_number<=0;
            end else if (busy && count!=0) begin
                count<=count-1'b1;
            end else if (busy) begin
                if (bit_number<8) begin
                    shift[bit_number]<=rx_sync[1];
                    bit_number<=bit_number+1'b1;
                    count<=DIV-1;
                end else begin
                    busy<=1'b0;
                    if (rx_sync[1]) begin data<=shift; valid<=1'b1; end
                end
            end
        end
    end
endmodule

module uart_tx_byte #(
    parameter integer CLK_HZ=50_000_000,
    parameter integer BAUD=115_200
) (
    input wire clk, input wire rst_n, input wire start, input wire [7:0] data,
    output reg tx, output reg busy
);
    localparam integer DIV=(CLK_HZ+BAUD/2)/BAUD;
    reg [15:0] count;
    reg [3:0] bit_number;
    reg [9:0] frame;
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tx<=1'b1; busy<=0; count<=0; bit_number<=0; frame<=0;
        end else if (start && !busy) begin
            frame<={1'b1,data,1'b0};
            tx<=1'b0; busy<=1'b1; count<=DIV-1; bit_number<=0;
        end else if (busy && count!=0) begin
            count<=count-1'b1;
        end else if (busy) begin
            if (bit_number==9) begin
                tx<=1'b1; busy<=1'b0;
            end else begin
                bit_number<=bit_number+1'b1;
                tx<=frame[bit_number+1'b1];
                count<=DIV-1;
            end
        end
    end
endmodule
