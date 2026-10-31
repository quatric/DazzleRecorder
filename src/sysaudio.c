/* Device name matching shared by the system audio backends. */
#include "sysaudio.h"

#include <stdlib.h>

static int contains_nocase(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle)
        return 0;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n) {
            char a = *h, b = *n;
            if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
            if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
            if (a != b)
                break;
            h++, n++;
        }
        if (!*n)
            return 1;
    }
    return 0;
}

int sa_name_matches(const char *name, const char *product)
{
    const char *env = getenv("DVC100_AUDIO_DEVICE");
    if (env && *env)
        return contains_nocase(name, env);
    return contains_nocase(name, product) || contains_nocase(name, "DAZZLE") ||
           contains_nocase(name, "ROXIO");
}
