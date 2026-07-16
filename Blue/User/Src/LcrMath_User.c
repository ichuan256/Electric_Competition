#include "LcrMath_User.h"

#include <math.h>
#include <string.h>

#define LCR_MATH_PI                 3.14159265358979323846
#define LCR_MATH_MIN_MAGNITUDE      1.0e-12
#define LCR_MATH_L_SLOPE_MIN        0.35
#define LCR_MATH_C_SLOPE_MAX       -0.35
#define LCR_MATH_R_SLOPE_MAX        0.20
#define LCR_MATH_REACTIVE_PHASE_MIN 5.0
#define LCR_MATH_RESISTIVE_PHASE_MAX 15.0
#define LCR_MATH_MAX_SLOPES         15U

double LcrMath_Magnitude(LcrComplex value)
{
  return sqrt((value.real * value.real) + (value.imag * value.imag));
}

static void LcrMath_Sort(double *values, uint8_t count)
{
  for (uint8_t i = 1U; i < count; i++)
  {
    double value = values[i];
    uint8_t position = i;

    while ((position > 0U) && (values[position - 1U] > value))
    {
      values[position] = values[position - 1U];
      position--;
    }
    values[position] = value;
  }
}

static double LcrMath_Median(double *values, uint8_t count)
{
  if (count == 0U)
  {
    return 0.0;
  }

  LcrMath_Sort(values, count);
  if ((count & 1U) != 0U)
  {
    return values[count / 2U];
  }
  return (values[(count / 2U) - 1U] + values[count / 2U]) * 0.5;
}

uint8_t LcrMath_CalculateImpedance(LcrComplex vin, LcrComplex vr,
                                   double reference_ohm,
                                   LcrComplex *impedance)
{
  LcrComplex dut_voltage;
  double denominator;

  if ((impedance == 0) || (reference_ohm <= 0.0))
  {
    return 0U;
  }

  dut_voltage.real = vin.real - vr.real;
  dut_voltage.imag = vin.imag - vr.imag;
  denominator = (vr.real * vr.real) + (vr.imag * vr.imag);
  if (denominator <= LCR_MATH_MIN_MAGNITUDE)
  {
    return 0U;
  }

  impedance->real = reference_ohm *
      ((dut_voltage.real * vr.real) +
       (dut_voltage.imag * vr.imag)) / denominator;
  impedance->imag = reference_ohm *
      ((dut_voltage.imag * vr.real) -
       (dut_voltage.real * vr.imag)) / denominator;
  return 1U;
}

uint8_t LcrMath_CalculateAdmittance(LcrComplex vin, LcrComplex vr,
                                    double reference_ohm,
                                    LcrComplex *admittance)
{
  LcrComplex dut_voltage;
  double denominator;
  if ((admittance == 0) || (reference_ohm <= 0.0))
  {
    return 0U;
  }
  dut_voltage.real = vin.real - vr.real;
  dut_voltage.imag = vin.imag - vr.imag;
  denominator = (dut_voltage.real * dut_voltage.real) +
                (dut_voltage.imag * dut_voltage.imag);
  if (denominator <= LCR_MATH_MIN_MAGNITUDE)
  {
    return 0U;
  }
  admittance->real =
      ((vr.real * dut_voltage.real) + (vr.imag * dut_voltage.imag)) /
      (reference_ohm * denominator);
  admittance->imag =
      ((vr.imag * dut_voltage.real) - (vr.real * dut_voltage.imag)) /
      (reference_ohm * denominator);
  return 1U;
}

LcrComponentType LcrMath_Classify(const LcrSweepPoint *points,
                                  uint8_t point_count,
                                  double *median_slope)
{
  double slopes[LCR_MATH_MAX_SLOPES];
  double phases[LCR_MATH_MAX_SLOPES + 1U];
  uint8_t slope_count = 0U;
  uint8_t phase_count = 0U;
  double slope;
  double phase;
  int16_t previous = -1;

  if ((points == 0) || (point_count < 2U))
  {
    return LCR_COMPONENT_UNKNOWN;
  }

  for (uint8_t i = 0U; i < point_count; i++)
  {
    double magnitude;

    if (points[i].valid == 0U)
    {
      continue;
    }
    magnitude = LcrMath_Magnitude(points[i].impedance);
    if ((magnitude <= LCR_MATH_MIN_MAGNITUDE) ||
        (points[i].frequency_hz == 0UL))
    {
      continue;
    }

    if (phase_count < (LCR_MATH_MAX_SLOPES + 1U))
    {
      phases[phase_count++] = atan2(points[i].impedance.imag,
                                   points[i].impedance.real) *
                              (180.0 / LCR_MATH_PI);
    }

    if ((previous >= 0) && (slope_count < LCR_MATH_MAX_SLOPES))
    {
      double previous_magnitude = LcrMath_Magnitude(points[previous].impedance);
      double frequency_ratio = (double)points[i].frequency_hz /
                               (double)points[previous].frequency_hz;
      if ((previous_magnitude > LCR_MATH_MIN_MAGNITUDE) &&
          (frequency_ratio > 1.0))
      {
        slopes[slope_count++] = log(magnitude / previous_magnitude) /
                                log(frequency_ratio);
      }
    }
    previous = (int16_t)i;
  }

  if ((slope_count == 0U) || (phase_count == 0U))
  {
    return LCR_COMPONENT_UNKNOWN;
  }

  slope = LcrMath_Median(slopes, slope_count);
  phase = LcrMath_Median(phases, phase_count);
  if (median_slope != 0)
  {
    *median_slope = slope;
  }

  if ((slope > LCR_MATH_L_SLOPE_MIN) &&
      (phase > LCR_MATH_REACTIVE_PHASE_MIN))
  {
    return LCR_COMPONENT_L;
  }
  if ((slope < LCR_MATH_C_SLOPE_MAX) &&
      (phase < -LCR_MATH_REACTIVE_PHASE_MIN))
  {
    return LCR_COMPONENT_C;
  }
  if ((fabs(slope) < LCR_MATH_R_SLOPE_MAX) &&
      (fabs(phase) < LCR_MATH_RESISTIVE_PHASE_MAX))
  {
    return LCR_COMPONENT_R;
  }
  return LCR_COMPONENT_UNKNOWN;
}

uint8_t LcrMath_SelectBestFrequency(const LcrSweepPoint *points,
                                    uint8_t point_count,
                                    double reference_ohm,
                                    uint32_t *frequency_hz)
{
  double best_error = 1.0e100;
  uint32_t best_frequency = 0UL;

  if ((points == 0) || (frequency_hz == 0) || (reference_ohm <= 0.0))
  {
    return 0U;
  }

  for (uint8_t i = 0U; i < point_count; i++)
  {
    double magnitude;
    double error;

    if (points[i].valid == 0U)
    {
      continue;
    }
    magnitude = LcrMath_Magnitude(points[i].impedance);
    if (magnitude <= LCR_MATH_MIN_MAGNITUDE)
    {
      continue;
    }
    error = fabs(log(magnitude / reference_ohm));
    if (error < best_error)
    {
      best_error = error;
      best_frequency = points[i].frequency_hz;
    }
  }

  if (best_frequency == 0UL)
  {
    return 0U;
  }
  *frequency_hz = best_frequency;
  return 1U;
}

uint8_t LcrMath_DeriveResult(LcrComponentType type,
                             uint32_t frequency_hz,
                             LcrComplex impedance,
                             LcrDerivedResult *result)
{
  double resistance_abs;

  if ((result == 0) || (frequency_hz == 0UL) ||
      (type == LCR_COMPONENT_UNKNOWN))
  {
    return 0U;
  }

  memset(result, 0, sizeof(*result));
  result->type = type;
  result->frequency_hz = (double)frequency_hz;
  result->magnitude_ohm = LcrMath_Magnitude(impedance);
  result->resistance_ohm = impedance.real;
  result->reactance_ohm = impedance.imag;
  result->phase_deg = atan2(impedance.imag, impedance.real) *
                      (180.0 / LCR_MATH_PI);
  resistance_abs = fabs(impedance.real);
  if (resistance_abs > LCR_MATH_MIN_MAGNITUDE)
  {
    result->quality_factor = fabs(impedance.imag) / resistance_abs;
  }

  if (type == LCR_COMPONENT_L)
  {
    if (impedance.imag <= LCR_MATH_MIN_MAGNITUDE)
    {
      return 0U;
    }
    result->inductance_h = impedance.imag /
                           (2.0 * LCR_MATH_PI * (double)frequency_hz);
  }
  else if (type == LCR_COMPONENT_C)
  {
    if (impedance.imag >= -LCR_MATH_MIN_MAGNITUDE)
    {
      return 0U;
    }
    result->capacitance_f = -1.0 /
        (2.0 * LCR_MATH_PI * (double)frequency_hz * impedance.imag);
  }

  return 1U;
}
