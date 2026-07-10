#include "AD9226_User.h"

uint32_t AD9226_Rec_Buf[1024];
double adc_value=0;
double adc_volt=0;
CollectFlag COLLECTFLAG;

void AD9226_Set_DMA_collection_flag(CollectFlag collectflag)
{
  COLLECTFLAG = collectflag;
}
CollectFlag AD9226_Get_DMA_Complete_Flag()
{
  return COLLECTFLAG;
}




	
/**
 * @brief  ��ȡ GPIOD �˿ڵ�ƽ
 * @note   ֻ���ص� 12 λ�������� ADC 12 λ���
 * @retval uint16_t 12 λ��ֵ (0~4095)
 */
double Read_GPIOD_12bit(void)
{
  uint16_t val;

  // ֱ�Ӷ��Ĵ��� IDR��GPIO �������ݼĴ���
  val = GPIOD->IDR & 0x0FFF; // �� 12 λ
//	double val_ = 0;
//	val_=Linear_ADCtoVoltage(val);
  return val;
}
double adc_to_volt(double adc)
{
	if (adc <= 72.100000f)
			return -0.00161017f * adc + 3.28150847f;  // 段1: x∈[2.8, 72.1]
	else if (adc <= 1747.800000f)
			return -0.00154705f * adc + 3.25398938f;  // 段2: x∈[72.1, 1747.8]
	else if (adc <= 1879.800000f)
			return -0.00150602f * adc + 3.18332530f;  // 段3: x∈[1747.8, 1879.8]
	else if (adc <= 2010.600000f)
			return -0.00152905f * adc + 3.22577064f;  // 段4: x∈[1879.8, 2010.6]
	else if (adc <= 2142.500000f)
			return -0.00149701f * adc + 3.15938323f;  // 段5: x∈[2010.6, 2142.5]
	else if (adc <= 2273.700000f)
			return -0.00152439f * adc + 3.21485366f;  // 段6: x∈[2142.5, 2273.7]
	else if (adc <= 2408.200000f)
			return -0.00147661f * adc + 3.10494444f;  // 段7: x∈[2273.7, 2408.2]
	else
			return -0.00153170f * adc + 3.23985073f;  // 段8: x∈[2408.2, 4093.8]
}
