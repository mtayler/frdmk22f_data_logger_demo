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
static void vPrintReadings(TimerHandle_t xTimer);
static void task_saveReadings(void *pvParameters);
static void task_printReadings(void *pvParameters);

static status_t cardHost_setFrequency(uint32_t frequency);
static status_t cardHost_exchange(uint8_t *in, uint8_t *out, uint32_t size);

static dspi_master_config_t SDHC_customConfig;

QueueHandle_t queue_readings;

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
	TimerHandle_t printTimer;

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
	    configASSERT(pdFALSE);
	}

	if (SDSPI_CheckReadOnly(&card)) {
		PRINTF("Error: card read-only.\r\n");
		configASSERT(pdFALSE);
	}

	if (SDSPI_SwitchToHighSpeed(&card) != kStatus_Success) {
		PRINTF("Card does not support high speed.\r\n");
	}

	SDSPI_EraseBlocks(&card, 0, card.blockCount);


	srand(time(NULL));

	/* Initialize the queue to send readings */
    queue_readings = xQueueCreate(10, sizeof(struct ReadingMessage *));
    configASSERT(queue_readings != NULL);
    vQueueAddToRegistry(queue_readings, "Readings Queue");
    PRINTF("Created queue_readings.\r\n");

    /* Start timer to generate readings */
    readingTimer = xTimerCreate("timer_createReading", configTICK_RATE_HZ, pdTRUE, NULL, vCreateReading);
    configASSERT(readingTimer != NULL);
    PRINTF("Created readingTimer.\r\n");
    configASSERT(xTimerStart(readingTimer, configTICK_RATE_HZ/4) == pdPASS);

    configASSERT(xTaskCreate(task_saveReadings, "task_saveReadings",
    		configMINIMAL_STACK_SIZE+200, &card, configMAX_PRIORITIES-2, NULL) == pdPASS);

    TaskHandle_t printReadings;
    configASSERT(xTaskCreate(task_printReadings, "task_printReadings",
    			configMINIMAL_STACK_SIZE+200, &card, configMAX_PRIORITIES-3, &printReadings) == pdPASS);

    /* Start timer to print out all readings */
    printTimer = xTimerCreate("timer_printReadings", 20 * configTICK_RATE_HZ, pdTRUE, printReadings, vPrintReadings);
    configASSERT(printTimer != NULL);
    PRINTF("Created print_timer.\r\n");
    configASSERT(xTimerStart(printTimer, 0) == pdPASS);

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

	for (;;) {
		if (blockIndex >= 1) {
			card = (sdspi_card_t *)pvParameters;
			readBuffer = pvPortMalloc(card->blockSize*blockIndex);

			taskENTER_CRITICAL();
			SDSPI_ReadBlocks(card, (uint8_t *)readBuffer, 0, blockIndex);
			taskEXIT_CRITICAL();

			// iterate over readings and print
			for (uint32_t i=0; i < card->blockSize*blockIndex / sizeof(struct CardReading); i += 1) {
				PRINTF("[%04d-%02d-%02d %02d:%02d:%02d] %f\r\n",
						readBuffer[i].timestamp.year, readBuffer[i].timestamp.month, readBuffer[i].timestamp.day,
						readBuffer[i].timestamp.hour, readBuffer[i].timestamp.minute, readBuffer[i].timestamp.second,
						readBuffer[i].reading);
			}
			PRINTF("Readings cleared.\r\n\r\n");

			msg = pvPortMalloc(sizeof(struct ReadingMessage));
			msg->ucMessageID = PRINTED_READINGS;
			if (xQueueSendToBack(queue_readings, &msg, 0) != pdPASS) {
				PRINTF("Couldn't post printed readings message, queue full.\r\n");
				vPortFree(msg);
			}
			vPortFree(readBuffer);
		} else {
			PRINTF("No readings to print.\r\n");
		}

		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
	}
}

static void vPrintReadings(TimerHandle_t xTimer) {
	xTaskNotifyGive((TaskHandle_t)pvTimerGetTimerID(xTimer));
}

/*!
 * @brief Task responsible for generating readings.
 */
static void vCreateReading(TimerHandle_t xTimer) {
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
