`timescale 1ns/1ps

// AD9744 顶层行为仿真测试平台。
module tb_ad9744_dds_top;
    reg sys_clk;
    reg sys_rst_n;
    reg mcu_uart_rxd;
    wire mcu_uart_txd;
    wire dac_clk;
    wire [13:0] dac_data;
    wire dac_sleep;
    wire dac2_clk;
    wire [13:0] dac2_data;
    wire dac2_sleep;
    wire led0;

    integer dac_edges;
    reg [13:0] previous_data;
    reg data_changed;
    realtime last_dac_edge;
    realtime dac_period;

    ad9744_dds_top dut (
        .sys_clk(sys_clk),
        .sys_rst_n(sys_rst_n),
        .mcu_uart_rxd(mcu_uart_rxd),
        .mcu_uart_txd(mcu_uart_txd),
        .dac_clk(dac_clk),
        .dac_data(dac_data),
        .dac_sleep(dac_sleep),
        .dac2_clk(dac2_clk),
        .dac2_data(dac2_data),
        .dac2_sleep(dac2_sleep),
        .led0(led0)
    );

    initial begin
        sys_clk = 1'b0;
        forever #10 sys_clk = ~sys_clk;
    end

    always @(posedge dac_clk) begin
        dac_edges = dac_edges + 1;
        if (last_dac_edge != 0.0)
            dac_period = $realtime - last_dac_edge;
        last_dac_edge = $realtime;
        if (dac_data !== previous_data)
            data_changed = 1'b1;
        previous_data = dac_data;
    end

    initial begin
        sys_rst_n    = 1'b0;
        mcu_uart_rxd = 1'b1;
        dac_edges    = 0;
        previous_data = 14'd0;
        data_changed = 1'b0;
        last_dac_edge = 0.0;
        dac_period = 0.0;

        #200;
        sys_rst_n = 1'b1;
        #5000;

        if (dac_sleep !== 1'b0)
            $fatal(1, "dac_sleep 应保持低电平");
        if (dac2_sleep !== 1'b0)
            $fatal(1, "dac2_sleep 应保持低电平");
        if (dac_edges < 400)
            $fatal(1, "DAC 时钟未正常运行，检测到的上升沿数量不足");
        if ((dac_period < 9.9) || (dac_period > 10.1))
            $fatal(1, "DAC 时钟周期错误，期望 10 ns，实际 %0.3f ns", dac_period);
        if (!data_changed)
            $fatal(1, "DAC 数据始终未变化");

        $display("仿真通过：DAC_CLK=%0.3f MHz，默认 DDS 数据正常变化", 1000.0/dac_period);
        $finish;
    end
endmodule
