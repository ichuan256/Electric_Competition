#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "LcrMath_User.h"

#define TEST_PI 3.14159265358979323846

static int nearly_equal(double actual, double expected, double relative_error)
{
  double scale = fabs(expected);
  if (scale < 1.0)
  {
    scale = 1.0;
  }
  return fabs(actual - expected) <= (scale * relative_error);
}

static LcrComplex divider_node(LcrComplex impedance, double reference_ohm)
{
  LcrComplex denominator = {reference_ohm + impedance.real, impedance.imag};
  double power = (denominator.real * denominator.real) +
                 (denominator.imag * denominator.imag);
  LcrComplex result;

  result.real = reference_ohm * denominator.real / power;
  result.imag = -reference_ohm * denominator.imag / power;
  return result;
}

static void test_impedance_round_trip(void)
{
  LcrComplex vin = {1.0, 0.0};
  LcrComplex expected = {12.5, -345.0};
  LcrComplex vr = divider_node(expected, 1000.0);
  LcrComplex actual = {0.0, 0.0};
  LcrComplex admittance = {0.0, 0.0};

  assert(LcrMath_CalculateImpedance(vin, vr, 1000.0, &actual) != 0U);
  assert(nearly_equal(actual.real, expected.real, 1.0e-9));
  assert(nearly_equal(actual.imag, expected.imag, 1.0e-9));
  assert(LcrMath_CalculateAdmittance(vin, vr, 1000.0, &admittance) != 0U);
  assert(nearly_equal(admittance.real,
                      expected.real / ((expected.real * expected.real) +
                                       (expected.imag * expected.imag)), 1.0e-9));
  assert(nearly_equal(admittance.imag,
                      -expected.imag / ((expected.real * expected.real) +
                                        (expected.imag * expected.imag)), 1.0e-9));
}

static void make_points(LcrSweepPoint *points, LcrComponentType type)
{
  static const uint32_t frequencies[10] = {
    1000U, 2000U, 5000U, 10000U, 20000U,
    50000U, 100000U, 200000U, 500000U, 1000000U
  };

  for (unsigned int i = 0U; i < 10U; i++)
  {
    double omega = 2.0 * TEST_PI * (double)frequencies[i];
    points[i].frequency_hz = frequencies[i];
    points[i].valid = 1U;
    points[i].impedance.real = 1.0;
    if (type == LCR_COMPONENT_R)
    {
      points[i].impedance.real = 1000.0;
      points[i].impedance.imag = 0.0;
    }
    else if (type == LCR_COMPONENT_L)
    {
      points[i].impedance.imag = omega * 0.01;
    }
    else
    {
      points[i].impedance.imag = -1.0 / (omega * 10.0e-9);
    }
  }
}

static void test_auto_classification(void)
{
  LcrSweepPoint points[10];
  double slope = 0.0;

  make_points(points, LCR_COMPONENT_R);
  assert(LcrMath_Classify(points, 10U, &slope) == LCR_COMPONENT_R);
  assert(fabs(slope) < 1.0e-9);

  make_points(points, LCR_COMPONENT_L);
  assert(LcrMath_Classify(points, 10U, &slope) == LCR_COMPONENT_L);
  assert(slope > 0.9);

  make_points(points, LCR_COMPONENT_C);
  assert(LcrMath_Classify(points, 10U, &slope) == LCR_COMPONENT_C);
  assert(slope < -0.9);
}

static void test_best_frequency_and_values(void)
{
  LcrSweepPoint points[10];
  uint32_t frequency_hz = 0U;
  LcrDerivedResult result;
  LcrComplex impedance;

  make_points(points, LCR_COMPONENT_L);
  assert(LcrMath_SelectBestFrequency(points, 10U, 1000.0,
                                     &frequency_hz) != 0U);
  assert(frequency_hz == 20000U);

  impedance.real = 2.0;
  impedance.imag = 2.0 * TEST_PI * 20000.0 * 0.01;
  assert(LcrMath_DeriveResult(LCR_COMPONENT_L, 20000U,
                              impedance, &result) != 0U);
  assert(nearly_equal(result.inductance_h, 0.01, 1.0e-9));

  impedance.imag = -1.0 / (2.0 * TEST_PI * 20000.0 * 10.0e-9);
  assert(LcrMath_DeriveResult(LCR_COMPONENT_C, 20000U,
                              impedance, &result) != 0U);
  assert(nearly_equal(result.capacitance_f, 10.0e-9, 1.0e-9));
}

int main(void)
{
  test_impedance_round_trip();
  test_auto_classification();
  test_best_frequency_and_values();
  puts("LcrMathTests: PASS");
  return 0;
}
