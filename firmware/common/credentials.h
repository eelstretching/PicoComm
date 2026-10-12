// Show network credentials for the bench firmware: compiled in with -DWIFI_SSID=... or asked for
// over USB serial at startup. Defines WIFI_SSID/WIFI_PASSWORD come from firmware/CMakeLists.txt.

#pragma once

#include <cstdio>
#include <cstring>

#include "pico/stdlib.h"

namespace bench {

inline char ssid[33];
inline char password[65];

// Reads a line from USB stdio with simple editing. Echoes '*' for each character when hidden.
inline void readLine(const char *prompt, char *buf, size_t size, bool hidden) {
    printf("%s", prompt);
    fflush(stdout);
    size_t len = 0;
    while (true) {
        int c = getchar_timeout_us(100 * 1000);
        if (c == PICO_ERROR_TIMEOUT) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (len == 0 && !hidden) {
                continue;
            }
            break;
        }
        if (c == 0x7f || c == '\b') {
            if (len > 0) {
                len--;
                printf("\b \b");
            }
        } else if (c >= 0x20 && c < 0x7f && len + 1 < size) {
            buf[len++] = (char)c;
            putchar(hidden ? '*' : c);
        }
        fflush(stdout);
    }
    buf[len] = '\0';
    printf("\n");
}

inline void getCredentials() {
    if (strlen(WIFI_SSID) > 0) {
        snprintf(ssid, sizeof(ssid), "%s", WIFI_SSID);
        snprintf(password, sizeof(password), "%s", WIFI_PASSWORD);
        sleep_ms(2000);  // give a serial terminal a moment to attach, but don't wait for one
        printf("Using compiled-in network \"%s\"\n", ssid);
        return;
    }
    while (!stdio_usb_connected()) {
        sleep_ms(100);
    }
    readLine("Show network SSID: ", ssid, sizeof(ssid), false);
    readLine("Password (blank for an open network): ", password, sizeof(password), true);
}

}  // namespace bench
