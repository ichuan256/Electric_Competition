`timescale 1ns/1ps

module tb_thd_harmonic_reconstruction;
    reg clk=0;
    reg rst_n=0;
    reg commit=0;
    reg clear_phase=1;
    reg enable=0;
    reg [31:0] fundamental_ftw=0;
    reg signed [13:0] dc_offset=0;
    reg [69:0] amplitude_flat=0;
    reg [159:0] phase_flat=0;
    wire [13:0] dac_code;

    thd_harmonic_reconstruction dut (
        .clk(clk), .rst_n(rst_n), .commit(commit),
        .clear_phase(clear_phase), .enable(enable),
        .fundamental_ftw(fundamental_ftw), .dc_offset(dc_offset),
        .amplitude_flat(amplitude_flat), .phase_flat(phase_flat),
        .dac_code(dac_code)
    );
    always #5 clk=~clk;

    integer sample_count;
    integer positive_crossings;
    integer sample_value;
    integer previous_value;
    integer minimum_value;
    integer maximum_value;

    initial begin
        #40 rst_n=1;
        fundamental_ftw=32'd42_949_673; // 1 MHz at 100 MHz sample rate
        amplitude_flat[13:0]=14'd4096;
        enable=1;
        @(posedge clk); commit=1;
        @(posedge clk); commit=0;

        sample_count=0; positive_crossings=0;
        previous_value=0; minimum_value=32767; maximum_value=-32768;
        repeat (1200) begin
            @(posedge clk);
            sample_value=$signed(dac_code);
            if (sample_count>20 && previous_value<0 && sample_value>=0)
                positive_crossings=positive_crossings+1;
            if (sample_count>20 && sample_value<minimum_value) minimum_value=sample_value;
            if (sample_count>20 && sample_value>maximum_value) maximum_value=sample_value;
            previous_value=sample_value;
            sample_count=sample_count+1;
        end
        if (positive_crossings<10 || positive_crossings>13) begin
            $display("FAIL: crossing count=%0d",positive_crossings); $fatal;
        end
        if (maximum_value<4000 || minimum_value>-4000) begin
            $display("FAIL: range %0d..%0d",minimum_value,maximum_value); $fatal;
        end

        // The datapath must saturate only once, after the five-way sum.
        fundamental_ftw=0;
        amplitude_flat={5{14'd8191}};
        phase_flat=0;
        @(posedge clk); commit=1;
        @(posedge clk); commit=0;
        repeat (12) @(posedge clk);
        if (dac_code!==14'h1FFF) begin
            $display("FAIL: positive saturation code=%h",dac_code); $fatal;
        end
        $display("PASS: 4096-point five-harmonic reconstruction datapath");
        $finish;
    end
endmodule
