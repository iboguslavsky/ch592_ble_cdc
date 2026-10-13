/*********************************************************************
 * File Name          : usb_cdc.c
 * Description        : High-performance USB CDC ACM (Virtual COM Port)
 *                      driver with stdio retargeting (printf / scanf)
 *                      specifically designed for the CH592 microcontroller.
 *********************************************************************/

#include "CH59x_common.h"
#include "CH59x_sys.h"
#include "CH59x_gpio.h"
#include "ISP592.h"
#include "usb_cdc.h"
#include <string.h>
#include <stdio.h>

/* ===================================================================
 * Buffer Sizes and Tuning
 * =================================================================== */
#define CDC_EP0_SIZE            64
#define CDC_EP1_SIZE            64
#define CDC_EP4_SIZE            8

#define CDC_TX_BUF_SIZE         512     /* Must be power of 2 */
#define CDC_RX_BUF_SIZE         256     /* Must be power of 2 */
#define CDC_LINE_BUF_SIZE       128

#define TX_TIMEOUT_US           50000   /* 50ms timeout for non-blocking flush */

/* ===================================================================
 * USB Hardware DMA Buffers (must be 4-byte aligned)
 * =================================================================== */
/* Ep0Buffer: 64B for EP0 OUT/IN + 64B for EP4 IN (RB_UEP4_TX_EN shares UEP0_DMA) */
__attribute__((aligned(4))) static uint8_t Ep0Buffer[128];

/* Ep1Buffer: offset 0..63 for Bulk OUT, offset 64..127 for Bulk IN */
__attribute__((aligned(4))) static uint8_t Ep1Buffer[128];

/* ===================================================================
 * USB Descriptors
 * =================================================================== */
/* USB Standard Device Descriptor (CDC ACM) */
static const uint8_t cdc_device_descriptor[18] = {
    0x12,                       /* bLength = 18 bytes */
    0x01,                       /* bDescriptorType = Device (1) */
    0x10, 0x01,                 /* bcdUSB = 1.10 */
    0x02,                       /* bDeviceClass = CDC (2) */
    0x00,                       /* bDeviceSubClass = 0 */
    0x00,                       /* bDeviceProtocol = 0 */
    CDC_EP0_SIZE,               /* bMaxPacketSize0 = 64 */
    0x86, 0x1A,                 /* idVendor = 0x1A86 (WCH) */
    0x40, 0x80,                 /* idProduct = 0x8040 (CH592 USB CDC ACM) */
    0x00, 0x01,                 /* bcdDevice = 1.00 */
    0x01,                       /* iManufacturer = 1 */
    0x02,                       /* iProduct = 2 */
    0x03,                       /* iSerialNumber = 3 */
    0x01                        /* bNumConfigurations = 1 */
};

/* USB Configuration Descriptor (2 Interfaces: CDC Comm + CDC Data) */
static const uint8_t cdc_config_descriptor[67] = {
    /* Configuration Descriptor (9 bytes) */
    0x09,                       /* bLength = 9 */
    0x02,                       /* bDescriptorType = Configuration (2) */
    67, 0x00,                   /* wTotalLength = 67 bytes */
    0x02,                       /* bNumInterfaces = 2 */
    0x01,                       /* bConfigurationValue = 1 */
    0x00,                       /* iConfiguration = 0 (None) */
    0x80,                       /* bmAttributes = Bus-powered */
    50,                         /* bMaxPower = 100 mA (50 * 2mA) */

    /* Interface 0: CDC Communication Interface (9 bytes) */
    0x09,                       /* bLength = 9 */
    0x04,                       /* bDescriptorType = Interface (4) */
    0x00,                       /* bInterfaceNumber = 0 */
    0x00,                       /* bAlternateSetting = 0 */
    0x01,                       /* bNumEndpoints = 1 (EP4 Interrupt IN) */
    0x02,                       /* bInterfaceClass = CDC Control (2) */
    0x02,                       /* bInterfaceSubClass = Abstract Control Model (2) */
    0x01,                       /* bInterfaceProtocol = Common AT Commands (1) */
    0x00,                       /* iInterface = 0 */

    /* Header Functional Descriptor (5 bytes) */
    0x05,                       /* bFunctionLength = 5 */
    0x24,                       /* bDescriptorType = CS_INTERFACE (0x24) */
    0x00,                       /* bDescriptorSubtype = Header (0) */
    0x10, 0x01,                 /* bcdCDC = 1.10 */

    /* Call Management Functional Descriptor (5 bytes) */
    0x05,                       /* bFunctionLength = 5 */
    0x24,                       /* bDescriptorType = CS_INTERFACE (0x24) */
    0x01,                       /* bDescriptorSubtype = Call Management (1) */
    0x00,                       /* bmCapabilities = Device does not handle call mgmt */
    0x01,                       /* bDataInterface = 1 */

    /* Abstract Control Management (ACM) Functional Descriptor (4 bytes) */
    0x04,                       /* bFunctionLength = 4 */
    0x24,                       /* bDescriptorType = CS_INTERFACE (0x24) */
    0x02,                       /* bDescriptorSubtype = ACM (2) */
    0x02,                       /* bmCapabilities = Supports Line Coding & Serial State */

    /* Union Functional Descriptor (5 bytes) */
    0x05,                       /* bFunctionLength = 5 */
    0x24,                       /* bDescriptorType = CS_INTERFACE (0x24) */
    0x06,                       /* bDescriptorSubtype = Union (6) */
    0x00,                       /* bControlInterface = 0 */
    0x01,                       /* bSubordinateInterface[0] = 1 */

    /* Endpoint 4 Descriptor: Interrupt IN Notification (7 bytes) */
    0x07,                       /* bLength = 7 */
    0x05,                       /* bDescriptorType = Endpoint (5) */
    0x84,                       /* bEndpointAddress = Endpoint 4 IN */
    0x03,                       /* bmAttributes = Interrupt (3) */
    CDC_EP4_SIZE, 0x00,         /* wMaxPacketSize = 8 bytes */
    0x01,                       /* bInterval = 1 ms */

    /* Interface 1: CDC Data Interface (9 bytes) */
    0x09,                       /* bLength = 9 */
    0x04,                       /* bDescriptorType = Interface (4) */
    0x01,                       /* bInterfaceNumber = 1 */
    0x00,                       /* bAlternateSetting = 0 */
    0x02,                       /* bNumEndpoints = 2 (EP1 OUT + EP1 IN) */
    0x0A,                       /* bInterfaceClass = CDC Data (0x0A) */
    0x00,                       /* bInterfaceSubClass = 0 */
    0x00,                       /* bInterfaceProtocol = 0 */
    0x00,                       /* iInterface = 0 */

    /* Endpoint 1 OUT Descriptor: Bulk OUT (7 bytes) */
    0x07,                       /* bLength = 7 */
    0x05,                       /* bDescriptorType = Endpoint (5) */
    0x01,                       /* bEndpointAddress = Endpoint 1 OUT */
    0x02,                       /* bmAttributes = Bulk (2) */
    CDC_EP1_SIZE, 0x00,         /* wMaxPacketSize = 64 bytes */
    0x00,                       /* bInterval = 0 */

    /* Endpoint 1 IN Descriptor: Bulk IN (7 bytes) */
    0x07,                       /* bLength = 7 */
    0x05,                       /* bDescriptorType = Endpoint (5) */
    0x81,                       /* bEndpointAddress = Endpoint 1 IN */
    0x02,                       /* bmAttributes = Bulk (2) */
    CDC_EP1_SIZE, 0x00,         /* wMaxPacketSize = 64 bytes */
    0x00                        /* bInterval = 0 */
};

/* String 0: Supported Language ID (0x0409 = English US) */
static const uint8_t cdc_str_lang_id[4] = { 0x04, 0x03, 0x09, 0x04 };

/* String 1: Manufacturer String ("WCH") */
static const uint8_t cdc_str_manufacturer[8] = {
    0x08, 0x03,
    'W', 0, 'C', 0, 'H', 0
};

/* String 2: Product String ("CH592 USB CDC") */
static const uint8_t cdc_str_product[28] = {
    0x1C, 0x03,
    'C', 0, 'H', 0, '5', 0, '9', 0, '2', 0, ' ', 0,
    'U', 0, 'S', 0, 'B', 0, ' ', 0, 'C', 0, 'D', 0, 'C', 0
};

/* String 3: Serial Number (dynamically populated from MAC) */
static uint8_t cdc_str_serial[38] = {
    0x26, 0x03,
    'C', 0, 'H', 0, '5', 0, '9', 0, '2', 0, '-', 0,
    '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0,
    '0', 0, '0', 0, '0', 0, '0', 0, '0', 0, '0', 0
};

/* ===================================================================
 * CDC Line Coding Structure
 * =================================================================== */
typedef struct __attribute__((packed)) {
    uint32_t baud_rate;
    uint8_t  stop_bits;     /* 0: 1 stop bit, 1: 1.5 stop bits, 2: 2 stop bits */
    uint8_t  parity;        /* 0: None, 1: Odd, 2: Even, 3: Mark, 4: Space */
    uint8_t  data_bits;     /* 5, 6, 7, 8 */
} cdc_line_coding_t;

static cdc_line_coding_t g_line_coding = {
    .baud_rate = 115200,
    .stop_bits = 0,
    .parity    = 0,
    .data_bits = 8
};

/* ===================================================================
 * Circular Ring Buffer Implementation
 * =================================================================== */
typedef struct {
    uint8_t  buffer[CDC_TX_BUF_SIZE];
    volatile uint16_t head;
    volatile uint16_t tail;
} tx_ring_t;

typedef struct {
    uint8_t  buffer[CDC_RX_BUF_SIZE];
    volatile uint16_t head;
    volatile uint16_t tail;
} rx_ring_t;

static tx_ring_t s_tx_ring;
static rx_ring_t s_rx_ring;

static inline uint16_t tx_ring_count(void) {
    return (uint16_t)((s_tx_ring.head - s_tx_ring.tail) & (CDC_TX_BUF_SIZE - 1));
}

static inline uint16_t tx_ring_free(void) {
    return (uint16_t)((CDC_TX_BUF_SIZE - 1) - tx_ring_count());
}

static inline uint16_t rx_ring_count(void) {
    return (uint16_t)((s_rx_ring.head - s_rx_ring.tail) & (CDC_RX_BUF_SIZE - 1));
}

static inline uint16_t rx_ring_free(void) {
    return (uint16_t)((CDC_RX_BUF_SIZE - 1) - rx_ring_count());
}

/* ===================================================================
 * State Variables
 * =================================================================== */
static volatile uint8_t  s_usb_configured = 0;
static volatile uint8_t  s_usb_dtr        = 0;
static volatile uint8_t  s_usb_rts        = 0;
static volatile uint8_t  s_tx_busy        = 0;
static volatile uint8_t  s_rx_paused      = 0;

static volatile uint8_t  s_dev_addr       = 0;
static volatile uint8_t  s_setup_req      = 0xFF;
static volatile uint16_t s_setup_len      = 0;
static volatile uint8_t  s_reboot_pending = 0;

static const uint8_t    *s_desc_ptr       = NULL;
static volatile uint16_t s_desc_rem_len   = 0;

/* Terminal / stdio options (0 for raw transparent bridge mode) */
static uint8_t s_echo_enabled = 0;
static uint8_t s_line_mode    = 0;

/* Internal buffer for line-mode stdin */
static char s_line_buf[CDC_LINE_BUF_SIZE];
static uint16_t s_line_head = 0;
static uint16_t s_line_tail = 0;

/* ===================================================================
 * Helper: Init Serial String from Hardware MAC
 * =================================================================== */
static void init_serial_string(void)
{
    uint8_t mac[6] = {0};
    GetMACAddress(mac);

    static const char hex_chars[] = "0123456789ABCDEF";
    int out_idx = 14; /* Offset after "CH592-" (7 wide chars = 14 bytes) */

    for (int i = 5; i >= 0; i--) {
        uint8_t b = mac[i];
        cdc_str_serial[out_idx++] = hex_chars[(b >> 4) & 0x0F];
        cdc_str_serial[out_idx++] = 0;
        cdc_str_serial[out_idx++] = hex_chars[b & 0x0F];
        cdc_str_serial[out_idx++] = 0;
    }
}

/* ===================================================================
 * USB CDC Hardware Initialization
 * =================================================================== */
void usb_cdc_init(void)
{
    /* Initialize ring buffers */
    s_tx_ring.head = 0;
    s_tx_ring.tail = 0;
    s_rx_ring.head = 0;
    s_rx_ring.tail = 0;

    s_usb_configured = 0;
    s_usb_dtr = 0;
    s_usb_rts = 0;
    s_tx_busy = 0;
    s_rx_paused = 0;
    s_line_head = 0;
    s_line_tail = 0;

    init_serial_string();

    /* Force USB bus disconnect by disabling pull-up, allowing host to detect reset */
    R8_UDEV_CTRL = 0;
    R8_USB_CTRL = 0;
    R16_PIN_ANALOG_IE &= ~(RB_PIN_USB_IE | RB_PIN_USB_DP_PU);
    mDelaymS(30);

    /* Configure USB endpoint mode:
     * EP1: TX_EN (IN) + RX_EN (OUT), BUF_MOD=0 (128B total: 64B OUT + 64B IN)
     * EP4: TX_EN (IN notification), shares UEP0_DMA offset 64..127 */
    R8_UEP4_1_MOD = RB_UEP4_TX_EN | RB_UEP1_TX_EN | RB_UEP1_RX_EN;
    R8_UEP2_3_MOD = 0x00;

    /* Point DMA registers to hardware packet buffers */
    R16_UEP0_DMA = (uint16_t)(uint32_t)&Ep0Buffer[0];
    R16_UEP1_DMA = (uint16_t)(uint32_t)&Ep1Buffer[0];

    /* Initial endpoint states:
     * EP0: ACK OUT, NAK IN
     * EP1: ACK OUT (ready to receive), NAK IN (no data yet), AUTO_TOG enabled
     * EP4: NAK IN */
    R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
    R8_UEP1_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
    R8_UEP4_CTRL = UEP_T_RES_NAK;

    /* Set default USB device address = 0 */
    R8_USB_DEV_AD = 0x00;

    /* Enable USB device pull-up, auto-busy during IRQ, and DMA */
    R8_USB_CTRL = RB_UC_DEV_PU_EN | RB_UC_INT_BUSY | RB_UC_DMA_EN;

    /* Enable physical port with pulldown disabled */
    R8_UDEV_CTRL = RB_UD_PD_DIS | RB_UD_PORT_EN;

    /* Enable USB analog transceiver and internal D+ pull-up */
    R16_PIN_ANALOG_IE |= RB_PIN_USB_IE | RB_PIN_USB_DP_PU;

    /* Clear any pending interrupt flags */
    R8_USB_INT_FG = 0xFF;

    /* Enable interrupts for Bus Reset, Suspend, and Transfer Completion */
    R8_USB_INT_EN = RB_UIE_BUS_RST | RB_UIE_SUSPEND | RB_UIE_TRANSFER;

    /* Enable USB IRQ in PFIC */
    PFIC_EnableIRQ(USB_IRQn);
}

/* ===================================================================
 * Status Getters
 * =================================================================== */
uint8_t usb_cdc_is_configured(void)
{
    return s_usb_configured;
}

uint8_t usb_cdc_is_connected(void)
{
    return (s_usb_configured && s_usb_dtr);
}

uint32_t usb_cdc_get_baud_rate(void)
{
    return g_line_coding.baud_rate;
}

/* ===================================================================
 * USB Interrupt Handler (runs from SRAM via .highcode for maximum speed)
 * =================================================================== */
__INTERRUPT
__HIGH_CODE
void USB_IRQHandler(void)
{
    uint8_t int_fg = R8_USB_INT_FG;

    if (int_fg & RB_UIF_TRANSFER)
    {
        uint8_t int_st = R8_USB_INT_ST;

        /* Non-SETUP transfers: IN or OUT token */
        if ((int_st & MASK_UIS_TOKEN) != MASK_UIS_TOKEN)
        {
            switch (int_st & (MASK_UIS_TOKEN | MASK_UIS_ENDP))
            {
                /* ---------------------------------------------------
                 * Endpoint 0 IN (Host read data on Control Endpoint)
                 * --------------------------------------------------- */
                case UIS_TOKEN_IN | 0:
                {
                    if (s_setup_req == 0x05) /* USB_SET_ADDRESS */
                    {
                        R8_USB_DEV_AD = (R8_USB_DEV_AD & RB_UDA_GP_BIT) | s_dev_addr;
                        R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                        s_setup_req = 0xFF;
                    }
                    else if (s_desc_rem_len > 0) /* Multi-packet descriptor upload */
                    {
                        uint8_t chunk = (s_desc_rem_len > CDC_EP0_SIZE) ? CDC_EP0_SIZE : s_desc_rem_len;
                        memcpy(Ep0Buffer, s_desc_ptr, chunk);
                        s_desc_ptr += chunk;
                        s_desc_rem_len -= chunk;

                        R8_UEP0_T_LEN = chunk;
                        R8_UEP0_CTRL ^= RB_UEP_T_TOG;
                        R8_UEP0_CTRL = (R8_UEP0_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
                    }
                    else
                    {
                        R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                        s_setup_req = 0xFF;
                    }
                    break;
                }

                /* ---------------------------------------------------
                 * Endpoint 0 OUT (Host sent data on Control Endpoint)
                 * --------------------------------------------------- */
                case UIS_TOKEN_OUT | 0:
                {
                    if (s_setup_req == 0x20) /* SET_LINE_CODING */
                    {
                        if (R8_USB_RX_LEN >= sizeof(cdc_line_coding_t))
                        {
                            memcpy(&g_line_coding, Ep0Buffer, sizeof(cdc_line_coding_t));
                            if (g_line_coding.baud_rate == 1200)
                            {
                                s_reboot_pending = 1;
                            }
                        }
                        /* Send 0-length status packet on IN */
                        R8_UEP0_T_LEN = 0;
                        R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
                        s_setup_req = 0xFF;
                    }
                    else
                    {
                        R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
                    }
                    break;
                }

                /* ---------------------------------------------------
                 * Endpoint 1 OUT (Host sent serial data to device)
                 * --------------------------------------------------- */
                case UIS_TOKEN_OUT | 1:
                {
                    if (int_st & RB_UIS_TOG_OK)
                    {
                        uint8_t rx_len = R8_USB_RX_LEN;
                        uint16_t free_sp = rx_ring_free();

                        if (rx_len <= free_sp)
                        {
                            for (uint8_t i = 0; i < rx_len; i++)
                            {
                                s_rx_ring.buffer[s_rx_ring.head] = Ep1Buffer[i];
                                s_rx_ring.head = (s_rx_ring.head + 1) & (CDC_RX_BUF_SIZE - 1);
                            }
                        }

                        /* Flow control: if remaining space is less than full packet, NAK next OUT */
                        if (rx_ring_free() < CDC_EP1_SIZE)
                        {
                            s_rx_paused = 1;
                            R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_NAK;
                        }
                        else
                        {
                            R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_ACK;
                        }
                    }
                    break;
                }

                /* ---------------------------------------------------
                 * Endpoint 1 IN (Host read previously sent serial data)
                 * --------------------------------------------------- */
                case UIS_TOKEN_IN | 1:
                {
                    uint16_t pending = tx_ring_count();
                    if (pending > 0)
                    {
                        uint8_t chunk = (pending > CDC_EP1_SIZE) ? CDC_EP1_SIZE : (uint8_t)pending;
                        for (uint8_t i = 0; i < chunk; i++)
                        {
                            Ep1Buffer[64 + i] = s_tx_ring.buffer[s_tx_ring.tail];
                            s_tx_ring.tail = (s_tx_ring.tail + 1) & (CDC_TX_BUF_SIZE - 1);
                        }
                        R8_UEP1_T_LEN = chunk;
                        R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;
                    }
                    else
                    {
                        s_tx_busy = 0;
                        R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
                    }
                    break;
                }

                /* ---------------------------------------------------
                 * Endpoint 4 IN (Interrupt Notification for CDC ACM)
                 * --------------------------------------------------- */
                case UIS_TOKEN_IN | 4:
                {
                    R8_UEP4_CTRL ^= RB_UEP_T_TOG;
                    R8_UEP4_CTRL = (R8_UEP4_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_NAK;
                    break;
                }

                default:
                    break;
            }
            R8_USB_INT_FG = RB_UIF_TRANSFER;
        }

        /* -----------------------------------------------------------
         * SETUP Packet on Endpoint 0
         * ----------------------------------------------------------- */
        if (int_st & RB_UIS_SETUP_ACT)
        {
            uint8_t req_type = Ep0Buffer[0];
            uint8_t req      = Ep0Buffer[1];
            uint16_t val     = (uint16_t)Ep0Buffer[2] | ((uint16_t)Ep0Buffer[3] << 8);
            uint16_t idx     = (uint16_t)Ep0Buffer[4] | ((uint16_t)Ep0Buffer[5] << 8);
            uint16_t len     = (uint16_t)Ep0Buffer[6] | ((uint16_t)Ep0Buffer[7] << 8);

            s_setup_req = req;
            s_setup_len = len;

            uint8_t err = 0;
            uint8_t tx_len = 0;
            const uint8_t *tx_data = NULL;

            /* Check request type: Standard vs Class */
            if ((req_type & 0x60) == 0x00) /* Standard Request */
            {
                switch (req)
                {
                    case 0x06: /* GET_DESCRIPTOR */
                    {
                        uint8_t desc_type = (uint8_t)(val >> 8);
                        uint8_t desc_idx  = (uint8_t)(val & 0xFF);

                        switch (desc_type)
                        {
                            case 0x01: /* Device Descriptor */
                                tx_data = cdc_device_descriptor;
                                tx_len = sizeof(cdc_device_descriptor);
                                break;

                            case 0x02: /* Configuration Descriptor */
                                tx_data = cdc_config_descriptor;
                                tx_len = sizeof(cdc_config_descriptor);
                                break;

                            case 0x03: /* String Descriptor */
                                if (desc_idx == 0) {
                                    tx_data = cdc_str_lang_id;
                                    tx_len = sizeof(cdc_str_lang_id);
                                } else if (desc_idx == 1) {
                                    tx_data = cdc_str_manufacturer;
                                    tx_len = sizeof(cdc_str_manufacturer);
                                } else if (desc_idx == 2) {
                                    tx_data = cdc_str_product;
                                    tx_len = sizeof(cdc_str_product);
                                } else if (desc_idx == 3) {
                                    tx_data = cdc_str_serial;
                                    tx_len = sizeof(cdc_str_serial);
                                } else {
                                    err = 1;
                                }
                                break;

                            default:
                                err = 1;
                                break;
                        }

                        if (!err && tx_data)
                        {
                            if (len < tx_len) tx_len = (uint8_t)len;
                            uint8_t chunk = (tx_len > CDC_EP0_SIZE) ? CDC_EP0_SIZE : tx_len;
                            memcpy(Ep0Buffer, tx_data, chunk);
                            s_desc_ptr = tx_data + chunk;
                            s_desc_rem_len = tx_len - chunk;
                            tx_len = chunk;
                        }
                        break;
                    }

                    case 0x05: /* SET_ADDRESS */
                    {
                        s_dev_addr = (uint8_t)(val & 0xFF);
                        tx_len = 0;
                        break;
                    }

                    case 0x09: /* SET_CONFIGURATION */
                    {
                        s_usb_configured = (uint8_t)(val & 0xFF) ? 1 : 0;
                        if (s_usb_configured)
                        {
                            R8_UEP1_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
                            R8_UEP4_CTRL = UEP_T_RES_NAK;
                        }
                        tx_len = 0;
                        break;
                    }

                    case 0x08: /* GET_CONFIGURATION */
                    {
                        Ep0Buffer[0] = s_usb_configured;
                        tx_len = 1;
                        break;
                    }

                    case 0x00: /* GET_STATUS */
                    {
                        Ep0Buffer[0] = 0x00;
                        Ep0Buffer[1] = 0x00;
                        tx_len = 2;
                        break;
                    }

                    case 0x01: /* CLEAR_FEATURE */
                    {
                        if ((req_type & 0x1F) == 0x02) /* Endpoint */
                        {
                            if (idx == 0x81) R8_UEP1_CTRL = (R8_UEP1_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK;
                            else if (idx == 0x01) R8_UEP1_CTRL = (R8_UEP1_CTRL & ~(RB_UEP_R_TOG | MASK_UEP_R_RES)) | UEP_R_RES_ACK;
                            else if (idx == 0x84) R8_UEP4_CTRL = (R8_UEP4_CTRL & ~(RB_UEP_T_TOG | MASK_UEP_T_RES)) | UEP_T_RES_NAK;
                        }
                        tx_len = 0;
                        break;
                    }

                    case 0x0A: /* GET_INTERFACE */
                    {
                        Ep0Buffer[0] = 0x00;
                        tx_len = 1;
                        break;
                    }

                    case 0x0B: /* SET_INTERFACE */
                    {
                        tx_len = 0;
                        break;
                    }

                    default:
                        err = 1;
                        break;
                }
            }
            else if ((req_type & 0x60) == 0x20) /* CDC ACM Class Request */
            {
                switch (req)
                {
                    case 0x20: /* SET_LINE_CODING */
                        /* 7 bytes will arrive in EP0 OUT data stage */
                        tx_len = 0;
                        break;

                    case 0x21: /* GET_LINE_CODING */
                        memcpy(Ep0Buffer, &g_line_coding, sizeof(cdc_line_coding_t));
                        tx_len = sizeof(cdc_line_coding_t);
                        if (tx_len > len) tx_len = (uint8_t)len;
                        break;

                    case 0x22: /* SET_CONTROL_LINE_STATE */
                        s_usb_dtr = (val & 0x01) ? 1 : 0;
                        s_usb_rts = (val & 0x02) ? 1 : 0;
                        tx_len = 0;
                        break;

                    case 0x23: /* SEND_BREAK */
                        tx_len = 0;
                        break;

                    default:
                        err = 1;
                        break;
                }
            }
            else
            {
                err = 1;
            }

            if (err)
            {
                R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
            }
            else
            {
                R8_UEP0_T_LEN = tx_len;
                /* Arm EP0 for DATA1 status or data stage */
                R8_UEP0_CTRL = RB_UEP_R_TOG | RB_UEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
            }

            R8_USB_INT_FG = RB_UIF_TRANSFER;
        }
    }
    else if (int_fg & RB_UIF_BUS_RST)
    {
        /* USB Bus Reset from host */
        R8_USB_DEV_AD = 0x00;
        s_usb_configured = 0;
        s_usb_dtr = 0;
        s_usb_rts = 0;
        s_tx_busy = 0;
        s_rx_paused = 0;

        R8_UEP0_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK;
        R8_UEP1_CTRL = UEP_R_RES_ACK | UEP_T_RES_NAK | RB_UEP_AUTO_TOG;
        R8_UEP4_CTRL = UEP_T_RES_NAK;

        R8_USB_INT_FG = RB_UIF_BUS_RST;
    }
    else if (int_fg & RB_UIF_SUSPEND)
    {
        R8_USB_INT_FG = RB_UIF_SUSPEND;
    }
    else
    {
        R8_USB_INT_FG = int_fg;
    }
}

/* ===================================================================
 * Data Transmission (Non-blocking / Buffered)
 * =================================================================== */
int usb_cdc_write(const void *buf, int len)
{
    if (len <= 0) return 0;
    const uint8_t *data = (const uint8_t *)buf;
    int written = 0;

    /* If USB not yet enumerated, discard to prevent blocking application boot */
    if (!s_usb_configured) {
        return len;
    }

    while (written < len)
    {
        PFIC_DisableIRQ(USB_IRQn);

        if (!s_tx_busy)
        {
            /* Hardware endpoint is idle: load packet directly into DMA buffer */
            int chunk = len - written;
            if (chunk > CDC_EP1_SIZE) chunk = CDC_EP1_SIZE;

            memcpy(&Ep1Buffer[64], data + written, chunk);
            written += chunk;
            s_tx_busy = 1;

            R8_UEP1_T_LEN = chunk;
            R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_T_RES) | UEP_T_RES_ACK;

            PFIC_EnableIRQ(USB_IRQn);
        }
        else
        {
            /* Hardware endpoint is busy: enqueue into TX ring buffer */
            uint16_t free_sp = tx_ring_free();
            if (free_sp > 0)
            {
                int chunk = len - written;
                if (chunk > free_sp) chunk = free_sp;

                for (int i = 0; i < chunk; i++) {
                    s_tx_ring.buffer[s_tx_ring.head] = data[written + i];
                    s_tx_ring.head = (s_tx_ring.head + 1) & (CDC_TX_BUF_SIZE - 1);
                }
                written += chunk;
                PFIC_EnableIRQ(USB_IRQn);
            }
            else
            {
                /* Ring buffer full: wait for interrupt to transmit */
                PFIC_EnableIRQ(USB_IRQn);

                uint32_t wait_us = 0;
                while (s_tx_busy && tx_ring_free() == 0)
                {
                    mDelayuS(10);
                    wait_us += 10;
                    if (wait_us >= TX_TIMEOUT_US || !s_usb_configured)
                    {
                        /* Host stopped polling; discard remainder to avoid hanging */
                        return len;
                    }
                }
            }
        }
    }

    return written;
}

int usb_cdc_putchar(int c)
{
    uint8_t ch = (uint8_t)c;
    return (usb_cdc_write(&ch, 1) == 1) ? c : -1;
}

void usb_cdc_flush(void)
{
    uint32_t wait_us = 0;
    while ((s_tx_busy || tx_ring_count() > 0) && s_usb_configured)
    {
        mDelayuS(10);
        wait_us += 10;
        if (wait_us >= TX_TIMEOUT_US) break;
    }
}

/* ===================================================================
 * Data Reception
 * =================================================================== */
int usb_cdc_available(void)
{
    return rx_ring_count();
}

int usb_cdc_getchar_nowait(void)
{
    if (rx_ring_count() == 0) return -1;

    PFIC_DisableIRQ(USB_IRQn);
    uint8_t ch = s_rx_ring.buffer[s_rx_ring.tail];
    s_rx_ring.tail = (s_rx_ring.tail + 1) & (CDC_RX_BUF_SIZE - 1);

    /* Unpause endpoint if buffer space is restored */
    if (s_rx_paused && rx_ring_free() >= CDC_EP1_SIZE)
    {
        s_rx_paused = 0;
        R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_ACK;
    }
    PFIC_EnableIRQ(USB_IRQn);

    return ch;
}

int usb_cdc_getchar_timeout(uint32_t timeout_ms)
{
    uint32_t elapsed = 0;
    while (usb_cdc_available() == 0)
    {
        if (!s_usb_configured) return -1;
        if (timeout_ms > 0 && elapsed >= timeout_ms) return -1;

        mDelaymS(1);
        elapsed++;
    }
    return usb_cdc_getchar_nowait();
}

int usb_cdc_read(void *buf, int len)
{
    if (len <= 0) return 0;
    uint8_t *dest = (uint8_t *)buf;
    int count = 0;

    /* Wait until at least 1 byte is available */
    while (usb_cdc_available() == 0)
    {
        if (!s_usb_configured) return -1;
        mDelayuS(50);
    }

    PFIC_DisableIRQ(USB_IRQn);
    while (count < len && rx_ring_count() > 0)
    {
        dest[count++] = s_rx_ring.buffer[s_rx_ring.tail];
        s_rx_ring.tail = (s_rx_ring.tail + 1) & (CDC_RX_BUF_SIZE - 1);
    }

    if (s_rx_paused && rx_ring_free() >= CDC_EP1_SIZE)
    {
        s_rx_paused = 0;
        R8_UEP1_CTRL = (R8_UEP1_CTRL & ~MASK_UEP_R_RES) | UEP_R_RES_ACK;
    }
    PFIC_EnableIRQ(USB_IRQn);

    return count;
}

/* ===================================================================
 * Interactive Line Reader (with Echo & Backspace handling)
 * =================================================================== */
void usb_cdc_set_echo(uint8_t enable)
{
    s_echo_enabled = enable;
}

void usb_cdc_set_line_mode(uint8_t enable)
{
    s_line_mode = enable;
}

int usb_cdc_readline(char *buf, int max_len)
{
    if (max_len <= 1 || buf == NULL) return -1;
    int idx = 0;

    while (1)
    {
        int ch = usb_cdc_getchar_timeout(0);
        if (ch < 0) return -1;

        /* Carriage Return or Line Feed: End of Line */
        if (ch == '\r' || ch == '\n')
        {
            if (s_echo_enabled) {
                usb_cdc_write("\r\n", 2);
            }
            buf[idx] = '\0';
            return idx;
        }
        /* Backspace (0x08) or DEL (0x7F) */
        else if (ch == 0x08 || ch == 0x7F)
        {
            if (idx > 0)
            {
                idx--;
                if (s_echo_enabled) {
                    usb_cdc_write("\b \b", 3);
                }
            }
        }
        /* Printable characters */
        else if (ch >= 0x20 && ch <= 0x7E)
        {
            if (idx < max_len - 1)
            {
                buf[idx++] = (char)ch;
                if (s_echo_enabled) {
                    uint8_t c = (uint8_t)ch;
                    usb_cdc_write(&c, 1);
                }
            }
        }
    }
}

/* ===================================================================
 * Newlib Stdio Retargeting (_write, _read, _isatty, _fstat)
 * =================================================================== */

/**
 * @brief Retarget standard output (printf / puts / putchar) to USB CDC
 */
int _write(int fd, char *buf, int size)
{
    (void)fd;
    return usb_cdc_write(buf, size);
}

/**
 * @brief Retarget standard input (scanf / getchar / fgets) to USB CDC
 */
int _read(int fd, char *buf, int size)
{
    (void)fd;
    if (size <= 0) return 0;

    /* If raw streaming mode is desired */
    if (!s_line_mode)
    {
        return usb_cdc_read(buf, size);
    }

    /* In line-editing mode: feed from internal line buffer */
    if (s_line_head == s_line_tail)
    {
        /* Buffer is empty, read next line interactively */
        int len = usb_cdc_readline(s_line_buf, CDC_LINE_BUF_SIZE - 2);
        if (len < 0) return -1;

        /* Append standard newline for stdio / scanf parsing */
        s_line_buf[len++] = '\n';
        s_line_buf[len] = '\0';

        s_line_head = 0;
        s_line_tail = (uint16_t)len;
    }

    int copied = 0;
    while (copied < size && s_line_head < s_line_tail)
    {
        buf[copied++] = s_line_buf[s_line_head++];
    }

    return copied;
}

int _isatty(int fd)
{
    if (fd >= 0 && fd <= 2) return 1;
    return 0;
}

int _fstat(int fd, void *st)
{
    (void)fd;
    (void)st;
    return 0;
}

int _close(int fd)
{
    (void)fd;
    return 0;
}

int _lseek(int fd, int ptr, int dir)
{
    (void)fd;
    (void)ptr;
    (void)dir;
    return 0;
}

uint8_t usb_cdc_is_reboot_requested(void)
{
    return s_reboot_pending;
}
