#ifndef __USB_CDC_H__
#define __USB_CDC_H__

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize USB CDC ACM hardware peripheral, endpoints, and ring buffers.
 *        Forces a USB bus disconnect/reconnect cycle to trigger host enumeration.
 */
void usb_cdc_init(void);

/**
 * @brief Check if USB device has been enumerated and configured by the host.
 * @return 1 if configured, 0 otherwise.
 */
uint8_t usb_cdc_is_configured(void);

/**
 * @brief Check if terminal emulator is open on the host (DTR line asserted).
 * @return 1 if terminal connected, 0 otherwise.
 */
uint8_t usb_cdc_is_connected(void);

/**
 * @brief Get current baud rate requested by host via SET_LINE_CODING.
 * @return Baud rate (e.g. 115200).
 */
uint32_t usb_cdc_get_baud_rate(void);

/**
 * @brief Transmit data over USB CDC ACM (Bulk IN).
 *        Non-blocking where possible; buffers in ring buffer and sends via ISR.
 *        Includes timeout protection so the CPU never hangs if USB is disconnected.
 * @param buf Pointer to data to send.
 * @param len Number of bytes to send.
 * @return Number of bytes actually written.
 */
int usb_cdc_write(const void *buf, int len);

/**
 * @brief Read data from USB CDC ACM receive buffer (Bulk OUT).
 * @param buf Pointer to destination buffer.
 * @param len Maximum bytes to read.
 * @return Number of bytes read.
 */
int usb_cdc_read(void *buf, int len);

/**
 * @brief Get number of bytes available in the receive ring buffer.
 * @return Number of available bytes.
 */
int usb_cdc_available(void);

/**
 * @brief Read a single character from USB CDC.
 * @return Character (0-255) or -1 if no character is available.
 */
int usb_cdc_getchar_nowait(void);

/**
 * @brief Blocking read for a single character (waits until received or timeout).
 * @param timeout_ms Timeout in milliseconds (0 = wait forever).
 * @return Character (0-255) or -1 on timeout/disconnect.
 */
int usb_cdc_getchar_timeout(uint32_t timeout_ms);

/**
 * @brief Send a single character over USB CDC.
 * @param c Character to send.
 * @return Character sent or -1 on failure.
 */
int usb_cdc_putchar(int c);

/**
 * @brief Flush transmit ring buffer (wait for all buffered data to be sent).
 */
void usb_cdc_flush(void);

/**
 * @brief Enable or disable local character echo during input.
 * @param enable 1 to echo characters, 0 for raw input.
 */
void usb_cdc_set_echo(uint8_t enable);

/**
 * @brief Enable or disable line editing mode for stdio _read / scanf.
 *        When enabled (default), typing in a terminal supports backspace and Enter.
 * @param enable 1 to enable line editing, 0 for raw stream.
 */
void usb_cdc_set_line_mode(uint8_t enable);

/**
 * @brief Interactive line reader with echo and backspace handling.
 * @param buf Buffer to store the read string.
 * @param max_len Maximum buffer length (including null terminator).
 * @return Number of characters read (excluding null terminator), or -1 on error.
 */
int usb_cdc_readline(char *buf, int max_len);

/**
 * @brief Check if host requested reset via 1200 bps touch-to-reset baud rate.
 * @return 1 if reboot requested, 0 otherwise.
 */
uint8_t usb_cdc_is_reboot_requested(void);

#ifdef __cplusplus
}
#endif

#endif /* __USB_CDC_H__ */
