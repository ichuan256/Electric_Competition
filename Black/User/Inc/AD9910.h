//-----------------------------------------------------------------
// AD9910����
// ͷ�ļ���: AD9910.h
// ��    ��: ���ǵ���
// ��ʼ����: 2019-12-1
// �������: 2019-12-25
// ��ǰ�汾: V1.0
// ��ʷ�汾:
//-----------------------------------------------------------------
#ifndef _AD9910_H
#define _AD9910_H
#include "stm32h7xx_hal.h"
#include "gpio.h"
#include "main.h"
#include "Delay.h"

#define AD9910_MRT1_Set	HAL_GPIO_WritePin(MRT1_GPIO_Port,MRT1_Pin,GPIO_PIN_SET)        
#define AD9910_MRT1_Clr 	HAL_GPIO_WritePin(MRT1_GPIO_Port,MRT1_Pin,GPIO_PIN_RESET)

#define AD9910_CSN1_Set HAL_GPIO_WritePin(CSN1_GPIO_Port,CSN1_Pin,GPIO_PIN_SET)        
#define AD9910_CSN1_Clr HAL_GPIO_WritePin(CSN1_GPIO_Port,CSN1_Pin,GPIO_PIN_RESET) 

#define AD9910_SCK1_Set HAL_GPIO_WritePin(SCK1_GPIO_Port,SCK1_Pin,GPIO_PIN_SET)        
#define AD9910_SCK1_Clr HAL_GPIO_WritePin(SCK1_GPIO_Port,SCK1_Pin,GPIO_PIN_RESET) 

#define AD9910_SDI1_Set HAL_GPIO_WritePin(SDI1_GPIO_Port,SDI1_Pin,GPIO_PIN_SET)        
#define AD9910_SDI1_Clr HAL_GPIO_WritePin(SDI1_GPIO_Port,SDI1_Pin,GPIO_PIN_RESET) 

#define AD9910_IUP1_Set HAL_GPIO_WritePin(IUP1_GPIO_Port,IUP1_Pin,GPIO_PIN_SET)        
#define AD9910_IUP1_Clr HAL_GPIO_WritePin(IUP1_GPIO_Port,IUP1_Pin,GPIO_PIN_RESET) 

#define AD9910_DRH1_Set HAL_GPIO_WritePin(DRH1_GPIO_Port,DRH1_Pin,GPIO_PIN_SET)     
#define AD9910_DRH1_Clr HAL_GPIO_WritePin(DRH1_GPIO_Port,DRH1_Pin,GPIO_PIN_RESET) 

#define AD9910_DRC1_Set HAL_GPIO_WritePin(DRC1_GPIO_Port,DRC1_Pin,GPIO_PIN_SET)         
#define AD9910_DRC1_Clr HAL_GPIO_WritePin(DRC1_GPIO_Port,DRC1_Pin,GPIO_PIN_RESET) 

#define AD9910_PF01_Set HAL_GPIO_WritePin(PF01_GPIO_Port,PF01_Pin,GPIO_PIN_SET)        
#define AD9910_PF01_Clr HAL_GPIO_WritePin(PF01_GPIO_Port,PF01_Pin,GPIO_PIN_RESET) 

#define AD9910_PF1_1_Set HAL_GPIO_WritePin(PF1_1_GPIO_Port,PF1_1_Pin,GPIO_PIN_SET)      
#define AD9910_PF1_1_Clr HAL_GPIO_WritePin(PF1_1_GPIO_Port,PF1_1_Pin,GPIO_PIN_RESET) 

#define AD9910_PF2_1_Set HAL_GPIO_WritePin(PF2_1_GPIO_Port,PF2_1_Pin,GPIO_PIN_SET)         
#define AD9910_PF2_1_Clr HAL_GPIO_WritePin(PF2_1_GPIO_Port,PF2_1_Pin,GPIO_PIN_RESET) 

#define AD9910_OSK1_Set HAL_GPIO_WritePin(OSK1_GPIO_Port,OSK1_Pin,GPIO_PIN_SET)  
#define AD9910_OSK1_Clr HAL_GPIO_WritePin(OSK1_GPIO_Port,OSK1_Pin,GPIO_PIN_RESET) 


extern void GPIO_Init_AD9910_1(void);
extern void Write_8bit_1(uint8_t dat)	;
extern void Write_32bit_1(uint32_t dat)	;

extern void AD9910_Init_1(void);

extern void AD9910_Singal_Profile_Init_1(void);
extern void AD9910_Singal_Profile_Set_1(uint8_t addr,uint32_t Freq,uint16_t Amp ,uint16_t Pha);
extern void Set_Profile_1(uint8_t num);

extern void AD9910_Osk_Init_1(void);
extern void AD9910_Osk_Set_1(void);

extern void AD9910_DRG_Fre_Init_1(void);
extern void AD9910_DRG_Freq_set_1(uint32_t upper_limit , uint32_t lower_limit ,uint32_t dec_step , uint32_t inc_step , uint16_t neg_rate ,uint16_t pos_rate);

extern void AD9910_DRG_AMP_Init_1(void);
extern void AD9910_DRG_Amp_Set_1( uint32_t upper_limit , uint32_t lower_limit ,uint32_t dec_step , uint32_t inc_step , uint16_t neg_rate ,uint16_t pos_rate);

extern void AD9910_RAM_Init_1(void);
extern void AD9910_RAM_ZB_Fre_Init_1(void);
extern void AD9910_RAM_ZB_Fre_Set_1(uint32_t Freq);

extern void AD9910_RAM_Fre_W_1(void);
extern void AD9910_RAM_AMP_W_1(void);
extern void AD9910_WAVE_RAM_AMP_W_1();
extern void AD9910_RAM_DIR_Fre_R_1(void);
extern void AD9910_RAM_DIR_AMP_R_1(void);

extern void AD9910_RAM_RAMP_UP_ONE_Fre_R_1(void);
extern void AD9910_RAM_RAMP_UP_ONE_AMP_R_1(void);

extern void AD9910_RAM_RAMP_UP_TWO_Fre_R_1(void);
extern void AD9910_RAM_RAMP_UP_TWO_AMP_R_1(void);

extern void AD9910_RAM_BID_RAMP_Fre_R_1(void);
extern void AD9910_RAM_BID_RAMP_AMP_R_1(void);

extern void AD9910_RAM_CON_BID_RAMP_Fre_R_1(void);
extern void AD9910_RAM_CON_BID_RAMP_AMP_R_1(void);

extern void AD9910_RAM_CON_RECIR_Fre_R_1(void);
extern void AD9910_RAM_CON_RECIR_AMP_R(uint32_t FRE_Out);
extern void AD9910_RAM_WAVE_AMP_R(void);
extern void AD9910_DRG_Pha_Init_1(void);
extern void AD9910_DRG_Pha_Set_1( uint32_t upper_limit , uint32_t lower_limit ,uint32_t dec_step , uint32_t inc_step , uint16_t neg_rate ,uint16_t pos_rate);

extern void AD9910_RAM_Pha_W_1(void);
extern void AD9910_RAM_DIR_PHA_R_1(void);
extern void AD9910_RAM_RAMP_UP_ONE_PHA_R_1(void);
extern void AD9910_RAM_RAMP_UP_TWO_PHA_R_1(void);
extern void AD9910_RAM_BID_RAMP_PHA_R_1(void);
extern void AD9910_RAM_CON_BID_RAMP_PHA_R_1(void);
extern void AD9910_RAM_CON_RECIR_PHA_R_1(void);

extern __IO uint32_t RAM_AMP[];
extern void AD9910_RAM_WAVE_Set();
extern void AD9910_LinearASF_Test();
extern void AD9910_FrequencySweep(uint32_t start_freq, uint32_t freq_step,uint16_t Amp,uint16_t phase);
extern void AD9910_IUP();

void Par_mod_1(uint8_t des ,uint16_t FF);
void AD9910_Init_Sin_1(int gain);
void Freq_convert_1(uint32_t Freq);
#endif
