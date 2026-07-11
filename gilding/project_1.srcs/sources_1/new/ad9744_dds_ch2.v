// 第二路 AD9744 DDS 通道。
// 本文件仅负责第二路波形生成、配置跨时钟域、补码限幅和 DAC 时钟转发。
module ad9744_dds_ch2 (
    input  wire sample_clk,
    input  wire sample_rst_n,
    input  wire cfg_toggle_sys,
    input  wire [31:0] cfg_ftw_sys,
    input  wire [31:0] cfg_phase_sys,
    input  wire [13:0] cfg_amplitude_sys,
    input  wire signed [13:0] cfg_offset_sys,
    input  wire [15:0] cfg_duty_sys,
    input  wire [2:0] cfg_wave_sys,
    input  wire cfg_enable_sys,
    output wire dac_clk,
    output wire [13:0] dac_data,
    output wire dac_sleep
);
    reg [2:0] cfg_toggle_sync;
    reg [31:0] ftw;
    reg [31:0] phase_offset;
    reg [13:0] amplitude;
    reg signed [13:0] dc_offset;
    reg [15:0] duty;
    reg [2:0] wave_sel;
    reg output_enable;

    always @(posedge sample_clk or negedge sample_rst_n) begin
        if (!sample_rst_n) begin
            cfg_toggle_sync <= 3'b000;
            ftw <= 32'd42_949_673;
            phase_offset <= 32'd0;
            amplitude <= 14'd8191;
            dc_offset <= 14'sd0;
            duty <= 16'h8000;
            wave_sel <= 3'd0;
            output_enable <= 1'b1;
        end else begin
            cfg_toggle_sync <= {cfg_toggle_sync[1:0], cfg_toggle_sys};
            if (cfg_toggle_sync[2] != cfg_toggle_sync[1]) begin
                ftw <= cfg_ftw_sys;
                phase_offset <= cfg_phase_sys;
                amplitude <= cfg_amplitude_sys;
                dc_offset <= cfg_offset_sys;
                duty <= cfg_duty_sys;
                wave_sel <= cfg_wave_sys;
                output_enable <= cfg_enable_sys;
            end
        end
    end

    reg [31:0] phase_acc;
    reg signed [14:0] wave_raw;
    reg signed [29:0] product;
    // 第二路数据寄存器必须进入 IOB，否则 100 MHz 半周期输出时序无法收敛。
    (* IOB = "TRUE" *) reg [13:0] dac_data_r;
    wire [31:0] phase = phase_acc + phase_offset;
    wire signed [14:0] sine_value = $signed({1'b0, sine_lut_256(phase[31:24])}) - 15'sd8192;
    wire signed [14:0] saw_value = $signed({1'b0, phase[31:18]}) - 15'sd8192;
    wire signed [15:0] tri_up = -16'sd8192 + $signed({1'b0, phase[30:18], 1'b0});
    wire signed [15:0] tri_value = phase[31] ? -tri_up - 16'sd1 : tri_up;
    wire signed [14:0] square_value = (phase[31:16] < duty) ? 15'sd8191 : -15'sd8192;
    wire signed [15:0] scaled_value = product >>> 13;
    wire signed [15:0] sum_value = scaled_value + dc_offset;
    wire signed [15:0] limited_value =
        (sum_value > 16'sd8191) ? 16'sd8191 :
        (sum_value < -16'sd8192) ? -16'sd8192 : sum_value;

    always @* begin
        case (wave_sel)
            3'd0: wave_raw = sine_value;
            3'd1: wave_raw = square_value;
            3'd2: wave_raw = tri_value[14:0];
            3'd3: wave_raw = saw_value;
            default: wave_raw = sine_value;
        endcase
    end

    always @(posedge sample_clk or negedge sample_rst_n) begin
        if (!sample_rst_n) begin
            phase_acc <= 32'd0;
            product <= 30'sd0;
            dac_data_r <= 14'd0;
        end else begin
            phase_acc <= phase_acc + ftw;
            product <= wave_raw * $signed({1'b0, amplitude});
            dac_data_r <= output_enable ? limited_value[13:0] : 14'd0;
        end
    end

    ODDR #(.DDR_CLK_EDGE("SAME_EDGE"), .INIT(1'b0), .SRTYPE("SYNC")) u_dac2_clk_oddr (
        .Q(dac_clk), .C(sample_clk), .CE(1'b1),
        .D1(1'b0), .D2(1'b1), .R(1'b0), .S(1'b0)
    );

    assign dac_data = dac_data_r;
    assign dac_sleep = 1'b0;

    function [13:0] sine_lut_256;
        input [7:0] addr;
        reg [5:0] qaddr;
        reg [13:0] mag;
        begin
            case (addr[7:6])
                2'b00: qaddr = addr[5:0];
                2'b01: qaddr = 6'd63 - addr[5:0];
                2'b10: qaddr = addr[5:0];
                default: qaddr = 6'd63 - addr[5:0];
            endcase
            case (qaddr)
                0:mag=0; 1:mag=201; 2:mag=402; 3:mag=603; 4:mag=803; 5:mag=1003; 6:mag=1202; 7:mag=1400;
                8:mag=1598; 9:mag=1795; 10:mag=1990; 11:mag=2185; 12:mag=2378; 13:mag=2569; 14:mag=2759; 15:mag=2948;
                16:mag=3135; 17:mag=3319; 18:mag=3502; 19:mag=3683; 20:mag=3861; 21:mag=4037; 22:mag=4211; 23:mag=4382;
                24:mag=4551; 25:mag=4716; 26:mag=4879; 27:mag=5039; 28:mag=5196; 29:mag=5350; 30:mag=5501; 31:mag=5648;
                32:mag=5792; 33:mag=5932; 34:mag=6069; 35:mag=6202; 36:mag=6332; 37:mag=6457; 38:mag=6579; 39:mag=6697;
                40:mag=6811; 41:mag=6920; 42:mag=7026; 43:mag=7127; 44:mag=7224; 45:mag=7316; 46:mag=7405; 47:mag=7488;
                48:mag=7567; 49:mag=7642; 50:mag=7712; 51:mag=7778; 52:mag=7838; 53:mag=7894; 54:mag=7946; 55:mag=7992;
                56:mag=8034; 57:mag=8070; 58:mag=8102; 59:mag=8129; 60:mag=8152; 61:mag=8169; 62:mag=8181; default:mag=8189;
            endcase
            sine_lut_256 = addr[7] ? (14'd8192 - mag) : (14'd8192 + mag);
        end
    endfunction
endmodule
