// AD9744 第一路最小硬件测试顶层。
//
// 用途：排除 UART、双通道 CDC、幅度乘法和波形选择逻辑的影响，
// 仅验证 Clock Wizard、DDS、DAC 数据总线和转发时钟。
// 将本模块设为 Top 后，只启用配套的 ad9744_minimal_test.xdc。
// clk_wiz_0 当前配置：clk_out1=150 MHz/0 度，clk_out2=150 MHz/18 度。

module ad9744_minimal_test #(
    parameter FIXED_CODE_TEST = 1'b0,
    parameter [13:0] FIXED_CODE = 14'h1000,
    // round(1 MHz * 2^32 / 150 MHz)
    parameter [31:0] PHASE_STEP = 32'd28_633_115
) (
    input  wire        sys_clk,
    input  wire        sys_rst_n,
    output wire        dac_clk,
    output wire [13:0] dac_data,
    output wire        dac_sleep,
    output wire        led0
);
    wire dds_clk;
    wire forward_clk;
    wire unused_clk_ch2;
    wire clk_locked;

    // 直接复用工程中已经生成的时钟 IP，第三路输出在本测试中不用。
    clk_wiz_0 u_clk_wiz (
        .clk_out1(dds_clk),
        .clk_out2(forward_clk),
        .clk_out3(unused_clk_ch2),
        .reset(~sys_rst_n),
        .locked(clk_locked),
        .clk_in1(sys_clk)
    );

    (* ASYNC_REG = "TRUE" *) reg [2:0] reset_sync;
    wire dds_rst_n = reset_sync[2];
    always @(posedge dds_clk or negedge sys_rst_n) begin
        if (!sys_rst_n)
            reset_sync <= 3'b000;
        else
            reset_sync <= {reset_sync[1:0], clk_locked};
    end

    reg [31:0] phase_acc;
    reg [7:0]  rom_addr;
    reg [13:0] sine_word;
    (* IOB = "TRUE" *) reg [13:0] dac_data_r;

    // 四级明确流水：累加器 -> 地址 -> LUT -> IOB。
    // 没有乘法、限幅、配置总线或跨时钟域数据路径。
    always @(posedge dds_clk or negedge dds_rst_n) begin
        if (!dds_rst_n) begin
            phase_acc  <= 32'd0;
            rom_addr   <= 8'd0;
            sine_word  <= 14'd0;
            dac_data_r <= 14'd0;
        end else begin
            phase_acc <= phase_acc + PHASE_STEP;
            rom_addr  <= phase_acc[31:24];
            // LUT内容以0x2000为零点（偏移二进制）；减去0x2000后，
            // 送往MODE=高电平的AD9744总线才是14位二进制补码。
            sine_word <= sine_lut_256(rom_addr) - 14'h2000;
            dac_data_r <= FIXED_CODE_TEST ? FIXED_CODE : sine_word;
        end
    end

    // clk_out2 已带 18 度相移；ODDR 产生专用、无组合毛刺的转发时钟。
    // D1=0、D2=1 使外部采样上升沿落在数据更新之后约 3.67 ns。
    ODDR #(
        .DDR_CLK_EDGE("SAME_EDGE"),
        .INIT(1'b0),
        .SRTYPE("SYNC")
    ) u_dac_clk_oddr (
        .Q(dac_clk),
        .C(forward_clk),
        .CE(1'b1),
        .D1(1'b0),
        .D2(1'b1),
        .R(1'b0),
        .S(1'b0)
    );

    assign dac_data  = dac_data_r;
    assign dac_sleep = 1'b0;
    assign led0      = clk_locked;

    // 256点原始正弦表采用偏移二进制存储，流水线中统一减去0x2000，
    // 转成MODE=高电平时AD9744要求的14位二进制补码。
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
