/* Native stand-in: USB CDC is not used on the host. */
#pragma once
#include <stdint.h>
typedef struct { int dummy; } USBD_HandleTypeDef;
