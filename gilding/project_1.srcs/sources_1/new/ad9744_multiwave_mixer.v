// DAC1四槽位多波形叠加器。TYPE: 0关闭、1方波、2三角波、3锯齿波。
// 三角波和锯齿波使用phase[31:20]，即4096个相位位置。
// 四路乘法后使用19位累加，并在输出端饱和到14位补码范围。
module ad9744_multiwave_mixer (
    input  wire         clk,
    input  wire         rst_n,
    input  wire         restart,
    input  wire [7:0]   type_flat,
    input  wire [127:0] ftw_flat,
    input  wire [127:0] phase_flat,
    input  wire [55:0]  amp_flat,
    input  wire [63:0]  duty_flat,
    input  wire signed [13:0] dc_offset,
    output reg [13:0] dac_data,
    output reg        clipping,
    output reg [31:0] clip_count
);
    reg [31:0] phase_acc [0:3];
    reg signed [13:0] raw [0:3];
    reg signed [13:0] raw_pipe [0:3];
    reg signed [27:0] product [0:3];
    reg signed [15:0] scaled [0:3];
    reg signed [17:0] sum01, sum23;
    reg signed [18:0] mix_sum;
    integer i;

    wire [1:0] type0 = type_flat[1:0];
    wire [1:0] type1 = type_flat[3:2];
    wire [1:0] type2 = type_flat[5:4];
    wire [1:0] type3 = type_flat[7:6];

    function signed [13:0] wave4096;
        input [1:0] type_sel;
        input [31:0] phase;
        input [15:0] duty;
        reg [11:0] addr;
        reg [9:0] frac;
        reg signed [14:0] value;
        begin
            addr = phase[31:20];
            frac = addr[9:0];
            case (type_sel)
                // 零相位定义为第一个上升沿：从负值跳到正值并保持高电平。
                2'd1: value = (phase[31:16] < duty) ? 15'sd8191 : -15'sd8192;
                // 零相位从0开始沿正斜率上升。
                2'd2: begin
                    case (addr[11:10])
                        2'b00: value =  $signed({1'b0,frac,3'b000});
                        2'b01: value =  15'sd8191-$signed({1'b0,frac,3'b000});
                        2'b10: value = -$signed({1'b0,frac,3'b000});
                        default:value = -15'sd8192+$signed({1'b0,frac,3'b000});
                    endcase
                end
                // 零相位位于最低点，随后逐点向上爬升。
                2'd3: value = -15'sd8192 + $signed({1'b0,addr,2'b00});
                default: value = 15'sd0;
            endcase
            wave4096 = value[13:0];
        end
    endfunction

    always @* begin
        raw[0] = wave4096(type0, phase_acc[0] + phase_flat[31:0],   duty_flat[15:0]);
        raw[1] = wave4096(type1, phase_acc[1] + phase_flat[63:32],  duty_flat[31:16]);
        raw[2] = wave4096(type2, phase_acc[2] + phase_flat[95:64],  duty_flat[47:32]);
        raw[3] = wave4096(type3, phase_acc[3] + phase_flat[127:96], duty_flat[63:48]);
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            for (i=0; i<4; i=i+1) begin
                phase_acc[i] <= 32'd0;
                raw_pipe[i] <= 14'sd0;
                product[i] <= 28'sd0;
                scaled[i] <= 16'sd0;
            end
            sum01 <= 18'sd0;
            sum23 <= 18'sd0;
            mix_sum <= 19'sd0;
            dac_data <= 14'd0;
            clipping <= 1'b0;
            clip_count <= 32'd0;
        end else if (restart) begin
            // 原子提交配置时清空全部相位和流水级，四槽位同步从零相位启动。
            for (i=0; i<4; i=i+1) begin
                phase_acc[i] <= 32'd0;
                raw_pipe[i] <= 14'sd0;
                product[i] <= 28'sd0;
                scaled[i] <= 16'sd0;
            end
            sum01 <= 18'sd0;
            sum23 <= 18'sd0;
            mix_sum <= 19'sd0;
            dac_data <= 14'd0;
            clipping <= 1'b0;
        end else begin
            phase_acc[0] <= phase_acc[0] + ftw_flat[31:0];
            phase_acc[1] <= phase_acc[1] + ftw_flat[63:32];
            phase_acc[2] <= phase_acc[2] + ftw_flat[95:64];
            phase_acc[3] <= phase_acc[3] + ftw_flat[127:96];

            // 先寄存波形原始码，再进入DSP幅度乘法，切断波形组合逻辑与乘法器
            // 的长路径；四槽完全同级，因此不会引入槽位间相移。
            raw_pipe[0] <= raw[0]; raw_pipe[1] <= raw[1];
            raw_pipe[2] <= raw[2]; raw_pipe[3] <= raw[3];
            product[0] <= raw_pipe[0] * $signed({1'b0,amp_flat[13:0]});
            product[1] <= raw_pipe[1] * $signed({1'b0,amp_flat[27:14]});
            product[2] <= raw_pipe[2] * $signed({1'b0,amp_flat[41:28]});
            product[3] <= raw_pipe[3] * $signed({1'b0,amp_flat[55:42]});
            for (i=0; i<4; i=i+1)
                scaled[i] <= product[i] >>> 13;

            sum01 <= scaled[0] + scaled[1];
            sum23 <= scaled[2] + scaled[3];
            mix_sum <= sum01 + sum23 + dc_offset;
            if (mix_sum > 19'sd8191) begin
                dac_data <= 14'h1FFF;
                clipping <= 1'b1;
                if (clip_count!=32'hFFFFFFFF) clip_count<=clip_count+1'b1;
            end else if (mix_sum < -19'sd8192) begin
                dac_data <= 14'h2000;
                clipping <= 1'b1;
                if (clip_count!=32'hFFFFFFFF) clip_count<=clip_count+1'b1;
            end else begin
                dac_data <= mix_sum[13:0];
                clipping <= 1'b0;
            end
        end
    end
endmodule

// Blue -> FPGA V2协议解析器。
// 帧：D3 91 VER DST SRC CMD FLAGS SEQ_L/H LEN_L/H PAYLOAD CRC_L/H 91 D3。
// 支持FPGA_CHANNEL_STAGE(20)和FPGA_COMMIT(21)，响应为CMD=7F的8字节ACK/NACK。
// 正弦由Black板外部DDS产生，因此V2波形1在FPGA混合器中映射为OFF；
// V2波形2/3/4分别映射为方波/三角波/锯齿波。
module uart_v2_dual_config #(parameter CLK_HZ=50_000_000, BAUD=115_200) (
    input  wire clk,
    input  wire rst_n,
    input  wire uart_rx,
    output wire uart_tx,
    output reg  activity,
    output reg  frame_ok_toggle,
    output reg  cfg0_toggle,
    output reg  cfg1_toggle,
    output reg  commit_toggle,
    output reg  [1:0] commit_mask,
    output reg  [3:0] commit_flags,
    input  wire commit_applied_toggle,
    output reg  [7:0]   type0_flat,
    output reg  [127:0] ftw0_flat,
    output reg  [127:0] phase0_flat,
    output reg  [55:0]  amp0_flat,
    output reg  [63:0]  duty0_flat,
    output reg  signed [13:0] offset0,
    output reg  cache0_mode,
    output reg  [12:0] cache0_points,
    output reg  enable0,
    output reg  [7:0]   type1_flat,
    output reg  [127:0] ftw1_flat,
    output reg  [127:0] phase1_flat,
    output reg  [55:0]  amp1_flat,
    output reg  [63:0]  duty1_flat,
    output reg  signed [13:0] offset1,
    output reg  cache1_mode,
    output reg  [12:0] cache1_points,
    output reg  enable1
);
    localparam [7:0] VERSION=8'h02, NODE_BLUE=8'h02, NODE_FPGA=8'h10;
    localparam [7:0] CMD_STAGE=8'h20, CMD_COMMIT=8'h21, CMD_ACK=8'h7F;
    localparam [7:0] FLAG_RESPONSE=8'h02, FLAG_ERROR=8'h08;
    localparam [7:0] ST_OK=8'h00, ST_BAD_VERSION=8'h01, ST_BAD_LENGTH=8'h02;
    localparam [7:0] ST_BAD_CRC=8'h03, ST_BAD_TARGET=8'h04, ST_BAD_COMMAND=8'h05;
    localparam [7:0] ST_BAD_FIELD=8'h06, ST_OUT_OF_RANGE=8'h07;
    localparam [7:0] ST_NOT_STAGED=8'h09;
    localparam integer MAX_PAYLOAD=80;
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
    uart_rx_byte #(.CLK_HZ(CLK_HZ),.BAUD(BAUD)) u_v2_rx(
        clk,rst_n,uart_rx,rx_valid,rx_byte);
    uart_tx_byte #(.CLK_HZ(CLK_HZ),.BAUD(BAUD)) u_v2_tx(
        clk,rst_n,tx_start,tx_byte,uart_tx,tx_busy);

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

    function [1:0] v2_wave_to_internal;
        input [7:0] wave;
        begin
            case (wave)
                8'd2: v2_wave_to_internal=2'd1;
                8'd3: v2_wave_to_internal=2'd2;
                8'd4: v2_wave_to_internal=2'd3;
                default: v2_wave_to_internal=2'd0;
            endcase
        end
    endfunction

    function [15:0] response_crc16;
        input [7:0] dst;
        input [7:0] flags;
        input [15:0] seq;
        input [7:0] request_cmd;
        input [7:0] status;
        input [15:0] detail;
        input [15:0] transaction_id;
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
            c=crc16_next(c,detail[15:8]); c=crc16_next(c,transaction_id[7:0]);
            c=crc16_next(c,transaction_id[15:8]);
            c=crc16_next(c,applied_mask[7:0]);
            c=crc16_next(c,applied_mask[15:8]);
            response_crc16=c;
        end
    endfunction

    reg [3:0] rx_state;
    reg [7:0] req_version,req_dst,req_src,req_cmd,req_flags;
    reg [15:0] req_seq,payload_len;
    reg [7:0] len_low;
    reg [6:0] payload_index;
    reg [7:0] payload [0:MAX_PAYLOAD-1];
    reg [15:0] rx_crc,rx_crc_received;
    reg crc_ok;
    reg [19:0] rx_timeout_count;

    reg response_pending;
    reg [4:0] response_index;
    reg [1:0] response_tx_state;
    reg [7:0] response_dst,response_flags,response_request_cmd,response_status;
    reg [15:0] response_seq,response_detail,response_transaction,response_mask;
    wire [15:0] response_crc = response_crc16(
        response_dst,response_flags,response_seq,response_request_cmd,
        response_status,response_detail,response_transaction,response_mask);

    reg last_response_valid;
    reg [7:0] last_src,last_cmd,last_status,last_flags;
    reg [15:0] last_seq,last_detail,last_transaction,last_mask;

    // COMMIT通过共享令牌送入100 MHz采样域；收到采样域回执后才返回ACK。
    reg commit_response_waiting;
    reg commit_applied_seen;
    reg [7:0] commit_response_dst;
    reg [15:0] commit_response_seq;
    reg [15:0] commit_response_transaction;
    reg [15:0] commit_response_mask;

    reg staged0_valid,staged1_valid;
    reg [15:0] staged0_transaction,staged1_transaction;
    reg [7:0] shadow0_type,shadow1_type;
    reg [127:0] shadow0_ftw,shadow0_phase,shadow1_ftw,shadow1_phase;
    reg [55:0] shadow0_amp,shadow1_amp;
    reg [63:0] shadow0_duty,shadow1_duty;
    reg signed [13:0] shadow0_offset,shadow1_offset;
    reg shadow0_cache,shadow1_cache,shadow0_enable,shadow1_enable;
    reg [12:0] shadow0_points,shadow1_points;

    integer i;
    integer base;
    reg valid_temp;
    reg [3:0] slot_seen_temp;
    reg [7:0] count_temp,channel_temp,mode_temp,channel_flags_temp;
    reg [7:0] slot_temp,wave_temp;
    reg [15:0] transaction_temp,period_temp,component_flags_temp;
    reg signed [15:0] offset_temp16;
    reg [7:0] type_temp;
    reg [127:0] ftw_temp,phase_temp;
    reg [55:0] amp_temp;
    reg [63:0] duty_temp;

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
            response_index<=0;
            last_response_valid<=1'b1;
            last_src<=req_src; last_cmd<=req_cmd; last_seq<=req_seq;
            last_status<=status_i;
            last_flags<=FLAG_RESPONSE|((status_i==ST_OK)?8'h00:FLAG_ERROR);
            last_detail<=detail_i; last_transaction<=transaction_i; last_mask<=mask_i;
        end
    endtask

    always @* begin
        case (response_index)
            0:tx_byte=8'hD3; 1:tx_byte=8'h91; 2:tx_byte=VERSION;
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

    // 帧载荷作为无复位小型存储区写入。将它与带异步复位的解析状态机分开，
    // 避免 Vivado 把动态索引写入误判为同优先级置位/复位寄存器。
    always @(posedge clk) begin
        if (rx_valid && rx_state==R_PAYLOAD)
            payload[payload_index] <= rx_byte;
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            rx_state<=R_D3; req_version<=0; req_dst<=0; req_src<=0;
            req_cmd<=0; req_flags<=0; req_seq<=0; payload_len<=0;
            len_low<=0; payload_index<=0; rx_crc<=16'hFFFF;
            rx_crc_received<=0; crc_ok<=0; rx_timeout_count<=0;
            activity<=0; frame_ok_toggle<=0;
            response_pending<=0; response_index<=0; response_tx_state<=0;
            tx_start<=0; last_response_valid<=0; last_src<=0; last_cmd<=0;
            last_seq<=0; last_status<=0; last_flags<=0; last_detail<=0;
            last_transaction<=0; last_mask<=0;
            cfg0_toggle<=0; cfg1_toggle<=0;
            commit_toggle<=0; commit_mask<=0; commit_flags<=0;
            commit_response_waiting<=0; commit_applied_seen<=0;
            commit_response_dst<=0; commit_response_seq<=0;
            commit_response_transaction<=0; commit_response_mask<=0;
            type0_flat<=8'h02; ftw0_flat<={96'd0,32'd42_949_673};
            phase0_flat<=0; amp0_flat<={42'd0,14'd8191};
            duty0_flat<={4{16'h8000}}; offset0<=0;
            cache0_mode<=0; cache0_points<=0; enable0<=1;
            type1_flat<=8'h01; ftw1_flat<={96'd0,32'd171_798_692};
            phase1_flat<=0; amp1_flat<={42'd0,14'd4096};
            duty1_flat<={4{16'h8000}}; offset1<=0;
            cache1_mode<=0; cache1_points<=0; enable1<=1;
            shadow0_type<=8'h02; shadow0_ftw<={96'd0,32'd42_949_673};
            shadow0_phase<=0; shadow0_amp<={42'd0,14'd8191};
            shadow0_duty<={4{16'h8000}}; shadow0_offset<=0;
            shadow0_cache<=0; shadow0_points<=0; shadow0_enable<=1;
            shadow1_type<=8'h01; shadow1_ftw<={96'd0,32'd171_798_692};
            shadow1_phase<=0; shadow1_amp<={42'd0,14'd4096};
            shadow1_duty<={4{16'h8000}}; shadow1_offset<=0;
            shadow1_cache<=0; shadow1_points<=0; shadow1_enable<=1;
            staged0_valid<=0; staged1_valid<=0;
            staged0_transaction<=0; staged1_transaction<=0;
        end else begin
            tx_start<=1'b0;

            if (commit_response_waiting &&
                (commit_applied_toggle!=commit_applied_seen)) begin
                commit_applied_seen<=commit_applied_toggle;
                commit_response_waiting<=1'b0;
                response_dst<=commit_response_dst;
                response_flags<=FLAG_RESPONSE;
                response_seq<=commit_response_seq;
                response_request_cmd<=CMD_COMMIT;
                response_status<=ST_OK;
                response_detail<=16'd0;
                response_transaction<=commit_response_transaction;
                response_mask<=commit_response_mask;
                response_pending<=1'b1;
                response_index<=0;
                last_response_valid<=1'b1;
                last_src<=NODE_BLUE; last_cmd<=CMD_COMMIT;
                last_seq<=commit_response_seq; last_status<=ST_OK;
                last_flags<=FLAG_RESPONSE; last_detail<=0;
                last_transaction<=commit_response_transaction;
                last_mask<=commit_response_mask;
            end

            if (rx_valid) begin
                activity<=~activity;
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
                    R_CRCL: begin rx_crc_received[7:0]<=rx_byte; rx_state<=R_CRCH; end
                    R_CRCH: begin
                        rx_crc_received[15:8]<=rx_byte;
                        crc_ok<=({rx_byte,rx_crc_received[7:0]}==rx_crc);
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
                                (req_seq==last_seq)&&last_response_valid&&crc_ok) begin
                                response_dst<=req_src; response_flags<=last_flags;
                                response_seq<=req_seq; response_request_cmd<=req_cmd;
                                response_status<=last_status; response_detail<=last_detail;
                                response_transaction<=last_transaction;
                                response_mask<=last_mask; response_pending<=1;
                                response_index<=0; frame_ok_toggle<=~frame_ok_toggle;
                            end else if (!crc_ok) begin
                                prepare_response(req_cmd,ST_BAD_CRC,0,0,0);
                            end else if (req_version!=VERSION) begin
                                prepare_response(req_cmd,ST_BAD_VERSION,0,0,0);
                            end else if ((req_dst!=NODE_FPGA)||(req_src!=NODE_BLUE)) begin
                                prepare_response(req_cmd,ST_BAD_TARGET,0,0,0);
                            end else if ((req_flags&8'hCE)!=0) begin
                                prepare_response(req_cmd,ST_BAD_FIELD,0,0,0);
                            end else begin
                                frame_ok_toggle<=~frame_ok_toggle;
                                if (req_cmd==CMD_STAGE) begin
                                    valid_temp=1'b1;
                                    transaction_temp={payload[1],payload[0]};
                                    channel_temp=payload[2]; mode_temp=payload[3];
                                    count_temp=payload[4]; channel_flags_temp=payload[5];
                                    offset_temp16=$signed({payload[7],payload[6]});
                                    period_temp={payload[9],payload[8]};
                                    type_temp=0; ftw_temp=0; phase_temp=0; amp_temp=0;
                                    duty_temp={4{16'h8000}}; slot_seen_temp=0;
                                    if ((payload_len!=(10+count_temp*16))||(count_temp>4)) valid_temp=0;
                                    if ((channel_temp>1)||((mode_temp!=1)&&(mode_temp!=2))) valid_temp=0;
                                    if ((channel_flags_temp&8'hFC)!=0) valid_temp=0;
                                    if ((offset_temp16>16'sd8191)||(offset_temp16< -16'sd8192)) valid_temp=0;
                                    if ((mode_temp==1)&&(period_temp!=0)) valid_temp=0;
                                    if ((mode_temp==2)&&((period_temp<16)||(period_temp>4096))) valid_temp=0;
                                    for (i=0;i<4;i=i+1) begin
                                        if (i<count_temp) begin
                                            base=10+i*16; slot_temp=payload[base]; wave_temp=payload[base+1];
                                            component_flags_temp={payload[base+3],payload[base+2]};
                                            if ((slot_temp>3)||slot_seen_temp[slot_temp]||
                                                ((component_flags_temp&16'hFFFE)!=0)||(wave_temp>4)||
                                                ((payload[base+13]&8'hE0)!=0)) valid_temp=0;
                                            else begin
                                                slot_seen_temp[slot_temp]=1'b1;
                                                case (slot_temp)
                                                    0: begin
                                                        type_temp[1:0]=((component_flags_temp[0])&&channel_flags_temp[0])?v2_wave_to_internal(wave_temp):0;
                                                        ftw_temp[31:0]={payload[base+7],payload[base+6],payload[base+5],payload[base+4]};
                                                        phase_temp[31:0]={payload[base+11],payload[base+10],payload[base+9],payload[base+8]};
                                                        amp_temp[13:0]={payload[base+13][5:0],payload[base+12]};
                                                        duty_temp[15:0]={payload[base+15],payload[base+14]};
                                                    end
                                                    1: begin
                                                        type_temp[3:2]=((component_flags_temp[0])&&channel_flags_temp[0])?v2_wave_to_internal(wave_temp):0;
                                                        ftw_temp[63:32]={payload[base+7],payload[base+6],payload[base+5],payload[base+4]};
                                                        phase_temp[63:32]={payload[base+11],payload[base+10],payload[base+9],payload[base+8]};
                                                        amp_temp[27:14]={payload[base+13][5:0],payload[base+12]};
                                                        duty_temp[31:16]={payload[base+15],payload[base+14]};
                                                    end
                                                    2: begin
                                                        type_temp[5:4]=((component_flags_temp[0])&&channel_flags_temp[0])?v2_wave_to_internal(wave_temp):0;
                                                        ftw_temp[95:64]={payload[base+7],payload[base+6],payload[base+5],payload[base+4]};
                                                        phase_temp[95:64]={payload[base+11],payload[base+10],payload[base+9],payload[base+8]};
                                                        amp_temp[41:28]={payload[base+13][5:0],payload[base+12]};
                                                        duty_temp[47:32]={payload[base+15],payload[base+14]};
                                                    end
                                                    default: begin
                                                        type_temp[7:6]=((component_flags_temp[0])&&channel_flags_temp[0])?v2_wave_to_internal(wave_temp):0;
                                                        ftw_temp[127:96]={payload[base+7],payload[base+6],payload[base+5],payload[base+4]};
                                                        phase_temp[127:96]={payload[base+11],payload[base+10],payload[base+9],payload[base+8]};
                                                        amp_temp[55:42]={payload[base+13][5:0],payload[base+12]};
                                                        duty_temp[63:48]={payload[base+15],payload[base+14]};
                                                    end
                                                endcase
                                            end
                                        end
                                    end
                                    if (valid_temp) begin
                                        if (channel_temp==0) begin
                                            shadow0_type<=type_temp; shadow0_ftw<=ftw_temp;
                                            shadow0_phase<=phase_temp; shadow0_amp<=amp_temp;
                                            shadow0_duty<=duty_temp; shadow0_offset<=offset_temp16[13:0];
                                            shadow0_cache<=(mode_temp==2); shadow0_points<=period_temp[12:0];
                                            shadow0_enable<=channel_flags_temp[0];
                                            staged0_transaction<=transaction_temp; staged0_valid<=1;
                                        end else begin
                                            shadow1_type<=type_temp; shadow1_ftw<=ftw_temp;
                                            shadow1_phase<=phase_temp; shadow1_amp<=amp_temp;
                                            shadow1_duty<=duty_temp; shadow1_offset<=offset_temp16[13:0];
                                            shadow1_cache<=(mode_temp==2); shadow1_points<=period_temp[12:0];
                                            shadow1_enable<=channel_flags_temp[0];
                                            staged1_transaction<=transaction_temp; staged1_valid<=1;
                                        end
                                        prepare_response(req_cmd,ST_OK,0,transaction_temp,(16'h0001<<channel_temp));
                                    end else begin
                                        prepare_response(req_cmd,ST_BAD_FIELD,0,transaction_temp,0);
                                    end
                                end else if (req_cmd==CMD_COMMIT) begin
                                    transaction_temp={payload[1],payload[0]};
                                    if ((payload_len!=4)||(payload[2]==0)||((payload[2]&8'hFC)!=0)||
                                        ((payload[3]&8'hF0)!=0)||
                                        ((payload[2]==8'h03)&&!payload[3][1])||
                                        ((payload[2]!=8'h03)&&payload[3][1])) begin
                                        prepare_response(req_cmd,ST_BAD_FIELD,0,transaction_temp,0);
                                    end else if (((payload[2]&1)&&(!staged0_valid||(staged0_transaction!=transaction_temp)))||
                                                 ((payload[2]&2)&&(!staged1_valid||(staged1_transaction!=transaction_temp)))) begin
                                        prepare_response(req_cmd,ST_NOT_STAGED,0,transaction_temp,0);
                                    end else begin
                                        if (payload[2]&1) begin
                                            type0_flat<=shadow0_type; ftw0_flat<=shadow0_ftw;
                                            phase0_flat<=shadow0_phase; amp0_flat<=shadow0_amp;
                                            duty0_flat<=shadow0_duty; offset0<=shadow0_offset;
                                            cache0_mode<=shadow0_cache; cache0_points<=shadow0_points;
                                            enable0<=shadow0_enable; cfg0_toggle<=~cfg0_toggle;
                                            staged0_valid<=0;
                                        end
                                        if (payload[2]&2) begin
                                            type1_flat<=shadow1_type; ftw1_flat<=shadow1_ftw;
                                            phase1_flat<=shadow1_phase; amp1_flat<=shadow1_amp;
                                            duty1_flat<=shadow1_duty; offset1<=shadow1_offset;
                                            cache1_mode<=shadow1_cache; cache1_points<=shadow1_points;
                                            enable1<=shadow1_enable; cfg1_toggle<=~cfg1_toggle;
                                            staged1_valid<=0;
                                        end
                                        // active配置总线先稳定，再翻转唯一提交令牌。bit0由采样域
                                        // 决定是否清零相位；bit1已在上方按双通道语义校验。
                                        commit_mask<=payload[2][1:0];
                                        commit_flags<=payload[3][3:0];
                                        commit_toggle<=~commit_toggle;
                                        commit_response_waiting<=1'b1;
                                        commit_response_dst<=req_src;
                                        commit_response_seq<=req_seq;
                                        commit_response_transaction<=transaction_temp;
                                        commit_response_mask<={8'd0,payload[2]};
                                    end
                                end else begin
                                    prepare_response(req_cmd,ST_BAD_COMMAND,0,0,0);
                                end
                            end
                        end
                    end
                endcase
            end else if (rx_state!=R_D3) begin
                if (rx_timeout_count>=RX_TIMEOUT_CYCLES-1) begin
                    rx_state<=R_D3; rx_timeout_count<=0;
                end else rx_timeout_count<=rx_timeout_count+1'b1;
            end else rx_timeout_count<=0;

            case (response_tx_state)
                0: if (response_pending&&!tx_busy) response_tx_state<=1;
                1: if (!tx_busy) begin tx_start<=1; response_tx_state<=2; end
                2: if (tx_busy) response_tx_state<=3;
                default: if (!tx_busy) begin
                    response_tx_state<=0;
                    if (response_index==22) begin response_pending<=0; response_index<=0; end
                    else response_index<=response_index+1'b1;
                end
            endcase
        end
    end
endmodule

// 周期缓存模式：收到配置后在备用BRAM中顺序计算一个周期。完成后切换到
// 只读循环播放路径，因此每个缓存周期输出完全相同。生成期间继续输出旧波形。
module ad9744_period_cache (
    input  wire         clk,
    input  wire         rst_n,
    input  wire         restart,
    input  wire         enable_request,
    input  wire [12:0]  period_points,
    input  wire [7:0]   type_flat,
    input  wire [127:0] ftw_flat,
    input  wire [127:0] phase_flat,
    input  wire [55:0]  amp_flat,
    input  wire [63:0]  duty_flat,
    input  wire signed [13:0] dc_offset,
    output reg          cache_active,
    output reg [13:0]   dac_data,
    output reg          clipping,
    output reg [31:0]   clip_count
);
    (* ram_style = "block" *) reg [13:0] cache_ram0 [0:4095];
    (* ram_style = "block" *) reg [13:0] cache_ram1 [0:4095];
    reg [13:0] cache_ram0_q, cache_ram1_q;

    reg active_bank, build_bank;
    reg build_busy, swap_pending, preload_pending;
    reg [12:0] active_points, build_points;
    reg [11:0] read_addr, build_addr;
    reg [1:0] build_slot;
    reg [1:0] build_stage;
    reg [31:0] build_phase [0:3];
    reg signed [18:0] point_sum;
    reg signed [13:0] build_raw;
    reg signed [28:0] build_product;
    integer j;

    reg [1:0] selected_type;
    reg [31:0] selected_ftw;
    reg [31:0] selected_phase_offset;
    reg [13:0] selected_amp;
    reg [15:0] selected_duty;

    function signed [13:0] cached_wave4096;
        input [1:0] type_sel;
        input [31:0] phase;
        input [15:0] duty;
        reg [11:0] addr;
        reg [9:0] frac;
        reg signed [14:0] value;
        begin
            addr = phase[31:20];
            frac = addr[9:0];
            case (type_sel)
                2'd1: value = (phase[31:16] < duty) ? 15'sd8191 : -15'sd8192;
                2'd2: begin
                    case (addr[11:10])
                        2'b00: value =  $signed({1'b0,frac,3'b000});
                        2'b01: value =  15'sd8191-$signed({1'b0,frac,3'b000});
                        2'b10: value = -$signed({1'b0,frac,3'b000});
                        default:value = -15'sd8192+$signed({1'b0,frac,3'b000});
                    endcase
                end
                2'd3: value = -15'sd8192+$signed({1'b0,addr,2'b00});
                default: value = 15'sd0;
            endcase
            cached_wave4096 = value[13:0];
        end
    endfunction

    always @* begin
        case (build_slot)
            2'd0: begin
                selected_type=type_flat[1:0]; selected_ftw=ftw_flat[31:0];
                selected_phase_offset=phase_flat[31:0]; selected_amp=amp_flat[13:0];
                selected_duty=duty_flat[15:0];
            end
            2'd1: begin
                selected_type=type_flat[3:2]; selected_ftw=ftw_flat[63:32];
                selected_phase_offset=phase_flat[63:32]; selected_amp=amp_flat[27:14];
                selected_duty=duty_flat[31:16];
            end
            2'd2: begin
                selected_type=type_flat[5:4]; selected_ftw=ftw_flat[95:64];
                selected_phase_offset=phase_flat[95:64]; selected_amp=amp_flat[41:28];
                selected_duty=duty_flat[47:32];
            end
            default: begin
                selected_type=type_flat[7:6]; selected_ftw=ftw_flat[127:96];
                selected_phase_offset=phase_flat[127:96]; selected_amp=amp_flat[55:42];
                selected_duty=duty_flat[63:48];
            end
        endcase
    end

    wire signed [13:0] selected_raw =
        cached_wave4096(selected_type,build_phase[build_slot]+selected_phase_offset,
                        selected_duty);
    wire signed [16:0] selected_scaled = build_product >>> 13;
    wire signed [19:0] completed_point = point_sum+selected_scaled+dc_offset;
    wire [13:0] completed_limited =
        (completed_point > 20'sd8191)  ? 14'h1FFF :
        (completed_point < -20'sd8192) ? 14'h2000 : completed_point[13:0];

    // RAM端口不能放进带异步复位的状态机，否则Vivado会将整块存储器展开成
    // 触发器。两个独立同步进程分别推断一块简单双口Block RAM。
    always @(posedge clk) begin
        if (build_busy && (build_stage==2) && (build_slot==3) && !build_bank)
            cache_ram0[build_addr] <= completed_limited;
        if (cache_active && !active_bank)
            cache_ram0_q <= cache_ram0[read_addr];
    end

    always @(posedge clk) begin
        if (build_busy && (build_stage==2) && (build_slot==3) && build_bank)
            cache_ram1[build_addr] <= completed_limited;
        if (cache_active && active_bank)
            cache_ram1_q <= cache_ram1[read_addr];
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cache_active<=0; dac_data<=0; active_bank<=0; build_bank<=1;
            build_busy<=0; swap_pending<=0; preload_pending<=0;
            active_points<=0; build_points<=0; read_addr<=0; build_addr<=0;
            build_slot<=0; build_stage<=0; point_sum<=0;
            build_raw<=0; build_product<=0;
            clipping<=0; clip_count<=0;
            for (j=0;j<4;j=j+1) build_phase[j]<=0;
        end else begin
            // 同步读缓存；稳定运行时地址在0..active_points-1之间循环。
            if (cache_active) begin
                dac_data <= active_bank ? cache_ram1_q : cache_ram0_q;
                if (preload_pending) begin
                    preload_pending<=0;
                    read_addr <= (active_points>1) ? 12'd1 : 12'd0;
                end else if (({1'b0,read_addr}+13'd1)>=active_points) begin
                    if (swap_pending) begin
                        active_bank<=build_bank; active_points<=build_points;
                        swap_pending<=0; preload_pending<=1; read_addr<=0;
                    end else begin
                        read_addr<=0;
                    end
                end else begin
                    read_addr<=read_addr+1'b1;
                end
            end

            if (restart) begin
                build_busy<=0; swap_pending<=0; preload_pending<=0;
                clipping<=0; clip_count<=0;
                if (!enable_request) begin
                    cache_active<=0;
                    read_addr<=0;
                end else begin
                    build_bank<=~active_bank;
                    build_points<=period_points;
                    build_addr<=0; build_slot<=0; build_stage<=0;
                    point_sum<=0; build_raw<=0; build_product<=0;
                    for (j=0;j<4;j=j+1) build_phase[j]<=0;
                    build_busy<=1;
                end
            end else if (build_busy) begin
                if (build_stage==0) begin
                    // 第一拍只完成波形映射并寄存原始补码。
                    build_raw<=selected_raw;
                    build_phase[build_slot]<=build_phase[build_slot]+selected_ftw;
                    build_stage<=1;
                end else if (build_stage==1) begin
                    // 第二拍只完成DSP幅度乘法并寄存乘积。
                    build_product<=build_raw*$signed({1'b0,selected_amp});
                    build_stage<=2;
                end else begin
                    // 第三拍完成缩放、累加或BRAM写入。
                    build_stage<=0;
                    if (build_slot==3) begin
                        if ((completed_point>20'sd8191)||
                            (completed_point< -20'sd8192)) begin
                            clipping<=1'b1;
                            if (clip_count!=32'hFFFFFFFF) clip_count<=clip_count+1'b1;
                        end
                        build_slot<=0; point_sum<=0;
                        if (({1'b0,build_addr}+13'd1)>=build_points) begin
                            build_busy<=0;
                            if (cache_active) begin
                                swap_pending<=1;
                            end else begin
                                active_bank<=build_bank; active_points<=build_points;
                                read_addr<=0; preload_pending<=1; cache_active<=1;
                            end
                        end else begin
                            build_addr<=build_addr+1'b1;
                        end
                    end else begin
                        point_sum<=point_sum+selected_scaled;
                        build_slot<=build_slot+1'b1;
                    end
                end
            end
        end
    end
endmodule

// 新多波形帧：A5 5A TARGET CONTROL, COUNT个13字节条目, OFFSET_L/H,
// 缓存模式再跟POINTS_L/H，最后XOR, 5A A5。CONTROL[7]选择缓存模式，
// CONTROL[2:0]为COUNT。条目：TYPE, FTW(4), PHASE(4), AMP(2), DUTY(2)。
// TARGET=0支持DAC1实时/缓存0..4槽；TARGET=1支持DAC2实时0..1槽。
module uart_multiwave_config #(parameter CLK_HZ=50_000_000, BAUD=115_200) (
    input wire clk, input wire rst_n, input wire uart_rx,
    output wire uart_tx, output reg activity, output reg cfg_toggle,
    output reg cfg_target,
    output reg [7:0] type_flat, output reg [127:0] ftw_flat,
    output reg [127:0] phase_flat, output reg [55:0] amp_flat,
    output reg [63:0] duty_flat, output reg signed [13:0] dc_offset,
    output reg cache_mode, output reg [12:0] cache_points
);
    localparam S_A5=0, S_5A=1, S_TARGET=2, S_COUNT=3, S_PAYLOAD=4,
               S_CHECK=5, S_END5A=6, S_ENDA5=7;
    wire rx_valid; wire [7:0] rx_byte; wire tx_busy;
    reg tx_start; reg [7:0] tx_byte;
    reg [2:0] state; reg [2:0] count;
    reg target_pending;
    reg cache_mode_pending;
    reg [6:0] payload_index; reg [6:0] payload_length;
    reg [7:0] checksum; reg checksum_ok; reg [1:0] ack_index; reg ack_pending;
    reg [7:0] ack_status;
    reg reply_armed;
    reg [1:0] ack_tx_state;
    reg [7:0] frame_mem [0:55];

    wire [1:0] entry_type0 = frame_mem[0][1:0];
    wire [1:0] entry_type1 = frame_mem[13][1:0];
    wire [1:0] entry_type2 = frame_mem[26][1:0];
    wire [1:0] entry_type3 = frame_mem[39][1:0];
    wire [31:0] entry_ftw0 = {frame_mem[4],frame_mem[3],frame_mem[2],frame_mem[1]};
    wire [31:0] entry_ftw1 = {frame_mem[17],frame_mem[16],frame_mem[15],frame_mem[14]};
    wire [31:0] entry_ftw2 = {frame_mem[30],frame_mem[29],frame_mem[28],frame_mem[27]};
    wire [31:0] entry_ftw3 = {frame_mem[43],frame_mem[42],frame_mem[41],frame_mem[40]};
    wire [31:0] entry_phase0 = {frame_mem[8],frame_mem[7],frame_mem[6],frame_mem[5]};
    wire [31:0] entry_phase1 = {frame_mem[21],frame_mem[20],frame_mem[19],frame_mem[18]};
    wire [31:0] entry_phase2 = {frame_mem[34],frame_mem[33],frame_mem[32],frame_mem[31]};
    wire [31:0] entry_phase3 = {frame_mem[47],frame_mem[46],frame_mem[45],frame_mem[44]};
    wire [13:0] entry_amp0 = {frame_mem[10][5:0],frame_mem[9]};
    wire [13:0] entry_amp1 = {frame_mem[23][5:0],frame_mem[22]};
    wire [13:0] entry_amp2 = {frame_mem[36][5:0],frame_mem[35]};
    wire [13:0] entry_amp3 = {frame_mem[49][5:0],frame_mem[48]};
    wire [15:0] entry_duty0 = {frame_mem[12],frame_mem[11]};
    wire [15:0] entry_duty1 = {frame_mem[25],frame_mem[24]};
    wire [15:0] entry_duty2 = {frame_mem[38],frame_mem[37]};
    wire [15:0] entry_duty3 = {frame_mem[51],frame_mem[50]};
    wire [15:0] decoded_cache_points =
        (count==0) ? {frame_mem[3], frame_mem[2]} :
        (count==1) ? {frame_mem[16],frame_mem[15]} :
        (count==2) ? {frame_mem[29],frame_mem[28]} :
        (count==3) ? {frame_mem[42],frame_mem[41]} :
                     {frame_mem[55],frame_mem[54]};

    uart_rx_byte #(.CLK_HZ(CLK_HZ),.BAUD(BAUD)) u_rx(
        clk,rst_n,uart_rx,rx_valid,rx_byte);
    uart_tx_byte #(.CLK_HZ(CLK_HZ),.BAUD(BAUD)) u_tx(
        clk,rst_n,tx_start,tx_byte,uart_tx,tx_busy);

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state<=S_A5; count<=0; payload_index<=0; payload_length<=0;
            target_pending<=0; cfg_target<=0;
            cache_mode_pending<=0;
            checksum<=0; checksum_ok<=0; activity<=0; cfg_toggle<=0;
            // 与DAC采样域默认值保持一致：槽位0为1 MHz满幅三角波。
            type_flat<=8'h02;
            ftw_flat<={96'd0,32'd42_949_673};
            phase_flat<=0;
            amp_flat<={42'd0,14'd8191};
            duty_flat<={4{16'h8000}}; dc_offset<=0;
            cache_mode<=0; cache_points<=0;
            tx_start<=0; tx_byte<=0; ack_index<=0; ack_pending<=0; ack_status<=0;
            reply_armed<=0; ack_tx_state<=0;
        end else begin
            tx_start <= 0;
            if (rx_valid) begin
                activity <= ~activity;
                case (state)
                    S_A5: begin
                        if (rx_byte==8'hA5) begin
                            state<=S_5A;
                            // 只有检测到一帧新的起始字节，才重新允许一次成功应答。
                            reply_armed<=1'b1;
                        end
                    end
                    S_5A: begin
                        if (rx_byte==8'h5A) state<=S_TARGET;
                        else state<=S_A5;
                    end
                    S_TARGET: begin
                        checksum<=rx_byte;
                        if (rx_byte<=8'd1) begin
                            target_pending<=rx_byte[0];
                            state<=S_COUNT;
                        end
                        else state<=S_A5;
                    end
                    S_COUNT: begin
                        if ((rx_byte[6:3]==4'd0) && (rx_byte[2:0]<=4) &&
                            (!target_pending ||
                             (!rx_byte[7] && (rx_byte[2:0]<=1)))) begin
                            count<=rx_byte[2:0]; cache_mode_pending<=rx_byte[7];
                            checksum<=checksum^rx_byte;
                            payload_index<=0;
                            payload_length<=rx_byte[2:0]*13+2+(rx_byte[7]?2:0);
                            state<=S_PAYLOAD;
                        end else state<=S_A5;
                    end
                    S_PAYLOAD: begin
                        frame_mem[payload_index]<=rx_byte;
                        checksum<=checksum^rx_byte;
                        payload_index<=payload_index+1'b1;
                        if (payload_index+1'b1==payload_length) state<=S_CHECK;
                    end
                    S_CHECK: begin checksum_ok<=(rx_byte==checksum); state<=S_END5A; end
                    S_END5A: begin
                        if (rx_byte==8'h5A) state<=S_ENDA5;
                        else state<=S_A5;
                    end
                    S_ENDA5: begin
                        state<=S_A5;
                        if (rx_byte==8'hA5 && checksum_ok &&
                            (!cache_mode_pending ||
                             ((decoded_cache_points>=16) &&
                              (decoded_cache_points<=4096)))) begin
                            // 每条总线在本周期只赋值一次，避免整总线与动态位段重叠赋值。
                            case (count)
                                0: begin
                                    type_flat<=0; ftw_flat<=0; phase_flat<=0; amp_flat<=0;
                                    duty_flat<={4{16'h8000}};
                                    dc_offset<={frame_mem[1][5:0],frame_mem[0]};
                                end
                                1: begin
                                    type_flat<={6'd0,entry_type0};
                                    ftw_flat<={96'd0,entry_ftw0};
                                    phase_flat<={96'd0,entry_phase0};
                                    amp_flat<={42'd0,entry_amp0};
                                    duty_flat<={{3{16'h8000}},entry_duty0};
                                    dc_offset<={frame_mem[14][5:0],frame_mem[13]};
                                end
                                2: begin
                                    type_flat<={4'd0,entry_type1,entry_type0};
                                    ftw_flat<={64'd0,entry_ftw1,entry_ftw0};
                                    phase_flat<={64'd0,entry_phase1,entry_phase0};
                                    amp_flat<={28'd0,entry_amp1,entry_amp0};
                                    duty_flat<={{2{16'h8000}},entry_duty1,entry_duty0};
                                    dc_offset<={frame_mem[27][5:0],frame_mem[26]};
                                end
                                3: begin
                                    type_flat<={2'd0,entry_type2,entry_type1,entry_type0};
                                    ftw_flat<={32'd0,entry_ftw2,entry_ftw1,entry_ftw0};
                                    phase_flat<={32'd0,entry_phase2,entry_phase1,entry_phase0};
                                    amp_flat<={14'd0,entry_amp2,entry_amp1,entry_amp0};
                                    duty_flat<={16'h8000,entry_duty2,entry_duty1,entry_duty0};
                                    dc_offset<={frame_mem[40][5:0],frame_mem[39]};
                                end
                                default: begin
                                    type_flat<={entry_type3,entry_type2,entry_type1,entry_type0};
                                    ftw_flat<={entry_ftw3,entry_ftw2,entry_ftw1,entry_ftw0};
                                    phase_flat<={entry_phase3,entry_phase2,entry_phase1,entry_phase0};
                                    amp_flat<={entry_amp3,entry_amp2,entry_amp1,entry_amp0};
                                    duty_flat<={entry_duty3,entry_duty2,entry_duty1,entry_duty0};
                                    dc_offset<={frame_mem[53][5:0],frame_mem[52]};
                                end
                            endcase
                            cache_mode<=cache_mode_pending;
                            cache_points<=cache_mode_pending ? decoded_cache_points[12:0] : 13'd0;
                            cfg_target<=target_pending;
                            cfg_toggle<=~cfg_toggle;
                            if (reply_armed) begin
                                ack_status<=8'h00; ack_pending<=1; ack_index<=0;
                                reply_armed<=1'b0;
                            end
                        end
                    end
                endcase
            end
            // MCU端若用轮询或较慢中断处理，连续无间隔字节可能产生ORE并表现为
            // 只收到第1、第3字节。每个应答字节之间加入约1 ms保护间隔。
            // 115200 baud连续发送四字节应答，不插入额外毫秒级间隔。
            case (ack_tx_state)
                2'd0: begin
                    if (ack_pending && !tx_busy) begin
                        case (ack_index)
                            0:tx_byte<=8'h5A;
                            1:tx_byte<=8'hC0;
                            2:tx_byte<=ack_status;
                            default:tx_byte<=8'h9A^ack_status;
                        endcase
                        ack_tx_state<=2'd1;
                    end
                end
                2'd1: begin
                    // 数据先稳定一拍，再启动发送。
                    if (!tx_busy) begin
                        tx_start<=1'b1;
                        ack_tx_state<=2'd2;
                    end
                end
                2'd2: begin
                    // 等待发送器确认busy，防止同一字节被重复启动。
                    if (tx_busy)
                        ack_tx_state<=2'd3;
                end
                default: begin
                    // 必须等待当前字节完整结束后，才切换到下一应答字节。
                    if (!tx_busy) begin
                        ack_tx_state<=2'd0;
                        if (ack_index==3) begin
                            ack_pending<=0;
                            ack_index<=0;
                        end else begin
                            ack_index<=ack_index+1'b1;
                        end
                    end
                end
            endcase
        end
    end
endmodule
