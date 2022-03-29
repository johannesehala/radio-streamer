/**
 * @file ldma_handler.c
 *
 * @brief   Here LDMA is initialized and LDMA channels are configured and started.
 *          LDMA IRQ handler is here.
 * 
 * 
 * @author Johannes Ehala, ProLab.
 * @license MIT
 *
 * Copyright ProLab, TTÜ. 2021
 */

#include "em_cmu.h"
#include "ldma_handler.h"

/**
 * @brief LDMA IRQ handler.
 */
void LDMA_IRQHandler(void)
{
    /* Get all pending and enabled interrupts. */
    uint32_t pending = LDMA_IntGetEnabled();

    /* Loop here on an LDMA error to enable debugging. */
    while (pending & LDMA_IF_ERROR) 
    {
        PLATFORM_LedsSet(PLATFORM_LedsGet()^4);
    }

    if(pending & ACC_LDMA_CHANNEL_UART_MASK)
    {
        /* Clear interrupt flag. */
        LDMA->IFC = ACC_LDMA_CHANNEL_UART_MASK;
    }
}

/**
 * @brief Initialize the LDMA controller.
 */
void ldma_init (void)
{
    CMU_ClockEnable(cmuClock_LDMA, true);
    
    LDMA_Init_t init = LDMA_INIT_DEFAULT; // Only priority based arbitration, no round-robin.
    LDMA_Init(&init);
    
    NVIC_ClearPendingIRQ(LDMA_IRQn);
    NVIC_EnableIRQ(LDMA_IRQn);
    NVIC_SetPriority(LDMA_IRQn, 3);
}

/**
 * @brief Start LDMA for memory to UART transfer.
 */
void ldma_uart_start(LDMA_Descriptor_t* uartDescriptor)
{
    LDMA_TransferCfg_t memToUartCfg = LDMA_TRANSFER_CFG_PERIPHERAL(ldmaPeripheralSignal_USART0_TXBL);

    LDMA_IntEnable(ACC_LDMA_CHANNEL_UART_MASK);
    
    LDMA_StartTransfer(ACC_LDMA_CHANNEL_UART, &memToUartCfg, uartDescriptor);
}

void ldma_uart_stop (void)
{
    LDMA_StopTransfer(ACC_LDMA_CHANNEL_UART);
}

bool ldma_busy()
{
    return !LDMA_TransferDone(ACC_LDMA_CHANNEL_UART);
}
