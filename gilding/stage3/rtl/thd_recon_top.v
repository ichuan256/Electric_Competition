`timescale 1ns/1ps

// Stage3 THD waveform reconstruction top level.
// Only DAC1, the Blue-FPGA UART and the two UART diagnostic LEDs are retained.
module thd_recon_top (
    input  wire        sys_clk,
    input  wire        sys_rst_n,
    input  wire        mcu_uart_rxd,
    output wire        mcu_uart_txd,
    output wire        led0,
    output wire        led1,
    output wire        dac_clk,
    output wire [13:0] dac_data,
    output wire        dac_sleep
);
    wire sys_clk_i;
    IBUF u_sys_clk_ibuf (.I(sys_clk), .O(sys_clk_i));

    wire sample_clk;
    wire clk_locked;
    thd_clocking u_clocking (
        .clk_in1(sys_clk_i),
        .resetn(sys_rst_n),
        .clk_out1(sample_clk),
        .locked(clk_locked)
    );

    reg [1:0] sample_reset_sync;
    wire sample_rst_n = sample_reset_sync[1];
    always @(posedge sample_clk or negedge sys_rst_n) begin
        if (!sys_rst_n)
            sample_reset_sync <= 2'b00;
        else
            sample_reset_sync <= {sample_reset_sync[0], clk_locked};
    end

    wire uart_activity_toggle;
    wire valid_frame_toggle;
    wire config_commit_toggle;
    wire [31:0] shadow_fundamental_ftw;
    wire signed [13:0] shadow_dc_offset;
    wire [69:0] shadow_amplitude_flat;
    wire [159:0] shadow_phase_flat;
    wire shadow_clear_phase;
    wire shadow_output_enable;

    thd_uart_v2 #(.CLK_HZ(50_000_000), .BAUD(115_200)) u_uart (
        .clk(sys_clk_i),
        .rst_n(sys_rst_n),
        .uart_rx(mcu_uart_rxd),
        .uart_tx(mcu_uart_txd),
        .activity_toggle(uart_activity_toggle),
        .valid_frame_toggle(valid_frame_toggle),
        .commit_toggle(config_commit_toggle),
        .fundamental_ftw(shadow_fundamental_ftw),
        .dc_offset(shadow_dc_offset),
        .amplitude_flat(shadow_amplitude_flat),
        .phase_flat(shadow_phase_flat),
        .clear_phase(shadow_clear_phase),
        .output_enable(shadow_output_enable)
    );

    // Stable multi-bit bus plus toggle synchronizer. The UART block does not
    // modify the shadow bus between STAGE and the following successful COMMIT.
    (* ASYNC_REG = "TRUE" *) reg [2:0] commit_sync;
    reg commit_pulse;
    reg [31:0] active_fundamental_ftw;
    reg signed [13:0] active_dc_offset;
    reg [69:0] active_amplitude_flat;
    reg [159:0] active_phase_flat;
    reg active_clear_phase;
    reg active_output_enable;
    always @(posedge sample_clk or negedge sample_rst_n) begin
        if (!sample_rst_n) begin
            commit_sync            <= 3'b000;
            commit_pulse           <= 1'b0;
            active_fundamental_ftw <= 32'd0;
            active_dc_offset       <= 14'sd0;
            active_amplitude_flat  <= 70'd0;
            active_phase_flat      <= 160'd0;
            active_clear_phase     <= 1'b1;
            active_output_enable   <= 1'b0;
        end else begin
            commit_sync  <= {commit_sync[1:0], config_commit_toggle};
            commit_pulse <= 1'b0;
            if (commit_sync[2] != commit_sync[1]) begin
                active_fundamental_ftw <= shadow_fundamental_ftw;
                active_dc_offset       <= shadow_dc_offset;
                active_amplitude_flat  <= shadow_amplitude_flat;
                active_phase_flat      <= shadow_phase_flat;
                active_clear_phase     <= shadow_clear_phase;
                active_output_enable   <= shadow_output_enable;
                commit_pulse           <= 1'b1;
            end
        end
    end

    wire [13:0] reconstructed_code;
    thd_harmonic_reconstruction u_reconstruction (
        .clk(sample_clk),
        .rst_n(sample_rst_n),
        .commit(commit_pulse),
        .clear_phase(active_clear_phase),
        .enable(active_output_enable),
        .fundamental_ftw(active_fundamental_ftw),
        .dc_offset(active_dc_offset),
        .amplitude_flat(active_amplitude_flat),
        .phase_flat(active_phase_flat),
        .dac_code(reconstructed_code)
    );

    // Register the final two's-complement code in the output IOB. Data changes
    // on the internal rising edge; the forwarded DAC clock rises 5 ns later.
    (* IOB = "TRUE" *) reg [13:0] dac_data_iob;
    always @(posedge sample_clk or negedge sample_rst_n) begin
        if (!sample_rst_n)
            dac_data_iob <= 14'd0;
        else
            dac_data_iob <= reconstructed_code;
    end

    wire forwarded_dac_clk;
    ODDR #(
        .DDR_CLK_EDGE("SAME_EDGE"), .INIT(1'b0), .SRTYPE("SYNC")
    ) u_dac_clk_oddr (
        .Q(forwarded_dac_clk), .C(sample_clk), .CE(1'b1),
        .D1(1'b0), .D2(1'b1), .R(1'b0), .S(1'b0)
    );

    assign dac_clk   = forwarded_dac_clk;
    assign dac_data  = dac_data_iob;
    assign dac_sleep = ~active_output_enable;

    // Both board LEDs are active-low. LED1 starts blinking after any UART byte;
    // PS_LED0 starts blinking after any complete CRC-valid, correctly addressed
    // V2 frame. This keeps physical-layer and frame-layer diagnostics separate.
    reg activity_d;
    reg frame_d;
    reg uart_seen;
    reg frame_seen;
    reg [23:0] led1_divider;
    reg [24:0] led0_divider;
    reg led1_r;
    reg led0_r;
    always @(posedge sys_clk_i or negedge sys_rst_n) begin
        if (!sys_rst_n) begin
            activity_d   <= 1'b0;
            frame_d      <= 1'b0;
            uart_seen    <= 1'b0;
            frame_seen   <= 1'b0;
            led1_divider <= 24'd0;
            led0_divider <= 25'd0;
            led1_r       <= 1'b1;
            led0_r       <= 1'b1;
        end else begin
            activity_d <= uart_activity_toggle;
            frame_d    <= valid_frame_toggle;
            if (activity_d != uart_activity_toggle) uart_seen  <= 1'b1;
            if (frame_d    != valid_frame_toggle)   frame_seen <= 1'b1;

            if (!uart_seen) begin
                led1_divider <= 24'd0;
                led1_r <= 1'b1;
            end else if (led1_divider == 24'd12_499_999) begin
                led1_divider <= 24'd0;
                led1_r <= ~led1_r;
            end else begin
                led1_divider <= led1_divider + 1'b1;
            end

            if (!frame_seen) begin
                led0_divider <= 25'd0;
                led0_r <= 1'b1;
            end else if (led0_divider == 25'd24_999_999) begin
                led0_divider <= 25'd0;
                led0_r <= ~led0_r;
            end else begin
                led0_divider <= led0_divider + 1'b1;
            end
        end
    end
    assign led0 = led0_r;
    assign led1 = led1_r;
endmodule

// Clocking Wizard wrapper. Synthesis always uses the IP. The behavioral model
// lets the RTL testbench run before output products are generated.
module thd_clocking (
    input  wire clk_in1,
    input  wire resetn,
    output wire clk_out1,
    output wire locked
);
`ifdef SYNTHESIS
    clk_wiz_0 u_clk_wiz (
        .clk_out1(clk_out1),
        .reset(~resetn),
        .locked(locked),
        .clk_in1(clk_in1)
    );
`else
    reg sim_clk;
    reg sim_locked;
    initial begin sim_clk=1'b0; sim_locked=1'b0; end
    always #5 sim_clk = resetn ? ~sim_clk : 1'b0;
    initial begin
        wait (resetn === 1'b1);
        #100 sim_locked=1'b1;
    end
    assign clk_out1=sim_clk;
    assign locked=sim_locked;
`endif
endmodule
