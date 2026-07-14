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

static AdcFftMeasureSnapshot adc_fft_state;
static uint16_t adc_fft_raw[ADC_FFT_SAMPLE_COUNT];
static uint8_t adc_fft_result_pending;

static uint32_t AdcFft_SupportedOrDefaultRate(uint32_t sample_rate_hz)
{
  if (AdcFftMeasure_IsSupportedSampleRate(sample_rate_hz) != 0U)
  {
    return sample_rate_hz;
  }
  return ADC_FFT_DEFAULT_SAMPLE_RATE;
}

static uint8_t AdcFft_ShouldUseHann(const AdcFftMeasurementRequest *request, uint16_t target_bin)
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

  bin_frequency_hz = ((uint64_t)target_bin * request->sample_rate_hz) / ADC_FFT_SAMPLE_COUNT;
  delta_hz = (request->reference_frequency_hz > bin_frequency_hz) ?
             (uint32_t)(request->reference_frequency_hz - bin_frequency_hz) :
             (uint32_t)(bin_frequency_hz - request->reference_frequency_hz);

  if (((request->flags & 0x01U) != 0U) && (delta_hz <= 2UL))
  {
    return 0U;
  }
  return 1U;
}

static uint8_t AdcFft_CaptureSoftware(void)
{
  uint32_t start_tick = HAL_GetTick();

  for (uint16_t i = 0U; i < ADC_FFT_SAMPLE_COUNT; i++)
  {
    if (HAL_ADC_Start(&hadc1) != HAL_OK)
    {
      (void)HAL_ADC_Stop(&hadc1);
      return 0U;
    }
    if (HAL_ADC_PollForConversion(&hadc1, 2U) != HAL_OK)
    {
      (void)HAL_ADC_Stop(&hadc1);
      return 0U;
    }
    adc_fft_raw[i] = (uint16_t)HAL_ADC_GetValue(&hadc1);
    (void)HAL_ADC_Stop(&hadc1);
  }

  adc_fft_state.last_capture_ticks = HAL_GetTick() - start_tick;
  return 1U;
}

static float AdcFft_WindowValue(uint16_t index, uint8_t use_hann)
{
  if (use_hann == 0U)
  {
    return 1.0f;
  }

  return 0.5f - (0.5f * cosf((ADC_FFT_TWO_PI * (float)index) /
                             (float)(ADC_FFT_SAMPLE_COUNT - 1U)));
}

static void AdcFft_ProcessFrame(void)
{
  const AdcFftMeasurementRequest *request = &adc_fft_state.active_request;
  AdcFftMeasurementResult result;
  uint64_t sum = 0ULL;
  uint16_t adc_min = 0xFFFFU;
  uint16_t adc_max = 0U;
  uint16_t target_bin = request->target_bin;
  uint16_t search_start;
  uint16_t search_end;
  uint16_t best_bin = 0U;
  float best_real = 0.0f;
  float best_imag = 0.0f;
  float best_mag2 = 0.0f;
  float noise_sum = 0.0f;
  uint8_t noise_count = 0U;
  uint8_t use_hann;
  float coherent_gain;
  uint32_t process_start = HAL_GetTick();

  memset(&result, 0, sizeof(result));
  result.sweep_id = request->sweep_id;
  result.point_id = request->point_id;
  result.reference_frequency_hz = request->reference_frequency_hz;
  result.sample_rate_hz = request->sample_rate_hz;

  for (uint16_t i = 0U; i < ADC_FFT_SAMPLE_COUNT; i++)
  {
    uint16_t code = adc_fft_raw[i];
    sum += code;
    if (code < adc_min)
    {
      adc_min = code;
    }
    if (code > adc_max)
    {
      adc_max = code;
    }
  }

  if (target_bin == ADC_FFT_TARGET_BIN_AUTO)
  {
    target_bin = AdcFftMeasure_CalculateTargetBin(request->reference_frequency_hz,
                                                  request->sample_rate_hz);
  }
  result.target_bin = target_bin;
  result.adc_min_code = adc_min;
  result.adc_max_code = adc_max;

  if ((target_bin == 0U) || (target_bin >= (ADC_FFT_SAMPLE_COUNT / 2U)))
  {
    result.status |= ADC_FFT_STATUS_REFERENCE_OUT_OF_RANGE;
    adc_fft_state.last_result = result;
    adc_fft_state.last_error = result.status;
    adc_fft_state.state = ADC_FFT_ERROR;
    adc_fft_state.error_count++;
    adc_fft_result_pending = 1U;
    return;
  }

  use_hann = AdcFft_ShouldUseHann(request, target_bin);
  coherent_gain = (use_hann != 0U) ? 0.5f : 1.0f;
  result.status |= (use_hann != 0U) ? ADC_FFT_STATUS_USED_HANN_WINDOW :
                                      ADC_FFT_STATUS_USED_RECT_WINDOW;
  result.status |= ADC_FFT_STATUS_SOFTWARE_CAPTURE;

  if ((adc_min <= ADC_FFT_OVERRANGE_LOW) || (adc_max >= ADC_FFT_OVERRANGE_HIGH))
  {
    result.status |= ADC_FFT_STATUS_ADC_OVERRANGE;
  }

  search_start = (target_bin > ADC_FFT_SEARCH_HALF_WIDTH) ?
                 (uint16_t)(target_bin - ADC_FFT_SEARCH_HALF_WIDTH) : 1U;
  search_end = (uint16_t)(target_bin + ADC_FFT_SEARCH_HALF_WIDTH);
  if (search_end > ((ADC_FFT_SAMPLE_COUNT / 2U) - 1U))
  {
    search_end = (uint16_t)((ADC_FFT_SAMPLE_COUNT / 2U) - 1U);
  }

  for (uint16_t bin = search_start; bin <= search_end; bin++)
  {
    float real = 0.0f;
    float imag = 0.0f;
    float angle_step = ADC_FFT_TWO_PI * (float)bin / (float)ADC_FFT_SAMPLE_COUNT;
    float cos_step = cosf(angle_step);
    float sin_step = sinf(angle_step);
    float c = 1.0f;
    float s = 0.0f;

    for (uint16_t n = 0U; n < ADC_FFT_SAMPLE_COUNT; n++)
    {
      float centered = (float)((int32_t)adc_fft_raw[n] - (int32_t)(sum / ADC_FFT_SAMPLE_COUNT));
      float sample_uv = (centered * (float)ADC_FFT_VREF_UV) / (float)ADC_FFT_FULL_SCALE_CODE;
      float windowed = sample_uv * AdcFft_WindowValue(n, use_hann);
      float next_c;
      float next_s;

      real += windowed * c;
      imag -= windowed * s;

      next_c = (c * cos_step) - (s * sin_step);
      next_s = (s * cos_step) + (c * sin_step);
      c = next_c;
      s = next_s;
    }

    {
      float mag2 = (real * real) + (imag * imag);
      if (mag2 > best_mag2)
      {
        best_mag2 = mag2;
        best_bin = bin;
        best_real = real;
        best_imag = imag;
      }
      else
      {
        noise_sum += mag2;
        noise_count++;
      }
    }
  }

  if (best_bin == 0U)
  {
    result.status |= ADC_FFT_STATUS_LOW_SNR;
  }
  else
  {
    float mag = sqrtf((best_real * best_real) + (best_imag * best_imag));
    float peak_uv = (2.0f * mag) / ((float)ADC_FFT_SAMPLE_COUNT * coherent_gain);
    float rms_uv = peak_uv / ADC_FFT_SQRT2;

    result.main_bin = best_bin;
    result.main_frequency_hz = (uint32_t)(((uint64_t)best_bin * request->sample_rate_hz) /
                                          ADC_FFT_SAMPLE_COUNT);
    result.voltage_uv_peak = (uint32_t)(peak_uv + 0.5f);
    result.voltage_uv_rms = (uint32_t)(rms_uv + 0.5f);

    if ((best_bin < search_start) || (best_bin > search_end))
    {
      result.status |= ADC_FFT_STATUS_PEAK_NOT_NEAR_REFERENCE;
    }
    if ((noise_count != 0U) && (best_mag2 < ((noise_sum / (float)noise_count) * 4.0f)))
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

  adc_fft_state.last_process_ticks = HAL_GetTick() - process_start;
  adc_fft_state.last_result = result;
  adc_fft_state.result_count++;
  adc_fft_result_pending = 1U;
  adc_fft_state.state = ADC_FFT_RESULT_READY;
}

void AdcFftMeasure_Init(void)
{
  memset(&adc_fft_state, 0, sizeof(adc_fft_state));
  adc_fft_state.state = ADC_FFT_IDLE;
  adc_fft_result_pending = 0U;
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

  numerator = ((uint64_t)frequency_hz * ADC_FFT_SAMPLE_COUNT) + (sample_rate_hz / 2UL);
  return (uint16_t)(numerator / sample_rate_hz);
}

uint8_t AdcFftMeasure_Start(const AdcFftMeasurementRequest *request)
{
  if (request == 0)
  {
    return 0U;
  }
  if ((adc_fft_state.state != ADC_FFT_IDLE) &&
      (adc_fft_state.state != ADC_FFT_RESULT_READY) &&
      (adc_fft_state.state != ADC_FFT_ERROR))
  {
    return 0U;
  }

  adc_fft_state.active_request = *request;
  adc_fft_state.active_request.sample_rate_hz =
      AdcFft_SupportedOrDefaultRate(request->sample_rate_hz);
  adc_fft_state.request_count++;
  adc_fft_state.state = ADC_FFT_ACCEPTED;
  adc_fft_result_pending = 0U;
  return 1U;
}

void AdcFftMeasure_Task(void)
{
  switch (adc_fft_state.state)
  {
  case ADC_FFT_ACCEPTED:
    adc_fft_state.state = ADC_FFT_PRE_DELAY;
    break;

  case ADC_FFT_PRE_DELAY:
    if (adc_fft_state.active_request.pre_capture_delay_us != 0U)
    {
      uint32_t delay_ms = (adc_fft_state.active_request.pre_capture_delay_us + 999UL) / 1000UL;
      HAL_Delay(delay_ms);
    }
    adc_fft_state.state = ADC_FFT_CAPTURING;
    break;

  case ADC_FFT_CAPTURING:
    if (AdcFft_CaptureSoftware() == 0U)
    {
      adc_fft_state.last_error = ADC_FFT_STATUS_FRAME_TIMEOUT;
      adc_fft_state.last_result.status = ADC_FFT_STATUS_FRAME_TIMEOUT;
      adc_fft_state.error_count++;
      adc_fft_result_pending = 1U;
      adc_fft_state.state = ADC_FFT_ERROR;
    }
    else
    {
      adc_fft_state.state = ADC_FFT_PROCESSING;
    }
    break;

  case ADC_FFT_PROCESSING:
    AdcFft_ProcessFrame();
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

  *result = adc_fft_state.last_result;
  adc_fft_result_pending = 0U;
  adc_fft_state.state = ADC_FFT_IDLE;
  return 1U;
}

AdcFftMeasureState AdcFftMeasure_GetState(void)
{
  return adc_fft_state.state;
}

AdcFftMeasureSnapshot AdcFftMeasure_GetSnapshot(void)
{
  return adc_fft_state;
}
