#include "AdcFftMeasure_User.h"

#include <math.h>
#include <string.h>
#include "adc.h"

#define ADC_FFT_VREF_UV              3300000UL
#define ADC_FFT_FULL_SCALE_CODE      65535UL
#define ADC_FFT_OVERRANGE_LOW        64U
#define ADC_FFT_OVERRANGE_HIGH       65471U
#define ADC_FFT_TWO_PI               6.2831853071795864769
#define ADC_FFT_SQRT2                1.4142135623730950488
#define ADC_FFT_CAPTURE_TIMEOUT_MS   20UL
#define ADC_FFT_FIT_PIVOT_MIN        1.0e-9

AdcFftMeasureSnapshot adc_fft_measure_state;
__ALIGNED(32) uint16_t adc_fft_raw[ADC_FFT_SAMPLE_COUNT];

static DMA_HandleTypeDef adc_fft_dma;
static TIM_HandleTypeDef adc_fft_timer;
static ADC_InitTypeDef adc_fft_saved_init;
static uint8_t adc_fft_saved_init_valid;
static uint8_t adc_fft_result_pending;
static uint8_t adc_fft_clock_ready;
static uint32_t adc_fft_state_tick;

static uint8_t AdcFft_ConfigureAdcClock(void)
{
  RCC_PeriphCLKInitTypeDef peripheral_clock = {0};
  uint32_t adc_source_hz;

  peripheral_clock.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  peripheral_clock.PLL3.PLL3M = 2U;
  adc_fft_measure_state.silicon_revision = HAL_GetREVID();
  /* Rev.V divides the selected ADC source by two internally. */
  peripheral_clock.PLL3.PLL3N =
      (adc_fft_measure_state.silicon_revision <= REV_ID_Y) ? 72U : 128U;
  peripheral_clock.PLL3.PLL3P = 2U;
  peripheral_clock.PLL3.PLL3Q = 2U;
  peripheral_clock.PLL3.PLL3R = 8U;
  peripheral_clock.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_2;
  peripheral_clock.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
  peripheral_clock.PLL3.PLL3FRACN = 0U;
  peripheral_clock.AdcClockSelection = RCC_ADCCLKSOURCE_PLL3;
  if (HAL_RCCEx_PeriphCLKConfig(&peripheral_clock) != HAL_OK)
  {
    return 0U;
  }
  adc_source_hz = HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_ADC);
  adc_fft_measure_state.adc_clock_hz =
      (adc_fft_measure_state.silicon_revision <= REV_ID_Y) ?
      adc_source_hz : (adc_source_hz / 2UL);
  return (((adc_fft_measure_state.silicon_revision <= REV_ID_Y) &&
           (adc_fft_measure_state.adc_clock_hz == 36000000UL)) ||
          ((adc_fft_measure_state.silicon_revision > REV_ID_Y) &&
           (adc_fft_measure_state.adc_clock_hz == 32000000UL))) ? 1U : 0U;
}

static uint8_t AdcFft_ConfigureDma(void)
{
  __HAL_RCC_DMA1_CLK_ENABLE();
  adc_fft_dma.Instance = DMA1_Stream1;
  adc_fft_dma.Init.Request = DMA_REQUEST_ADC1;
  adc_fft_dma.Init.Direction = DMA_PERIPH_TO_MEMORY;
  adc_fft_dma.Init.PeriphInc = DMA_PINC_DISABLE;
  adc_fft_dma.Init.MemInc = DMA_MINC_ENABLE;
  adc_fft_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
  adc_fft_dma.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
  adc_fft_dma.Init.Mode = DMA_NORMAL;
  adc_fft_dma.Init.Priority = DMA_PRIORITY_VERY_HIGH;
  adc_fft_dma.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
  if (HAL_DMA_Init(&adc_fft_dma) != HAL_OK)
  {
    return 0U;
  }
  __HAL_LINKDMA(&hadc1, DMA_Handle, adc_fft_dma);
  HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 5U, 0U);
  HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);
  return 1U;
}

static uint8_t AdcFft_ConfigureTimer(uint32_t sample_rate_hz)
{
  TIM_MasterConfigTypeDef master = {0};
  uint32_t timer_clock_hz = HAL_RCC_GetPCLK1Freq();
  uint32_t timer_period;

  if ((RCC->D2CFGR & RCC_D2CFGR_D2PPRE1) != 0U)
  {
    timer_clock_hz *= 2UL;
  }
  if ((sample_rate_hz == 0UL) || (timer_clock_hz < sample_rate_hz))
  {
    return 0U;
  }

  timer_period = timer_clock_hz / sample_rate_hz;
  if ((timer_period == 0UL) ||
      ((timer_clock_hz / timer_period) != sample_rate_hz) ||
      ((timer_clock_hz % timer_period) != 0UL))
  {
    return 0U;
  }

  adc_fft_measure_state.timer_clock_hz = timer_clock_hz;

  __HAL_RCC_TIM6_CLK_ENABLE();
  adc_fft_timer.Instance = TIM6;
  adc_fft_timer.Init.Prescaler = 0U;
  adc_fft_timer.Init.CounterMode = TIM_COUNTERMODE_UP;
  adc_fft_timer.Init.Period = timer_period - 1UL;
  adc_fft_timer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  adc_fft_timer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&adc_fft_timer) != HAL_OK)
  {
    return 0U;
  }

  master.MasterOutputTrigger = TIM_TRGO_UPDATE;
  master.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&adc_fft_timer, &master) != HAL_OK)
  {
    return 0U;
  }

  adc_fft_measure_state.timer_prescaler = adc_fft_timer.Instance->PSC;
  adc_fft_measure_state.timer_period = adc_fft_timer.Instance->ARR;
  adc_fft_measure_state.actual_sample_rate_hz = timer_clock_hz /
      ((adc_fft_measure_state.timer_prescaler + 1UL) *
       (adc_fft_measure_state.timer_period + 1UL));
  return (adc_fft_measure_state.actual_sample_rate_hz == sample_rate_hz) ? 1U : 0U;
}

static uint8_t AdcFft_ConfigureAdc(void)
{
  ADC_ChannelConfTypeDef channel = {0};

  adc_fft_saved_init = hadc1.Init;
  adc_fft_saved_init_valid = 1U;
  (void)HAL_ADC_Stop(&hadc1);

  hadc1.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV1;
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIG_T6_TRGO;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc1.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DMA_ONESHOT;
  hadc1.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    return 0U;
  }

  channel.Channel = ADC_CHANNEL_10;
  channel.Rank = ADC_REGULAR_RANK_1;
  channel.SamplingTime = ADC_SAMPLETIME_2CYCLES_5;
  channel.SingleDiff = ADC_SINGLE_ENDED;
  channel.OffsetNumber = ADC_OFFSET_NONE;
  channel.Offset = 0;
  channel.OffsetRightShift = DISABLE;
  channel.OffsetSignedSaturation = DISABLE;
  return (HAL_ADC_ConfigChannel(&hadc1, &channel) == HAL_OK) ? 1U : 0U;
}

static void AdcFft_RestoreAdc(void)
{
  ADC_ChannelConfTypeDef channel = {0};

  (void)HAL_TIM_Base_Stop(&adc_fft_timer);
  (void)HAL_ADC_Stop_DMA(&hadc1);
  HAL_NVIC_DisableIRQ(DMA1_Stream1_IRQn);
  (void)HAL_DMA_DeInit(&adc_fft_dma);

  if (adc_fft_saved_init_valid == 0U)
  {
    return;
  }
  hadc1.DMA_Handle = 0;
  hadc1.Init = adc_fft_saved_init;
  (void)HAL_ADC_Init(&hadc1);

  channel.Channel = ADC_CHANNEL_10;
  channel.Rank = ADC_REGULAR_RANK_1;
  channel.SamplingTime = ADC_SAMPLETIME_810CYCLES_5;
  channel.SingleDiff = ADC_SINGLE_ENDED;
  channel.OffsetNumber = ADC_OFFSET_NONE;
  channel.Offset = 0;
  channel.OffsetRightShift = DISABLE;
  channel.OffsetSignedSaturation = DISABLE;
  (void)HAL_ADC_ConfigChannel(&hadc1, &channel);
  adc_fft_saved_init_valid = 0U;
}

static uint8_t AdcFft_StartCapture(void)
{
  if ((AdcFft_ConfigureDma() == 0U) ||
      (AdcFft_ConfigureTimer(adc_fft_measure_state.active_request.sample_rate_hz) == 0U) ||
      (AdcFft_ConfigureAdc() == 0U))
  {
    AdcFft_RestoreAdc();
    return 0U;
  }

  SCB_CleanInvalidateDCache_by_Addr((uint32_t *)adc_fft_raw, sizeof(adc_fft_raw));
  if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_fft_raw, ADC_FFT_SAMPLE_COUNT) != HAL_OK)
  {
    AdcFft_RestoreAdc();
    return 0U;
  }
  if (HAL_TIM_Base_Start(&adc_fft_timer) != HAL_OK)
  {
    AdcFft_RestoreAdc();
    return 0U;
  }
  adc_fft_state_tick = HAL_GetTick();
  return 1U;
}

static uint8_t AdcFft_Solve3x3(double matrix[3][4], double solution[3])
{
  for (uint8_t column = 0U; column < 3U; column++)
  {
    uint8_t pivot = column;
    double pivot_abs = fabs(matrix[pivot][column]);

    for (uint8_t row = (uint8_t)(column + 1U); row < 3U; row++)
    {
      double candidate_abs = fabs(matrix[row][column]);
      if (candidate_abs > pivot_abs)
      {
        pivot = row;
        pivot_abs = candidate_abs;
      }
    }
    if (pivot_abs < ADC_FFT_FIT_PIVOT_MIN)
    {
      return 0U;
    }
    if (pivot != column)
    {
      for (uint8_t item = column; item < 4U; item++)
      {
        double temp = matrix[column][item];
        matrix[column][item] = matrix[pivot][item];
        matrix[pivot][item] = temp;
      }
    }

    for (uint8_t row = (uint8_t)(column + 1U); row < 3U; row++)
    {
      double factor = matrix[row][column] / matrix[column][column];
      for (uint8_t item = column; item < 4U; item++)
      {
        matrix[row][item] -= factor * matrix[column][item];
      }
    }
  }

  for (int8_t row = 2; row >= 0; row--)
  {
    double value = matrix[row][3];
    for (uint8_t column = (uint8_t)(row + 1); column < 3U; column++)
    {
      value -= matrix[row][column] * solution[column];
    }
    solution[row] = value / matrix[row][row];
  }
  return 1U;
}

static int32_t AdcFft_RoundToI32(double value)
{
  return (value >= 0.0) ? (int32_t)(value + 0.5) : (int32_t)(value - 0.5);
}

static void AdcFft_ProcessFrame(void)
{
  const AdcFftMeasurementRequest *request = &adc_fft_measure_state.active_request;
  AdcFftMeasurementResult result;
  uint16_t adc_min = 0xFFFFU;
  uint16_t adc_max = 0U;
  uint32_t sample_rate_hz = adc_fft_measure_state.actual_sample_rate_hz;
  double normal[3][4] = {{0.0}};
  double coefficients[3] = {0.0, 0.0, 0.0};
  double phase_step;
  double step_sin;
  double step_cos;
  double basis_sin = 0.0;
  double basis_cos = 1.0;
  uint32_t process_start = HAL_GetTick();

  memset(&result, 0, sizeof(result));
  result.sweep_id = request->sweep_id;
  result.point_id = request->point_id;
  result.reference_frequency_hz = request->reference_frequency_hz;
  result.sample_rate_hz = sample_rate_hz;
  result.status = ADC_FFT_STATUS_TIMER_TRIGGERED_DMA_CAPTURE |
                  ADC_FFT_STATUS_USED_LEAST_SQUARES;
  adc_fft_measure_state.fit_sin_uv = 0;
  adc_fft_measure_state.fit_cos_uv = 0;
  adc_fft_measure_state.fit_offset_uv = 0;

  for (uint16_t i = 0U; i < ADC_FFT_SAMPLE_COUNT; i++)
  {
    uint16_t code = adc_fft_raw[i];
    if (code < adc_min) { adc_min = code; }
    if (code > adc_max) { adc_max = code; }
  }

  result.target_bin = AdcFftMeasure_CalculateTargetBin(request->reference_frequency_hz,
                                                       sample_rate_hz);
  result.adc_min_code = adc_min;
  result.adc_max_code = adc_max;

  if ((adc_min <= ADC_FFT_OVERRANGE_LOW) || (adc_max >= ADC_FFT_OVERRANGE_HIGH))
  {
    result.status |= ADC_FFT_STATUS_ADC_OVERRANGE | ADC_FFT_STATUS_AMPLITUDE_SATURATED;
  }

  phase_step = ((double)ADC_FFT_TWO_PI * (double)request->reference_frequency_hz) /
               (double)sample_rate_hz;
  step_sin = sin(phase_step);
  step_cos = cos(phase_step);

  for (uint16_t i = 0U; i < ADC_FFT_SAMPLE_COUNT; i++)
  {
    double sample = (double)adc_fft_raw[i];
    double next_sin;

    normal[0][0] += basis_sin * basis_sin;
    normal[0][1] += basis_sin * basis_cos;
    normal[0][2] += basis_sin;
    normal[0][3] += sample * basis_sin;
    normal[1][1] += basis_cos * basis_cos;
    normal[1][2] += basis_cos;
    normal[1][3] += sample * basis_cos;
    normal[2][2] += 1.0;
    normal[2][3] += sample;

    next_sin = (basis_sin * step_cos) + (basis_cos * step_sin);
    basis_cos = (basis_cos * step_cos) - (basis_sin * step_sin);
    basis_sin = next_sin;
  }

  normal[1][0] = normal[0][1];
  normal[2][0] = normal[0][2];
  normal[2][1] = normal[1][2];

  if (AdcFft_Solve3x3(normal, coefficients) == 0U)
  {
    result.status |= ADC_FFT_STATUS_LOW_SNR;
  }
  else
  {
    double uv_per_code = (double)ADC_FFT_VREF_UV /
                         (double)ADC_FFT_FULL_SCALE_CODE;
    double peak_uv = sqrt((coefficients[0] * coefficients[0]) +
                          (coefficients[1] * coefficients[1])) * uv_per_code;
    double rms_uv = peak_uv / (double)ADC_FFT_SQRT2;
    double offset_uv = coefficients[2] * uv_per_code;
    uint32_t peak_uv_rounded = (uint32_t)(peak_uv + 0.5);
    int32_t offset_uv_rounded = AdcFft_RoundToI32(offset_uv);

    adc_fft_measure_state.fit_sin_uv =
        AdcFft_RoundToI32(coefficients[0] * uv_per_code);
    adc_fft_measure_state.fit_cos_uv =
        AdcFft_RoundToI32(coefficients[1] * uv_per_code);
    adc_fft_measure_state.fit_offset_uv = offset_uv_rounded;
    result.main_bin = result.target_bin;
    result.main_frequency_hz = request->reference_frequency_hz;
    result.voltage_uv_peak = peak_uv_rounded;
    result.voltage_uv_rms = (uint32_t)(rms_uv + 0.5);
    result.voltage_uv_min = offset_uv_rounded - (int32_t)peak_uv_rounded;
    result.voltage_uv_max = offset_uv_rounded + (int32_t)peak_uv_rounded;
    result.voltage_uv_pp = peak_uv_rounded * 2UL;
    if (result.voltage_uv_peak < 1000UL)
    {
      result.status |= ADC_FFT_STATUS_LOW_SNR;
    }
  }

  if ((result.status & (ADC_FFT_STATUS_REFERENCE_OUT_OF_RANGE |
                        ADC_FFT_STATUS_ADC_OVERRANGE |
                        ADC_FFT_STATUS_FRAME_TIMEOUT |
                        ADC_FFT_STATUS_PEAK_NOT_NEAR_REFERENCE |
                        ADC_FFT_STATUS_LOW_SNR)) == 0U)
  {
    result.status |= ADC_FFT_STATUS_VALID;
  }

  adc_fft_measure_state.last_process_ticks = HAL_GetTick() - process_start;
  adc_fft_measure_state.last_result = result;
  adc_fft_measure_state.result_count++;
  adc_fft_result_pending = 1U;
  adc_fft_measure_state.state = ADC_FFT_RESULT_READY;
}

static void AdcFft_SetCaptureError(uint16_t error)
{
  AdcFftMeasurementResult *result = &adc_fft_measure_state.last_result;

  memset(result, 0, sizeof(*result));
  result->sweep_id = adc_fft_measure_state.active_request.sweep_id;
  result->point_id = adc_fft_measure_state.active_request.point_id;
  result->reference_frequency_hz = adc_fft_measure_state.active_request.reference_frequency_hz;
  result->sample_rate_hz = adc_fft_measure_state.actual_sample_rate_hz;
  result->target_bin = AdcFftMeasure_CalculateTargetBin(
      adc_fft_measure_state.active_request.reference_frequency_hz,
      result->sample_rate_hz);
  result->status = error;
  adc_fft_measure_state.last_error = error;
  adc_fft_measure_state.error_count++;
  adc_fft_result_pending = 1U;
  adc_fft_measure_state.state = ADC_FFT_ERROR;
}

void AdcFftMeasure_Init(void)
{
  memset(&adc_fft_measure_state, 0, sizeof(adc_fft_measure_state));
  memset(adc_fft_raw, 0, sizeof(adc_fft_raw));
  adc_fft_clock_ready = AdcFft_ConfigureAdcClock();
  adc_fft_measure_state.state = ADC_FFT_IDLE;
  adc_fft_result_pending = 0U;
  adc_fft_saved_init_valid = 0U;
  adc_fft_state_tick = HAL_GetTick();
}

uint8_t AdcFftMeasure_IsSupportedSampleRate(uint32_t sample_rate_hz)
{
  return (sample_rate_hz == ADC_FFT_DEFAULT_SAMPLE_RATE) ? 1U : 0U;
}

uint16_t AdcFftMeasure_CalculateTargetBin(uint32_t frequency_hz, uint32_t sample_rate_hz)
{
  uint64_t numerator;

  if (sample_rate_hz == 0UL)
  {
    return 0U;
  }
  numerator = ((uint64_t)frequency_hz * ADC_FFT_SAMPLE_COUNT) +
              (sample_rate_hz / 2UL);
  return (uint16_t)(numerator / sample_rate_hz);
}

uint8_t AdcFftMeasure_Start(const AdcFftMeasurementRequest *request)
{
  if ((request == 0) || (adc_fft_clock_ready == 0U) ||
      (request->fft_length != ADC_FFT_SAMPLE_COUNT) ||
      (AdcFftMeasure_IsSupportedSampleRate(request->sample_rate_hz) == 0U))
  {
    return 0U;
  }
  if ((adc_fft_measure_state.state != ADC_FFT_IDLE) &&
      (adc_fft_measure_state.state != ADC_FFT_RESULT_READY) &&
      (adc_fft_measure_state.state != ADC_FFT_ERROR))
  {
    return 0U;
  }

  adc_fft_measure_state.active_request = *request;
  adc_fft_measure_state.request_count++;
  adc_fft_measure_state.state = ADC_FFT_ACCEPTED;
  adc_fft_result_pending = 0U;
  adc_fft_state_tick = HAL_GetTick();
  return 1U;
}

void AdcFftMeasure_Task(void)
{
  switch (adc_fft_measure_state.state)
  {
    case ADC_FFT_ACCEPTED:
      adc_fft_measure_state.state = ADC_FFT_PRE_DELAY;
      adc_fft_state_tick = HAL_GetTick();
      break;

    case ADC_FFT_PRE_DELAY:
      if ((HAL_GetTick() - adc_fft_state_tick) >=
          ((adc_fft_measure_state.active_request.pre_capture_delay_us + 999UL) / 1000UL))
      {
        adc_fft_measure_state.state = ADC_FFT_ARM_DMA;
      }
      break;

    case ADC_FFT_ARM_DMA:
      if (AdcFft_StartCapture() != 0U)
      {
        adc_fft_measure_state.state = ADC_FFT_CAPTURING;
      }
      else
      {
        AdcFft_SetCaptureError(ADC_FFT_STATUS_ADC_DMA_ERROR);
      }
      break;

    case ADC_FFT_CAPTURING:
      if ((HAL_GetTick() - adc_fft_state_tick) >= ADC_FFT_CAPTURE_TIMEOUT_MS)
      {
        AdcFft_RestoreAdc();
        AdcFft_SetCaptureError(ADC_FFT_STATUS_FRAME_TIMEOUT);
      }
      break;

    case ADC_FFT_FRAME_READY:
      AdcFft_RestoreAdc();
      SCB_InvalidateDCache_by_Addr((uint32_t *)adc_fft_raw, sizeof(adc_fft_raw));
      adc_fft_measure_state.last_capture_ticks = HAL_GetTick() - adc_fft_state_tick;
      adc_fft_measure_state.state = ADC_FFT_PROCESSING;
      break;

    case ADC_FFT_PROCESSING:
      AdcFft_ProcessFrame();
      break;

    case ADC_FFT_ERROR:
      if (adc_fft_saved_init_valid != 0U)
      {
        AdcFft_RestoreAdc();
      }
      break;

    default:
      break;
  }
}

uint8_t AdcFftMeasure_TakeResult(AdcFftMeasurementResult *result)
{
  if ((result == 0) || (adc_fft_result_pending == 0U))
  {
    return 0U;
  }
  *result = adc_fft_measure_state.last_result;
  adc_fft_result_pending = 0U;
  adc_fft_measure_state.state = ADC_FFT_IDLE;
  return 1U;
}

AdcFftMeasureState AdcFftMeasure_GetState(void)
{
  return adc_fft_measure_state.state;
}

AdcFftMeasureSnapshot AdcFftMeasure_GetSnapshot(void)
{
  return adc_fft_measure_state;
}

void DMA1_Stream1_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&adc_fft_dma);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if ((hadc == &hadc1) && (adc_fft_measure_state.state == ADC_FFT_CAPTURING))
  {
    (void)HAL_TIM_Base_Stop(&adc_fft_timer);
    adc_fft_measure_state.state = ADC_FFT_FRAME_READY;
  }
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
  if ((hadc == &hadc1) && (adc_fft_measure_state.state == ADC_FFT_CAPTURING))
  {
    (void)HAL_TIM_Base_Stop(&adc_fft_timer);
    AdcFft_SetCaptureError(ADC_FFT_STATUS_ADC_DMA_ERROR);
  }
}
