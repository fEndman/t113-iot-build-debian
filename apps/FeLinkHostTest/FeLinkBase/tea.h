#ifndef _FELINK_DEV_TEA_
#define _FELINK_DEV_TEA_

#include "felink.h"

int fl_xxtea_encrypt(uint32_t *buf, size_t len, uint32_t *key);
int fl_xxtea_decrypt(uint32_t *buf, size_t len, uint32_t *key);
int fl_xxtea_byte_array_encrypt(uint8_t *plaintext, size_t byte_len, uint32_t *key);
int fl_xxtea_byte_array_decrypt(uint8_t *ciphetext, size_t byte_len, uint32_t *key);

#endif // !_FELINK_DEV_TEA_
