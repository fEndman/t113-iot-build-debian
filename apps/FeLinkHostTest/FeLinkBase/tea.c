#include "tea.h"
#include <string.h>

#define MX (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((sum ^ y) + (key[(p & 3) ^ e] ^ z))
#define DELTA 0x9e3779b9

int fl_xxtea_encrypt(uint32_t *plaintext, size_t dword_len, uint32_t *key)
{
    uint32_t *buf = (uint32_t *)plaintext;
    size_t n = dword_len - 1, p;
    uint32_t z, y, q = 6 + 52 / (n + 1), sum = 0, e;

    if (n < 1)
        return ENODATA;

    z = buf[n];
    while (0 < q--)
    {
        sum += DELTA;
        e = sum >> 2 & 3;

        for (p = 0; p < n; p++)
        {
            y = buf[p + 1];
            z = buf[p] += MX;
        }

        y = buf[0];
        z = buf[n] += MX;
    }

    return 0;
}

int fl_xxtea_decrypt(uint32_t *ciphetext, size_t dword_len, uint32_t *key)
{
    uint32_t *buf = (uint32_t *)ciphetext;
    size_t n = dword_len - 1, p;
    uint32_t z, y, q = 6 + 52 / (n + 1), sum = q * DELTA, e;

    if (n < 1)
        return ENODATA;

    y = buf[0];
    while (sum != 0)
    {
        e = sum >> 2 & 3;

        for (p = n; p > 0; p--)
        {
            z = buf[p - 1];
            y = buf[p] -= MX;
        }

        z = buf[n];
        y = buf[0] -= MX;
        sum -= DELTA;
    }

    return 0;
}

int fl_xxtea_byte_array_encrypt(uint8_t *plaintext, size_t byte_len, uint32_t *key)
{
    if (byte_len % 4 != 0)
        return ENODATA;

    size_t dword_len = byte_len / 4;
    //字节对齐
    uint32_t buf[dword_len];
    memcpy(buf, plaintext, byte_len);
    int res = fl_xxtea_encrypt(buf, dword_len, key);
    memcpy(plaintext, buf, byte_len);

    return res;
}

int fl_xxtea_byte_array_decrypt(uint8_t *ciphetext, size_t byte_len, uint32_t *key)
{
    if (byte_len % 4 != 0)
        return ENODATA;

    size_t dword_len = byte_len / 4;
    //字节对齐
    uint32_t buf[dword_len];
    memcpy(buf, ciphetext, byte_len);
    int res = fl_xxtea_decrypt(buf, dword_len, key);
    memcpy(ciphetext, buf, byte_len);

    return res;
}
