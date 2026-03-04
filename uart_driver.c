/*
 * uart_driver.c
 * Created on: July 27, 2021
 * Author: Shyama M. Gandhi
 * Modified by : Antonio Andara
 * Modified on : February 22, 2026
 * TXTRIG-based UART driver
 */

#include "uart_driver.h"
#include "task.h"
#include <xil_printf.h>

// -------------------------------------------------
// Global variables
// -------------------------------------------------
XUartPs UART;
XUartPs_Config *Config;
INTC InterruptController;
u32 IntrMask;

// Queues
QueueHandle_t xTxQueue;
QueueHandle_t xRxQueue;

// Interrupt counters
int countRxIrq;
int countTxIrq;
int byteCount;

// -------------------------------------------------
// Interrupt Handler
// -------------------------------------------------
void interruptHandler(void *CallBackRef, u32 event, unsigned int EventData)
{
    u32 isrStatus;

    isrStatus = XUartPs_ReadReg(UART_BASEADDR, XUARTPS_ISR_OFFSET);

    // RX events
    if (isrStatus & (XUARTPS_IXR_RXFULL | XUARTPS_IXR_RXOVR)){
        handleReceiveEvent();
    }

    // TX EMPTY event
    if (isrStatus & XUARTPS_IXR_TXEMPTY){
        handleSentEvent();
    }

    // Clear interrupts
    XUartPs_WriteReg(UART_BASEADDR, XUARTPS_ISR_OFFSET, isrStatus);
}

// -------------------------------------------------
// RX ISR
// -------------------------------------------------
void handleReceiveEvent()
{
    countRxIrq++;
    u8 receive_buffer;
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;


    while (XUartPs_IsReceiveData(UART_BASEADDR)){
        receive_buffer = XUartPs_ReadReg(UART_BASEADDR, UART_FIFO_OFFSET);

        xQueueSendFromISR(xRxQueue, &receive_buffer, &xHigherPriorityTaskWoken);

    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// -------------------------------------------------
// TX ISR
// -------------------------------------------------
void handleSentEvent()
{
    countTxIrq++;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    u8 txByte;

    if (xQueueReceiveFromISR(
            xTxQueue,
            &txByte,
            &xHigherPriorityTaskWoken
        ) == pdPASS)
    {
        if (!(XUartPs_ReadReg(
                UART.Config.BaseAddress,
                XUARTPS_SR_OFFSET
            ) & XUARTPS_SR_TXFULL))
        {
            XUartPs_WriteReg(
                UART.Config.BaseAddress,
                XUARTPS_FIFO_OFFSET,
                txByte
            );
        }
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// -------------------------------------------------
// TXEMPTY control (ADDED FOR PART3)
// -------------------------------------------------
void enableTxEmpty()
{
    IntrMask = XUartPs_GetInterruptMask(&UART);
    IntrMask |= XUARTPS_IXR_TXEMPTY;
    XUartPs_SetInterruptMask(&UART, IntrMask);
}

void disableTxEmpty()
{
    IntrMask = XUartPs_GetInterruptMask(&UART);
    IntrMask &= ~XUARTPS_IXR_TXEMPTY;
    XUartPs_SetInterruptMask(&UART, IntrMask);
}

// -------------------------------------------------
// Public API
// -------------------------------------------------
BaseType_t myReceiveData(void)
{
    return (uxQueueMessagesWaiting(xRxQueue) > 0);
}


BaseType_t myTransmitFull(void)
{
    return (uxQueueSpacesAvailable(xTxQueue) == 0);
}

// ADDED FOR PART3
void mySendByte(u8 data)
{
    BaseType_t empty =
        (uxQueueMessagesWaiting(xTxQueue) == 0);

    if (empty){
        XUartPs_WriteReg(
            UART.Config.BaseAddress,
            XUARTPS_FIFO_OFFSET,
            data
        );
    }
    else{
        xQueueSend(
            xTxQueue,
            &data,
            0
        );

        enableTxEmpty();
    }
}

// ADDED FOR PART3
u8 myReceiveByte(void)
{
    u8 recv = 0;

    if (myReceiveData()){
        xQueueReceive(
            xRxQueue,
            (void*)&recv,
            portMAX_DELAY
        );

        byteCount++;   // ← ADD THIS LINE
    }

    return recv;
}

// ADDED FOR PART3
void mySendString(const char* str)
{
    int i = 0;

    while (str[i] != '\0'){
        mySendByte((u8)str[i]);
        i++;
    }
}


// -------------------------------------------------
// Initialization
// -------------------------------------------------
int initializeUART(void)
{
    int Status;

    Config = XUartPs_LookupConfig(UART_DEVICE_ID);
    if (NULL == Config){
        return XST_FAILURE;
    }

    Status = XUartPs_CfgInitialize(&UART, Config, Config->BaseAddress);
    if (Status != XST_SUCCESS){
        return XST_FAILURE;
    }

    return XST_SUCCESS;
}

int setupInterruptSystem(INTC *IntcInstancePtr, XUartPs *UartInstancePtr, u16 UartIntrId)
{
    int Status;
    XScuGic_Config *IntcConfig;

    IntcConfig = XScuGic_LookupConfig(INTC_DEVICE_ID);
    if (NULL == IntcConfig)
        return XST_FAILURE;

    Status = XScuGic_CfgInitialize(IntcInstancePtr, IntcConfig, IntcConfig->CpuBaseAddress);
    if (Status != XST_SUCCESS)
        return XST_FAILURE;

    Xil_ExceptionRegisterHandler(XIL_EXCEPTION_ID_INT, (Xil_ExceptionHandler)XScuGic_InterruptHandler, IntcInstancePtr);

    Status = XScuGic_Connect(IntcInstancePtr, UartIntrId, (Xil_ExceptionHandler)XUartPs_InterruptHandler, (void *)UartInstancePtr);
    if (Status != XST_SUCCESS)
        return XST_FAILURE;

    XScuGic_Enable(IntcInstancePtr, UartIntrId);
    Xil_ExceptionEnable();

    XUartPs_SetHandler(UartInstancePtr, (XUartPs_Handler)interruptHandler, UartInstancePtr);

    // -------------------------------------------------
    // IMPORTANT: RX FIFO trigger level
    // -------------------------------------------------
    XUartPs_SetFifoThreshold(UartInstancePtr, 1);  // interrupt triggers when FIFO <= 1

    // UART interrupt mask, Enable the interrupt when the receive buffer has reached a particular threshold
    IntrMask = XUARTPS_IXR_TOUT | XUARTPS_IXR_RXFULL  |
               XUARTPS_IXR_RXOVR | XUARTPS_IXR_TXEMPTY;

    XUartPs_SetInterruptMask(UartInstancePtr, IntrMask);
    XUartPs_SetOperMode(UartInstancePtr, XUARTPS_OPER_MODE_NORMAL);

    return XST_SUCCESS;
}
