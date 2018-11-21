/*
 * Copyright 2016-2018 NXP Semiconductor, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * o Redistributions of source code must retain the above copyright notice, this list
 *   of conditions and the following disclaimer.
 *
 * o Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or
 *   other materials provided with the distribution.
 *
 * o Neither the name of NXP Semiconductor, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
 
/**
 * @file    MK22FN512xxx12_Project.c
 * @brief   Application entry point.
 */
#include <stdio.h>
#include "board.h"
#include "peripherals.h"
#include "pin_mux.h"
#include "clock_config.h"
#include "MK22F51212.h"
#include "fsl_debug_console.h"

/* TODO: insert other include files here. */
#include "fsl_gpio.h"
#include "fsl_rtc.h"
#include "fsl_sdspi.h"
#include "fsl_dspi_freertos.h"

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "queue.h"
#include "time.h"
/* TODO: insert other definitions and declarations here. */

#define NEW_READING (1)
#define PRINTED_READINGS (2)

#define READING_GEN_PERIOD (configTICK_RATE_HZ/6)
#define READING_GEN_COUNT (1)

#define SDSPI SPI0
#define SDSPI_MASTER_PCS_FOR_INIT kDSPI_Pcs0
#define SDSPI_MASTER_CLK_SRC DSPI0_CLK_SRC
#define SDSPI_MASTER_CLK_FREQ CLOCK_GetFreq(DSPI0_CLK_SRC)
#define SDSPI_MASTER_PCS_FOR_TRANSFER kDSPI_MasterPcs0


/* Task priorities. */
/*******************************************************************************
 * Prototypes
 ******************************************************************************/
static void vCreateReading(TimerHandle_t xTimer);
static void task_saveReadings(void *pvParameters);
static void task_printReadings(void *pvParameters);

static status_t cardHost_setFrequency(uint32_t frequency);
static status_t cardHost_exchange(uint8_t *in, uint8_t *out, uint32_t size);

static dspi_master_config_t SDHC_customConfig;

QueueHandle_t queue_readings;
TaskHandle_t printReadings;

uint32_t blocksPerReading;
uint32_t blockIndex;
uint32_t bytesWritten;

struct ReadingMessage {
	char ucMessageID;
	rtc_datetime_t timestamp;
	float reading;
};

struct CardReading {
	rtc_datetime_t timestamp;
	float reading;
};

static sdspi_host_t cardHost;

/*
 * @brief   Application entry point.
 */
int main(void) {
	TimerHandle_t readingTimer;

	rtc_config_t rtcConfig;
    rtc_datetime_t date;
	static sdspi_card_t card;

  	/* Init board hardware. */
    BOARD_InitBootPins();
    BOARD_InitBootClocks();
    BOARD_InitBootPeripherals();
  	/* Init FSL debug console. */
    BOARD_InitDebugConsole();
    /* Init SDHC SPI peripheral */
    BOARD_InitSDHCPeripheral();
    BOARD_InitLEDsPeripheral();

    /* RTC init */
    RTC_GetDefaultConfig(&rtcConfig);
    RTC_Init(RTC, &rtcConfig);
    /* Select RTC clock source */
    RTC_SetClockSource(RTC);
    date.year = 2018;
    date.month = 11;
    date.day = 23;
    date.hour = 0;
    date.minute = 0;
    date.second = 0;
    RTC_StopTimer(RTC);
    RTC_SetDatetime(RTC, &date);
    RTC_StartTimer(RTC);

	/* Initialize SD card */
	cardHost.busBaudRate = BOARD_SDHC_config.ctarConfig.baudRate;
	cardHost.setFrequency = cardHost_setFrequency;
	cardHost.exchange = cardHost_exchange;
	card.host = &cardHost;

	if (kStatus_Success != SDSPI_Init(&card))
	{
	    SDSPI_Deinit(&card);
	    PRINTF("Couldn't init SD card.\r\n");
	    configASSERT(pdFALSE);
	}

	if (SDSPI_CheckReadOnly(&card)) {
		PRINTF("Error: card read-only.\r\n");
		configASSERT(pdFALSE);
	}

	if (SDSPI_SwitchToHighSpeed(&card) != kStatus_Success) {
		PRINTF("Card does not support high speed.\r\n");
	}

//	SDSPI_EraseBlocks(&card, 0, card.blockCount);


	srand(time(NULL));

	/* Initialize the queue to send readings */
    queue_readings = xQueueCreate(10, sizeof(struct ReadingMessage *));
    configASSERT(queue_readings != NULL);
    vQueueAddToRegistry(queue_readings, "Readings Queue");
    PRINTF("Created queue_readings.\r\n");

    /* Start timer to generate readings */
    readingTimer = xTimerCreate("timer_createReading", READING_GEN_PERIOD, pdTRUE, NULL, vCreateReading);
    configASSERT(readingTimer != NULL);
    PRINTF("Created readingTimer.\r\n");
    configASSERT(xTimerStart(readingTimer, READING_GEN_PERIOD) == pdPASS);

    // Start task to save generated readings from a queue
    configASSERT(xTaskCreate(task_saveReadings, "task_saveReadings",
    		configMINIMAL_STACK_SIZE+200, &card, configMAX_PRIORITIES-2, NULL) == pdPASS);
    PRINTF("Created saveReadings task.\r\n");

    // Start task to print readings when SW2 is pressed
    configASSERT(xTaskCreate(task_printReadings, "task_printReadings",
    			configMINIMAL_STACK_SIZE+200, &card, configMAX_PRIORITIES-3, &printReadings) == pdPASS);
    PRINTF("Created printReadings task.\r\n");


    vTaskStartScheduler();
    for (;;);
}

static status_t cardHost_setFrequency(uint32_t frequency) {
	SDHC_customConfig = BOARD_SDHC_config;
	SDHC_customConfig.ctarConfig.baudRate = frequency;
	DSPI_MasterInit(BOARD_SDHC_PERIPHERAL, &SDHC_customConfig, BOARD_SDHC_CLK_FREQ);
	return kStatus_Success;
}

static status_t cardHost_exchange(uint8_t *in, uint8_t *out, uint32_t size) {
	// Setup transfer with data and CTAR and PCS
	dspi_transfer_t masterXfer;
	masterXfer.txData = in;
	masterXfer.rxData = out;
	masterXfer.dataSize = size;
    masterXfer.configFlags = kDSPI_MasterCtar0 | kDSPI_MasterPcs0 | kDSPI_MasterActiveAfterTransfer;

	return DSPI_MasterTransferBlocking(SDSPI, &masterXfer);
}

static void task_saveReadings(void *pvParameters) {
	sdspi_card_t * card;
	struct ReadingMessage * buf;
	struct CardReading * writeBuffer;
	uint8_t queuedReadings;
	struct CardReading reading;

	queuedReadings = 0;
	card = (sdspi_card_t *)pvParameters;
	writeBuffer = pvPortMalloc(card->blockSize);

	for (;;) {
		if (xQueueReceive(queue_readings, &buf, portMAX_DELAY) == pdTRUE) {
			configASSERT(buf);
			if (buf->ucMessageID == NEW_READING) {
				reading.timestamp = buf->timestamp;
				reading.reading = buf->reading;

				// add reading to the write buffer and increment queued
				writeBuffer[queuedReadings++]= reading;

				// check if we've filled a block with readings
				if (queuedReadings*sizeof(reading) >= card->blockSize-sizeof(reading)) {
					taskENTER_CRITICAL();
					// then write block and increment block index
					SDSPI_WriteBlocks(card, (uint8_t *)writeBuffer, blockIndex++, 1);
					taskEXIT_CRITICAL();
					configASSERT(blockIndex < card->blockCount);
					queuedReadings = 0; // no more queued readings
				}
			} else if (buf->ucMessageID == PRINTED_READINGS) {
				blockIndex = 0;
			}
			vPortFree(buf);
		} else {
			PRINTF("Failed to receive message.\r\n");
		}
	}
}

static void task_printReadings(void *pvParameters) {
	sdspi_card_t * card;
	struct CardReading * readBuffer;
	struct ReadingMessage * msg;

	card = (sdspi_card_t *)pvParameters;
	readBuffer = pvPortMalloc(card->blockSize);

	// Started print function, printout buttons can be enabled
    BOARD_InitBUTTONsPeripheral();
    // Using API calls in button interrupt (notify), so set priority low enough
	NVIC_SetPriority(BOARD_SW2_IRQ, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY);
	for (;;) {
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		PRINTF("Received print request: printing %0.2fkB of readings\r\n", blockIndex*(card->blockSize/1024.0F));
		if (blockIndex >= 1) {
			// print out blocks loading 1 block at a time
			for (size_t startBlock=0; startBlock < blockIndex; startBlock++) {

				taskENTER_CRITICAL();
				SDSPI_ReadBlocks(card, (uint8_t *)readBuffer, startBlock, 1);
				taskEXIT_CRITICAL();

				// iterate over readings and print
				for (uint32_t i=0; i < card->blockSize / sizeof(struct CardReading); i++) {
					PRINTF("[%04d-%02d-%02d %02d:%02d:%02d] %f\r\n",
							readBuffer[i].timestamp.year, readBuffer[i].timestamp.month, readBuffer[i].timestamp.day,
							readBuffer[i].timestamp.hour, readBuffer[i].timestamp.minute, readBuffer[i].timestamp.second,
							readBuffer[i].reading);
				}
			}

			msg = pvPortMalloc(sizeof(struct ReadingMessage));
			msg->ucMessageID = PRINTED_READINGS;
			if (xQueueSendToBack(queue_readings, &msg, 0) != pdPASS) {
				PRINTF("Couldn't post printed readings message, queue full.\r\n");
				vPortFree(msg);
			}
			PRINTF("Readings cleared.\r\n\r\n");
		} else {
			PRINTF("No readings to print.\r\n");
		}
	}
}

//static void vPrintReadings(TimerHandle_t xTimer) {
//	xTaskNotifyGive((TaskHandle_t)pvTimerGetTimerID(xTimer));
//}

/*!
 * @brief Task responsible for generating readings.
 */
static void vCreateReading(TimerHandle_t xTimer) {
	for (size_t i=0; i < READING_GEN_COUNT; i++) {
		struct ReadingMessage * reading = pvPortMalloc(sizeof(struct ReadingMessage));
		configASSERT(reading);
		reading->ucMessageID = NEW_READING;
		RTC_GetDatetime(RTC, &(reading->timestamp));
		reading->reading = (float)rand()/100000;

		if (xQueueSendToBackFromISR(queue_readings, &reading, NULL) != pdPASS) {
			PRINTF("Couldn't post reading, queue full.\r\n");
			vPortFree(reading);
		}
	}
}

void BOARD_SW2_IRQHANDLER(void) {
		BaseType_t xHigherPriorityTaskWoken = pdFALSE;
		/* Clear external interrupt flag. */
	    GPIO_PortClearInterruptFlags(BOARD_SW2_GPIO, 1U << BOARD_SW2_GPIO_PIN);
	    /* Change state of button. */
	    configASSERT(printReadings);
	    vTaskNotifyGiveFromISR(printReadings, &xHigherPriorityTaskWoken);
	    /* Add for ARM errata 838869, affects Cortex-M4, Cortex-M4F Store immediate overlapping
	      exception return operation might vector to incorrect interrupt */
	#if defined __CORTEX_M && (__CORTEX_M == 4U)
	    __DSB();
	#endif
}
