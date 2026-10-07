/**
 * Build de diagnóstico del USB host (ver usbh_diag.h y docs/USBH-DIAG.md).
 * Sólo se compila cuando USBH_DIAG está definido.
 */
#include "ch.h"
#include "hal.h"
#include "core_cm7.h"
#include "usbh_diag.h"

volatile uint32_t usbh_diag_isr_cycles;
volatile uint32_t usbh_diag_isr_count;

uint32_t usbh_diag_isr_pct;
uint32_t usbh_diag_isr_hz;

static uint32_t diag_acc_cycles;
static uint32_t diag_acc_count;
static uint32_t diag_acc_ms;

void usbh_diag_init(void) {
    /* DWT->CYCCNT ya lo usa patch.c, asegurarse de que corre. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void usbh_diag_tick(void) {
    uint32_t cycles;
    uint32_t count;

    chSysLock();
    cycles = usbh_diag_isr_cycles;
    count = usbh_diag_isr_count;
    usbh_diag_isr_cycles = 0;
    usbh_diag_isr_count = 0;
    chSysUnlock();

    diag_acc_cycles += cycles;
    diag_acc_count += count;
    diag_acc_ms += 100;

    if (diag_acc_ms >= 1000) {
        /* STM32_SYS_CK ciclos por segundo; /100 da ciclos por 1%. */
        uint32_t one_pct = STM32_SYS_CK / 100u;
        usbh_diag_isr_pct = (diag_acc_cycles + (one_pct / 2u)) / one_pct;
        usbh_diag_isr_hz = diag_acc_count;
        diag_acc_cycles = 0;
        diag_acc_count = 0;
        diag_acc_ms = 0;
    }
}

uint32_t usbh_diag_reported_load(uint32_t dspLoadPct) {
    uint32_t pct = usbh_diag_isr_pct;
    (void)dspLoadPct;
    return (pct > 100u) ? 100u : pct;
}