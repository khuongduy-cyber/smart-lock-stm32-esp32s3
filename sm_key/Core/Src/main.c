/* USER CODE BEGIN Header */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "string.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

#define MAX_RFID_CARDS 120

typedef struct
{
    uint8_t valid;
    uint8_t uid[4];

} RFID_Card_t;

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define RFID_FLASH_ADDR       0x0800FC00UL
#define RFID_FLASH_PAGE_SIZE  1024UL

/*
 * MAGIC dùng d? ki?m tra Flash
 * "RFID"
 */
#define RFID_FLASH_MAGIC      0x52464944UL

#define RFID_FLASH_HEADER_SIZE    4U
#define RFID_RECORD_SIZE          5U
#define RFID_FLASH_CHECKSUM_SIZE  2U
#define RFID_CHECKSUM_OFFSET      (RFID_FLASH_HEADER_SIZE + (MAX_RFID_CARDS * RFID_RECORD_SIZE))
#define RFID_FLASH_DATA_SIZE      (RFID_CHECKSUM_OFFSET + RFID_FLASH_CHECKSUM_SIZE)
#define RFID_LEGACY_CARDS         20U
#define RFID_LEGACY_CHECKSUM_OFFSET (RFID_FLASH_HEADER_SIZE + (RFID_LEGACY_CARDS * RFID_RECORD_SIZE))
#if RFID_FLASH_DATA_SIZE > RFID_FLASH_PAGE_SIZE
#error "RFID database exceeds the reserved Flash page"
#endif

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

SPI_HandleTypeDef hspi1;
UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */

uint8_t status;

uint8_t str[16];

uint8_t sNum[5];
RFID_Card_t rfidCards[MAX_RFID_CARDS];
/*
 * Ch?ng d?c liên t?c 1 th?
 */
uint8_t cardDetected = 0;
uint8_t uart_rx;
volatile uint8_t addRFIDRequest = 0;
volatile uint8_t waitDeleteID = 0;
volatile uint8_t deleteRFIDRequest = 0;
volatile uint8_t deleteRFID_ID = 0;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/

void SystemClock_Config(void);

static void MX_GPIO_Init(void);

static void MX_SPI1_Init(void);

static void MX_USART1_UART_Init(void);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */




void RFID_SendMessage(char *msg)
{
    HAL_UART_Transmit(
        &huart1,
        (uint8_t *)msg,
        strlen(msg),
        100
    );
}




uint16_t RFID_CalculateChecksum(
    uint8_t *data,
    uint16_t length
)
{
    uint16_t checksum = 0;


    for(uint16_t i = 0; i < length; i++)
    {
        checksum += data[i];
    }


    return checksum;
}


int RFID_FindUID(uint8_t *uid)
{
    for(int i = 0; i < MAX_RFID_CARDS; i++)
    {
        if(rfidCards[i].valid == 1)
        {
            if(
                memcmp(
                    rfidCards[i].uid,
                    uid,
                    4
                ) == 0
            )
            {
                return i;
            }
        }
    }


    return -1;
}


int RFID_FindFreeID(void)
{
    for(int i = 0; i < MAX_RFID_CARDS; i++)
    {
        if(rfidCards[i].valid == 0)
        {
            return i;
        }
    }


    return -1;
}




uint8_t RFID_Flash_Save(void)
{
    

    uint8_t flashData[RFID_FLASH_DATA_SIZE];


    memset(
        flashData,
        0,
        sizeof(flashData)
    );


    

    uint32_t magic = RFID_FLASH_MAGIC;


    memcpy(
        &flashData[0],
        &magic,
        4
    );


    

    uint16_t index = 4;


    for(int i = 0; i < MAX_RFID_CARDS; i++)
    {
        /*
         * Valid
         */
        flashData[index] =
            rfidCards[i].valid;

        index++;


        /*
         * UID 4 byte
         */
        memcpy(
            &flashData[index],
            rfidCards[i].uid,
            4
        );


        index += 4;
    }


   

    uint16_t checksum =
        RFID_CalculateChecksum(
            flashData,
            RFID_CHECKSUM_OFFSET
        );


    memcpy(
        &flashData[RFID_CHECKSUM_OFFSET],
        &checksum,
        2
    );


    

    HAL_FLASH_Unlock();


    

    FLASH_EraseInitTypeDef EraseInitStruct;

    uint32_t PageError = 0;


    EraseInitStruct.TypeErase =
        FLASH_TYPEERASE_PAGES;

    EraseInitStruct.PageAddress =
        RFID_FLASH_ADDR;

    EraseInitStruct.NbPages =
        1;


    if(
        HAL_FLASHEx_Erase(
            &EraseInitStruct,
            &PageError
        ) != HAL_OK
    )
    {
        HAL_FLASH_Lock();

        return 0;
    }


    

    uint32_t address =
        RFID_FLASH_ADDR;


    for(
        uint16_t i = 0;
        i < sizeof(flashData);
        i += 2
    )
    {
        uint16_t halfWord;


        halfWord =
            (uint16_t)flashData[i];


        halfWord |=
            ((uint16_t)flashData[i + 1] << 8);


        if(
            HAL_FLASH_Program(
                FLASH_TYPEPROGRAM_HALFWORD,
                address,
                halfWord
            ) != HAL_OK
        )
        {
            HAL_FLASH_Lock();

            return 0;
        }


        address += 2;
    }


    HAL_FLASH_Lock();


    return 1;
}




uint8_t RFID_Flash_Load(void)
{
    uint8_t flashData[RFID_FLASH_DATA_SIZE];


    

    memcpy(
        flashData,
        (uint8_t *)RFID_FLASH_ADDR,
        sizeof(flashData)
    );


    

    uint32_t magic;


    memcpy(
        &magic,
        &flashData[0],
        4
    );


    if(magic != RFID_FLASH_MAGIC)
    {
        /*
         * Flash chua có database
         */

        memset(
            rfidCards,
            0,
            sizeof(rfidCards)
        );


        return 0;
    }


    

    uint16_t savedChecksum;


    memcpy(
        &savedChecksum,
        &flashData[RFID_CHECKSUM_OFFSET],
        2
    );


    uint16_t calculatedChecksum =
        RFID_CalculateChecksum(
            flashData,
            RFID_CHECKSUM_OFFSET
        );


    if(savedChecksum != calculatedChecksum)
    {
        uint16_t legacySavedChecksum;

        memcpy(
            &legacySavedChecksum,
            &flashData[RFID_LEGACY_CHECKSUM_OFFSET],
            2
        );

        uint16_t legacyCalculatedChecksum =
            RFID_CalculateChecksum(
                flashData,
                RFID_LEGACY_CHECKSUM_OFFSET
            );

        if(legacySavedChecksum == legacyCalculatedChecksum)
        {
            memset(
                rfidCards,
                0,
                sizeof(rfidCards)
            );

            uint16_t legacyIndex = RFID_FLASH_HEADER_SIZE;

            for(int i = 0; i < RFID_LEGACY_CARDS; i++)
            {
                rfidCards[i].valid =
                    flashData[legacyIndex++];

                if(rfidCards[i].valid != 1)
                {
                    rfidCards[i].valid = 0;
                }

                memcpy(
                    rfidCards[i].uid,
                    &flashData[legacyIndex],
                    4
                );

                legacyIndex += 4;
            }

            return RFID_Flash_Save();
        }

        memset(
            rfidCards,
            0,
            sizeof(rfidCards)
        );


        return 0;
    }


    

    uint16_t index = 4;


    for(int i = 0; i < MAX_RFID_CARDS; i++)
    {
        /*
         * Valid
         */

        rfidCards[i].valid =
            flashData[index];


        index++;


        /*
         * Ch? ch?p nh?n 0 ho?c 1
         */

        if(rfidCards[i].valid != 1)
        {
            rfidCards[i].valid = 0;
        }


        /*
         * UID
         */

        memcpy(
            rfidCards[i].uid,
            &flashData[index],
            4
        );


        index += 4;
    }


    return 1;
}



int RFID_Add(uint8_t *uid)
{
    

    int oldID =
        RFID_FindUID(uid);


    if(oldID >= 0)
    {
        return oldID;
    }


    

    int newID =
        RFID_FindFreeID();


    if(newID < 0)
    {
        return -1;
    }


    

    RFID_Card_t backup =
        rfidCards[newID];


    

    rfidCards[newID].valid = 1;


    memcpy(
        rfidCards[newID].uid,
        uid,
        4
    );


    

    if(!RFID_Flash_Save())
    {
        /*
         * Flash l?i
         * restore RAM
         */

        rfidCards[newID] =
            backup;


        return -1;
    }


    return newID;
}




uint8_t RFID_Delete(uint8_t cardID)
{
   

    if(cardID >= MAX_RFID_CARDS)
    {
        return 0;
    }


    

    if(rfidCards[cardID].valid == 0)
    {
        return 0;
    }


    

    RFID_Card_t backup =
        rfidCards[cardID];


    

    rfidCards[cardID].valid =
        0;


    memset(
        rfidCards[cardID].uid,
        0,
        4
    );


    

    if(!RFID_Flash_Save())
    {
        /*
         * Flash l?i
         * restore
         */

        rfidCards[cardID] =
            backup;


        return 0;
    }


    return 1;
}




void RFID_PrintDatabase(void)
{
    char msg[80];


    for(int i = 0; i < MAX_RFID_CARDS; i++)
    {
        if(rfidCards[i].valid == 1)
        {
            sprintf(
                msg,
                "ID:%d UID:%02X %02X %02X %02X\r\n",
                i,
                rfidCards[i].uid[0],
                rfidCards[i].uid[1],
                rfidCards[i].uid[2],
                rfidCards[i].uid[3]
            );


            HAL_UART_Transmit(
                &huart1,
                (uint8_t *)msg,
                strlen(msg),
                100
            );
        }
    }
}


uint16_t RFID_Count(void)
{
    uint16_t count = 0;

    for(int i = 0; i < MAX_RFID_CARDS; i++)
    {
        if(rfidCards[i].valid == 1)
        {
            count++;
        }
    }

    return count;
}


void RFID_SendCount(void)
{
    char msg[24];

    sprintf(
        msg,
        "COUNT:%u\n",
        (unsigned int)RFID_Count()
    );

    RFID_SendMessage(msg);
}

/* USER CODE END 0 */


/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

    /* MCU Configuration--------------------------------------------------------*/

    HAL_Init();


    /* Configure the system clock */

    SystemClock_Config();


    /* Initialize all configured peripherals */

    MX_GPIO_Init();

    MX_SPI1_Init();

    MX_USART1_UART_Init();


    /* USER CODE BEGIN 2 */

    

    MFRC522_Init();


    

    if(RFID_Flash_Load())
    {
        RFID_SendMessage(
            "RFID_FLASH_OK\n"
        );
    }

    else
    {
        RFID_SendMessage(
            "RFID_FLASH_EMPTY\n"
        );
    }

    RFID_SendCount();


    

    HAL_UART_Receive_IT(
        &huart1,
        &uart_rx,
        1
    );


    /* USER CODE END 2 */


    /* Infinite loop */
    /* USER CODE BEGIN WHILE */

    while(1)
    {

        

        if(deleteRFIDRequest)
        {
            /*
             * Copy ID tru?c
             */
            uint8_t deleteID =
                deleteRFID_ID;


            /*
             * Reset flag
             */
            deleteRFIDRequest = 0;


            char msg[30];


            if(
                RFID_Delete(
                    deleteID
                )
            )
            {
                sprintf(
                    msg,
                    "DEL_OK:%d\n",
                    deleteID
                );


                RFID_SendMessage(
                    msg
                );

                RFID_SendCount();
            }

            else
            {
                RFID_SendMessage(
                    "ID_EMPTY\n"
                );
            }
        }


        

        status =
            MFRC522_Request(
                PICC_REQIDL,
                str
            );


        

        if(status == MI_OK)
        {
            uint32_t authStartTick = HAL_GetTick();

            status =
                MFRC522_Anticoll(
                    str
                );


            if(status == MI_OK)
            {

                /*
                 * Ch? x? lư m?t l?n khi dua th? vào
                 */

                if(cardDetected == 0)
                {
                    cardDetected = 1;


                    memcpy(
                        sNum,
                        str,
                        5
                    );


                    

                    if(addRFIDRequest)
                    {

                        /*
                         * Ki?m tra th? dă t?n t?i chua
                         */

                        int oldID =
                            RFID_FindUID(str);


                        if(oldID >= 0)
                        {
                            char msg[30];


                            sprintf(
                                msg,
                                "EXISTS:%d\n",
                                oldID
                            );


                            RFID_SendMessage(
                                msg
                            );


                            /*
                             * Thoát ADD mode
                             */

                            addRFIDRequest = 0;
                        }

                        else
                        {

                            /*
                             * T́m ID + luu Flash
                             */

                            int newID =
                                RFID_Add(str);


                            if(newID >= 0)
                            {
                                char msg[30];


                                sprintf(
                                    msg,
                                    "ADD_OK:%d\n",
                                    newID
                                );


                                RFID_SendMessage(
                                    msg
                                );

                                RFID_SendCount();
                            }

                            else
                            {
                                RFID_SendMessage(
                                    "FULL\n"
                                );
                            }


                            /*
                             * Thoát ADD mode
                             */

                            addRFIDRequest = 0;
                        }
                    }


                    /* ========================================
                       NORMAL MODE
                       ======================================== */

                    else
                    {

                        /*
                         * T́m UID trong database
                         */

                        int cardID =
                            RFID_FindUID(str);


                        /*
                         * Có th?
                         */

                        if(cardID >= 0)
                        {
                            char msg[48];
                            uint32_t authTimeMs =
                                HAL_GetTick() - authStartTick;

                            sprintf(
                                msg,
                                "RFID_OK:%d:%lu\n",
                                cardID,
                                (unsigned long)authTimeMs
                            );

                            RFID_SendMessage(msg);
                        }

                        else
                        {
                            char msg[40];
                            uint32_t authTimeMs =
                                HAL_GetTick() - authStartTick;

                            sprintf(
                                msg,
                                "RFID_DENIED:%lu\n",
                                (unsigned long)authTimeMs
                            );

                            RFID_SendMessage(msg);
                        }

                    }
                }
            }
        }


        /* ====================================================
           NO CARD
           ==================================================== */

        else
        {
            /*
             * Th? dă l?y ra
             *
             * cho phép d?c th? ti?p theo
             */

            cardDetected = 0;
        }
    }

    /* USER CODE END WHILE */
}


/* ============================================================
   SYSTEM CLOCK
   ============================================================ */

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};

    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};


    RCC_OscInitStruct.OscillatorType =
        RCC_OSCILLATORTYPE_HSE;


    RCC_OscInitStruct.HSEState =
        RCC_HSE_ON;


    RCC_OscInitStruct.HSEPredivValue =
        RCC_HSE_PREDIV_DIV1;


    RCC_OscInitStruct.HSIState =
        RCC_HSI_ON;


    RCC_OscInitStruct.PLL.PLLState =
        RCC_PLL_ON;


    RCC_OscInitStruct.PLL.PLLSource =
        RCC_PLLSOURCE_HSE;


    RCC_OscInitStruct.PLL.PLLMUL =
        RCC_PLL_MUL9;


    if(
        HAL_RCC_OscConfig(
            &RCC_OscInitStruct
        ) != HAL_OK
    )
    {
        Error_Handler();
    }


    RCC_ClkInitStruct.ClockType =
        RCC_CLOCKTYPE_HCLK |
        RCC_CLOCKTYPE_SYSCLK |
        RCC_CLOCKTYPE_PCLK1 |
        RCC_CLOCKTYPE_PCLK2;


    RCC_ClkInitStruct.SYSCLKSource =
        RCC_SYSCLKSOURCE_PLLCLK;


    RCC_ClkInitStruct.AHBCLKDivider =
        RCC_SYSCLK_DIV1;


    RCC_ClkInitStruct.APB1CLKDivider =
        RCC_HCLK_DIV2;


    RCC_ClkInitStruct.APB2CLKDivider =
        RCC_HCLK_DIV1;


    if(
        HAL_RCC_ClockConfig(
            &RCC_ClkInitStruct,
            FLASH_LATENCY_2
        ) != HAL_OK
    )
    {
        Error_Handler();
    }
}


/* ============================================================
   SPI1
   RC522
   ============================================================ */

static void MX_SPI1_Init(void)
{
    hspi1.Instance =
        SPI1;


    hspi1.Init.Mode =
        SPI_MODE_MASTER;


    hspi1.Init.Direction =
        SPI_DIRECTION_2LINES;


    hspi1.Init.DataSize =
        SPI_DATASIZE_8BIT;


    hspi1.Init.CLKPolarity =
        SPI_POLARITY_LOW;


    hspi1.Init.CLKPhase =
        SPI_PHASE_1EDGE;


    hspi1.Init.NSS =
        SPI_NSS_SOFT;


    hspi1.Init.BaudRatePrescaler =
        SPI_BAUDRATEPRESCALER_64;


    hspi1.Init.FirstBit =
        SPI_FIRSTBIT_MSB;


    hspi1.Init.TIMode =
        SPI_TIMODE_DISABLE;


    hspi1.Init.CRCCalculation =
        SPI_CRCCALCULATION_DISABLE;


    hspi1.Init.CRCPolynomial =
        10;


    if(
        HAL_SPI_Init(
            &hspi1
        ) != HAL_OK
    )
    {
        Error_Handler();
    }
}




static void MX_USART1_UART_Init(void)
{
    huart1.Instance =
        USART1;


    huart1.Init.BaudRate =
        115200;


    huart1.Init.WordLength =
        UART_WORDLENGTH_8B;


    huart1.Init.StopBits =
        UART_STOPBITS_1;


    huart1.Init.Parity =
        UART_PARITY_NONE;


    huart1.Init.Mode =
        UART_MODE_TX_RX;


    huart1.Init.HwFlowCtl =
        UART_HWCONTROL_NONE;


    huart1.Init.OverSampling =
        UART_OVERSAMPLING_16;


    if(
        HAL_UART_Init(
            &huart1
        ) != HAL_OK
    )
    {
        Error_Handler();
    }
}




static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};


    /* GPIO Ports Clock Enable */

    __HAL_RCC_GPIOD_CLK_ENABLE();

    __HAL_RCC_GPIOA_CLK_ENABLE();

    __HAL_RCC_GPIOB_CLK_ENABLE();


    /* PA4 */

    HAL_GPIO_WritePin(
        GPIOA,
        GPIO_PIN_4,
        GPIO_PIN_RESET
    );


    /* PB0 */

    HAL_GPIO_WritePin(
        GPIOB,
        GPIO_PIN_0,
        GPIO_PIN_RESET
    );


    /* Configure GPIO pin : PA4 */

    GPIO_InitStruct.Pin =
        GPIO_PIN_4;


    GPIO_InitStruct.Mode =
        GPIO_MODE_OUTPUT_PP;


    GPIO_InitStruct.Pull =
        GPIO_NOPULL;


    GPIO_InitStruct.Speed =
        GPIO_SPEED_FREQ_LOW;


    HAL_GPIO_Init(
        GPIOA,
        &GPIO_InitStruct
    );


    /* Configure GPIO pin : PB0 */

    GPIO_InitStruct.Pin =
        GPIO_PIN_0;


    GPIO_InitStruct.Mode =
        GPIO_MODE_OUTPUT_PP;


    GPIO_InitStruct.Pull =
        GPIO_NOPULL;


    GPIO_InitStruct.Speed =
        GPIO_SPEED_FREQ_LOW;


    HAL_GPIO_Init(
        GPIOB,
        &GPIO_InitStruct
    );
}


/* USER CODE BEGIN 4 */



void HAL_UART_RxCpltCallback(
    UART_HandleTypeDef *huart
)
{
    if(huart->Instance == USART1)
    {

        

        if(waitDeleteID)
        {
            deleteRFID_ID =
                uart_rx;


            deleteRFIDRequest =
                1;


            waitDeleteID =
                0;
        }


        

        else if(uart_rx == 'A')
        {
            addRFIDRequest =
                1;
        }


        

        else if(uart_rx == 'D')
        {
            waitDeleteID =
                1;
        }


        

        HAL_UART_Receive_IT(
            &huart1,
            &uart_rx,
            1
        );
    }
}

/* USER CODE END 4 */


/* ============================================================
   ERROR
   ============================================================ */

void Error_Handler(void)
{
    __disable_irq();


    while(1)
    {

    }
}


#ifdef USE_FULL_ASSERT

void assert_failed(
    uint8_t *file,
    uint32_t line
)
{

}

#endif
