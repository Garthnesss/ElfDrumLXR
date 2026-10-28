/* Native (host) stand-in for the STM32 HAL: just the types and Cortex-M intrinsics the sound engine uses.
   Adapted from mechanical-dance/PHX-D01 shim/stm32f7xx_hal.h (GPL-3.0; see NOTICE).
   ElfDrumLXR extensions, marked below: __SADD16 and the __SMLALD family. The pinned firmware does
   not actually call them (the only __SADD16 occurrences in the source are inside comments in
   mixer.c), but ARCHITECTURE.md lists the full ARM DSP surface, so the shim provides them and
   tests/test_intrinsics.c verifies them against hand-computed ARM semantics. */
#pragma once
#include <stdint.h>
#include <string.h>
#define __IO volatile
#define __attribute_section(x)
static inline int16_t __ssat16_1(int32_t x){ return x > 32767 ? 32767 : x < -32768 ? -32768 : (int16_t)x; }
static inline uint32_t __QADD16(uint32_t a, uint32_t b){ return (uint16_t)__ssat16_1((int16_t)a + (int16_t)b) | ((uint32_t)(uint16_t)__ssat16_1((int16_t)(a>>16) + (int16_t)(b>>16)) << 16); }
static inline uint32_t __QSUB16(uint32_t a, uint32_t b){ return (uint16_t)__ssat16_1((int16_t)a - (int16_t)b) | ((uint32_t)(uint16_t)__ssat16_1((int16_t)(a>>16) - (int16_t)(b>>16)) << 16); }
/* ElfDrumLXR extension: SADD16 is the non-saturating twin of QADD16 — halfword results wrap modulo 2^16. */
static inline uint32_t __SADD16(uint32_t a, uint32_t b){ return (uint16_t)((int16_t)a + (int16_t)b) | ((uint32_t)(uint16_t)((int16_t)(a>>16) + (int16_t)(b>>16)) << 16); }
/* ElfDrumLXR extension: dual 16x16 signed multiply, accumulated into a 64-bit result (L = low*low + high*high;
   LDX swaps the second operand's halfwords first). */
static inline int64_t __SMLALD(uint32_t a, uint32_t b, int64_t acc){ return acc + (int64_t)((int16_t)a * (int16_t)b) + (int64_t)((int16_t)(a>>16) * (int16_t)(b>>16)); }
static inline int64_t __SMLALDX(uint32_t a, uint32_t b, int64_t acc){ return acc + (int64_t)((int16_t)a * (int16_t)(b>>16)) + (int64_t)((int16_t)(a>>16) * (int16_t)b); }
#define __SSAT(x, n) ((int32_t)(x) > ((1<<((n)-1))-1) ? ((1<<((n)-1))-1) : (int32_t)(x) < -(1<<((n)-1)) ? -(1<<((n)-1)) : (int32_t)(x))
#define __USAT(x, n) ((int32_t)(x) < 0 ? 0 : (int32_t)(x) > ((1<<(n))-1) ? ((1<<(n))-1) : (int32_t)(x))
#define __disable_irq()
#define __enable_irq()
#define __DSB()
#define __ISB()
static inline uint32_t __PKHBT(uint32_t a, uint32_t b, int s){ return (a & 0xFFFF) | ((b << s) & 0xFFFF0000u); }
static inline uint32_t __PKHTB(uint32_t a, uint32_t b, int s){ return (a & 0xFFFF0000u) | ((b >> s) & 0xFFFF); }
typedef struct { int dummy; } RNG_HandleTypeDef;
typedef int HAL_StatusTypeDef;
#define HAL_OK 0
#include <stdlib.h>
static inline HAL_StatusTypeDef HAL_RNG_GenerateRandomNumber(RNG_HandleTypeDef *h, uint32_t *v){ (void)h; *v = arc4random(); return HAL_OK; }
static inline uint32_t HAL_RNG_GetRandomNumber(RNG_HandleTypeDef *h){ (void)h; return arc4random(); }
static struct { uint32_t CYCCNT, CTRL; } DWT_s;
#define DWT (&DWT_s)
static inline uint32_t HAL_RCC_GetHCLKFreq(void){ return 216000000u; }
static inline uint32_t HAL_GetTick(void){ return 0; }
#define __STATIC_INLINE static inline
#define __INLINE inline
typedef struct { int dummy; } I2S_HandleTypeDef;
typedef struct { int dummy; } UART_HandleTypeDef;
typedef struct { int dummy; } TIM_HandleTypeDef;
typedef struct { int dummy; } DMA_HandleTypeDef;
typedef struct { int dummy; } ADC_HandleTypeDef;
typedef struct { int dummy; } SPI_HandleTypeDef;
typedef struct { volatile uint32_t IDR, ODR, BSRR; } GPIO_TypeDef;
extern GPIO_TypeDef native_gpio[11];  /* A..K; IDR bits read as jack-detect switches, set in engine.c */
#define GPIOA (&native_gpio[0])
#define GPIOB (&native_gpio[1])
#define GPIOC (&native_gpio[2])
#define GPIOD (&native_gpio[3])
#define GPIOE (&native_gpio[4])
#define GPIO_PIN_0 0x0001u
#define GPIO_PIN_1 0x0002u
#define GPIO_PIN_2 0x0004u
#define GPIO_PIN_3 0x0008u
#define GPIO_PIN_4 0x0010u
#define GPIO_PIN_5 0x0020u
#define GPIO_PIN_6 0x0040u
#define GPIO_PIN_7 0x0080u
#define GPIO_PIN_8 0x0100u
#define GPIO_PIN_9 0x0200u
#define GPIO_PIN_10 0x0400u
#define GPIO_PIN_11 0x0800u
#define GPIO_PIN_12 0x1000u
#define GPIO_PIN_13 0x2000u
#define GPIO_PIN_14 0x4000u
#define GPIO_PIN_15 0x8000u
typedef enum { GPIO_PIN_RESET = 0, GPIO_PIN_SET } GPIO_PinState;
#define RESET 0
#define SET 1
static inline GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *p, uint16_t pin){ return (p->IDR & pin) ? GPIO_PIN_SET : GPIO_PIN_RESET; }
static inline void HAL_GPIO_WritePin(GPIO_TypeDef *p, uint16_t pin, GPIO_PinState s){ if (s) p->ODR |= pin; else p->ODR &= ~pin; }
static inline void HAL_GPIO_TogglePin(GPIO_TypeDef *p, uint16_t pin){ p->ODR ^= pin; }
#define __CLZ(x) ((uint8_t)((x) ? __builtin_clz(x) : 32))
static inline int32_t __QADD(int32_t a, int32_t b){ int64_t r = (int64_t)a + b; return r > INT32_MAX ? INT32_MAX : r < INT32_MIN ? INT32_MIN : (int32_t)r; }
static inline int32_t __QSUB(int32_t a, int32_t b){ int64_t r = (int64_t)a - b; return r > INT32_MAX ? INT32_MAX : r < INT32_MIN ? INT32_MIN : (int32_t)r; }
#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE static inline
#endif
