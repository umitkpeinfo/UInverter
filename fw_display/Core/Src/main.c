/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    main.c
  * @brief   Ultra Display R1 — 14-segment multiplexed display controller
  *
  * @details
  *   Bare-metal firmware for the STM32F100R8 display MCU.
  *   Communicates with the main inverter MCU via USART1 using the
  *   M72-compatible ASCII/hex packet protocol.
  *
  *   RX: receives segment packets (type 01) with display text
  *   TX: sends button packets (type 02) with 3-button bitmask
  *
  *   TIM2 ISR multiplexes 6 digits at 1 kHz (167 Hz visible refresh).
  *   Main loop applies received text and polls buttons every 50 ms.
  *
  * @company PE Info
  * @author  Umit Kayacik
  * @date    2026
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "disp_config.h"
#include "disp_font.h"
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef htim2;

UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */

/* 14-segment bitmaps for each digit position */
static volatile uint16_t disp_segments[DIG_COUNT];
static volatile uint8_t  disp_active_digit;
static volatile uint32_t mux_tick;

/* Incoming display text from UART (double-buffered) */
static volatile char rx_display_text[DIG_COUNT + 1];
static volatile uint8_t rx_text_updated;

/* UART RX packet parser state */
static uint8_t  rx_pkt_buf[PROTO_MAX_PACKET];
static uint8_t  rx_pkt_pos;
static uint8_t  rx_pkt_expected_len;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_TIM2_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ── Segment / Digit GPIO tables for fast iteration ──────────────── */

typedef struct { GPIO_TypeDef *port; uint16_t pin; } GpioPin_t;

static const GpioPin_t seg_pins[SEG_COUNT] = {
    { SEG_A_PORT,  SEG_A_PIN  },  { SEG_B_PORT,  SEG_B_PIN  },
    { SEG_C_PORT,  SEG_C_PIN  },  { SEG_D_PORT,  SEG_D_PIN  },
    { SEG_E_PORT,  SEG_E_PIN  },  { SEG_F_PORT,  SEG_F_PIN  },
    { SEG_G1_PORT, SEG_G1_PIN },  { SEG_G2_PORT, SEG_G2_PIN },
    { SEG_H_PORT,  SEG_H_PIN  },  { SEG_J_PORT,  SEG_J_PIN  },
    { SEG_K_PORT,  SEG_K_PIN  },  { SEG_L_PORT,  SEG_L_PIN  },
    { SEG_M_PORT,  SEG_M_PIN  },  { SEG_N_PORT,  SEG_N_PIN  },
    { SEG_DP_PORT, SEG_DP_PIN },
};

static const GpioPin_t dig_pins[DIG_COUNT] = {
    { DIG0_PORT, DIG0_PIN }, { DIG1_PORT, DIG1_PIN },
    { DIG2_PORT, DIG2_PIN }, { DIG3_PORT, DIG3_PIN },
    { DIG4_PORT, DIG4_PIN }, { DIG5_PORT, DIG5_PIN },
};

/* ── Hex helpers (shared with inverter-side protocol) ────────────── */

static const char hex_chars[] = "0123456789ABCDEF";

static void hex2_encode(uint8_t *dst, uint8_t val)
{
    dst[0] = (uint8_t)hex_chars[(val >> 4) & 0x0F];
    dst[1] = (uint8_t)hex_chars[val & 0x0F];
}

static uint8_t hex_val(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0xFF;
}

static uint8_t hex2_decode(const uint8_t *p)
{
    uint8_t hi = hex_val((char)p[0]);
    uint8_t lo = hex_val((char)p[1]);
    if (hi > 0x0F || lo > 0x0F) return 0xFF;
    return (uint8_t)((hi << 4) | lo);
}

static uint8_t is_hex_char(uint8_t c)
{
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'F') ||
           (c >= 'a' && c <= 'f');
}

static uint8_t calc_checksum(const uint8_t *pkt, uint8_t len)
{
    uint16_t sum = 0;
    for (uint8_t i = 0; i < len; i++) {
        if (i == 5 || i == 6) continue;
        sum += pkt[i];
    }
    return (uint8_t)(sum & 0xFF);
}

/* ── Display multiplexing ────────────────────────────────────────── */

static void set_segments(uint16_t seg_bitmap)
{
    for (uint8_t i = 0; i < SEG_COUNT; i++)
        HAL_GPIO_WritePin(seg_pins[i].port, seg_pins[i].pin,
                          (seg_bitmap & (1U << i)) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void deselect_all_digits(void)
{
    for (uint8_t i = 0; i < DIG_COUNT; i++)
        HAL_GPIO_WritePin(dig_pins[i].port, dig_pins[i].pin, GPIO_PIN_RESET);
}

static void update_display_from_text(const char *text)
{
    for (uint8_t i = 0; i < DIG_COUNT; i++) {
        if (text[i] == '\0') {
            for (uint8_t j = i; j < DIG_COUNT; j++)
                disp_segments[j] = 0;
            break;
        }
        disp_segments[i] = font14_lookup(text[i]);
    }
}

/* ── Button reading (active LOW, HW pull-ups on PCB) ─────────────── */

static uint8_t read_buttons(void)
{
    uint8_t config = 0;
    if (HAL_GPIO_ReadPin(BTN_SCR_PORT, BTN_SCR_PIN) == GPIO_PIN_RESET)
        config |= BTN_MASK_SCR;
    if (HAL_GPIO_ReadPin(BTN_INC_PORT, BTN_INC_PIN) == GPIO_PIN_RESET)
        config |= BTN_MASK_INC;
    if (HAL_GPIO_ReadPin(BTN_DEC_PORT, BTN_DEC_PIN) == GPIO_PIN_RESET)
        config |= BTN_MASK_DEC;
    return config;
}

/* ── UART TX: button packet ──────────────────────────────────────── */

static void send_button_packet(uint8_t config)
{
    uint8_t pkt[9];
    pkt[0] = (uint8_t)PROTO_LEAD_CHAR;
    hex2_encode(&pkt[1], 9);
    hex2_encode(&pkt[3], PROTO_TYPE_BUTTON);
    pkt[5] = '0'; pkt[6] = '0';
    hex2_encode(&pkt[7], config);
    uint8_t cksum = calc_checksum(pkt, 9);
    hex2_encode(&pkt[5], cksum);
    HAL_UART_Transmit(&huart1, pkt, 9, 5);
}

/* ── UART RX: packet parser ─────────────────────────────────────── */

static void dispatch_rx_packet(const uint8_t *pkt, uint8_t len)
{
    uint8_t expected = calc_checksum(pkt, len);
    uint8_t received = hex2_decode(&pkt[5]);
    if (received != expected) return;

    uint8_t msg_type = hex2_decode(&pkt[3]);
    if (msg_type != PROTO_TYPE_SEGMENT) return;

    uint8_t text_start = PROTO_HEADER_LEN + 2;
    uint8_t text_len = (len > text_start) ? (len - text_start) : 0;
    if (text_len > DIG_COUNT) text_len = DIG_COUNT;

    char buf[DIG_COUNT + 1];
    memcpy(buf, &pkt[text_start], text_len);
    buf[text_len] = '\0';

    __disable_irq();
    memcpy((void *)rx_display_text, buf, text_len + 1);
    rx_text_updated = 1;
    __enable_irq();
}

static void parse_rx_byte(uint8_t c)
{
    if (c == PROTO_LEAD_CHAR) {
        rx_pkt_buf[0] = c;
        rx_pkt_pos = 1;
        rx_pkt_expected_len = 0;
        return;
    }
    if (rx_pkt_pos == 0) return;

    if (rx_pkt_pos < PROTO_HEADER_LEN) {
        if (!is_hex_char(c)) { rx_pkt_pos = 0; return; }
        rx_pkt_buf[rx_pkt_pos++] = c;
        if (rx_pkt_pos == 3) {
            rx_pkt_expected_len = hex2_decode(&rx_pkt_buf[1]);
            if (rx_pkt_expected_len < PROTO_HEADER_LEN ||
                rx_pkt_expected_len > PROTO_MAX_PACKET)
            { rx_pkt_pos = 0; return; }
        }
        return;
    }
    if (c == 0) { rx_pkt_pos = 0; return; }
    rx_pkt_buf[rx_pkt_pos++] = c;
    if (rx_pkt_pos >= rx_pkt_expected_len) {
        dispatch_rx_packet(rx_pkt_buf, rx_pkt_expected_len);
        rx_pkt_pos = 0;
    }
}

/* ── Display + button GPIO init (segments, digits, buttons) ──────── */

static void GPIO_Init_DisplayHW(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef gi = {0};
    gi.Mode = GPIO_MODE_OUTPUT_PP;
    gi.Pull = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_HIGH;

    for (uint8_t i = 0; i < SEG_COUNT; i++) {
        gi.Pin = seg_pins[i].pin;
        HAL_GPIO_Init(seg_pins[i].port, &gi);
        HAL_GPIO_WritePin(seg_pins[i].port, seg_pins[i].pin, GPIO_PIN_RESET);
    }
    for (uint8_t i = 0; i < DIG_COUNT; i++) {
        gi.Pin = dig_pins[i].pin;
        HAL_GPIO_Init(dig_pins[i].port, &gi);
        HAL_GPIO_WritePin(dig_pins[i].port, dig_pins[i].pin, GPIO_PIN_RESET);
    }

    gi.Mode = GPIO_MODE_INPUT;
    gi.Pull = GPIO_NOPULL;
    gi.Pin = BTN_SCR_PIN; HAL_GPIO_Init(BTN_SCR_PORT, &gi);
    gi.Pin = BTN_INC_PIN; HAL_GPIO_Init(BTN_INC_PORT, &gi);
    gi.Pin = BTN_DEC_PIN; HAL_GPIO_Init(BTN_DEC_PORT, &gi);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */
  GPIO_Init_DisplayHW();
  update_display_from_text("UL  R1");

  /* Enable USART1 RXNE interrupt (NVIC already enabled by CubeMX MSP) */
  USART1->CR1 |= USART_CR1_RXNEIE;

  /* Start TIM2 for display multiplexing at 1 kHz per digit */
  HAL_NVIC_SetPriority(TIM2_IRQn, 1, 0);
  HAL_NVIC_EnableIRQ(TIM2_IRQn);
  HAL_TIM_Base_Start_IT(&htim2);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    if (rx_text_updated) {
        char local[DIG_COUNT + 1];
        __disable_irq();
        memcpy(local, (const void *)rx_display_text, sizeof(local));
        rx_text_updated = 0;
        __enable_irq();

        __disable_irq();
        update_display_from_text(local);
        __enable_irq();
    }

    uint32_t now = mux_tick;
    static uint32_t last_btn_tick;
    if ((now - last_btn_tick) >= BTN_TX_INTERVAL) {
        last_btn_tick = now;
        send_button_packet(read_buttons());
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL3;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = (24000000U / DISP_MUX_FREQ_HZ) - 1U;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/** TIM2 ISR — display multiplex (~1 kHz, cycles through 6 digits) */
void TIM2_IRQHandler(void)
{
    if (__HAL_TIM_GET_FLAG(&htim2, TIM_FLAG_UPDATE) &&
        __HAL_TIM_GET_IT_SOURCE(&htim2, TIM_IT_UPDATE))
    {
        __HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_UPDATE);

        deselect_all_digits();
        set_segments(disp_segments[disp_active_digit]);
        HAL_GPIO_WritePin(dig_pins[disp_active_digit].port,
                          dig_pins[disp_active_digit].pin, GPIO_PIN_SET);

        disp_active_digit = (disp_active_digit + 1U) % DIG_COUNT;
        mux_tick++;
    }
}

/** Called from USART1_IRQHandler (stm32f1xx_it.c) for each received byte. */
void DISP_UART_RxByte(uint8_t byte)
{
    parse_rx_byte(byte);
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
