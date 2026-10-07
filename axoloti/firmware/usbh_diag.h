/**
 * Build de diagnóstico: mide cuánto CPU se come el ISR del USB host.
 *
 * Por qué: con un controlador MIDI enchufado, la 'dsp load' que reporta el
 * patcher sube aunque el patch no use MIDI. dspLoadPct (patch.c) se mide con
 * DWT->CYCCNT, o sea tiempo de reloj de pared: incluye el tiempo en que el hilo
 * DSP queda desalojado por el ISR del USB host (prioridad NVIC 6, por encima de
 * todos los hilos de ChibiOS) más la contención de memoria/cache del DMA del host.
 *
 * Este build mide el primero: ciclos de CPU dentro de Vector174, promediados en
 * ventanas de 1 s, y reporta ese porcentaje en el campo dspload del ack (o sea,
 * la barra de 'dsp' del patcher muestra la carga del ISR del USB).
 *
 * Sólo se compila con -DUSBH_DIAG=1 (ver docs/USBH-DIAG.md).
 */
#ifndef USBH_DIAG_H
#define USBH_DIAG_H

#include <stdint.h>

/* Acumulados por el ISR del OTG, drenados por usbh_diag_tick(). */
extern volatile uint32_t usbh_diag_isr_cycles;
extern volatile uint32_t usbh_diag_isr_count;

/* Resultados, actualizados una vez por segundo. */
extern uint32_t usbh_diag_isr_pct; /* % de ciclos de CPU dentro del ISR del USB */
extern uint32_t usbh_diag_isr_hz;  /* IRQs del OTG por segundo */

void usbh_diag_init(void);

/* Llamado cada ~100 ms desde el hilo sysmon. */
void usbh_diag_tick(void);

/* Valor a reportar en el campo dspload del ack. */
uint32_t usbh_diag_reported_load(uint32_t dspLoadPct);

#endif