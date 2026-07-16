// 面向领航者 ZYNQ 的可扩展 AD9744 波形发生器。
// AD9744 模块 MODE 已上拉为高电平，dac_data 使用 14 位二进制补码。
// 综合前先运行一次 create_clock_ip.tcl，创建 50 MHz -> 100 MHz 的 clk_wiz_0。

module ad9744_dds_top (
    input  wire        sys_clk,
    input  wire        sys_rst_n,
    input  wire        key1_n,
    input  wire        mcu_uart_rxd,
    output wire        mcu_uart_txd,
    output wire        dac_clk,
    output wire [13:0] dac_data,
    output wire        dac_sleep,
    output wire        dac2_clk,
    output wire [13:0] dac2_data,
    output wire        dac2_sleep,
    output wire        ad9910_ref_clk_25m,
    output wire        led0,
    output wire        led1
);
    wire sample_clk;
    wire ref_clk_25m;
    wire clk_locked;
    wire sample_rst_n;

    ad9744_clocking u_clocking (
        .clk_out1(sample_clk),
        .clk_out4(ref_clk_25m),
        .resetn(sys_rst_n),
        .locked(clk_locked),
        .clk_in1(sys_clk)
    );

    wire        cfg_toggle_sys;
    wire [31:0] cfg_ftw_sys;
    wire [31:0] cfg_phase_sys;
    wire [13:0] cfg_amplitude_sys;
    wire signed [13:0] cfg_offset_sys;
    wire [15:0] cfg_duty_sys;
    wire [2:0]  cfg_wave_sys;
    wire        cfg_enable_sys;
    wire        cfg2_toggle_sys;
    wire [31:0] cfg2_ftw_sys;
    wire [31:0] cfg2_phase_sys;
    wire [13:0] cfg2_amplitude_sys;
    wire signed [13:0] cfg2_offset_sys;
    wire [15:0] cfg2_duty_sys;
    wire [2:0]  cfg2_wave_sys;
    wire        cfg2_enable_sys;
    wire        uart_activity;
    wire v2_frame_ok_toggle_sys;
    wire mix0_cfg_toggle_sys, mix1_cfg_toggle_sys;
    wire mix_commit_toggle_sys;
    wire [1:0] mix_commit_mask_sys;
    wire [3:0] mix_commit_flags_sys;
    wire mix_commit_applied_sys;
    wire [7:0] mix0_type_sys, mix1_type_sys;
    wire [127:0] mix0_ftw_sys, mix0_phase_sys;
    wire [127:0] mix1_ftw_sys, mix1_phase_sys;
    wire [55:0] mix0_amp_sys, mix1_amp_sys;
    wire [63:0] mix0_duty_sys, mix1_duty_sys;
    wire signed [13:0] mix0_offset_sys, mix1_offset_sys;
    wire mix0_cache_mode_sys, mix1_cache_mode_sys;
    wire [12:0] mix0_cache_points_sys, mix1_cache_points_sys;
    wire mix0_enable_sys, mix1_enable_sys;

    uart_v2_dual_config #(.CLK_HZ(50_000_000), .BAUD(115_200)) u_mix_uart (
        .clk(sys_clk), .rst_n(sys_rst_n), .uart_rx(mcu_uart_rxd),
        .uart_tx(mcu_uart_txd), .activity(uart_activity),
        .frame_ok_toggle(v2_frame_ok_toggle_sys),
        .cfg0_toggle(mix0_cfg_toggle_sys), .cfg1_toggle(mix1_cfg_toggle_sys),
        .commit_toggle(mix_commit_toggle_sys),
        .commit_mask(mix_commit_mask_sys), .commit_flags(mix_commit_flags_sys),
        .commit_applied_toggle(mix_commit_applied_sys),
        .type0_flat(mix0_type_sys), .ftw0_flat(mix0_ftw_sys),
        .phase0_flat(mix0_phase_sys), .amp0_flat(mix0_amp_sys),
        .duty0_flat(mix0_duty_sys), .offset0(mix0_offset_sys),
        .cache0_mode(mix0_cache_mode_sys), .cache0_points(mix0_cache_points_sys),
        .enable0(mix0_enable_sys),
        .type1_flat(mix1_type_sys), .ftw1_flat(mix1_ftw_sys),
        .phase1_flat(mix1_phase_sys), .amp1_flat(mix1_amp_sys),
        .duty1_flat(mix1_duty_sys), .offset1(mix1_offset_sys),
        .cache1_mode(mix1_cache_mode_sys), .cache1_points(mix1_cache_points_sys),
        .enable1(mix1_enable_sys)
    );

    // 保留旧单命令模块的默认寄存器，实际UART由多波形协议解析器统一接收。
    wire legacy_uart_unused, legacy_activity_unused;
    uart_config #(.CLK_HZ(50_000_000), .BAUD(115_200)) u_legacy_defaults (
        .clk(sys_clk), .rst_n(sys_rst_n), .uart_rx(1'b1),
        .uart_tx(legacy_uart_unused), .activity(legacy_activity_unused),
        .cfg_toggle(cfg_toggle_sys), .ftw(cfg_ftw_sys),
        .phase_offset(cfg_phase_sys), .amplitude(cfg_amplitude_sys),
        .dc_offset(cfg_offset_sys), .duty(cfg_duty_sys),
        .wave_sel(cfg_wave_sys), .output_enable(cfg_enable_sys),
        .cfg2_toggle(cfg2_toggle_sys), .ftw2(cfg2_ftw_sys),
        .phase_offset2(cfg2_phase_sys), .amplitude2(cfg2_amplitude_sys),
        .dc_offset2(cfg2_offset_sys), .duty2(cfg2_duty_sys),
        .wave_sel2(cfg2_wave_sys), .output_enable2(cfg2_enable_sys)
    );

    reg [1:0] reset_sync;
    always @(posedge sample_clk or negedge sys_rst_n) begin
        if (!sys_rst_n) reset_sync <= 2'b00;
        else            reset_sync <= {reset_sync[0], clk_locked};
    end
    assign sample_rst_n = reset_sync[1];

    // 两个DAC共享一个提交令牌。单一同步链保证双通道配置只能在同一个
    // 100 MHz采样边沿切换，避免两条独立同步链带来一拍的不确定差异。
    (* ASYNC_REG = "TRUE" *) reg [2:0] mix_commit_sync;
    reg [7:0] mix_type;
    reg [127:0] mix_ftw, mix_phase;
    reg [55:0] mix_amp;
    reg [63:0] mix_duty;
    reg signed [13:0] mix_offset;
    reg mix_cache_mode;
    reg [12:0] mix_cache_points;
    reg mix_enable;
    reg mix_update;
    reg mix_phase_reset;
    reg [7:0] mix1_type;
    reg [127:0] mix1_ftw, mix1_phase;
    reg [55:0] mix1_amp;
    reg [63:0] mix1_duty;
    reg signed [13:0] mix1_offset;
    reg mix1_cache_mode;
    reg [12:0] mix1_cache_points;
    reg mix1_enable;
    reg mix1_update;
    reg mix1_phase_reset;
    reg mix_commit_applied_sample;
    always @(posedge sample_clk or negedge sample_rst_n) begin
        if (!sample_rst_n) begin
            mix_commit_sync <= 3'b000;
            mix_update <= 1'b0;
            mix_phase_reset <= 1'b0;
            mix1_update <= 1'b0;
            mix1_phase_reset <= 1'b0;
            mix_commit_applied_sample <= 1'b0;
            // 上电诊断默认值：槽位0输出1 MHz满幅三角波，其余槽位关闭。
            mix_type <= 8'h02;
            mix_ftw <= {96'd0,32'd42_949_673};
            mix_phase <= 128'd0;
            mix_amp <= {42'd0,14'd8191};
            mix_duty <= {4{16'h8000}};
            mix_offset <= 14'sd0;
            mix_cache_mode <= 1'b0;
            mix_cache_points <= 13'd0;
            mix_enable <= 1'b1;
            // DAC2上电诊断默认值：槽位0输出4 MHz半幅方波。
            mix1_type <= 8'h01;
            mix1_ftw <= {96'd0,32'd171_798_692};
            mix1_phase <= 128'd0;
            mix1_amp <= {42'd0,14'd4096};
            mix1_duty <= {4{16'h8000}};
            mix1_offset <= 14'sd0;
            mix1_cache_mode <= 1'b0;
            mix1_cache_points <= 13'd0;
            mix1_enable <= 1'b1;
        end else begin
            mix_commit_sync <= {mix_commit_sync[1:0],mix_commit_toggle_sys};
            mix_update <= 1'b0;
            mix_phase_reset <= 1'b0;
            mix1_update <= 1'b0;
            mix1_phase_reset <= 1'b0;
            if (mix_commit_sync[2] != mix_commit_sync[1]) begin
                // commit_flags.bit0只在显式同步启动时清零相位；普通0x08/0x0A
                // 提交仅切换参数，连续相位累加器继续运行。
                if (mix_commit_mask_sys[0]) begin
                mix_type <= mix0_type_sys; mix_ftw <= mix0_ftw_sys;
                mix_phase <= mix0_phase_sys; mix_amp <= mix0_amp_sys;
                mix_duty <= mix0_duty_sys; mix_offset <= mix0_offset_sys;
                mix_cache_mode <= mix0_cache_mode_sys;
                mix_cache_points <= mix0_cache_points_sys;
                mix_enable <= mix0_enable_sys;
                    mix_update <= 1'b1;
                    mix_phase_reset <= mix_commit_flags_sys[0];
                end
                if (mix_commit_mask_sys[1]) begin
                mix1_type <= mix1_type_sys; mix1_ftw <= mix1_ftw_sys;
                mix1_phase <= mix1_phase_sys; mix1_amp <= mix1_amp_sys;
                mix1_duty <= mix1_duty_sys; mix1_offset <= mix1_offset_sys;
                mix1_cache_mode <= mix1_cache_mode_sys;
                mix1_cache_points <= mix1_cache_points_sys;
                mix1_enable <= mix1_enable_sys;
                    mix1_update <= 1'b1;
                    mix1_phase_reset <= mix_commit_flags_sys[0];
                end
                mix_commit_applied_sample <= ~mix_commit_applied_sample;
            end
        end
    end

    // 将采样域“已应用”翻转同步回UART域，ACK只在配置真正进入采样域后返回。
    (* ASYNC_REG = "TRUE" *) reg [2:0] mix_commit_applied_sync;
    always @(posedge sys_clk or negedge sys_rst_n) begin
        if (!sys_rst_n) mix_commit_applied_sync <= 3'b000;
        else            mix_commit_applied_sync <= {mix_commit_applied_sync[1:0],
                                                    mix_commit_applied_sample};
    end
    assign mix_commit_applied_sys = mix_commit_applied_sync[2];

    wire [13:0] mixed_dac_data;
    (* MARK_DEBUG = "TRUE" *) wire mix_clipping;
    (* MARK_DEBUG = "TRUE" *) wire [31:0] mix_clip_count;
    ad9744_multiwave_mixer u_multiwave_mixer (
        .clk(sample_clk), .rst_n(sample_rst_n),
        .restart(mix_phase_reset),
        .type_flat(mix_type), .ftw_flat(mix_ftw), .phase_flat(mix_phase),
        .amp_flat(mix_amp), .duty_flat(mix_duty), .dc_offset(mix_offset),
        .dac_data(mixed_dac_data), .clipping(mix_clipping), .clip_count(mix_clip_count)
    );

    wire [13:0] cached_dac_data;
    wire cache_active;
    (* MARK_DEBUG = "TRUE" *) wire cache_clipping;
    (* MARK_DEBUG = "TRUE" *) wire [31:0] cache_clip_count;
    ad9744_period_cache u_period_cache (
        .clk(sample_clk), .rst_n(sample_rst_n), .restart(mix_update),
        .enable_request(mix_cache_mode), .period_points(mix_cache_points),
        .type_flat(mix_type), .ftw_flat(mix_ftw), .phase_flat(mix_phase),
        .amp_flat(mix_amp), .duty_flat(mix_duty), .dc_offset(mix_offset),
        .cache_active(cache_active), .dac_data(cached_dac_data),
        .clipping(cache_clipping), .clip_count(cache_clip_count)
    );

    // DAC2复用与DAC1相同的四槽实时混合器和双Bank周期缓存。
    wire [13:0] mixed_dac2_data;
    (* MARK_DEBUG = "TRUE" *) wire mix1_clipping;
    (* MARK_DEBUG = "TRUE" *) wire [31:0] mix1_clip_count;
    ad9744_multiwave_mixer u_multiwave_mixer_dac2 (
        .clk(sample_clk), .rst_n(sample_rst_n), .restart(mix1_phase_reset),
        .type_flat(mix1_type), .ftw_flat(mix1_ftw), .phase_flat(mix1_phase),
        .amp_flat(mix1_amp), .duty_flat(mix1_duty), .dc_offset(mix1_offset),
        .dac_data(mixed_dac2_data), .clipping(mix1_clipping), .clip_count(mix1_clip_count)
    );

    wire [13:0] cached_dac2_data;
    wire cache2_active;
    (* MARK_DEBUG = "TRUE" *) wire cache2_clipping;
    (* MARK_DEBUG = "TRUE" *) wire [31:0] cache2_clip_count;
    ad9744_period_cache u_period_cache_dac2 (
        .clk(sample_clk), .rst_n(sample_rst_n), .restart(mix1_update),
        .enable_request(mix1_cache_mode), .period_points(mix1_cache_points),
        .type_flat(mix1_type), .ftw_flat(mix1_ftw), .phase_flat(mix1_phase),
        .amp_flat(mix1_amp), .duty_flat(mix1_duty), .dc_offset(mix1_offset),
        .cache_active(cache2_active), .dac_data(cached_dac2_data),
        .clipping(cache2_clipping), .clip_count(cache2_clip_count)
    );

    // 更新翻转信号跨时钟域期间，多位配置总线保持稳定。
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
            ftw             <= 32'd42_949_673; // 100 MHz 采样时钟下输出 1 MHz
            phase_offset    <= 32'd0;
            amplitude       <= 14'd8191;
            dc_offset       <= 14'sd0;
            duty            <= 16'h8000;
            wave_sel        <= 3'd0;
            output_enable   <= 1'b1;
        end else begin
            cfg_toggle_sync <= {cfg_toggle_sync[1:0], cfg_toggle_sys};
            if (cfg_toggle_sync[2] != cfg_toggle_sync[1]) begin
                ftw           <= cfg_ftw_sys;
                phase_offset  <= cfg_phase_sys;
                amplitude     <= cfg_amplitude_sys;
                dc_offset     <= cfg_offset_sys;
                duty          <= cfg_duty_sys;
                wave_sel      <= cfg_wave_sys;
                output_enable <= cfg_enable_sys;
            end
        end
    end

    reg [31:0] phase_acc;
    reg signed [14:0] wave_raw;
    reg signed [29:0] product;
    // 强制把 DAC 输出寄存器放入 IOB，缩短寄存器到封装管脚的延迟。
    (* IOB = "TRUE" *) reg [13:0] dac_data_r;
    wire [31:0] phase = phase_acc + phase_offset;
    wire [7:0] sine_addr = phase[31:24];
    wire signed [14:0] sine_value = $signed({1'b0, sine_lut_256(sine_addr)}) - 15'sd8192;
    wire signed [14:0] saw_value = $signed({1'b0, phase[31:18]}) - 15'sd8192;
    wire signed [15:0] tri_up = -16'sd8192 + $signed({1'b0, phase[30:18], 1'b0});
    wire signed [15:0] tri_value = phase[31] ? -tri_up - 16'sd1 : tri_up;
    wire signed [14:0] square_value = (phase[31:16] < duty) ? 15'sd8191 : -15'sd8192;
    wire signed [15:0] scaled_value = product >>> 13;
    wire signed [15:0] sum_value = scaled_value + dc_offset;
    wire signed [15:0] limited_value =
        (sum_value > 16'sd8191)  ? 16'sd8191 :
        (sum_value < -16'sd8192) ? -16'sd8192 : sum_value;

    always @* begin
        case (wave_sel)
            3'd0: wave_raw = sine_value;
            3'd1: wave_raw = square_value;
            3'd2: wave_raw = tri_value[14:0];
            3'd3: wave_raw = saw_value;
            default: wave_raw = sine_value; // 为 BRAM/任意波形引擎预留扩展入口
        endcase
    end

    // 流水线：数据在 sample_clk 上升沿更新；半个采样周期后，DAC 在转发时钟
    // 上升沿采样，从而满足 AD9744 的建立时间和保持时间要求。
    always @(posedge sample_clk or negedge sample_rst_n) begin
        if (!sample_rst_n) begin
            phase_acc  <= 32'd0;
            product    <= 30'sd0;
            dac_data_r <= 14'd0;
        end else begin
            phase_acc <= phase_acc + ftw;
            product   <= wave_raw * $signed({1'b0, amplitude});
            dac_data_r <= output_enable ? limited_value[13:0] : 14'd0;
        end
    end

    wire forwarded_clk;
    ODDR #(.DDR_CLK_EDGE("SAME_EDGE"), .INIT(1'b0), .SRTYPE("SYNC")) u_dac_clk_oddr (
        .Q(forwarded_clk), .C(sample_clk), .CE(1'b1),
        .D1(1'b0), .D2(1'b1), .R(1'b0), .S(1'b0)
    );

    // 模式选择后再经过统一IOB寄存器，避免BRAM/DDS选择多路器直接落到管脚。
    (* IOB = "TRUE" *) reg [13:0] dac_output_r;
    always @(posedge sample_clk or negedge sample_rst_n) begin
        if (!sample_rst_n) dac_output_r <= 14'd0;
        else               dac_output_r <= mix_enable
                                             ? (cache_active ? cached_dac_data : mixed_dac_data)
                                             : 14'd0;
    end
    wire forwarded_clk2;
    ODDR #(.DDR_CLK_EDGE("SAME_EDGE"), .INIT(1'b0), .SRTYPE("SYNC")) u_dac2_clk_oddr (
        .Q(forwarded_clk2), .C(sample_clk), .CE(1'b1),
        .D1(1'b0), .D2(1'b1), .R(1'b0), .S(1'b0)
    );

    // 与DAC采样时钟同源的25 MHz参考时钟。使用Clocking Wizard的专用输出
    // 和ODDR转发到管脚，避免用普通逻辑分频带来的占空比及布线抖动。
    wire forwarded_ref_clk_25m;
    ODDR #(.DDR_CLK_EDGE("SAME_EDGE"), .INIT(1'b0), .SRTYPE("SYNC")) u_ad9910_ref_clk_oddr (
        .Q(forwarded_ref_clk_25m), .C(ref_clk_25m), .CE(clk_locked),
        .D1(1'b1), .D2(1'b0), .R(1'b0), .S(1'b0)
    );

    (* IOB = "TRUE" *) reg [13:0] dac2_output_r;
    always @(posedge sample_clk or negedge sample_rst_n) begin
        if (!sample_rst_n) dac2_output_r <= 14'd0;
        else               dac2_output_r <= mix1_enable
                                              ? (cache2_active ? cached_dac2_data : mixed_dac2_data)
                                              : 14'd0;
    end

    assign dac_clk    = forwarded_clk;
    assign dac_data   = dac_output_r;
    assign dac_sleep  = ~mix_enable;
    assign dac2_clk   = forwarded_clk2;
    assign dac2_data  = dac2_output_r;
    assign dac2_sleep = ~mix1_enable;
    assign ad9910_ref_clk_25m = forwarded_ref_clk_25m;
    // LED0/LED1均为低电平点亮，下面由诊断状态机驱动。

    // UART物理接收诊断：收到任意一个完整UART字节后，LED1永久以1 Hz闪烁。
    // 此指示不要求帧头、长度、异或校验或帧尾正确，可用于定位问题是在
    // 串口物理层之前，还是在新协议解析阶段。
    // 当前行为：仅完整配置帧成功提交后，LED1锁存为亮。
    reg uart_activity_d;
    reg frame_ok_toggle_d;
    reg uart_seen;
    reg frame_ok_seen;
    reg [23:0] led1_divider;
    reg [24:0] led0_divider;
    reg led1_r;
    reg led0_r;
    always @(posedge sys_clk or negedge sys_rst_n) begin
        if (!sys_rst_n) begin
            uart_activity_d <= 1'b0;
            frame_ok_toggle_d <= 1'b0;
            uart_seen <= 1'b0;
            frame_ok_seen <= 1'b0;
            led1_divider <= 24'd0;
            led0_divider <= 25'd0;
            led1_r <= 1'b1;
            led0_r <= 1'b1;
        end else begin
            uart_activity_d <= uart_activity;
            frame_ok_toggle_d <= v2_frame_ok_toggle_sys;

            // 任意一个UART字节被接收器识别后，LED1开始持续闪烁。
            if (uart_activity != uart_activity_d)
                uart_seen <= 1'b1;
            // 完整帧成功校验并提交（已处理结束帧）后，LED0开始持续闪烁。
            if (v2_frame_ok_toggle_sys != frame_ok_toggle_d)
                frame_ok_seen <= 1'b1;

            // LED1约2 Hz闪烁：每0.25秒翻转一次。
            if (!uart_seen) begin
                led1_divider <= 24'd0;
                led1_r <= 1'b1;
            end else if (led1_divider == 24'd12_499_999) begin
                led1_divider <= 24'd0;
                led1_r <= ~led1_r;
            end else begin
                led1_divider <= led1_divider + 1'b1;
            end

            // PS_LED0约1 Hz闪烁：每0.5秒翻转一次。
            if (!frame_ok_seen) begin
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

// 时钟封装：综合时必须调用 Clocking Wizard IP；行为仿真时使用等效模型，
// 避免尚未生成 IP 输出产品时出现“找不到 clk_wiz_0”展开错误。
module ad9744_clocking (
    input  wire clk_in1,
    input  wire resetn,
    output wire clk_out1,
    output wire clk_out4,
    output wire locked
);
`ifdef SYNTHESIS
    clk_wiz_0 u_clk_wiz (
        .clk_out1(clk_out1),
        .clk_out4(clk_out4),
        .reset(~resetn),
        .locked(locked),
        .clk_in1(clk_in1)
    );
`else
    reg sim_clk;
    reg sim_clk25;
    reg sim_locked;

    initial begin
        sim_clk    = 1'b0;
        sim_clk25  = 1'b0;
        sim_locked = 1'b0;
    end

    always #5 sim_clk = resetn ? ~sim_clk : 1'b0;
    always #20 sim_clk25 = resetn ? ~sim_clk25 : 1'b0;

    initial begin
        wait (resetn === 1'b1);
        #100;
        sim_locked = 1'b1;
        wait (resetn === 1'b0);
        sim_locked = 1'b0;
    end

    assign clk_out1 = sim_clk;
    assign clk_out4 = sim_clk25;
    assign locked   = sim_locked;
`endif
endmodule

// UART 帧：A5、命令、data[7:0]、data[15:8]、data[23:16]、data[31:24]、异或校验。
// 应答帧：5A、命令、状态(00)、异或校验。仅在异或校验正确后提交配置。
module uart_config #(parameter CLK_HZ=50_000_000, BAUD=115_200) (
    input wire clk, input wire rst_n, input wire uart_rx, output wire uart_tx,
    output reg activity, output reg cfg_toggle, output reg [31:0] ftw,
    output reg [31:0] phase_offset, output reg [13:0] amplitude,
    output reg signed [13:0] dc_offset, output reg [15:0] duty,
    output reg [2:0] wave_sel, output reg output_enable,
    output reg cfg2_toggle, output reg [31:0] ftw2,
    output reg [31:0] phase_offset2, output reg [13:0] amplitude2,
    output reg signed [13:0] dc_offset2, output reg [15:0] duty2,
    output reg [2:0] wave_sel2, output reg output_enable2
);
    wire rx_valid; wire [7:0] rx_byte; wire tx_busy;
    reg tx_start; reg [7:0] tx_byte; reg [2:0] index; reg [7:0] command;
    reg [31:0] payload; reg [7:0] checksum; reg [1:0] ack_index; reg ack_pending;
    uart_rx_byte #(.CLK_HZ(CLK_HZ),.BAUD(BAUD)) u_rx(clk,rst_n,uart_rx,rx_valid,rx_byte);
    uart_tx_byte #(.CLK_HZ(CLK_HZ),.BAUD(BAUD)) u_tx(clk,rst_n,tx_start,tx_byte,uart_tx,tx_busy);
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            index<=0; command<=0; payload<=0; checksum<=0; tx_start<=0; tx_byte<=0;
            ack_index<=0; ack_pending<=0; activity<=0; cfg_toggle<=0;
            ftw<=32'd42_949_673; phase_offset<=0; amplitude<=14'd8191;
            dc_offset<=0; duty<=16'h8000; wave_sel<=0; output_enable<=1;
            // DAC2上电默认4 MHz方波；与ad9744_dds_ch2内部复位值保持一致。
            cfg2_toggle<=0; ftw2<=32'd171_798_692; phase_offset2<=0;
            amplitude2<=14'd4096; dc_offset2<=0; duty2<=16'h8000;
            wave_sel2<=3'd1; output_enable2<=1;
        end else begin
            tx_start <= 0;
            if (rx_valid) begin
                activity <= ~activity;
                if (index==0) begin
                    if (rx_byte==8'hA5) begin index<=1; checksum<=8'hA5; end
                end else if (index==1) begin command<=rx_byte; checksum<=checksum^rx_byte; index<=2; end
                else if (index>=2 && index<=5) begin
                    case(index) 2:payload[7:0]<=rx_byte; 3:payload[15:8]<=rx_byte;
                                4:payload[23:16]<=rx_byte; default:payload[31:24]<=rx_byte; endcase
                    checksum<=checksum^rx_byte; index<=index+1'b1;
                end else begin
                    index<=0;
                    if (rx_byte==checksum) begin
                        case(command)
                            8'h01: ftw<=payload;
                            8'h02: phase_offset<=payload;
                            8'h03: amplitude<=payload[13:0];
                            8'h04: dc_offset<=payload[13:0];
                            8'h05: duty<=payload[15:0];
                            8'h06: wave_sel<=payload[2:0];
                            8'h07: output_enable<=payload[0];
                            8'h81: ftw2<=payload;
                            8'h82: phase_offset2<=payload;
                            8'h83: amplitude2<=payload[13:0];
                            8'h84: dc_offset2<=payload[13:0];
                            8'h85: duty2<=payload[15:0];
                            8'h86: wave_sel2<=payload[2:0];
                            8'h87: output_enable2<=payload[0];
                            default: ;
                        endcase
                        if (command>=8'h01 && command<=8'h07) cfg_toggle<=~cfg_toggle;
                        if (command>=8'h81 && command<=8'h87) cfg2_toggle<=~cfg2_toggle;
                        ack_pending<=1; ack_index<=0;
                    end
                end
            end
            if (ack_pending && !tx_busy && !tx_start) begin
                tx_start<=1;
                case(ack_index) 0:tx_byte<=8'h5A; 1:tx_byte<=command; 2:tx_byte<=8'h00;
                                default:tx_byte<=8'h5A^command; endcase
                if (ack_index==3) begin ack_pending<=0; ack_index<=0; end
                else ack_index<=ack_index+1'b1;
            end
        end
    end
endmodule

module uart_rx_byte #(parameter CLK_HZ=50_000_000, BAUD=115_200) (
    input wire clk,input wire rst_n,input wire rx,output reg valid,output reg [7:0] data);
    localparam integer DIV=CLK_HZ/BAUD; reg [1:0] sync; reg busy; reg [15:0] count; reg [3:0] bitno; reg [7:0] shift;
    always @(posedge clk or negedge rst_n) begin
        if(!rst_n) begin sync<=3;busy<=0;count<=0;bitno<=0;shift<=0;valid<=0;data<=0;end else begin
            sync<={sync[0],rx}; valid<=0;
            if(!busy && sync[1] && !sync[0]) begin busy<=1;count<=DIV+DIV/2-1;bitno<=0;end
            else if(busy && count!=0) count<=count-1'b1;
            else if(busy) begin
                if(bitno<8) begin shift[bitno]<=sync[1];bitno<=bitno+1'b1;count<=DIV-1;end
                else begin busy<=0;if(sync[1]) begin data<=shift;valid<=1;end end
            end
        end
    end
endmodule

module uart_tx_byte #(parameter CLK_HZ=50_000_000, BAUD=115_200) (
    input wire clk,input wire rst_n,input wire start,input wire [7:0] data,
    output reg tx,output reg busy);
    localparam integer DIV=CLK_HZ/BAUD; reg [15:0] count; reg [3:0] bitno; reg [9:0] frame;
    always @(posedge clk or negedge rst_n) begin
        if(!rst_n) begin tx<=1;busy<=0;count<=0;bitno<=0;frame<=0;end else begin
            if(start&&!busy) begin frame<={1'b1,data,1'b0};tx<=0;busy<=1;count<=DIV-1;bitno<=0;end
            else if(busy&&count!=0) count<=count-1'b1;
            else if(busy) begin
                if(bitno==9) begin tx<=1;busy<=0;end
                else begin bitno<=bitno+1'b1;tx<=frame[bitno+1'b1];count<=DIV-1;end
            end
        end
    end
endmodule
