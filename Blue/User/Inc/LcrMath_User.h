#ifndef _LCR_MATH_USER_H_
#define _LCR_MATH_USER_H_

#include <stdint.h>

typedef struct {
  double real;
  double imag;
} LcrComplex;

typedef enum {
  LCR_COMPONENT_UNKNOWN = 0,
  LCR_COMPONENT_R,
  LCR_COMPONENT_L,
  LCR_COMPONENT_C
} LcrComponentType;

typedef struct {
  uint32_t frequency_hz;
  LcrComplex impedance;
  uint8_t valid;
} LcrSweepPoint;

typedef struct {
  LcrComponentType type;
  double frequency_hz;
  double magnitude_ohm;
  double resistance_ohm;
  double reactance_ohm;
  double phase_deg;
  double inductance_h;
  double capacitance_f;
  double quality_factor;
} LcrDerivedResult;

uint8_t LcrMath_CalculateImpedance(LcrComplex vin, LcrComplex vr,
                                   double reference_ohm,
                                   LcrComplex *impedance);
uint8_t LcrMath_CalculateAdmittance(LcrComplex vin, LcrComplex vr,
                                    double reference_ohm,
                                    LcrComplex *admittance);
LcrComponentType LcrMath_Classify(const LcrSweepPoint *points,
                                  uint8_t point_count,
                                  double *median_slope);
uint8_t LcrMath_SelectBestFrequency(const LcrSweepPoint *points,
                                    uint8_t point_count,
                                    double reference_ohm,
                                    uint32_t *frequency_hz);
uint8_t LcrMath_DeriveResult(LcrComponentType type,
                             uint32_t frequency_hz,
                             LcrComplex impedance,
                             LcrDerivedResult *result);
double LcrMath_Magnitude(LcrComplex value);

#endif
