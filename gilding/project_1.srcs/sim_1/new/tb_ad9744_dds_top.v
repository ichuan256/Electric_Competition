`timescale 1ns/1ps

// AD9744 顶层行为仿真测试平台。
module tb_ad9744_dds_top;
    reg sys_clk;
    reg sys_rst_n;
    reg key1_n;
    reg mcu_uart_rxd;
    wire mcu_uart_txd;
    wire dac_clk;
    wire [13:0] dac_data;
    wire dac_sleep;
    wire dac2_clk;
    wire [13:0] dac2_data;
    wire dac2_sleep;
    wire led0;
    wire led1;

    integer dac_edges;
    reg [13:0] previous_data;
    reg data_changed;
    realtime last_dac_edge;
    realtime dac_period;
    reg dac2_check_enable;
    integer dac2_high_samples;
    integer dac2_low_samples;
    reg ack_seen;

    ad9744_dds_top dut (
        .sys_clk(sys_clk),
        .sys_rst_n(sys_rst_n),
        .key1_n(key1_n),
        .mcu_uart_rxd(mcu_uart_rxd),
        .mcu_uart_txd(mcu_uart_txd),
        .dac_clk(dac_clk),
        .dac_data(dac_data),
        .dac_sleep(dac_sleep),
        .dac2_clk(dac2_clk),
        .dac2_data(dac2_data),
        .dac2_sleep(dac2_sleep),
        .led0(led0),
        .led1(led1)
    );

    task uart_send_byte;
        input [7:0] value;
        integer bit_index;
        begin
            mcu_uart_rxd = 1'b0;
            #8680;
            for (bit_index=0; bit_index<8; bit_index=bit_index+1) begin
                mcu_uart_rxd = value[bit_index];
                #8680;
            end
            mcu_uart_rxd = 1'b1;
            #8680;
        end
    endtask

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

    always @(posedge dac2_clk) begin
        if (dac2_check_enable) begin
            if (dac2_data == 14'h1000)
                dac2_high_samples = dac2_high_samples + 1;
            else if (dac2_data == 14'h3000)
                dac2_low_samples = dac2_low_samples + 1;
            else
                $fatal(1, "DAC2默认方波出现非满量程补码：%h", dac2_data);
        end
    end

    always @(negedge mcu_uart_txd)
        if (sys_rst_n) ack_seen = 1'b1;

    initial begin
        sys_rst_n    = 1'b0;
        key1_n       = 1'b1;
        mcu_uart_rxd = 1'b1;
        dac_edges    = 0;
        previous_data = 14'd0;
        data_changed = 1'b0;
        last_dac_edge = 0.0;
        dac_period = 0.0;
        dac2_check_enable = 1'b0;
        dac2_high_samples = 0;
        dac2_low_samples = 0;
        ack_seen = 1'b0;

        #200;
        sys_rst_n = 1'b1;
        #500;
        dac2_check_enable = 1'b1;
        #4500;

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
        if ((dac2_high_samples == 0) || (dac2_low_samples == 0))
            $fatal(1, "DAC2默认方波没有完成高低电平切换");

        // TARGET=1，实时单槽：DAC2改为1 MHz、半幅锯齿波；DAC1必须保持默认。
        dac2_check_enable = 1'b0;
        uart_send_byte(8'hA5); uart_send_byte(8'h5A);
        uart_send_byte(8'h01); uart_send_byte(8'h01); uart_send_byte(8'h03);
        uart_send_byte(8'h29); uart_send_byte(8'h5C);
        uart_send_byte(8'h8F); uart_send_byte(8'h02);
        uart_send_byte(8'h00); uart_send_byte(8'h00);
        uart_send_byte(8'h00); uart_send_byte(8'h00);
        uart_send_byte(8'h00); uart_send_byte(8'h10);
        uart_send_byte(8'h00); uart_send_byte(8'h80);
        uart_send_byte(8'h00); uart_send_byte(8'h00);
        uart_send_byte(8'h6B); uart_send_byte(8'h5A); uart_send_byte(8'hA5);
        #50000;

        if (dut.u_dac2.ftw !== 32'd42_949_673)
            $fatal(1, "TARGET=1未更新DAC2频率");
        if (dut.u_dac2.wave_sel !== 3'd3)
            $fatal(1, "TARGET=1未更新DAC2波形");
        if (dut.u_dac2.amplitude !== 14'd4096)
            $fatal(1, "TARGET=1未更新DAC2幅度");
        if ((dut.mix_type !== 8'h02) ||
            (dut.mix_ftw[31:0] !== 32'd42_949_673))
            $fatal(1, "TARGET=1错误修改了DAC1配置");
        if (!ack_seen)
            $fatal(1, "TARGET=1正确帧未产生UART应答");

        // TARGET=0回归：DAC1改为1 MHz满幅锯齿波；DAC2配置必须保持不变。
        ack_seen = 1'b0;
        uart_send_byte(8'hA5); uart_send_byte(8'h5A);
        uart_send_byte(8'h00); uart_send_byte(8'h01); uart_send_byte(8'h03);
        uart_send_byte(8'h29); uart_send_byte(8'h5C);
        uart_send_byte(8'h8F); uart_send_byte(8'h02);
        uart_send_byte(8'h00); uart_send_byte(8'h00);
        uart_send_byte(8'h00); uart_send_byte(8'h00);
        uart_send_byte(8'hFF); uart_send_byte(8'h1F);
        uart_send_byte(8'h00); uart_send_byte(8'h80);
        uart_send_byte(8'h00); uart_send_byte(8'h00);
        uart_send_byte(8'h9A); uart_send_byte(8'h5A); uart_send_byte(8'hA5);
        #50000;

        if ((dut.mix_type !== 8'h03) ||
            (dut.mix_ftw[31:0] !== 32'd42_949_673) ||
            (dut.mix_amp[13:0] !== 14'd8191))
            $fatal(1, "TARGET=0未保持原有DAC1配置路径");
        if ((dut.u_dac2.ftw !== 32'd42_949_673) ||
            (dut.u_dac2.wave_sel !== 3'd3) ||
            (dut.u_dac2.amplitude !== 14'd4096))
            $fatal(1, "TARGET=0错误修改了DAC2配置");
        if (!ack_seen)
            $fatal(1, "TARGET=0正确帧未产生UART应答");

        $display("仿真通过：DAC_CLK=%0.3f MHz，TARGET=0/1路由隔离且均返回应答",
                 1000.0/dac_period);
        $finish;
    end
endmodule
