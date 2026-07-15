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
    reg [7:0] tx_payload [0:79];
    integer tx_payload_len;
    reg cfg0_toggle_before;
    reg cfg1_toggle_before;

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

    function [15:0] crc16_next;
        input [15:0] crc_in;
        input [7:0] data_in;
        integer k;
        reg [15:0] c;
        begin
            c = crc_in ^ {data_in,8'h00};
            for (k=0;k<8;k=k+1)
                c = c[15] ? ((c << 1) ^ 16'h1021) : (c << 1);
            crc16_next = c;
        end
    endfunction

    task uart_send_v2;
        input [7:0] command;
        input [15:0] sequence;
        input corrupt_crc;
        integer n;
        reg [15:0] crc;
        reg [7:0] b;
        begin
            uart_send_byte(8'hD3); uart_send_byte(8'h91);
            crc=16'hFFFF;
            b=8'h02; uart_send_byte(b); crc=crc16_next(crc,b);
            b=8'h10; uart_send_byte(b); crc=crc16_next(crc,b);
            b=8'h02; uart_send_byte(b); crc=crc16_next(crc,b);
            b=command; uart_send_byte(b); crc=crc16_next(crc,b);
            b=8'h01; uart_send_byte(b); crc=crc16_next(crc,b);
            b=sequence[7:0]; uart_send_byte(b); crc=crc16_next(crc,b);
            b=sequence[15:8]; uart_send_byte(b); crc=crc16_next(crc,b);
            b=tx_payload_len[7:0]; uart_send_byte(b); crc=crc16_next(crc,b);
            b=8'h00; uart_send_byte(b); crc=crc16_next(crc,b);
            for (n=0;n<tx_payload_len;n=n+1) begin
                uart_send_byte(tx_payload[n]);
                crc=crc16_next(crc,tx_payload[n]);
            end
            if (corrupt_crc) crc=crc^16'h0001;
            uart_send_byte(crc[7:0]); uart_send_byte(crc[15:8]);
            uart_send_byte(8'h91); uart_send_byte(8'hD3);
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
            if ((dac2_data == 14'h1000) || (dac2_data == 14'h0FFF))
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

        dac2_check_enable = 1'b0;

        // V2 CHANNEL_STAGE：暂存DAC2的1 MHz半幅锯齿波，尚不得影响活动输出。
        tx_payload_len=26;
        tx_payload[0]=8'h34; tx_payload[1]=8'h12; tx_payload[2]=8'h01;
        tx_payload[3]=8'h01; tx_payload[4]=8'h01; tx_payload[5]=8'h03;
        tx_payload[6]=0; tx_payload[7]=0; tx_payload[8]=0; tx_payload[9]=0;
        tx_payload[10]=0; tx_payload[11]=8'h04; tx_payload[12]=1; tx_payload[13]=0;
        tx_payload[14]=8'h29; tx_payload[15]=8'h5C; tx_payload[16]=8'h8F; tx_payload[17]=8'h02;
        tx_payload[18]=0; tx_payload[19]=0; tx_payload[20]=0; tx_payload[21]=0;
        tx_payload[22]=0; tx_payload[23]=8'h10; tx_payload[24]=0; tx_payload[25]=8'h80;
        uart_send_v2(8'h20,16'h0002,1'b0);
        #2500000;
        if ((dut.mix1_type!==8'h01)||(dut.mix1_ftw[31:0]!==32'd171_798_692))
            $fatal(1,"CHANNEL_STAGE提前改变了DAC2活动配置");
        if ((dut.u_mix_uart.response_status!==8'h00)||(dut.u_mix_uart.response_request_cmd!==8'h20))
            $fatal(1,"DAC2 CHANNEL_STAGE没有返回V2 OK应答");

        // V2 COMMIT：只有此帧成功后DAC2才切换。
        tx_payload_len=4; tx_payload[0]=8'h34; tx_payload[1]=8'h12;
        tx_payload[2]=8'h02; tx_payload[3]=8'h09;
        uart_send_v2(8'h21,16'h0003,1'b0);
        #2500000;
        if ((dut.mix1_type[1:0]!==2'd3)||(dut.mix1_ftw[31:0]!==32'd42_949_673)||
            (dut.mix1_amp[13:0]!==14'd4096))
            $fatal(1,"DAC2 COMMIT没有应用1 MHz半幅锯齿波");
        if ((dut.mix_type!==8'h02)||(dut.mix_ftw[31:0]!==32'd42_949_673))
            $fatal(1,"DAC2事务错误修改了DAC1");
        if ((dut.u_mix_uart.response_status!==8'h00)||(dut.u_mix_uart.response_mask!==16'h0002))
            $fatal(1,"DAC2 COMMIT应答的applied_mask错误");

        // 同一事务分别暂存两路，再用mask=3原子提交。
        tx_payload_len=26;
        tx_payload[0]=8'h22; tx_payload[1]=8'h22; tx_payload[2]=8'h00;
        tx_payload[3]=8'h01; tx_payload[4]=8'h01; tx_payload[5]=8'h03;
        tx_payload[6]=0; tx_payload[7]=0; tx_payload[8]=0; tx_payload[9]=0;
        tx_payload[10]=0; tx_payload[11]=8'h02; tx_payload[12]=1; tx_payload[13]=0;
        tx_payload[14]=8'h29; tx_payload[15]=8'h5C; tx_payload[16]=8'h8F; tx_payload[17]=8'h02;
        tx_payload[18]=0; tx_payload[19]=0; tx_payload[20]=0; tx_payload[21]=0;
        tx_payload[22]=0; tx_payload[23]=8'h08; tx_payload[24]=0; tx_payload[25]=8'h80;
        uart_send_v2(8'h20,16'h0004,1'b0); #2500000;

        tx_payload[2]=8'h01; tx_payload[11]=8'h03;
        tx_payload[14]=8'h52; tx_payload[15]=8'hB8; tx_payload[16]=8'h1E; tx_payload[17]=8'h05;
        uart_send_v2(8'h20,16'h0005,1'b0); #2500000;

        tx_payload_len=4; tx_payload[0]=8'h22; tx_payload[1]=8'h22;
        tx_payload[2]=8'h03; tx_payload[3]=8'h0B;
        uart_send_v2(8'h21,16'h0006,1'b0); #2500000;
        if ((dut.mix_type[1:0]!==2'd1)||(dut.mix1_type[1:0]!==2'd2))
            $fatal(1,"双通道COMMIT波形路由错误");
        if ((dut.mix_ftw[31:0]!==32'd42_949_673)||
            (dut.mix1_ftw[31:0]!==32'd85_899_346))
            $fatal(1,"双通道COMMIT频率配置错误");
        if (dut.u_mix_uart.response_mask!==16'h0003)
            $fatal(1,"双通道COMMIT没有返回applied_mask=3");

        // ACK丢失时Blue会用同一SEQ重发；重复COMMIT只能重发响应，不能再次切相位。
        cfg0_toggle_before=dut.u_mix_uart.cfg0_toggle;
        cfg1_toggle_before=dut.u_mix_uart.cfg1_toggle;
        uart_send_v2(8'h21,16'h0006,1'b0); #2500000;
        if ((dut.u_mix_uart.cfg0_toggle!==cfg0_toggle_before)||
            (dut.u_mix_uart.cfg1_toggle!==cfg1_toggle_before))
            $fatal(1,"重复SRC+SEQ+CMD导致COMMIT被再次执行");
        if (dut.u_mix_uart.response_status!==8'h00)
            $fatal(1,"重复COMMIT没有重发缓存ACK");

        // CRC错误不得改变活动配置，并必须返回BAD_CRC。
        tx_payload_len=4; tx_payload[0]=8'h22; tx_payload[1]=8'h22;
        tx_payload[2]=8'h03; tx_payload[3]=8'h0B;
        uart_send_v2(8'h21,16'h0007,1'b1); #2500000;
        if (dut.u_mix_uart.response_status!==8'h03)
            $fatal(1,"CRC错误没有返回BAD_CRC");
        if ((dut.mix_type[1:0]!==2'd1)||(dut.mix1_type[1:0]!==2'd2))
            $fatal(1,"CRC错误帧改变了活动输出");
        if (!ack_seen) $fatal(1,"V2请求没有产生UART响应");

        $display("仿真通过：V2暂存/提交、双DAC原子切换、CRC保护与ACK均正确，DAC_CLK=%0.3f MHz",
                 1000.0/dac_period);
        $finish;
    end
endmodule
