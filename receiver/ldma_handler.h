/**
 * @file ldma_handler.h
 *
 * @author Johannes Ehala, ProLab.
 * @license MIT
 *
 * Copyright ProLab, TTÜ. 2021
 */

#ifndef LDMA_HANDLER_H_
#define LDMA_HANDLER_H_

#include "em_ldma.h"

#define ACC_LDMA_CHANNEL_UART	        1 // Channel number 0...7
#define ACC_LDMA_CHANNEL_UART_MASK      (1 << ACC_LDMA_CHANNEL_UART)

void ldma_init (void);
void ldma_uart_start (LDMA_Descriptor_t* uartDescriptor);
void ldma_uart_stop ();
bool ldma_busy();

#endif // LDMA_HANDLER_H_
