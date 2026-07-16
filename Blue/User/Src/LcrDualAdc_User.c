#include "LcrDualAdc_User.h"

#include <math.h>
#include <string.h>
#include "LcrAuto_User.h"
#include "adc.h"

#define LCR_DUAL_ADC_VREF_UV             3300000.0
#define LCR_DUAL_ADC_FULL_SCALE_CODE     65535.0
#define LCR_DUAL_ADC_OVERRANGE_LOW       64U
#define LCR_DUAL_ADC_OVERRANGE_HIGH      65471U
#define LCR_DUAL_ADC_LOW_SNR_PEAK_UV     1000.0
#define LCR_DUAL_ADC_TWO_PI              6.2831853071795864769
#define LCR_DUAL_ADC_FIT_PIVOT_MIN       1.0e-9
#define LCR_DUAL_ADC_CAPTURE_TIMEOUT_MS  50UL
#define LCR_DUAL_ADC_IRQ_NONE             0U
#define LCR_DUAL_ADC_IRQ_COMPLETE         0x01U
#define LCR_DUAL_ADC_IRQ_ERROR            0x02U

/* ADC12 common CDR: ADC1 is bits 15:0, ADC2 is bits 31:16. */
__ALIGNED(32) static uint32_t lcr_dual_raw[LCR_DUAL_ADC_SAMPLE_COUNT];

static ADC_HandleTypeDef lcr_adc2;
static DMA_HandleTypeDef lcr_dma;
static TIM_HandleTypeDef lcr_timer;
static LcrDualAdcSnapshot lcr_dual_snapshot;
static LcrCaptureSample lcr_pending_sample;
static uint8_t lcr_result_pending;
static uint32_t lcr_capture_tick;
static volatile uint8_t lcr_irq_event;
static volatile uint32_t lcr_irq_error;
static volatile uint32_t lcr_irq_dma_error;

static uint8_t LcrDualAdc_ConfigureClock(void)
{
  RCC_PeriphCLKInitTypeDef peripheral_clock = {0};
  uint32_t adc_source_hz;
  uint32_t silicon_revision = HAL_GetREVID();

  peripheral_clock.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  peripheral_clock.PLL3.PLL3M = 2U;
  /* STM32H750 revision V divides the selected ADC source by two internally. */
  peripheral_clock.PLL3.PLL3N = (silicon_revision <= REV_ID_Y) ? 72U : 128U;
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
  lcr_dual_snapshot.adc_clock_hz = (silicon_revision <= REV_ID_Y) ?
                                   adc_source_hz : (adc_source_hz / 2UL);
  return (((silicon_revision <= REV_ID_Y) &&
           (lcr_dual_snapshot.adc_clock_hz == 36000000UL)) ||
          ((silicon_revision > REV_ID_Y) &&
           (lcr_dual_snapshot.adc_clock_hz == 32000000UL))) ? 1U : 0U;
}

static uint8_t LcrDualAdc_ConfigureDma(void)
{
  __HAL_RCC_DMA1_CLK_ENABLE();
  lcr_dma.Instance = DMA1_Stream2;
  lcr_dma.Init.Request = DMA_REQUEST_ADC1;
  lcr_dma.Init.Direction = DMA_PERIPH_TO_MEMORY;
  lcr_dma.Init.PeriphInc = DMA_PINC_DISABLE;
  lcr_dma.Init.MemInc = DMA_MINC_ENABLE;
  lcr_dma.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
  lcr_dma.Init.MemDataAlignment = DMA_MDATAALIGN_WORD;
  lcr_dma.Init.Mode = DMA_NORMAL;
  lcr_dma.Init.Priority = DMA_PRIORITY_VERY_HIGH;
  lcr_dma.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
  if (HAL_DMA_Init(&lcr_dma) != HAL_OK)
  {
    return 0U;
  }

  __HAL_LINKDMA(&hadc1, DMA_Handle, lcr_dma);
  HAL_NVIC_SetPriority(DMA1_Stream2_IRQn, 5U, 0U);
  HAL_NVIC_EnableIRQ(DMA1_Stream2_IRQn);
  return 1U;
}

static void LcrDualAdc_FillCommonInit(ADC_InitTypeDef *init)
{
  memset(init, 0, sizeof(*init));
  init->ClockPrescaler = ADC_CLOCK_ASYNC_DIV1;
  init->Resolution = ADC_RESOLUTION_16B;
  init->ScanConvMode = ADC_SCAN_DISABLE;
  init->EOCSelection = ADC_EOC_SINGLE_CONV;
  init->LowPowerAutoWait = DISABLE;
  init->ContinuousConvMode = DISABLE;
  init->NbrOfConversion = 1U;
  init->DiscontinuousConvMode = DISABLE;
  init->NbrOfDiscConversion = 1U;
  init->ConversionDataManagement = ADC_CONVERSIONDATA_DR;
  init->Overrun = ADC_OVR_DATA_OVERWRITTEN;
  init->LeftBitShift = ADC_LEFTBITSHIFT_NONE;
  init->OversamplingMode = DISABLE;
}

static uint8_t LcrDualAdc_ConfigureAdcs(void)
{
  GPIO_InitTypeDef gpio = {0};
  ADC_ChannelConfTypeDef channel = {0};
  ADC_MultiModeTypeDef multimode = {0};

  __HAL_RCC_ADC12_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1;
  gpio.Mode = GPIO_MODE_ANALOG;
  gpio.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOC, &gpio);

  (void)HAL_ADC_Stop(&hadc1);
  LcrDualAdc_FillCommonInit(&hadc1.Init);
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIG_T6_TRGO;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc1.Init.ConversionDataManagement = ADC_CONVERSIONDATA_DMA_ONESHOT;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    return 0U;
  }

  lcr_adc2.Instance = ADC2;
  LcrDualAdc_FillCommonInit(&lcr_adc2.Init);
  lcr_adc2.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  lcr_adc2.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  if (HAL_ADC_Init(&lcr_adc2) != HAL_OK)
  {
    return 0U;
  }

  channel.Rank = ADC_REGULAR_RANK_1;
  channel.SamplingTime = ADC_SAMPLETIME_2CYCLES_5;
  channel.SingleDiff = ADC_SINGLE_ENDED;
  channel.OffsetNumber = ADC_OFFSET_NONE;
  channel.Offset = 0;
  channel.OffsetRightShift = DISABLE;
  channel.OffsetSignedSaturation = DISABLE;

  channel.Channel = ADC_CHANNEL_10;
  if (HAL_ADC_ConfigChannel(&hadc1, &channel) != HAL_OK)
  {
    return 0U;
  }
  channel.Channel = ADC_CHANNEL_11;
  if (HAL_ADC_ConfigChannel(&lcr_adc2, &channel) != HAL_OK)
  {
    return 0U;
  }

  if ((HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET_LINEARITY,
                                   ADC_SINGLE_ENDED) != HAL_OK) ||
      (HAL_ADCEx_Calibration_Start(&lcr_adc2, ADC_CALIB_OFFSET_LINEARITY,
                                   ADC_SINGLE_ENDED) != HAL_OK))
  {
    return 0U;
  }

  multimode.Mode = ADC_DUALMODE_REGSIMULT;
  multimode.DualModeData = ADC_DUALMODEDATAFORMAT_32_10_BITS;
  multimode.TwoSamplingDelay = ADC_TWOSAMPLINGDELAY_1CYCLE;
  return (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &multimode) == HAL_OK) ? 1U : 0U;
}

static uint32_t LcrDualAdc_SelectSampleRate(uint32_t frequency_hz)
{
  if (frequency_hz <= 2000UL)
  {
    return 200000UL;
  }
  if (frequency_hz <= 5000UL)
  {
    return 500000UL;
  }
  if (frequency_hz <= 10000UL)
  {
    return 1000000UL;
  }
  return 2500000UL;
}

static uint8_t LcrDualAdc_ConfigureTimer(uint32_t requested_rate_hz)
{
  TIM_MasterConfigTypeDef master = {0};
  uint32_t timer_clock_hz = HAL_RCC_GetPCLK1Freq();
  uint32_t timer_period;

  if ((RCC->D2CFGR & RCC_D2CFGR_D2PPRE1) != 0U)
  {
    timer_clock_hz *= 2UL;
  }
  if ((requested_rate_hz == 0UL) || (timer_clock_hz < requested_rate_hz))
  {
    return 0U;
  }

  timer_period = timer_clock_hz / requested_rate_hz;
  if ((timer_period == 0UL) || ((timer_clock_hz % timer_period) != 0UL) ||
      ((timer_clock_hz / timer_period) != requested_rate_hz))
  {
    return 0U;
  }

  __HAL_RCC_TIM6_CLK_ENABLE();
  lcr_timer.Instance = TIM6;
  lcr_timer.Init.Prescaler = 0U;
  lcr_timer.Init.CounterMode = TIM_COUNTERMODE_UP;
  lcr_timer.Init.Period = timer_period - 1UL;
  lcr_timer.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  lcr_timer.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&lcr_timer) != HAL_OK)
  {
    return 0U;
  }

  master.MasterOutputTrigger = TIM_TRGO_UPDATE;
  master.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&lcr_timer, &master) != HAL_OK)
  {
    return 0U;
  }

  lcr_dual_snapshot.sample_rate_hz = timer_clock_hz /
      ((lcr_timer.Instance->PSC + 1UL) * (lcr_timer.Instance->ARR + 1UL));
  return (lcr_dual_snapshot.sample_rate_hz == requested_rate_hz) ? 1U : 0U;
}

static HAL_StatusTypeDef LcrDualAdc_StopDma(void)
{
  HAL_StatusTypeDef status = HAL_ADCEx_MultiModeStop_DMA(&hadc1);

  /* A normal one-shot DMA is already READY when its complete callback runs.
   * HAL_DMA_Abort then reports NO_XFER and Stop_DMA leaves ERROR_DMA latched,
   * which incorrectly poisons the next capture. Treat only that exact case as
   * a successful stop; all real DMA errors remain visible to the caller. */
  if ((status == HAL_ERROR) &&
      (lcr_dma.ErrorCode == HAL_DMA_ERROR_NO_XFER))
  {
    CLEAR_BIT(hadc1.State, HAL_ADC_STATE_ERROR_DMA);
    CLEAR_BIT(hadc1.ErrorCode, HAL_ADC_ERROR_DMA);
    lcr_dma.ErrorCode = HAL_DMA_ERROR_NONE;
    status = HAL_OK;
  }

  return status;
}

static uint8_t LcrDualAdc_Solve3x5(double matrix[3][5],
                                   double vin_coeff[3],
                                   double vr_coeff[3])
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
    if (pivot_abs < LCR_DUAL_ADC_FIT_PIVOT_MIN)
    {
      return 0U;
    }
    if (pivot != column)
    {
      for (uint8_t item = column; item < 5U; item++)
      {
        double temporary = matrix[column][item];
        matrix[column][item] = matrix[pivot][item];
        matrix[pivot][item] = temporary;
      }
    }

    for (uint8_t row = (uint8_t)(column + 1U); row < 3U; row++)
    {
      double factor = matrix[row][column] / matrix[column][column];
      for (uint8_t item = column; item < 5U; item++)
      {
        matrix[row][item] -= factor * matrix[column][item];
      }
    }
  }

  for (int8_t row = 2; row >= 0; row--)
  {
    double vin_value = matrix[row][3];
    double vr_value = matrix[row][4];
    for (uint8_t column = (uint8_t)(row + 1); column < 3U; column++)
    {
      vin_value -= matrix[row][column] * vin_coeff[column];
      vr_value -= matrix[row][column] * vr_coeff[column];
    }
    vin_coeff[row] = vin_value / matrix[row][row];
    vr_coeff[row] = vr_value / matrix[row][row];
  }
  return 1U;
}

static void LcrDualAdc_SetInvalidResult(uint32_t hal_error,
                                        LcrDualAdcErrorSource error_source)
{
  memset(&lcr_pending_sample, 0, sizeof(lcr_pending_sample));
  lcr_pending_sample.frequency_hz = lcr_dual_snapshot.frequency_hz;
  lcr_pending_sample.sample_rate_hz = lcr_dual_snapshot.sample_rate_hz;
  lcr_dual_snapshot.vin_peak_uv = 0UL;
  lcr_dual_snapshot.vr_peak_uv = 0UL;
  lcr_dual_snapshot.last_status = 0U;
  lcr_dual_snapshot.last_hal_error = hal_error;
  lcr_dual_snapshot.error_source = error_source;
  lcr_dual_snapshot.error_count++;
  lcr_dual_snapshot.state = LCR_DUAL_ADC_ERROR;
  lcr_result_pending = 1U;
}

static void LcrDualAdc_ProcessFrame(void)
{
  double normal[3][5] = {{0.0}};
  double vin_coeff[3] = {0.0, 0.0, 0.0};
  double vr_coeff[3] = {0.0, 0.0, 0.0};
  double phase_step;
  double step_sin;
  double step_cos;
  double basis_sin = 0.0;
  double basis_cos = 1.0;
  double uv_per_code = LCR_DUAL_ADC_VREF_UV / LCR_DUAL_ADC_FULL_SCALE_CODE;
  double vin_peak_uv;
  double vr_peak_uv;
  uint16_t vin_min = 0xFFFFU;
  uint16_t vin_max = 0U;
  uint16_t vr_min = 0xFFFFU;
  uint16_t vr_max = 0U;
  uint16_t status = LCR_CAPTURE_STATUS_VALID;

  memset(&lcr_pending_sample, 0, sizeof(lcr_pending_sample));
  lcr_pending_sample.frequency_hz = lcr_dual_snapshot.frequency_hz;
  lcr_pending_sample.sample_rate_hz = lcr_dual_snapshot.sample_rate_hz;

  phase_step = (LCR_DUAL_ADC_TWO_PI * (double)lcr_dual_snapshot.frequency_hz) /
               (double)lcr_dual_snapshot.sample_rate_hz;
  step_sin = sin(phase_step);
  step_cos = cos(phase_step);

  for (uint16_t i = 0U; i < LCR_DUAL_ADC_SAMPLE_COUNT; i++)
  {
    uint32_t packed = lcr_dual_raw[i];
    uint16_t vin_code = (uint16_t)(packed & 0xFFFFUL);
    uint16_t vr_code = (uint16_t)(packed >> 16);
    double vin_sample = (double)vin_code;
    double vr_sample = (double)vr_code;
    double next_sin;

    if (vin_code < vin_min) { vin_min = vin_code; }
    if (vin_code > vin_max) { vin_max = vin_code; }
    if (vr_code < vr_min) { vr_min = vr_code; }
    if (vr_code > vr_max) { vr_max = vr_code; }

    normal[0][0] += basis_sin * basis_sin;
    normal[0][1] += basis_sin * basis_cos;
    normal[0][2] += basis_sin;
    normal[0][3] += vin_sample * basis_sin;
    normal[0][4] += vr_sample * basis_sin;
    normal[1][1] += basis_cos * basis_cos;
    normal[1][2] += basis_cos;
    normal[1][3] += vin_sample * basis_cos;
    normal[1][4] += vr_sample * basis_cos;
    normal[2][2] += 1.0;
    normal[2][3] += vin_sample;
    normal[2][4] += vr_sample;

    next_sin = (basis_sin * step_cos) + (basis_cos * step_sin);
    basis_cos = (basis_cos * step_cos) - (basis_sin * step_sin);
    basis_sin = next_sin;
  }

  lcr_dual_snapshot.vin_min_code = vin_min;
  lcr_dual_snapshot.vin_max_code = vin_max;
  lcr_dual_snapshot.vr_min_code = vr_min;
  lcr_dual_snapshot.vr_max_code = vr_max;
  lcr_dual_snapshot.raw_valid = 1U;
  normal[1][0] = normal[0][1];
  normal[2][0] = normal[0][2];
  normal[2][1] = normal[1][2];

  if (LcrDualAdc_Solve3x5(normal, vin_coeff, vr_coeff) == 0U)
  {
    LcrDualAdc_SetInvalidResult(HAL_ADC_ERROR_INTERNAL,
                                LCR_DUAL_ADC_ERROR_FIT);
    return;
  }

  /* For x=a*sin(wt)+b*cos(wt)+c, the complex phasor is b-j*a. */
  lcr_pending_sample.vin.real = vin_coeff[1] * uv_per_code;
  lcr_pending_sample.vin.imag = -vin_coeff[0] * uv_per_code;
  lcr_pending_sample.vr.real = vr_coeff[1] * uv_per_code;
  lcr_pending_sample.vr.imag = -vr_coeff[0] * uv_per_code;
  lcr_pending_sample.vin_dc_uv = vin_coeff[2] * uv_per_code;
  lcr_pending_sample.vr_dc_uv = vr_coeff[2] * uv_per_code;
  vin_peak_uv = sqrt((vin_coeff[0] * vin_coeff[0]) +
                     (vin_coeff[1] * vin_coeff[1])) * uv_per_code;
  vr_peak_uv = sqrt((vr_coeff[0] * vr_coeff[0]) +
                    (vr_coeff[1] * vr_coeff[1])) * uv_per_code;
  lcr_dual_snapshot.vin_peak_uv = (uint32_t)(vin_peak_uv + 0.5);
  lcr_dual_snapshot.vr_peak_uv = (uint32_t)(vr_peak_uv + 0.5);

  if ((vin_min <= LCR_DUAL_ADC_OVERRANGE_LOW) ||
      (vin_max >= LCR_DUAL_ADC_OVERRANGE_HIGH) ||
      (vr_min <= LCR_DUAL_ADC_OVERRANGE_LOW) ||
      (vr_max >= LCR_DUAL_ADC_OVERRANGE_HIGH))
  {
    status |= LCR_CAPTURE_STATUS_OVERRANGE;
  }
  if ((vin_peak_uv < LCR_DUAL_ADC_LOW_SNR_PEAK_UV) ||
      (vr_peak_uv < LCR_DUAL_ADC_LOW_SNR_PEAK_UV))
  {
    status |= LCR_CAPTURE_STATUS_LOW_SNR;
  }
  lcr_pending_sample.status = status;
  lcr_dual_snapshot.last_status = status;
  lcr_dual_snapshot.error_source = LCR_DUAL_ADC_ERROR_NONE;
  lcr_dual_snapshot.capture_count++;
  lcr_dual_snapshot.state = LCR_DUAL_ADC_RESULT_READY;
  lcr_result_pending = 1U;
}

void LcrDualAdc_Init(void)
{
  memset(&lcr_dual_snapshot, 0, sizeof(lcr_dual_snapshot));
  memset(lcr_dual_raw, 0, sizeof(lcr_dual_raw));
  memset(&lcr_pending_sample, 0, sizeof(lcr_pending_sample));
  lcr_result_pending = 0U;
  lcr_irq_event = LCR_DUAL_ADC_IRQ_NONE;
  lcr_irq_error = HAL_ADC_ERROR_NONE;
  lcr_irq_dma_error = HAL_DMA_ERROR_NONE;
  lcr_dual_snapshot.state = LCR_DUAL_ADC_IDLE;

  if ((LcrDualAdc_ConfigureClock() != 0U) &&
      (LcrDualAdc_ConfigureDma() != 0U) &&
      (LcrDualAdc_ConfigureAdcs() != 0U))
  {
    lcr_dual_snapshot.hardware_ready = 1U;
  }
  else
  {
    lcr_dual_snapshot.hardware_ready = 0U;
    lcr_dual_snapshot.state = LCR_DUAL_ADC_ERROR;
    lcr_dual_snapshot.error_count++;
  }
}

LcrDualAdcSnapshot LcrDualAdc_GetSnapshot(void)
{
  return lcr_dual_snapshot;
}

uint8_t LcrCapture_IsAvailable(void)
{
  return lcr_dual_snapshot.hardware_ready;
}

uint8_t LcrCapture_Start(uint32_t frequency_hz, uint32_t reference_mohm)
{
  uint32_t sample_rate_hz;

  (void)reference_mohm;
  if ((lcr_dual_snapshot.hardware_ready == 0U) || (frequency_hz == 0UL) ||
      ((lcr_dual_snapshot.state != LCR_DUAL_ADC_IDLE) &&
       (lcr_dual_snapshot.state != LCR_DUAL_ADC_RESULT_READY) &&
       (lcr_dual_snapshot.state != LCR_DUAL_ADC_ERROR)))
  {
    return 0U;
  }

  lcr_dual_snapshot.frequency_hz = frequency_hz;
  lcr_dual_snapshot.vin_min_code = 0U;
  lcr_dual_snapshot.vin_max_code = 0U;
  lcr_dual_snapshot.vr_min_code = 0U;
  lcr_dual_snapshot.vr_max_code = 0U;
  lcr_dual_snapshot.vin_peak_uv = 0UL;
  lcr_dual_snapshot.vr_peak_uv = 0UL;
  lcr_dual_snapshot.last_status = 0U;
  lcr_dual_snapshot.raw_valid = 0U;
  lcr_dual_snapshot.frame_complete = 0U;
  lcr_dual_snapshot.irq_error_seen = 0U;
  lcr_dual_snapshot.error_source = LCR_DUAL_ADC_ERROR_NONE;
  lcr_dual_snapshot.dma_remaining = LCR_DUAL_ADC_SAMPLE_COUNT;
  lcr_dual_snapshot.last_hal_error = HAL_ADC_ERROR_NONE;
  lcr_dual_snapshot.last_dma_error = HAL_DMA_ERROR_NONE;

  sample_rate_hz = LcrDualAdc_SelectSampleRate(frequency_hz);
  if (LcrDualAdc_ConfigureTimer(sample_rate_hz) == 0U)
  {
    lcr_dual_snapshot.error_source = LCR_DUAL_ADC_ERROR_TIMER_CONFIG;
    lcr_dual_snapshot.last_hal_error = HAL_ADC_ERROR_INTERNAL;
    return 0U;
  }

  lcr_result_pending = 0U;
  lcr_irq_event = LCR_DUAL_ADC_IRQ_NONE;
  lcr_irq_error = HAL_ADC_ERROR_NONE;
  lcr_irq_dma_error = HAL_DMA_ERROR_NONE;
  SCB_CleanInvalidateDCache_by_Addr((uint32_t *)lcr_dual_raw,
                                    sizeof(lcr_dual_raw));
  if (HAL_ADCEx_MultiModeStart_DMA(&hadc1, lcr_dual_raw,
                                   LCR_DUAL_ADC_SAMPLE_COUNT) != HAL_OK)
  {
    lcr_dual_snapshot.last_hal_error = hadc1.ErrorCode;
    lcr_dual_snapshot.last_dma_error = lcr_dma.ErrorCode;
    lcr_dual_snapshot.error_source = LCR_DUAL_ADC_ERROR_DMA_START;
    return 0U;
  }
  lcr_dual_snapshot.dma_remaining = __HAL_DMA_GET_COUNTER(&lcr_dma);
  if (HAL_TIM_Base_Start(&lcr_timer) != HAL_OK)
  {
    (void)LcrDualAdc_StopDma();
    lcr_dual_snapshot.last_hal_error = HAL_ADC_ERROR_INTERNAL;
    lcr_dual_snapshot.error_source = LCR_DUAL_ADC_ERROR_TIMER_START;
    return 0U;
  }

  lcr_capture_tick = HAL_GetTick();
  lcr_dual_snapshot.state = LCR_DUAL_ADC_CAPTURING;
  return 1U;
}

void LcrCapture_Task(void)
{
  if (lcr_dual_snapshot.state == LCR_DUAL_ADC_CAPTURING)
  {
    uint8_t irq_event = lcr_irq_event;

    __DMB();
    if ((irq_event & LCR_DUAL_ADC_IRQ_ERROR) != 0U)
    {
      uint32_t hal_error = lcr_irq_error;
      lcr_irq_event = LCR_DUAL_ADC_IRQ_NONE;
      lcr_dual_snapshot.dma_remaining = __HAL_DMA_GET_COUNTER(&lcr_dma);
      lcr_dual_snapshot.frame_complete =
          ((irq_event & LCR_DUAL_ADC_IRQ_COMPLETE) != 0U) ? 1U : 0U;
      lcr_dual_snapshot.irq_error_seen = 1U;
      lcr_dual_snapshot.last_dma_error = lcr_irq_dma_error;
      (void)LcrDualAdc_StopDma();
      LcrDualAdc_SetInvalidResult(hal_error, LCR_DUAL_ADC_ERROR_ADC_IRQ);
    }
    else if ((irq_event & LCR_DUAL_ADC_IRQ_COMPLETE) != 0U)
    {
      lcr_irq_event = LCR_DUAL_ADC_IRQ_NONE;
      lcr_dual_snapshot.dma_remaining = __HAL_DMA_GET_COUNTER(&lcr_dma);
      lcr_dual_snapshot.frame_complete = 1U;
      lcr_dual_snapshot.state = LCR_DUAL_ADC_FRAME_READY;
    }
    else if ((HAL_GetTick() - lcr_capture_tick) >= LCR_DUAL_ADC_CAPTURE_TIMEOUT_MS)
    {
      lcr_dual_snapshot.dma_remaining = __HAL_DMA_GET_COUNTER(&lcr_dma);
      (void)HAL_TIM_Base_Stop(&lcr_timer);
      (void)LcrDualAdc_StopDma();
      LcrDualAdc_SetInvalidResult(HAL_TIMEOUT,
                                  LCR_DUAL_ADC_ERROR_CAPTURE_TIMEOUT);
    }
  }
  else if (lcr_dual_snapshot.state == LCR_DUAL_ADC_FRAME_READY)
  {
    (void)LcrDualAdc_StopDma();
    SCB_InvalidateDCache_by_Addr((uint32_t *)lcr_dual_raw,
                                sizeof(lcr_dual_raw));
    lcr_dual_snapshot.state = LCR_DUAL_ADC_PROCESSING;
  }
  else if (lcr_dual_snapshot.state == LCR_DUAL_ADC_PROCESSING)
  {
    LcrDualAdc_ProcessFrame();
  }
}

uint8_t LcrCapture_TakeSample(LcrCaptureSample *sample)
{
  if ((sample == 0) || (lcr_result_pending == 0U))
  {
    return 0U;
  }

  *sample = lcr_pending_sample;
  lcr_result_pending = 0U;
  lcr_dual_snapshot.state = LCR_DUAL_ADC_IDLE;
  return 1U;
}

void DMA1_Stream2_IRQHandler(void)
{
  HAL_DMA_IRQHandler(&lcr_dma);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if ((hadc == &hadc1) &&
      (lcr_dual_snapshot.state == LCR_DUAL_ADC_CAPTURING))
  {
    (void)HAL_TIM_Base_Stop(&lcr_timer);
    __DMB();
    lcr_irq_event |= LCR_DUAL_ADC_IRQ_COMPLETE;
  }
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
  if ((hadc == &hadc1) &&
      (lcr_dual_snapshot.state == LCR_DUAL_ADC_CAPTURING))
  {
    (void)HAL_TIM_Base_Stop(&lcr_timer);
    lcr_irq_error = hadc1.ErrorCode;
    lcr_irq_dma_error = lcr_dma.ErrorCode;
    __DMB();
    lcr_irq_event |= LCR_DUAL_ADC_IRQ_ERROR;
  }
}
