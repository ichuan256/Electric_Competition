#include "LogDetector_User.h"

static LogDetectorSample log_detector_last_sample = {0};

static int16_t LogDetector_MvToDbmX10(uint16_t mv)
{
  int32_t dbm_x10;

  /* Pin_dBm = Vout / 0.025 - 84.
     Vout is in mV, so Pin_dBm = mv / 25 - 84. */
  dbm_x10 = (((int32_t)mv * 10) / 25) - 840;

  if (dbm_x10 > 32767)
  {
    dbm_x10 = 32767;
  }
  else if (dbm_x10 < -32768)
  {
    dbm_x10 = -32768;
  }

  return (int16_t)dbm_x10;
}

void LogDetector_Init(void)
{
  log_detector_last_sample.raw = 0;
  log_detector_last_sample.mv = 0;
  log_detector_last_sample.dbm_x10 = LogDetector_MvToDbmX10(0);
  log_detector_last_sample.valid = 0;

  (void)HAL_ADCEx_Calibration_Start(&hadc1, ADC_CALIB_OFFSET_LINEARITY, ADC_SINGLE_ENDED);
}

LogDetectorSample LogDetector_ReadAverage(uint16_t sample_count)
{
  uint64_t sum = 0;
  uint16_t ok_count = 0;
  LogDetectorSample sample;

  if (sample_count == 0U)
  {
    sample_count = LOG_DETECTOR_DEFAULT_AVG_COUNT;
  }

  for (uint16_t i = 0; i < sample_count; i++)
  {
    if (HAL_ADC_Start(&hadc1) != HAL_OK)
    {
      break;
    }

    if (HAL_ADC_PollForConversion(&hadc1, 2U) == HAL_OK)
    {
      sum += HAL_ADC_GetValue(&hadc1);
      ok_count++;
    }

    (void)HAL_ADC_Stop(&hadc1);
  }

  if (ok_count == 0U)
  {
    sample.raw = log_detector_last_sample.raw;
    sample.mv = log_detector_last_sample.mv;
    sample.dbm_x10 = log_detector_last_sample.dbm_x10;
    sample.valid = 0;
    log_detector_last_sample = sample;
    return sample;
  }

  sample.raw = (uint32_t)(sum / ok_count);
  sample.mv = (uint16_t)((sample.raw * LOG_DETECTOR_ADC_VREF_MV) / LOG_DETECTOR_ADC_FULL_SCALE_RAW);
  sample.dbm_x10 = LogDetector_MvToDbmX10(sample.mv);
  sample.valid = 1;
  log_detector_last_sample = sample;

  return sample;
}

LogDetectorSample LogDetector_GetLastSample(void)
{
  return log_detector_last_sample;
}

const char *LogDetector_StatusText(uint8_t valid)
{
  return (valid != 0U) ? "OK" : "ADC ERR";
}
