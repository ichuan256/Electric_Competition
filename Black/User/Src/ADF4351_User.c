#include "ADF4351_User.h"

int32_t Freq=350;																						// 初始频率F=35M
int32_t Freq_last=0;
extern uint32_t R;
uint8_t PLL_flag=0;

//0.1N MHz
void ADF4351_SetFreq(uint32_t N)
{
	if(Freq_last!=N)
	{
		PLL_flag=1;
		Freq_last=N;
	}
	
  if(PLL_flag==1)																												// 当更改频率时，写入数据
	{
		PLL_flag=0;
		if (N>=690&&N<=1370)	 ADF4351_Wdata(0x0050443c);  // if,else if中得语句是判断此时频率得范围		  
		else if (N>1370&&N<=2740) ADF4351_Wdata(0x0040443c); // 根据频率的范围确定我们要更新此时寄存器4（RF diver）的值
		else if (N>2740&&N<=5490) ADF4351_Wdata(0x0030143c);
		else if (N>5490&&N<=10990) ADF4351_Wdata(0x0020143c); 
		else if (N>10990&&N<=21990) ADF4351_Wdata(0x0010143c); 
		else if (N>21990)  ADF4351_Wdata(0x0000143c); 
		else ADF4351_Wdata(0x0060443c);	
																		
		ADF4351_Wdata(0x00000000|N<<15); 	   // 把要输出频率的字写入寄存器0，改变输出频率
	}
}
