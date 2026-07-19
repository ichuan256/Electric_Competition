`timescale 1ns/1ps

module thd_harmonic_reconstruction (
    input  wire         clk,
    input  wire         rst_n,
    input  wire         commit,
    input  wire         clear_phase,
    input  wire         enable,
    input  wire [31:0]  fundamental_ftw,
    input  wire signed [13:0] dc_offset,
    input  wire [69:0]  amplitude_flat,
    input  wire [159:0] phase_flat,
    output reg  [13:0]  dac_code
);
    reg [31:0] fundamental_phase;
    reg [2:0] pipeline_valid;

    wire [31:0] phase1 = fundamental_phase + phase_flat[31:0]    + 32'h4000_0000;
    wire [31:0] phase2 = (fundamental_phase << 1) + phase_flat[63:32]  + 32'h4000_0000;
    wire [31:0] phase3 = fundamental_phase + (fundamental_phase << 1)
                        + phase_flat[95:64]  + 32'h4000_0000;
    wire [31:0] phase4 = (fundamental_phase << 2) + phase_flat[127:96] + 32'h4000_0000;
    wire [31:0] phase5 = fundamental_phase + (fundamental_phase << 2)
                        + phase_flat[159:128] + 32'h4000_0000;

    wire signed [13:0] sine1, sine2, sine3, sine4, sine5;
    sine_lut_4096_5p u_sine_lut (
        .clk(clk),
        .addr0(phase1[31:20]), .addr1(phase2[31:20]),
        .addr2(phase3[31:20]), .addr3(phase4[31:20]),
        .addr4(phase5[31:20]),
        .data0(sine1), .data1(sine2), .data2(sine3),
        .data3(sine4), .data4(sine5)
    );

    reg signed [28:0] product1, product2, product3, product4, product5;
    reg signed [19:0] wide_sum;

    function signed [16:0] round_q13;
        input signed [28:0] value;
        reg signed [29:0] magnitude;
        begin
            if (value >= 0)
                round_q13 = (value + 29'sd4096) >>> 13;
            else begin
                magnitude = -value;
                round_q13 = -((magnitude + 30'sd4096) >>> 13);
            end
        end
    endfunction

    wire signed [16:0] scaled1 = round_q13(product1);
    wire signed [16:0] scaled2 = round_q13(product2);
    wire signed [16:0] scaled3 = round_q13(product3);
    wire signed [16:0] scaled4 = round_q13(product4);
    wire signed [16:0] scaled5 = round_q13(product5);
    wire signed [19:0] next_sum =
        {{3{scaled1[16]}},scaled1} + {{3{scaled2[16]}},scaled2} +
        {{3{scaled3[16]}},scaled3} + {{3{scaled4[16]}},scaled4} +
        {{3{scaled5[16]}},scaled5} + {{6{dc_offset[13]}},dc_offset};

    wire signed [13:0] saturated =
        (wide_sum > 20'sd8191)  ? 14'sh1FFF :
        (wide_sum < -20'sd8192) ? 14'sh2000 : wide_sum[13:0];

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            fundamental_phase <= 32'd0;
            pipeline_valid     <= 3'b000;
            product1           <= 29'sd0;
            product2           <= 29'sd0;
            product3           <= 29'sd0;
            product4           <= 29'sd0;
            product5           <= 29'sd0;
            wide_sum           <= 20'sd0;
            dac_code           <= 14'd0;
        end else if (commit) begin
            if (clear_phase) fundamental_phase <= 32'd0;
            pipeline_valid <= 3'b000;
            product1 <= 29'sd0; product2 <= 29'sd0; product3 <= 29'sd0;
            product4 <= 29'sd0; product5 <= 29'sd0;
            wide_sum <= 20'sd0;
            dac_code <= 14'd0;
        end else if (!enable) begin
            pipeline_valid <= 3'b000;
            product1 <= 29'sd0; product2 <= 29'sd0; product3 <= 29'sd0;
            product4 <= 29'sd0; product5 <= 29'sd0;
            wide_sum <= 20'sd0;
            dac_code <= 14'd0;
        end else begin
            fundamental_phase <= fundamental_phase + fundamental_ftw;
            pipeline_valid <= {pipeline_valid[1:0],1'b1};

            product1 <= sine1 * $signed({1'b0,amplitude_flat[13:0]});
            product2 <= sine2 * $signed({1'b0,amplitude_flat[27:14]});
            product3 <= sine3 * $signed({1'b0,amplitude_flat[41:28]});
            product4 <= sine4 * $signed({1'b0,amplitude_flat[55:42]});
            product5 <= sine5 * $signed({1'b0,amplitude_flat[69:56]});
            wide_sum <= next_sum;
            dac_code <= pipeline_valid[2] ? saturated : 14'd0;
        end
    end
endmodule

// One logical 4096x14 sine table with five synchronous read ports. Vivado may
// replicate the ROM to provide five samples per 100 MHz cycle; every replica
// uses the same initialization image and therefore has identical quantization.
module sine_lut_4096_5p (
    input wire clk,
    input wire [11:0] addr0, addr1, addr2, addr3, addr4,
    output reg signed [13:0] data0, data1, data2, data3, data4
);
    (* rom_style = "block" *) reg signed [13:0] rom [0:4095];
    initial $readmemh("sine_lut_4096.mem", rom);
    always @(posedge clk) begin
        data0 <= rom[addr0];
        data1 <= rom[addr1];
        data2 <= rom[addr2];
        data3 <= rom[addr3];
        data4 <= rom[addr4];
    end
endmodule
