#include "Delay.h"

// DWT-based precise delay for STM32H750 @ 480MHz
// All delays use the DWT cycle counter (increments at CPU clock rate)

static void DWT_Init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) == 0U)
    {
        DWT->CYCCNT = 0;
        DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    }
}

void Delay_Init(void)
{
    DWT_Init();
}

void Delay_ns(uint32_t ns)
{
    DWT_Init();
    // cycles = (SysClk / 1e6) * ns / 1e3  →  SysClk * ns / 1e9
    // At 480MHz: 480 cycles/us, ~0.48 cycles/ns
    uint32_t cycles = (SystemCoreClock / 1000000UL) * ns / 1000UL;
    if (cycles < 25) cycles = 25;  // min ~50ns, AD9910 CS/IUP pulse requirement
    uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles);
}

void Delay_us(uint32_t us)
{
    DWT_Init();
    uint32_t cycles = (SystemCoreClock / 1000000UL) * us;
    uint32_t start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < cycles);
}

void Delay_ms(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++)
        Delay_us(1000);
}
