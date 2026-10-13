/*********************************************************************
 * File Name          : bridge.h
 * Description        : Header for BLE to USB CDC Transparent Bridge
 *********************************************************************/

#ifndef __BRIDGE_H__
#define __BRIDGE_H__

#include <stdint.h>
#include "CONFIG.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TMOS Task Events */
#define BRIDGE_START_DEVICE_EVT       0x0001
#define BRIDGE_PARAM_UPDATE_EVT       0x0002
#define BRIDGE_PUMP_USB_TO_BLE_EVT    0x0004
#define BRIDGE_BTN_UNLOCK_EVT         0x0008
#define BRIDGE_PHY_UPDATE_EVT         0x0010

/* Hardware Pins */
#define BOOT_BUTTON_PIN               GPIO_Pin_22

/**
 * @brief Initialize BLE stack GAP/GATT and BLE UART service for bridge.
 */
void Bridge_Init(void);

/**
 * @brief TMOS event processor for Bridge application.
 */
uint16_t Bridge_ProcessEvent(uint8_t task_id, uint16_t events);

/**
 * @brief Main polling loop function called from Main_Circulation().
 *        Transfers pending USB CDC bytes to BLE notification packets.
 */
void Bridge_Poll(void);

/**
 * @brief Check BOOT button (PB22) and print diagnostic status over USB CDC.
 */
void Bridge_CheckButton(void);

#ifdef __cplusplus
}
#endif

#endif /* __BRIDGE_H__ */
