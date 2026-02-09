#ifndef _FELINK_DEV_USERCONF_
#define _FELINK_DEV_USERCONF_

#define FELINK_DEFAULT_TIMEOUT  100
#define FELINK_DEFAULT_MAXRET   4

#define FELINK_uECC_CURVE   uECC_secp160r1()

#define FELINK_uECC_CURVE_SIZE      (uECC_curve_public_key_size(FELINK_uECC_CURVE) / 2)
#define FELINK_uECC_PRI_KEY_SIZE    (uECC_curve_private_key_size(FELINK_uECC_CURVE))
#define FELINK_uECC_PUB_KEY_SIZE    (uECC_curve_public_key_size(FELINK_uECC_CURVE))

#endif // !_FELINK_DEV_USERCON
