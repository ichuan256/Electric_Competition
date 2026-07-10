#include "AD9910_User.h"

uint8_t dds_mode = DDS_SINE;  // 当前DDS模式，切换模式时需重新初始化
float dds_factor=1.0f;

// 复数增益数组
// 因为需要考虑频谱镜像位置，所以长度为 2048 * 2（FFT 对称部分）
// 复数包含实部和虚部，因此再乘以 2
float study_gain[4096 * 2] = {0.0f};
// 幅度增益数组
// 理论上只需约 2050 个点，这里取 2100 以保证冗余
float study_amp[2100];

/**
 * @brief DDS输出正弦波
 *
 * @param hz     输出频率（Hz）
 * @param factor 幅值因子（0~1，预留，暂未使用）
 * @param mvpp   目标输出峰峰电压（mV）
 *
 * 说明：
 *   - 幅度计算公式：Amp = max_asf * mvpp / FULL_SCALE_MVPP
 *   - FULL_SCALE_MVPP 为本板实测满量程电压（ASF=0x3FFF 时的输出电压）
 *   - 当前板实测满量程约 780mVpp，故常数值需按实际硬件标定
 *   - 公式中的 3572.3 为原始参考值（对应约 5 倍后端放大的标准板），已按本板修正
 *   - ASF 为 14 位，占用寄存器 [13:0]，范围 0x0000~0x3FFF
 */
void dds_output_sine(uint32_t hz, float factor, uint32_t mvpp)
{
    // 检查模式是否为正弦波，不是则初始化
    if (dds_mode != DDS_SINE)
    {
        AD9910_Init_1();
        AD9910_Singal_Profile_Init_1();
        dds_mode = DDS_SINE;
    }

    // 最大幅值ASF寄存器
    uint16_t max_asf = 0x3FFF;

    // 本板满量程约780mVpp（ASF=0x3FFF时），下方常数需按实际板子标定
    uint16_t Amp = (uint16_t)(max_asf * mvpp / 780.0f/92*100);
    if (Amp > max_asf) Amp = max_asf;  // 防溢出，ASF 仅 14 位 [13:0]

    // 设置DDS信号输出
    AD9910_Singal_Profile_Set_1(0, hz, Amp, 0);

    // 更新DDS寄存器
    AD9910_IUP();
}


/**
 * 获取目标幅度与相位差，同时计算复增益
 *
 * 参数：
 *  target_hz  : 目标频率（Hz）
 *  dds_amp    : DDS信号幅值（mV）
 *  tar_amp    : 输入信号幅值（mV）
 *  phase_diff : 输入与DDS信号相位差（rad）
 *  re, im     : 复增益实部和虚部
 *  fft_point  : FFT点数
 *  sample_rate: 采样率（Hz）
 *
 * 功能：
 * 1. ADC采样两路信号（输入信号和DDS反馈信号）
 * 2. 对两路信号FFT
 * 3. 根据目标频率找到对应FFT bin
 * 4. 计算幅值、相位差以及复增益
 */
//void get_target_amplitude_and_phase(
//    float target_hz,
//    float *dds_amp,
//    float *tar_amp,
//    float *phase_diff,
//    float *re,
//    float *im,
//    uint32_t fft_point,
//    uint32_t sample_rate
//)
//{
//    // 采样输入信号（偏置1250 mV）
//    start_adc(sample_rate, fft_point);
//    for (uint32_t i = 0; i < fft_point; i++)
//    {
//        adc_value[2 * i]     = (((float)adc_buf[i] / 65536.0f) * 3300.0f - 1250.0f); // 采样值 -> mV
//        adc_value[2 * i + 1] = 0.0f; // 虚部置0
//    }

//    // 采样DDS反馈信号（偏置1660 mV）
//    for (uint32_t i = 0; i < fft_point; i++)
//    {
//        adc_value2[2 * i]     = ((float)adc_buf2[i] / 65536.0f) * 3300.0f - 1660.0f;
//        adc_value2[2 * i + 1] = 0.0f;
//    }

//    // FFT计算
//    if (fft_point == 4096)
//    {
//        arm_cfft_f32(&cfft_s_f32_4096, adc_value, 0, 1);
//        arm_cfft_f32(&cfft_s_f32_4096, adc_value2, 0, 1);
//    }
//    else if (fft_point == 1024)
//    {
//        arm_cfft_f32(&cfft_s_f32_1024, adc_value, 0, 1);
//        arm_cfft_f32(&cfft_s_f32_1024, adc_value2, 0, 1);
//    }

//    // 根据目标频率计算FFT对应的bin
//    uint32_t bin_index = (target_hz * fft_point + sample_rate / 2) / sample_rate;

//    // 计算输入信号幅值
//    *tar_amp = calc_amplitude(bin_index, fft_point, adc_value, 1); // 1-bin幅值

//    // 计算相位差（输入 vs DDS信号）
//    *phase_diff = calc_phase_diff(bin_index, fft_point, adc_value, adc_value2);

//    // 计算复增益
//    calc_compledx_gain(bin_index, fft_point, adc_value, adc_value2, re, im);

//    // 计算DDS信号幅值
//    *dds_amp = calc_amplitude(bin_index, fft_point, adc_value2, 1); // 1-bin幅值
//}

/**
 * @brief 扫频测量函数
 *
 * 1. 扫描两个频率区间：
 *    - 200 Hz ~ 10 kHz：100 Hz 步进，细分采样，获得更精细的低频幅频特性
 *    - 10 kHz ~ 400 kHz：200 Hz 步进，快速扫描高频段
 * 2. 每个频点采集两次幅值与相位，取平均值，计算幅度增益和复数增益
 * 3. 结果存入 study_amp[]（幅值增益）和 study_gain[]（复数增益，带相位）
 * 4. 最后调用 analyze_filter_type_full() 自动判断滤波器类型
 */
//void sweep_fre()
//{
//    uint32_t freq = 100;
//    float dds_amp = 0.0f, tar_amp = 0.0f, phase = 0.0f;
//    float re = 0.0f, im = 0.0f;
//    uint32_t study_amp_index = 0; // 幅频特性数组索引
//    uint32_t sutdy_gain_index = 2; // 复增益数组索引（跳过直流项）
//    // 实际 DDS 输出是 1500 mVpp，但因前端有 2 倍放大和 50 欧匹配电路
//    // 因此此处初始设置为 750 mVpp
//    uint32_t sweep_mvpp = 750;
//    uint8_t gain = 1;

//    // --------------------------------------------------------------------
//    // 第一段：200 Hz ~ 10 kHz，100 Hz 步进，细扫低频
//    // --------------------------------------------------------------------
//    /*
//        设计原因：
//        在 200 Hz ~ 10 kHz 范围内，以 100 Hz 步进进行更密集的采样，
//        以便更精细地刻画低频段的幅度增益曲线；
//        从 10 kHz 开始改为 200 Hz 步进以加快高频段扫描速度。
//    */
//    for (; freq < 10000;)
//    {
//        uint8_t count = 0;

//        dds_output_sine(freq, dds_factor, sweep_mvpp);
//        HAL_Delay(100);

//        float re_sum = 0, im_sum = 0, tar_amp_sum = 0;
//        float dds_amp_sum = 0;

//        // -------------------------------
//        // 下面是 PGA 增益调节逻辑（未启用）
//        // -------------------------------
////      gain = calc_gain(sweep_mvpp);
////      set_input_gain(gain);
////      uart0("set gain %d\r\n",gain);

//        // 采集一次幅值与相位
//        //get_target_amplitude_and_phase(freq, &dds_amp, &tar_amp, &phase, &re, &im, 4096, 409600);
//        dds_amp *= 2;
//        tar_amp *= 2;
//        // ↑ 因为 DDS 输出分两路后放大 2 倍，实际采到的值需要乘 2

//        // -------------------------------
//        // 动态调整输出幅度逻辑（未启用）
//        // 目的是保持测量信号在合适幅度范围，提高测量精度
//        // -------------------------------
////      if (tar_amp > 900) { ... }
////      else if (tar_amp >= 400) { ... }
////      else if (tar_amp > 100) { ... }
////      else { ... }

//        // 重复采集两次，累加求平均
//        for (uint8_t i = 0; i < 2; i++)
//        {
//            count++;
//            //get_target_amplitude_and_phase(freq, &dds_amp, &tar_amp, &phase, &re, &im, 4096, 409600);
//            dds_amp_sum += (dds_amp * 2 / gain);
//            tar_amp *= 2;
//            re_sum += (re / gain);
//            im_sum += (im / gain);
//            tar_amp_sum += tar_amp;
//        }

//        // 平均处理
//        re = re_sum / count;
//        im = im_sum / count;
//        tar_amp = tar_amp_sum / count;
//        dds_amp = dds_amp_sum / count;

//        // 每隔 200 Hz 存一次复数增益（共轭对称位置同时写入）
//        if (freq % 200 == 0)
//        {
//            study_gain[sutdy_gain_index] = re;
//            study_gain[sutdy_gain_index + 1] = im;
//            study_gain[4096 * 2 - sutdy_gain_index] = re;
//            study_gain[4096 * 2 - sutdy_gain_index + 1] = -im;
//            sutdy_gain_index += 2;
//        }

//        // 记录幅值增益
//        study_amp[study_amp_index++] = tar_amp / dds_amp;

//        freq += 100; // 步进 100 Hz
//    }

//    // --------------------------------------------------------------------
//    // 第二段：10.2 kHz ~ 400 kHz，200 Hz 步进，快速扫高频
//    // --------------------------------------------------------------------
//    for (freq = 10200; freq <= 400000;)
//    {
//        dds_output_sine(freq, dds_factor, sweep_mvpp);
//        HAL_Delay(2);

//        float re_sum = 0, im_sum = 0, tar_amp_sum = 0;
//        float dds_amp_sum = 0;
//        uint8_t gain = 1;
//        uint8_t count = 0;

//        // -------------------------------
//        // PGA 放大逻辑（未启用）
//        // 目的是在 DDS 输出电压较小时，将反馈信号放大，提高测量精度
//        // 并考虑谐振时高 Q 值带来的 20 倍增益
//        // -------------------------------
////      gain = calc_gain(sweep_mvpp);
////      set_input_gain(gain);

//        get_target_amplitude_and_phase(freq, &dds_amp, &tar_amp, &phase, &re, &im, 4096, 819200);
//        dds_amp = (dds_amp * 2 / gain);
//        tar_amp *= 2;

//        // -------------------------------
//        // 动态调整输出幅度逻辑（未启用）
//        // -------------------------------
////      if (tar_amp > 900) { ... }
////      else if (tar_amp >= 400) { ... }
////      else if (tar_amp > 100) { ... }
////      else { ... }

//        // 重复采集两次，累加求平均
//        for (uint8_t i = 0; i < 2; i++)
//        {
//            count++;
//            get_target_amplitude_and_phase(freq, &dds_amp, &tar_amp, &phase, &re, &im, 4096, 819200);
//            dds_amp_sum += (dds_amp * 2 / gain);
//            tar_amp *= 2;
//            re_sum += (re / gain);
//            im_sum += (im / gain);
//            tar_amp_sum += tar_amp;
//        }

//        // 平均处理
//        re = re_sum / count;
//        im = im_sum / count;
//        tar_amp = tar_amp_sum / count;
//        dds_amp = dds_amp_sum / count;

//        // 保存幅值增益
//        study_amp[study_amp_index++] = tar_amp / dds_amp;

//        // 保存复数增益（及其镜像共轭位置）
//        study_gain[sutdy_gain_index] = re;
//        study_gain[sutdy_gain_index + 1] = im;
//        study_gain[4096 * 2 - sutdy_gain_index] = re;
//        study_gain[4096 * 2 - sutdy_gain_index + 1] = -im;
//        sutdy_gain_index += 2;

//        // 实时更新 UI 显示
//        debug("page3.t6.txt=\"%d\"\xff\xff\xff", freq);

//        freq += 200; // 步进 200 Hz
//    }

//    // --------------------------------------------------------------------
//    // 扫描结束处理
//    // --------------------------------------------------------------------
//    dds_output_sine(freq, dds_factor, 0); // 停止输出

//    // 直流项置零，不考虑直流增益（DDS 无法输出直流，且电路为交流耦合）
//    study_gain[0] = study_gain[1] = 0.0f;

////  set_input_gain(1); // 还原增益（未启用）

//    // 自动判断滤波器类型
//    uint8_t type = analyze_filter_type_full(study_amp, study_amp_index);
//    switch (type)
//    {
//        case 1:
//            uart0("全通滤波器\n");
//            // 题目中没有全通类型，这里强行改为高通输出
//            debug("page3.t3.txt=\"HIGH-Pass\"\xff\xff\xff");
//            break;
//        case 2:
//            uart0("低通滤波器\n");
//            debug("page3.t3.txt=\"Low-Pass\"\xff\xff\xff");
//            break;
//        case 3:
//            uart0("高通滤波器\n");
//            debug("page3.t3.txt=\"High-Pass\"\xff\xff\xff");
//            break;
//        case 4:
//            uart0("带通滤波器\n");
//            debug("page3.t3.txt=\"Band-Pass\"\xff\xff\xff");
//            break;
//        case 5:
//            uart0("带阻滤波器\n");
//            debug("page3.t3.txt=\"Band-Stop\"\xff\xff\xff");
//            break;
//        default:
//            uart0("未知滤波器类型\n");
//            // 同上，输出高通以“骗分”
//            debug("page3.t3.txt=\"HIGH-Pass\"\xff\xff\xff");
//            break;
//    }
//}
