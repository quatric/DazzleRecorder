/* "Stop after" times for the Mac window: reading what a person typed and naming a number of minutes.
 * (The same code as app_stop_after_label / app_parse_stop_after in app_core.c, which this window does not link.) */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SA_MAX 5999
#define SA_PRESETS 9
static const int sa_preset_minutes[SA_PRESETS] = { 0, 5, 10, 15, 30, 60, 90, 120, 180 };
static const char *const sa_preset_names[SA_PRESETS] = { "No limit", "5 min", "10 min", "15 min", "30 min",
                                                         "1 hour", "1.5 hours", "2 hours", "3 hours" };

static int sa_parse(const char *text)
{
    char buf[64];
    size_t n = 0;
    for (; text && *text && n < sizeof(buf) - 1; text++)
        if (*text != ' ' && *text != '\t')
            buf[n++] = (char)(*text >= 'A' && *text <= 'Z' ? *text + 32 : *text);
    buf[n] = 0;
    if (!n)
        return -1;
    if (!strcmp(buf, "nolimit") || !strcmp(buf, "none") || !strcmp(buf, "off"))
        return 0;
    char *end;
    double v = strtod(buf, &end);
    if (end == buf || v < 0)
        return -1;
    double minutes;
    if (*end == ':') { /* h:mm */
        char *end2;
        long m = strtol(end + 1, &end2, 10);
        if (end2 == end + 1 || m < 0 || m > 59 || *end2)
            return -1;
        minutes = (long)v * 60.0 + (double)m;
    } else {
        const char *rest = end;
        while (*rest >= 'a' && *rest <= 'z')
            rest++;
        size_t ul = (size_t)(rest - end);
        char unit[16];
        if (ul >= sizeof(unit))
            return -1;
        memcpy(unit, end, ul);
        unit[ul] = 0;
        if (!*unit || !strcmp(unit, "m") || !strcmp(unit, "min") || !strcmp(unit, "mins") || !strcmp(unit, "minute") ||
            !strcmp(unit, "minutes")) {
            minutes = v;
            if (*rest)
                return -1;
        } else if (!strcmp(unit, "h") || !strcmp(unit, "hr") || !strcmp(unit, "hrs") || !strcmp(unit, "hour") ||
                   !strcmp(unit, "hours")) {
            minutes = v * 60.0;
            if (*rest) { /* "2h15", "2h15min" */
                char *end2;
                long m = strtol(rest, &end2, 10);
                if (end2 == rest || m < 0 || m > 59)
                    return -1;
                if (strcmp(end2, "") && strcmp(end2, "m") && strcmp(end2, "min") && strcmp(end2, "mins"))
                    return -1;
                minutes += (double)m;
            }
        } else {
            return -1;
        }
    }
    minutes += 0.5;
    if (minutes > SA_MAX)
        return SA_MAX;
    return (int)minutes;
}

static void sa_label(int minutes, char *buf, int buflen)
{
    int i;
    for (i = 0; i < SA_PRESETS; i++)
        if (sa_preset_minutes[i] == minutes) {
            snprintf(buf, (size_t)buflen, "%s", sa_preset_names[i]);
            return;
        }
    if (minutes < 60)
        snprintf(buf, (size_t)buflen, "%d min", minutes);
    else if (minutes % 60 == 0)
        snprintf(buf, (size_t)buflen, "%d h", minutes / 60);
    else
        snprintf(buf, (size_t)buflen, "%d h %d min", minutes / 60, minutes % 60);
}

static int sa_from_index(int index)
{
    return index >= 0 && index < SA_PRESETS ? sa_preset_minutes[index] : 0;
}
