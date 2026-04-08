#ifndef _GALVO_HAL_H_
#define _GALVO_HAL_H_

#include <stdint.h>

int galvo_open(void);
int galvo_set_ab(uint16_t a, uint16_t b);
void galvo_close(void);

#endif