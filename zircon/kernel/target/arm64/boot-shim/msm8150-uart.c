// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <zircon/compiler.h>
#include <stdint.h>
#include "debug.h"

#define SE_UART_TX_TRANS_LEN        0x270
#define SE_GENI_M_CMD0              0x600
#define UART_START_TX               1
#define M_OPCODE_SHFT               27
#define SE_GENI_M_IRQ_STATUS        0x610
#define M_TX_FIFO_WATERMARK_EN      (1 << 30)
#define SE_GENI_M_IRQ_CLEAR         0x618
#define SE_GENI_TX_FIFOn		    0x700
#define SE_GENI_TX_WATERMARK_REG    0x80C
#define M_CMD_DONE_EN       (1 << 0)

#define UARTREG(reg)    (*(volatile uint32_t*)(0x00a90000 + (reg)))
#define hw_mb()         __asm__ volatile("dmb sy" : : : "memory")

void uart_pputc(char c) {
    UARTREG(SE_GENI_TX_WATERMARK_REG) = 2;
    UARTREG(SE_UART_TX_TRANS_LEN) = 1;
    UARTREG(SE_GENI_M_CMD0) = UART_START_TX << M_OPCODE_SHFT;
    hw_mb();

    while (!(UARTREG(SE_GENI_M_IRQ_STATUS) & M_TX_FIFO_WATERMARK_EN))
        ;

    UARTREG(SE_GENI_TX_FIFOn) = c;
    UARTREG(SE_GENI_M_IRQ_CLEAR) = M_TX_FIFO_WATERMARK_EN;

    while (!(UARTREG(SE_GENI_M_IRQ_STATUS) & M_CMD_DONE_EN))
        ;
    UARTREG(SE_GENI_M_IRQ_CLEAR) = M_CMD_DONE_EN;
}
