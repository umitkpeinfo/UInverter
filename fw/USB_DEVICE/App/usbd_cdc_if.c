/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    usbd_cdc_if.c
  * @brief   USB CDC (Virtual COM Port) — command parser for UltraLogic R1
  *
  * @details
  *   Parses ASCII commands from the PC application (sw/main.py) and
  *   dispatches them to the UltraLogic driver functions.
  *
  *   SET commands (PC → FW):
  *     SET:FREQ:<hz>                  output frequency (1-400 Hz)
  *     SET:SWF:<hz>                   switching frequency (1-16 kHz)
  *     SET:MOD:<0-1155>               modulation index (per-mille)
  *     SET:SVPWM:0                    emergency stop (PWM + relay + state)
  *     SET:CHG:STOP|CLEAR             stop drive / full drive reset
  *     SET:DRV:START|RUN|STOP|RESET   drive state machine
  *     SET:DISP:<text>                front-panel display text
  *
  *   GET commands (FW → PC, response prefixed with $):
  *     GET:REG    → $REG,BDTR:…,CCER:…,CR1:…
  *     GET:DRV    → $DRV,S:…,F:…,V:…,…,DC:<diag_code>
  *     GET:DIAG   → $DIAG,DC:<active>,N:<count>,H0:<code>@<tick>,…
  *     GET:BTN    → $BTN,RAW:…,SCR:…,INC:…,DEC:…
  *     GET:HEAP   → $HEAP,FREE:…,MIN:…,T01:…,T02:…,T03:…,T05:…
  *
  * @company PE Info
  * @author  Umit Kayacik
  * @date    2026
  ******************************************************************************
  * Copyright (c) 2026 PE Info.  All rights reserved.
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "usbd_cdc_if.h"

/* USER CODE BEGIN INCLUDE */
#include "ul_drivers.h"
#include "ul_display.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
/* USER CODE END INCLUDE */

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* Private variables ---------------------------------------------------------*/

/* USER CODE END PV */

/** @addtogroup STM32_USB_OTG_DEVICE_LIBRARY
  * @brief Usb device library.
  * @{
  */

/** @addtogroup USBD_CDC_IF
  * @{
  */

/** @defgroup USBD_CDC_IF_Private_TypesDefinitions USBD_CDC_IF_Private_TypesDefinitions
  * @brief Private types.
  * @{
  */

/* USER CODE BEGIN PRIVATE_TYPES */

/* USER CODE END PRIVATE_TYPES */

/**
  * @}
  */

/** @defgroup USBD_CDC_IF_Private_Defines USBD_CDC_IF_Private_Defines
  * @brief Private defines.
  * @{
  */

/* USER CODE BEGIN PRIVATE_DEFINES */
#define CMD_REPLY_QUEUE_DEPTH  4U
/* USER CODE END PRIVATE_DEFINES */

/**
  * @}
  */

/** @defgroup USBD_CDC_IF_Private_Macros USBD_CDC_IF_Private_Macros
  * @brief Private macros.
  * @{
  */

/* USER CODE BEGIN PRIVATE_MACRO */

/* USER CODE END PRIVATE_MACRO */

/**
  * @}
  */

/** @defgroup USBD_CDC_IF_Private_Variables USBD_CDC_IF_Private_Variables
  * @brief Private variables.
  * @{
  */
/* Create buffer for reception and transmission           */
/* It's up to user to redefine and/or remove those define */
/** Received data over USB are stored in this buffer      */
uint8_t UserRxBufferFS[APP_RX_DATA_SIZE];

/** Data to send over USB CDC are stored in this buffer   */
uint8_t UserTxBufferFS[APP_TX_DATA_SIZE];

/* USER CODE BEGIN PRIVATE_VARIABLES */
static uint8_t LineCoding[7] = {
    0x00, 0xC2, 0x01, 0x00,  /* 115200 bps (little-endian) */
    0x00,                     /* 1 stop bit  */
    0x00,                     /* no parity   */
    0x08                      /* 8 data bits */
};
/* Query replies need stable storage until the USB IN transfer completes. */
static uint8_t  CmdReplyQueue[CMD_REPLY_QUEUE_DEPTH][APP_TX_DATA_SIZE];
static uint16_t CmdReplyLen[CMD_REPLY_QUEUE_DEPTH];
static uint8_t  CmdReplyHead = 0U;
static uint8_t  CmdReplyTail = 0U;
static uint8_t  CmdReplyCount = 0U;
/* USER CODE END PRIVATE_VARIABLES */

/**
  * @}
  */

/** @defgroup USBD_CDC_IF_Exported_Variables USBD_CDC_IF_Exported_Variables
  * @brief Public variables.
  * @{
  */

extern USBD_HandleTypeDef hUsbDeviceFS;

/* USER CODE BEGIN EXPORTED_VARIABLES */

/* USER CODE END EXPORTED_VARIABLES */

/**
  * @}
  */

/** @defgroup USBD_CDC_IF_Private_FunctionPrototypes USBD_CDC_IF_Private_FunctionPrototypes
  * @brief Private functions declaration.
  * @{
  */

static int8_t CDC_Init_FS(void);
static int8_t CDC_DeInit_FS(void);
static int8_t CDC_Control_FS(uint8_t cmd, uint8_t* pbuf, uint16_t length);
static int8_t CDC_Receive_FS(uint8_t* pbuf, uint32_t *Len);
static int8_t CDC_TransmitCplt_FS(uint8_t *pbuf, uint32_t *Len, uint8_t epnum);

/* USER CODE BEGIN PRIVATE_FUNCTIONS_DECLARATION */
static void _dispatch_cmd(const char *line);
static void _queue_cmd_reply(const uint8_t *buf, uint16_t len);
static void _kick_cmd_reply_tx(void);
/* USER CODE END PRIVATE_FUNCTIONS_DECLARATION */

/**
  * @}
  */

USBD_CDC_ItfTypeDef USBD_Interface_fops_FS =
{
  CDC_Init_FS,
  CDC_DeInit_FS,
  CDC_Control_FS,
  CDC_Receive_FS,
  CDC_TransmitCplt_FS
};

/* Private functions ---------------------------------------------------------*/
/**
  * @brief  Initializes the CDC media low layer over the FS USB IP
  * @retval USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t CDC_Init_FS(void)
{
  /* USER CODE BEGIN 3 */
  /* Set Application Buffers */
  USBD_CDC_SetTxBuffer(&hUsbDeviceFS, UserTxBufferFS, 0);
  USBD_CDC_SetRxBuffer(&hUsbDeviceFS, UserRxBufferFS);
  return (USBD_OK);
  /* USER CODE END 3 */
}

/**
  * @brief  DeInitializes the CDC media low layer
  * @retval USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t CDC_DeInit_FS(void)
{
  /* USER CODE BEGIN 4 */
  return (USBD_OK);
  /* USER CODE END 4 */
}

/**
  * @brief  Manage the CDC class requests
  * @param  cmd: Command code
  * @param  pbuf: Buffer containing command data (request parameters)
  * @param  length: Number of data to be sent (in bytes)
  * @retval Result of the operation: USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t CDC_Control_FS(uint8_t cmd, uint8_t* pbuf, uint16_t length)
{
  /* USER CODE BEGIN 5 */
  switch(cmd)
  {
    case CDC_SEND_ENCAPSULATED_COMMAND:

    break;

    case CDC_GET_ENCAPSULATED_RESPONSE:

    break;

    case CDC_SET_COMM_FEATURE:

    break;

    case CDC_GET_COMM_FEATURE:

    break;

    case CDC_CLEAR_COMM_FEATURE:

    break;

  /*******************************************************************************/
  /* Line Coding Structure                                                       */
  /*-----------------------------------------------------------------------------*/
  /* Offset | Field       | Size | Value  | Description                          */
  /* 0      | dwDTERate   |   4  | Number |Data terminal rate, in bits per second*/
  /* 4      | bCharFormat |   1  | Number | Stop bits                            */
  /*                                        0 - 1 Stop bit                       */
  /*                                        1 - 1.5 Stop bits                    */
  /*                                        2 - 2 Stop bits                      */
  /* 5      | bParityType |  1   | Number | Parity                               */
  /*                                        0 - None                             */
  /*                                        1 - Odd                              */
  /*                                        2 - Even                             */
  /*                                        3 - Mark                             */
  /*                                        4 - Space                            */
  /* 6      | bDataBits  |   1   | Number Data bits (5, 6, 7, 8 or 16).          */
  /*******************************************************************************/
    case CDC_SET_LINE_CODING:
      memcpy(LineCoding, pbuf, sizeof(LineCoding));
    break;

    case CDC_GET_LINE_CODING:
      memcpy(pbuf, LineCoding, sizeof(LineCoding));
    break;

    case CDC_SET_CONTROL_LINE_STATE:

    break;

    case CDC_SEND_BREAK:

    break;

  default:
    break;
  }

  return (USBD_OK);
  /* USER CODE END 5 */
}

/**
  * @brief  Data received over USB OUT endpoint are sent over CDC interface
  *         through this function.
  *
  *         @note
  *         This function will issue a NAK packet on any OUT packet received on
  *         USB endpoint until exiting this function. If you exit this function
  *         before transfer is complete on CDC interface (ie. using DMA controller)
  *         it will result in receiving more data while previous ones are still
  *         not sent.
  *
  * @param  Buf: Buffer of data to be received
  * @param  Len: Number of data received (in bytes)
  * @retval Result of the operation: USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t CDC_Receive_FS(uint8_t* Buf, uint32_t *Len)
{
  /* USER CODE BEGIN 6 */
  if (*Len >= APP_RX_DATA_SIZE)
      *Len = APP_RX_DATA_SIZE - 1;
  Buf[*Len] = '\0';

  char *line = (char *)Buf;
  while (*line != '\0') {
      while (*line == '\r' || *line == '\n' || *line == ' ')
          line++;
      if (*line == '\0')
          break;

      char *delim = line;
      while (*delim != '\0' && *delim != '\r' && *delim != '\n')
          delim++;

      char saved = *delim;
      *delim = '\0';
      char *trim = delim;
      while (trim > line && *(trim - 1) == ' ')
          *--trim = '\0';

      if (*line != '\0')
          _dispatch_cmd(line);

      if (saved == '\0')
          break;
      line = delim + 1;
  }

  USBD_CDC_SetRxBuffer(&hUsbDeviceFS, &Buf[0]);
  USBD_CDC_ReceivePacket(&hUsbDeviceFS);
  return (USBD_OK);
  /* USER CODE END 6 */
}

static void _dispatch_cmd(const char *line)
{
  /* ── SVPWM parameter commands ──────────────────────────────────── */

  if (strncmp(line, "SET:FREQ:", 9) == 0) {
      UL_SVPWM_SetOutFreq((uint32_t)strtoul(line + 9, NULL, 10));

  } else if (strncmp(line, "SET:SWF:", 8) == 0) {
      UL_SVPWM_SetSwFreq((uint32_t)strtoul(line + 8, NULL, 10));

  } else if (strncmp(line, "SET:MOD:", 8) == 0) {
      UL_SVPWM_SetModIndex((uint32_t)strtoul(line + 8, NULL, 10));

  } else if (strncmp(line, "SET:SVPWM:", 10) == 0) {
      if (strtoul(line + 10, NULL, 10) == 0)
          UL_Drive_Stop();

  /* ── Charge relay / drive state machine commands ────────────── */

  } else if (strncmp(line, "SET:CHG:", 8) == 0) {
      const char *arg = line + 8;
      if (strcmp(arg, "STOP") == 0 || strcmp(arg, "0") == 0)
          UL_Drive_Stop();
      else if (strcmp(arg, "CLEAR") == 0)
          UL_Drive_Reset();

  } else if (strncmp(line, "SET:DRV:", 8) == 0) {
      const char *arg = line + 8;
      if      (strcmp(arg, "START") == 0) UL_Drive_Start();
      else if (strcmp(arg, "RUN")   == 0) UL_Drive_Run();
      else if (strcmp(arg, "STOP")  == 0) UL_Drive_Stop();
      else if (strcmp(arg, "RESET") == 0) UL_Drive_Reset();

  /* ── Display commands ──────────────────────────────────────── */

  } else if (strncmp(line, "SET:DISP:", 9) == 0) {
      UL_Display_SendText(line + 9);

  /* ── Query commands (responses are queued for stable TX storage) ─── */

  } else if (strncmp(line, "GET:REG", 7) == 0) {
      int n = snprintf((char *)UserTxBufferFS, APP_TX_DATA_SIZE,
                        "$REG,BDTR:%08lX,CCER:%08lX,CR1:%08lX\r\n",
                        (unsigned long)UL_SVPWM_ReadBDTR(),
                        (unsigned long)UL_SVPWM_ReadCCER(),
                        (unsigned long)UL_SVPWM_ReadCR1());
      if (n > 0 && (size_t)n < APP_TX_DATA_SIZE)
          _queue_cmd_reply(UserTxBufferFS, (uint16_t)n);

  } else if (strncmp(line, "GET:BTN", 7) == 0) {
      const UL_DispButtons_t *b = UL_Display_GetButtons();
      int n = snprintf((char *)UserTxBufferFS, APP_TX_DATA_SIZE,
                        "$BTN,RAW:%02X,SCR:%lu,INC:%lu,DEC:%lu\r\n",
                        (unsigned)b->raw,
                        (unsigned long)b->scr_count,
                        (unsigned long)b->inc_count,
                        (unsigned long)b->dec_count);
      if (n > 0 && (size_t)n < APP_TX_DATA_SIZE)
          _queue_cmd_reply(UserTxBufferFS, (uint16_t)n);
  } else if (strncmp(line, "GET:DRV", 7) == 0) {
      static const char * const drv_names[] = {
          "IDLE", "PRCHG", "READY", "RUN", "STOP", "FAULT"
      };
      DrvState_t ds = UL_Drive_GetState();
      if ((int)ds < 0 || (int)ds > 5) ds = DRV_STATE_FAULT;
      uint16_t   ff = UL_Fault_Get();
      const UL_Meas_t *m = UL_Meas_Get();
      uint32_t   bdtr = UL_SVPWM_ReadBDTR();
      unsigned ct = !HAL_GPIO_ReadPin(BRK_CUR_CPU_GPIO_Port, BRK_CUR_CPU_Pin);
      unsigned dc = UL_Diag_GetCode();
      unsigned ry = UL_ChargeSwitch_State();
      int n = snprintf((char *)UserTxBufferFS, APP_TX_DATA_SIZE,
                        "$DRV,S:%s,F:%04X,V:%.1f,IU:%.2f,IW:%.2f,"
                        "S1:%u,S3:%u,BDTR:%08lX,MOE:%u,CT:%u,DC:%u,RY:%u\r\n",
                        drv_names[(int)ds],
                        (unsigned)ff,
                        (double)m->v_bus,
                        (double)m->i_u,
                        (double)m->i_w,
                        (unsigned)m->shunt1_raw,
                        (unsigned)m->shunt3_raw,
                        (unsigned long)bdtr,
                        (unsigned)((bdtr >> 15) & 1U),
                        ct, dc, ry);
      if (n > 0 && (size_t)n < APP_TX_DATA_SIZE)
          _queue_cmd_reply(UserTxBufferFS, (uint16_t)n);

  } else if (strncmp(line, "GET:DIAG", 8) == 0) {
      uint8_t count = 0;
      const DiagEntry_t *hist = UL_Diag_GetHistory(&count);
      char *p = (char *)UserTxBufferFS;
      int   rem = (int)APP_TX_DATA_SIZE;
      int   w = snprintf(p, (size_t)rem, "$DIAG,DC:%u,N:%u",
                          (unsigned)UL_Diag_GetCode(), (unsigned)count);
      p += w; rem -= w;
      for (uint8_t i = 0; i < count && rem > 20; i++) {
          w = snprintf(p, (size_t)rem, ",H%u:%u@%lu",
                        (unsigned)i,
                        (unsigned)hist[i].code,
                        (unsigned long)hist[i].tick_ms);
          p += w; rem -= w;
      }
      w = snprintf(p, (size_t)rem, "\r\n");
      p += w;
      int total = (int)(p - (char *)UserTxBufferFS);
      if (total > 0 && (size_t)total < APP_TX_DATA_SIZE)
          _queue_cmd_reply(UserTxBufferFS, (uint16_t)total);

  } else if (strncmp(line, "GET:HEAP", 8) == 0) {
      extern osThreadId myTask01Handle, myTask02Handle,
                        myTask03Handle, myTask05Handle;
      int n = snprintf((char *)UserTxBufferFS, APP_TX_DATA_SIZE,
                        "$HEAP,FREE:%u,MIN:%u,"
                        "T01:%u,T02:%u,T03:%u,T05:%u\r\n",
                        (unsigned)xPortGetFreeHeapSize(),
                        (unsigned)xPortGetMinimumEverFreeHeapSize(),
                        (unsigned)uxTaskGetStackHighWaterMark(
                            (TaskHandle_t)myTask01Handle),
                        (unsigned)uxTaskGetStackHighWaterMark(
                            (TaskHandle_t)myTask02Handle),
                        (unsigned)uxTaskGetStackHighWaterMark(
                            (TaskHandle_t)myTask03Handle),
                        (unsigned)uxTaskGetStackHighWaterMark(
                            (TaskHandle_t)myTask05Handle));
      if (n > 0 && (size_t)n < APP_TX_DATA_SIZE)
          _queue_cmd_reply(UserTxBufferFS, (uint16_t)n);
  }
}

/**
  * @brief  CDC_Transmit_FS
  *         Data to send over USB IN endpoint are sent over CDC interface
  *         through this function.
  *         @note
  *
  *
  * @param  Buf: Buffer of data to be sent
  * @param  Len: Number of data to be sent (in bytes)
  * @retval USBD_OK if all operations are OK else USBD_FAIL or USBD_BUSY
  */
uint8_t CDC_Transmit_FS(uint8_t* Buf, uint16_t Len)
{
  uint8_t result = USBD_OK;
  /* USER CODE BEGIN 7 */
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  USBD_CDC_HandleTypeDef *hcdc = (USBD_CDC_HandleTypeDef*)hUsbDeviceFS.pClassData;
  if (hcdc == NULL || hcdc->TxState != 0){
    __set_PRIMASK(primask);
    return USBD_BUSY;
  }
  USBD_CDC_SetTxBuffer(&hUsbDeviceFS, Buf, Len);
  result = USBD_CDC_TransmitPacket(&hUsbDeviceFS);
  __set_PRIMASK(primask);
  /* USER CODE END 7 */
  return result;
}

/**
  * @brief  CDC_TransmitCplt_FS
  *         Data transmitted callback
  *
  *         @note
  *         This function is IN transfer complete callback used to inform user that
  *         the submitted Data is successfully sent over USB.
  *
  * @param  Buf: Buffer of data to be received
  * @param  Len: Number of data received (in bytes)
  * @retval Result of the operation: USBD_OK if all operations are OK else USBD_FAIL
  */
static int8_t CDC_TransmitCplt_FS(uint8_t *Buf, uint32_t *Len, uint8_t epnum)
{
  uint8_t result = USBD_OK;
  /* USER CODE BEGIN 13 */
  UNUSED(Buf);
  UNUSED(Len);
  UNUSED(epnum);
  if (CmdReplyCount > 0U && Buf == CmdReplyQueue[CmdReplyHead]) {
      CmdReplyHead = (uint8_t)((CmdReplyHead + 1U) % CMD_REPLY_QUEUE_DEPTH);
      CmdReplyCount--;
  }
  _kick_cmd_reply_tx();
  /* USER CODE END 13 */
  return result;
}

/* USER CODE BEGIN PRIVATE_FUNCTIONS_IMPLEMENTATION */

static void _queue_cmd_reply(const uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0U || len >= APP_TX_DATA_SIZE)
        return;
    if (CmdReplyCount >= CMD_REPLY_QUEUE_DEPTH)
        return;

    uint8_t tail = CmdReplyTail;
    memcpy(CmdReplyQueue[tail], buf, len);
    CmdReplyLen[tail] = len;
    CmdReplyTail = (uint8_t)((tail + 1U) % CMD_REPLY_QUEUE_DEPTH);
    CmdReplyCount++;

    _kick_cmd_reply_tx();
}

static void _kick_cmd_reply_tx(void)
{
    if (CmdReplyCount == 0U)
        return;

    (void)CDC_Transmit_FS(CmdReplyQueue[CmdReplyHead], CmdReplyLen[CmdReplyHead]);
}

/* USER CODE END PRIVATE_FUNCTIONS_IMPLEMENTATION */

/**
  * @}
  */

/**
  * @}
  */

