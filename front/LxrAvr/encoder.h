#ifndef ENCODER_H_
#define ENCODER_H_

#include <stdint.h>

void encode_init(void);
int8_t encode_stableRead4(void);
uint8_t encode_readButton(void);

#endif /* ENCODER_H_ */
