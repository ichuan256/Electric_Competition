#include "AdcFftMeasure_User.h"

#include <math.h>
#include <string.h>
#include "adc.h"

#define ADC_FFT_VREF_UV              3300000UL
#define ADC_FFT_FULL_SCALE_CODE      65535UL
#define ADC_FFT_OVERRANGE_LOW        64U
#define ADC_FFT_OVERRANGE_HIGH       65471U
#define ADC_FFT_SEARCH_HALF_WIDTH    2U
#define ADC_FFT_TWO_PI               6.2831853071795864769f
#define ADC_FFT_SQRT2                1.4142135623730950488f
#define ADC_FFT_CAPTURE_TIMEOUT_MS   20UL

AdcFftMeasureSnapshot adc_fft_measure_state;
__ALIGNED(32) uint16_t adc_fft_raw[ADC_FFT_SAMPLE_COUNT];

static float adc_fft_real[ADC_FFT_SAMPLE_COUNT];
static float adc_fft_imag[ADC_FFT_SAMPLE_COUNT];
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

  peripheral_clock.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  peripheral_clock.PLL3.PLL3M = 2U;
  peripheral_clock.PLL3.PLL3N = 96U;
  peripheral_clock.PLL3.PLL3P = 2U;
  peripheral_clock.PLL3.PLL3Q = 2U;
  peripheral_clock.PLL3.PLL3R = 8U;
  peripheral_clock.PLL3.PLL3RGE = RCC_PLL3VCIRANGE_2;
  peripheral_clock.PLL3.PLL3VCOSEL = RCC_PLL3VCOWIDE;
  peripheral_clock.PLL3.PLL3FRACN = 0U;
  peripheral_clock.AdcClockSelection = RCC_ADCCLKSOURCE_PLL3;
  return (HAL_RCCEx_PeriphCLKConfig(&peripheral_clock) == HAL_OK) ? 1U : 0U;
}

static uint8_t AdcFft_ShouldUseHann(const AdcFftMeasurementRequest *request,
                                    uint16_t target_bin)
{
  uint64_t bin_frequency_hz;
  uint32_t delta_hz;

  if (request->window_mode == ADC_FFT_WINDOW_HANN)
  {
    return 1U;
  }
  if (request->window_mode == ADC_FFT_WINDOW_RECTANGULAR)
  {
    return 0U;
  }

  bin_frequency_hz = ((uint64_t)target_bin * request->sample_rate_hz) /
                     ADC_FFT_SAMPLE_COUNT;
  delta_hz = (request->reference_frequency_hz > bin_frequency_hz) ?
             (uint32_t)(request->reference_frequency_hz - bin_frequency_hz) :
             (uint32_t)(bin_frequency_hz - request->reference_frequency_hz);
  return (((request->flags & 0x01U) != 0U) && (delta_hz <= 2UL)) ? 0U : 1U;
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

  if ((RCC->D2CFGR & RCC_D2CFGR_D2PPRE1) != 0U)
  {
    timer_clock_hz *= 2UL;
  }
  if ((sample_rate_hz == 0UL) || (timer_clock_hz < sample_rate_hz) ||
      ((timer_clock_hz % sample_rate_hz) != 0UL))
  {
    return 0U;
  }

  __HAL_RCC_TIM6_CLK_ENABLE();
  adc_fft_timer.Instance = TIM6;
  adc_fft_timer.Init.Prescaler = 0U;
  adc_fft_timer.Init.CounterMode = TIM_COUNTERMODE_UP;
  adc_fft_timer.Init.Period = (timer_clock_hz / sample_rate_hz) - 1UL;
  adc_fft_timer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  adc_fft_timer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&adc_fft_timer) != HAL_OK)
  {
    return 0U;
  }

  master.MasterOutputTrigger = TIM_TRGO_UPDATE;
  master.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  return (HAL_TIMEx_MasterConfigSynchronization(&adc_fft_timer, &master) == HAL_OK) ? 1U : 0U;
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

static void AdcFft_Fft(float *real, float *imag)
{
  uint16_t j = 0U;

  for (uint16_t i = 1U; i < ADC_FFT_SAMPLE_COUNT; i++)
  {
    uint16_t bit = ADC_FFT_SAMPLE_COUNT >> 1;
    while ((j & bit) != 0U)
    {
      j ^= bit;
      bit >>= 1;
    }
    j ^= bit;
    if (i < j)
    {
      float temp = real[i];
      real[i] = real[j];
      real[j] = temp;
      temp = imag[i];
      imag[i] = imag[j];
      imag[j] = temp;
    }
  }

  for (uint16_t length = 2U; length <= ADC_FFT_SAMPLE_COUNT; length <<= 1)
  {
    uint16_t half = length >> 1;
    float angle = -ADC_FFT_TWO_PI / (float)length;
    float step_real = cosf(angle);
    float step_imag = sinf(angle);

    for (uint16_t base = 0U; base < ADC_FFT_SAMPLE_COUNT; base += length)
    {
      float twiddle_real = 1.0f;
      float twiddle_imag = 0.0f;
      for (uint16_t k = 0U; k < half; k++)
      {
        uint16_t even = (uint16_t)(base + k);
        uint16_t odd = (uint16_t)(even + half);
        float odd_real = (real[odd] * twiddle_real) - (imag[odd] * twiddle_imag);
        float odd_imag = (real[odd] * twiddle_imag) + (imag[odd] * twiddle_real);
        float next_twiddle_real;

        real[odd] = real[even] - odd_real;
        imag[odd] = imag[even] - odd_imag;
        real[even] += odd_real;
        imag[even] += odd_imag;
        next_twiddle_real = (twiddle_real * step_real) - (twiddle_imag * step_imag);
        twiddle_imag = (twiddle_real * step_imag) + (twiddle_imag * step_real);
        twiddle_real = next_twiddle_real;
      }
    }
  }
}

static void AdcFft_ProcessFrame(void)
{
  const AdcFftMeasurementRequest *request = &adc_fft_measure_state.active_request;
  AdcFftMeasurementResult result;
  uint64_t sum = 0ULL;
  uint16_t adc_min = 0xFFFFU;
  uint16_t adc_max = 0U;
  uint16_t target_bin = request->target_bin;
  uint16_t search_start;
  uint16_t search_end;
  uint16_t best_bin = 0U;
  float best_mag2 = 0.0f;
  float coherent_gain_sum = 0.0f;
  uint8_t use_hann;
  uint32_t process_start = HAL_GetTick();

  memset(&result, 0, sizeof(result));
  result.sweep_id = request->sweep_id;
  result.point_id = request->point_id;
  result.reference_frequency_hz = request->reference_frequency_hz;
  result.sample_rate_hz = request->sample_rate_hz;
  result.status = ADC_FFT_STATUS_TIMER_TRIGGERED_DMA_CAPTURE;

  for (uint16_t i = 0U; i < ADC_FFT_SAMPLE_COUNT; i++)
  {
    uint16_t code = adc_fft_raw[i];
    sum += code;
    if (code < adc_min) { adc_min = code; }
    if (code > adc_max) { adc_max = code; }
  }

  if (target_bin == ADC_FFT_TARGET_BIN_AUTO)
  {
    target_bin = AdcFftMeasure_CalculateTargetBin(request->reference_frequency_hz,
                                                  request->sample_rate_hz);
  }
  result.target_bin = target_bin;
  result.adc_min_code = adc_min;
  result.adc_max_code = adc_max;
  use_hann = AdcFft_ShouldUseHann(request, target_bin);
  result.status |= (use_hann != 0U) ? ADC_FFT_STATUS_USED_HANN_WINDOW :
                                      ADC_FFT_STATUS_USED_RECT_WINDOW;

  if ((adc_min <= ADC_FFT_OVERRANGE_LOW) || (adc_max >= ADC_FFT_OVERRANGE_HIGH))
  {
    result.status |= ADC_FFT_STATUS_ADC_OVERRANGE | ADC_FFT_STATUS_AMPLITUDE_SATURATED;
  }

  for (uint16_t i = 0U; i < ADC_FFT_SAMPLE_COUNT; i++)
  {
    float window = 1.0f;
    float centered = (float)((int32_t)adc_fft_raw[i] -
                             (int32_t)(sum / ADC_FFT_SAMPLE_COUNT));
    if (use_hann != 0U)
    {
      window = 0.5f - (0.5f * cosf((ADC_FFT_TWO_PI * (float)i) /
                                    (float)(ADC_FFT_SAMPLE_COUNT - 1U)));
    }
    coherent_gain_sum += window;
    adc_fft_real[i] = centered * ((float)ADC_FFT_VREF_UV /
                                  (float)ADC_FFT_FULL_SCALE_CODE) * window;
    adc_fft_imag[i] = 0.0f;
  }

  AdcFft_Fft(adc_fft_real, adc_fft_imag);
  search_start = (target_bin > ADC_FFT_SEARCH_HALF_WIDTH) ?
                 (uint16_t)(target_bin - ADC_FFT_SEARCH_HALF_WIDTH) : 1U;
  search_end = (uint16_t)(target_bin + ADC_FFT_SEARCH_HALF_WIDTH);
  if (search_end >= (ADC_FFT_SAMPLE_COUNT / 2U))
  {
    search_end = (ADC_FFT_SAMPLE_COUNT / 2U) - 1U;
  }

  for (uint16_t bin = search_start; bin <= search_end; bin++)
  {
    float mag2 = (adc_fft_real[bin] * adc_fft_real[bin]) +
                 (adc_fft_imag[bin] * adc_fft_imag[bin]);
    if (mag2 > best_mag2)
    {
      best_mag2 = mag2;
      best_bin = bin;
    }
  }

  if ((best_bin == 0U) || (coherent_gain_sum <= 0.0f))
  {
    result.status |= ADC_FFT_STATUS_LOW_SNR;
  }
  else
  {
    float magnitude = sqrtf(best_mag2);
    float peak_uv = (2.0f * magnitude) / coherent_gain_sum;
    float rms_uv = peak_uv / ADC_FFT_SQRT2;

    result.main_bin = best_bin;
    result.main_frequency_hz = (uint32_t)(((uint64_t)best_bin * request->sample_rate_hz) /
                                          ADC_FFT_SAMPLE_COUNT);
    result.voltage_uv_peak = (uint32_t)(peak_uv + 0.5f);
    result.voltage_uv_rms = (uint32_t)(rms_uv + 0.5f);
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
  result->sample_rate_hz = adc_fft_measure_state.active_request.sample_rate_hz;
  result->target_bin = adc_fft_measure_state.active_request.target_bin;
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
  return ((sample_rate_hz == 1000000UL) ||
          (sample_rate_hz == 2000000UL) ||
          (sample_rate_hz == 4000000UL)) ? 1U : 0U;
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
