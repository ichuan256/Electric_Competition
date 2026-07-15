// DAC1四槽位多波形叠加器。TYPE: 0关闭、1方波、2三角波、3锯齿波。
// 三角波和锯齿波使用phase[31:20]，即4096个相位位置。
// 按系统设计约定不做饱和限幅，由MCU保证最终和在-8192..8191内。
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
    output reg [13:0] dac_data
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
            // 有意不饱和：MCU必须保证mix_sum位于14位补码有效范围。
            dac_data <= mix_sum[13:0];
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
    output reg [13:0]   dac_data
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

    // RAM端口不能放进带异步复位的状态机，否则Vivado会将整块存储器展开成
    // 触发器。两个独立同步进程分别推断一块简单双口Block RAM。
    always @(posedge clk) begin
        if (build_busy && (build_stage==2) && (build_slot==3) && !build_bank)
            cache_ram0[build_addr] <= completed_point[13:0];
        if (cache_active && !active_bank)
            cache_ram0_q <= cache_ram0[read_addr];
    end

    always @(posedge clk) begin
        if (build_busy && (build_stage==2) && (build_slot==3) && build_bank)
            cache_ram1[build_addr] <= completed_point[13:0];
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
