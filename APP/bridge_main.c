/*********************************************************************
 * File Name          : bridge_main.c
 * Description        : Main entry point for CH592 BLE to USB CDC bridge.
 *********************************************************************/

#include "CONFIG.h"
#include "HAL.h"
#include "CH59x_common.h"
#include "CH59x_sys.h"
#include "CH59x_gpio.h"
#include "usb_cdc.h"
#include "bridge.h"

/*********************************************************************
 * GLOBAL TYPEDEFS & BUFFERS
 *********************************************************************/
__attribute__((aligned(4))) uint32_t MEM_BUF[BLE_MEMHEAP_SIZE / 4];

/*********************************************************************
 * MAIN EVENT LOOP
 * Runs TMOS scheduler, USB-to-BLE packet pump, and button monitor
 *********************************************************************/
__HIGH_CODE
__attribute__((noinline))
void Main_Circulation(void)
{
    while (1)
    {
        /* Run BLE stack events & timers */
        TMOS_SystemProcess();

        /* Pump pending bytes between USB CDC and BLE */
        Bridge_Poll();

        /* Poll BOOT button for status report */
        Bridge_CheckButton();

        /* Reboot MCU if host sets 1200 bps touch-to-reset baud rate */
        if (usb_cdc_is_reboot_requested())
        {
            mDelaymS(30);
            SYS_ResetExecute();
        }
    }
}

/*********************************************************************
 * MAIN FUNCTION
 *********************************************************************/
int main(void)
{
#if (defined(DCDC_ENABLE)) && (DCDC_ENABLE == TRUE)
    PWR_DCDCCfg(ENABLE);
#endif

    /* Use 60MHz PLL clock for consistent USB and BLE radio timing */
    SetSysClock(CLK_SOURCE_PLL_60MHz);

    /* GPIO parking:
     * - Protect PB10 & PB11: USB D- / D+
     * - Protect PB14 & PB15: 32MHz main oscillator
     * - Protect PB22: BOOT button strapping pin (active low with pullup) */
    GPIOA_ModeCfg(GPIO_Pin_All, GPIO_ModeIN_PD);
    GPIOB_ModeCfg(GPIO_Pin_All & ~(GPIO_Pin_10 | GPIO_Pin_11 | GPIO_Pin_14 | GPIO_Pin_15 | GPIO_Pin_22), GPIO_ModeIN_PD);
    GPIOB_ModeCfg(BOOT_BUTTON_PIN, GPIO_ModeIN_PU);

    /* Initialize USB CDC ACM Virtual COM Port subsystem */
    usb_cdc_init();

    /* Initialize WCH BLE Controller & Host Stack */
    CH59x_BLEInit();
    HAL_Init();
    GAPRole_PeripheralInit();

    /* Initialize Bridge Application & Nordic NUS GATT Service */
    Bridge_Init();

    /* Enter main event loop */
    Main_Circulation();
}
