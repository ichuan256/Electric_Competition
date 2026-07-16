#ifndef _SPECTRUM_DDS_USER_H_
#define _SPECTRUM_DDS_USER_H_

#include <stdint.h>

#define SPECTRUM_DDS_AMPLITUDE_CODE_MAX            8191U
#define SPECTRUM_DDS_SUM_TARGET_MAX_MVPP             600UL
#define SPECTRUM_DDS_GAIN_PPM                     956700UL
#define SPECTRUM_DDS_COMMAND_MAX_MVPP                 780UL
/* AD9910_User applies 100/92 internally, so 718 mVpp reaches ASF=0x3FFF. */
#define SPECTRUM_DDS_FULL_SCALE_COMMAND_MVPP          718UL

static inline uint32_t SpectrumDds_AmplitudeCommandMvpp(
    uint16_t amplitude_code, uint8_t pure_sine)
{
  uint32_t command_mvpp;

  if (amplitude_code > SPECTRUM_DDS_AMPLITUDE_CODE_MAX)
  {
    amplitude_code = SPECTRUM_DDS_AMPLITUDE_CODE_MAX;
  }

  if (pure_sine != 0U)
  {
    return (uint32_t)((((uint64_t)amplitude_code *
                        SPECTRUM_DDS_FULL_SCALE_COMMAND_MVPP) +
                       (SPECTRUM_DDS_AMPLITUDE_CODE_MAX / 2U)) /
                      SPECTRUM_DDS_AMPLITUDE_CODE_MAX);
  }

  command_mvpp = (uint32_t)((((uint64_t)amplitude_code *
                               SPECTRUM_DDS_SUM_TARGET_MAX_MVPP *
                               1000000ULL) +
                              ((uint64_t)SPECTRUM_DDS_AMPLITUDE_CODE_MAX *
                               SPECTRUM_DDS_GAIN_PPM / 2ULL)) /
                             ((uint64_t)SPECTRUM_DDS_AMPLITUDE_CODE_MAX *
                              SPECTRUM_DDS_GAIN_PPM));
  if (command_mvpp > SPECTRUM_DDS_COMMAND_MAX_MVPP)
  {
    command_mvpp = SPECTRUM_DDS_COMMAND_MAX_MVPP;
  }
  return command_mvpp;
}

#endif
