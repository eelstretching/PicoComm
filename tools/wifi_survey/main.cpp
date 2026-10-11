// WiFi site survey for the show network.
//
// Carry a Pico 2 W around the lawn on a USB cable from a laptop and open its serial port. Once a
// second it prints the signal strength (RSSI) of the access point it's joined to, a quality label,
// the min/avg RSSI over the last 10 seconds, and round-trip times of ICMP pings to the gateway (the
// show router), four pings a second with a 32-byte payload, the same size as a PicoComm packet.
//
// Press Enter at a prop to drop a numbered marker with a summary of the last 10 seconds, so a log
// of the walk can be read back afterwards.
//
// WiFi power saving is turned off, as PicoComm will run it (see docs/protocol-design.md). Ping
// times with it left on would be dominated by the radio's beacon-interval naps.

#include <cstdio>
#include <cstring>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/prot/ip4.h"
#include "lwip/raw.h"

namespace {

constexpr uint32_t kReportIntervalMs = 1000;
constexpr uint32_t kPingIntervalMs = 250;
constexpr uint64_t kPingTimeoutUs = 1000 * 1000;
constexpr uint16_t kPingId = 0x5053;  // "PS"
constexpr size_t kPingPayload = 32;
constexpr int kWindowSeconds = 10;
constexpr int kHeaderEvery = 20;

//
// Credentials: compiled in with -DWIFI_SSID=... -DWIFI_PASSWORD=..., or typed in at startup.
//

char ssid[33];
char password[65];

// Reads a line from USB stdio with simple editing. Echoes '*' for each character when hidden.
void readLine(const char *prompt, char *buf, size_t size, bool hidden) {
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
                continue;  // swallow the \n of a \r\n pair, and stray blank lines
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

void getCredentials() {
    if (strlen(WIFI_SSID) > 0) {
        snprintf(ssid, sizeof(ssid), "%s", WIFI_SSID);
        snprintf(password, sizeof(password), "%s", WIFI_PASSWORD);
        printf("Using compiled-in network \"%s\"\n", ssid);
        return;
    }
    readLine("Show network SSID: ", ssid, sizeof(ssid), false);
    readLine("Password (blank for an open network): ", password, sizeof(password), true);
}

//
// Startup scan: what else is on the air, and which channel the show network is on.
//

struct ScanEntry {
    uint8_t bssid[6];
    char ssid[33];
    uint16_t channel;
    int16_t rssi;
};

constexpr int kMaxScan = 40;
ScanEntry scanEntries[kMaxScan];
int scanCount = 0;

int scanCallback(void *, const cyw43_ev_scan_result_t *result) {
    if (result == nullptr) {
        return 0;
    }
    // The same access point is reported several times; keep its strongest reading.
    for (int i = 0; i < scanCount; i++) {
        if (memcmp(scanEntries[i].bssid, result->bssid, 6) == 0) {
            if (result->rssi > scanEntries[i].rssi) {
                scanEntries[i].rssi = result->rssi;
            }
            return 0;
        }
    }
    if (scanCount == kMaxScan) {
        return 0;
    }
    ScanEntry &e = scanEntries[scanCount++];
    memcpy(e.bssid, result->bssid, 6);
    size_t n = result->ssid_len < 32 ? result->ssid_len : 32;
    memcpy(e.ssid, result->ssid, n);
    e.ssid[n] = '\0';
    e.channel = result->channel;
    e.rssi = result->rssi;
    return 0;
}

void runScan() {
    printf("\nScanning...\n");
    scanCount = 0;
    cyw43_wifi_scan_options_t opts = {};
    if (cyw43_wifi_scan(&cyw43_state, &opts, nullptr, scanCallback) != 0) {
        printf("  scan failed to start\n");
        return;
    }
    while (cyw43_wifi_scan_active(&cyw43_state)) {
        sleep_ms(50);
    }

    // Strongest first.
    for (int i = 1; i < scanCount; i++) {
        ScanEntry e = scanEntries[i];
        int j = i - 1;
        while (j >= 0 && scanEntries[j].rssi < e.rssi) {
            scanEntries[j + 1] = scanEntries[j];
            j--;
        }
        scanEntries[j + 1] = e;
    }

    int showChannel = -1;
    printf("  %-32s  ch  RSSI  BSSID\n", "SSID");
    for (int i = 0; i < scanCount; i++) {
        const ScanEntry &e = scanEntries[i];
        bool mine = strcmp(e.ssid, ssid) == 0;
        if (mine && showChannel < 0) {
            showChannel = e.channel;
        }
        printf("%c %-32s  %2u  %4d  %02x:%02x:%02x:%02x:%02x:%02x\n", mine ? '>' : ' ',
               e.ssid[0] ? e.ssid : "(hidden)", e.channel, e.rssi, e.bssid[0], e.bssid[1], e.bssid[2],
               e.bssid[3], e.bssid[4], e.bssid[5]);
    }
    if (showChannel < 0) {
        printf("  \"%s\" not seen in the scan.\n", ssid);
        return;
    }
    // Channels 5 apart don't overlap at 20 MHz width; closer ones share airtime with us.
    int overlapping = 0;
    for (int i = 0; i < scanCount; i++) {
        int d = (int)scanEntries[i].channel - showChannel;
        if (strcmp(scanEntries[i].ssid, ssid) != 0 && d > -5 && d < 5) {
            overlapping++;
        }
    }
    printf("  \"%s\" is on channel %d; %d other network(s) on overlapping channels.\n", ssid,
           showChannel, overlapping);
}

//
// Connection.
//

bool connect() {
    uint32_t auth = strlen(password) > 0 ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;
    printf("\nConnecting to \"%s\"...\n", ssid);
    int err = cyw43_arch_wifi_connect_timeout_ms(ssid, strlen(password) > 0 ? password : nullptr,
                                                 auth, 20000);
    if (err != 0) {
        printf("  failed (error %d)\n", err);
        return false;
    }
    // Power saving off: the radio stays awake, so pings measure the network, not beacon naps.
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);

    cyw43_arch_lwip_begin();
    const netif *n = &cyw43_state.netif[CYW43_ITF_STA];
    char ip[16], gw[16];
    ip4addr_ntoa_r(netif_ip4_addr(n), ip, sizeof(ip));
    ip4addr_ntoa_r(netif_ip4_gw(n), gw, sizeof(gw));
    cyw43_arch_lwip_end();

    uint8_t bssid[6] = {};
    cyw43_wifi_get_bssid(&cyw43_state, bssid);
    printf("  connected: address %s, gateway %s, AP %02x:%02x:%02x:%02x:%02x:%02x\n", ip, gw,
           bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5]);
    return true;
}

bool linkUp() {
    return cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP;
}

//
// ICMP pings to the gateway. Replies arrive in lwIP's receive callback (background context); the
// main loop only touches the slots while holding the lwIP lock.
//

struct PingSlot {
    uint16_t seq;
    bool used;      // a ping was sent from this slot and hasn't been counted in a report yet
    bool replied;
    uint64_t sentUs;
    uint32_t rttUs;
};

constexpr int kPingSlots = 16;  // > pings in flight within the timeout
PingSlot pingSlots[kPingSlots];
uint16_t pingSeq = 0;
raw_pcb *pingPcb = nullptr;

uint8_t pingReceive(void *, raw_pcb *, pbuf *p, const ip_addr_t *) {
    uint64_t now = time_us_64();
    ip_hdr iph;
    if (pbuf_copy_partial(p, &iph, sizeof(iph), 0) != sizeof(iph)) {
        return 0;
    }
    icmp_echo_hdr echo;
    if (pbuf_copy_partial(p, &echo, sizeof(echo), IPH_HL_BYTES(&iph)) != sizeof(echo)) {
        return 0;
    }
    if (ICMPH_TYPE(&echo) != ICMP_ER || echo.id != PP_HTONS(kPingId)) {
        return 0;  // not ours: let lwIP have it
    }
    uint16_t seq = lwip_ntohs(echo.seqno);
    PingSlot &s = pingSlots[seq % kPingSlots];
    if (s.used && !s.replied && s.seq == seq && now - s.sentUs < kPingTimeoutUs) {
        s.replied = true;
        s.rttUs = (uint32_t)(now - s.sentUs);
    }
    pbuf_free(p);
    return 1;
}

void pingInit() {
    cyw43_arch_lwip_begin();
    pingPcb = raw_new(IP_PROTO_ICMP);
    raw_recv(pingPcb, pingReceive, nullptr);
    raw_bind(pingPcb, IP_ADDR_ANY);
    cyw43_arch_lwip_end();
}

void pingSend() {
    cyw43_arch_lwip_begin();
    const netif *n = &cyw43_state.netif[CYW43_ITF_STA];
    ip_addr_t gw;
    ip_addr_copy_from_ip4(gw, *netif_ip4_gw(n));
    PingSlot &s = pingSlots[pingSeq % kPingSlots];
    pbuf *p = pbuf_alloc(PBUF_IP, sizeof(icmp_echo_hdr) + kPingPayload, PBUF_RAM);
    if (p != nullptr && !s.used && !ip_addr_isany(&gw)) {
        auto *echo = static_cast<icmp_echo_hdr *>(p->payload);
        ICMPH_TYPE_SET(echo, ICMP_ECHO);
        ICMPH_CODE_SET(echo, 0);
        echo->chksum = 0;
        echo->id = PP_HTONS(kPingId);
        echo->seqno = lwip_htons(pingSeq);
        auto *data = reinterpret_cast<uint8_t *>(echo + 1);
        for (size_t i = 0; i < kPingPayload; i++) {
            data[i] = (uint8_t)i;
        }
        echo->chksum = inet_chksum(echo, p->len);

        s.seq = pingSeq;
        s.used = true;
        s.replied = false;
        s.sentUs = time_us_64();
        raw_sendto(pingPcb, p, &gw);
        pingSeq++;
    }
    if (p != nullptr) {
        pbuf_free(p);
    }
    cyw43_arch_lwip_end();
}

//
// Per-second results, kept for the last kWindowSeconds seconds.
//

struct Second {
    int32_t rssi;
    bool rssiValid;
    int ok;
    int lost;
    uint32_t rttMin;
    uint32_t rttMax;
    uint64_t rttSum;
};

Second window[kWindowSeconds];
int windowCount = 0;
int windowNext = 0;

// Counts every ping that has finished (replied, or timed out) since the last call. Each ping is
// counted exactly once.
void collectPings(Second &sec) {
    sec.ok = 0;
    sec.lost = 0;
    sec.rttMin = UINT32_MAX;
    sec.rttMax = 0;
    sec.rttSum = 0;
    uint64_t now = time_us_64();
    cyw43_arch_lwip_begin();
    for (PingSlot &s : pingSlots) {
        if (!s.used) {
            continue;
        }
        if (s.replied) {
            sec.ok++;
            sec.rttSum += s.rttUs;
            if (s.rttUs < sec.rttMin) sec.rttMin = s.rttUs;
            if (s.rttUs > sec.rttMax) sec.rttMax = s.rttUs;
            s.used = false;
        } else if (now - s.sentUs >= kPingTimeoutUs) {
            sec.lost++;
            s.used = false;
        }
    }
    cyw43_arch_lwip_end();
}

void clearPings() {
    cyw43_arch_lwip_begin();
    for (PingSlot &s : pingSlots) {
        s.used = false;
    }
    cyw43_arch_lwip_end();
}

const char *quality(int32_t rssi) {
    if (rssi >= -60) return "excellent";
    if (rssi >= -70) return "good";
    if (rssi >= -80) return "marginal";
    return "POOR";
}

void printHeader() {
    printf("\n  time   RSSI  quality    | last %ds RSSI min  avg | ping ok/sent  min   avg   max ms\n",
           kWindowSeconds);
}

void printPingStats(int ok, int lost, uint32_t rttMin, uint32_t rttMax, uint64_t rttSum) {
    printf("%3d/%-3d", ok, ok + lost);
    if (ok > 0) {
        printf("  %5.1f %5.1f %5.1f", rttMin / 1000.0, rttSum / 1000.0 / ok, rttMax / 1000.0);
    } else {
        printf("      -     -     -");
    }
}

void printSecond(uint32_t seconds, const Second &sec) {
    int32_t lo = 0;
    int64_t sum = 0;
    int n = 0;
    for (int i = 0; i < windowCount; i++) {
        if (window[i].rssiValid) {
            if (n == 0 || window[i].rssi < lo) lo = window[i].rssi;
            sum += window[i].rssi;
            n++;
        }
    }
    printf("%5lus  ", (unsigned long)seconds);
    if (sec.rssiValid) {
        printf("%4ld  %-9s  |", (long)sec.rssi, quality(sec.rssi));
    } else {
        printf("   ?  %-9s  |", "");
    }
    if (n > 0) {
        printf("            %4ld %5.1f | ", (long)lo, (double)sum / n);
    } else {
        printf("               -     - | ");
    }
    printf("     ");
    printPingStats(sec.ok, sec.lost, sec.rttMin, sec.rttMax, sec.rttSum);
    printf("\n");
}

void printMarker(int marker) {
    int32_t lo = 0, hi = 0;
    int64_t sum = 0;
    int n = 0, ok = 0, lost = 0;
    uint32_t rttMin = UINT32_MAX, rttMax = 0;
    uint64_t rttSum = 0;
    for (int i = 0; i < windowCount; i++) {
        const Second &s = window[i];
        if (s.rssiValid) {
            if (n == 0 || s.rssi < lo) lo = s.rssi;
            if (n == 0 || s.rssi > hi) hi = s.rssi;
            sum += s.rssi;
            n++;
        }
        ok += s.ok;
        lost += s.lost;
        rttSum += s.rttSum;
        if (s.ok > 0 && s.rttMin < rttMin) rttMin = s.rttMin;
        if (s.ok > 0 && s.rttMax > rttMax) rttMax = s.rttMax;
    }
    printf("\n=== MARK %d: last %d s ", marker, windowCount);
    if (n > 0) {
        double avg = (double)sum / n;
        printf("RSSI min %ld avg %.1f max %ld (%s)", (long)lo, avg, (long)hi,
               quality((int32_t)(avg - 0.5)));
    } else {
        printf("RSSI none");
    }
    printf(", ping ");
    printPingStats(ok, lost, rttMin, rttMax, rttSum);
    printf(" ms ===\n");
}

}  // namespace

int main() {
    stdio_init_all();

    // Nothing is worth printing until someone's listening.
    while (!stdio_usb_connected()) {
        sleep_ms(100);
    }
    sleep_ms(500);
    printf("\nPicoComm WiFi survey\n");

    if (cyw43_arch_init_with_country(CYW43_COUNTRY_USA) != 0) {
        printf("cyw43 init failed\n");
        while (true) {
            sleep_ms(1000);
        }
    }
    cyw43_arch_enable_sta_mode();

    getCredentials();
    runScan();
    while (!connect()) {
        sleep_ms(2000);
    }
    pingInit();

    printf("\nPress Enter to drop a marker at a prop. Quality: excellent >= -60, good >= -70,\n"
           "marginal >= -80 dBm, POOR below that.\n");

    int lines = 0;
    int marker = 0;
    uint32_t start = to_ms_since_boot(get_absolute_time());
    uint32_t nextReport = start + kReportIntervalMs;
    uint32_t nextPing = start;

    while (true) {
        uint32_t now = to_ms_since_boot(get_absolute_time());

        int c = getchar_timeout_us(0);
        if (c == '\r' || c == '\n') {
            printMarker(++marker);
            lines = 0;
        }

        if (!linkUp()) {
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
            printf("\n*** link down after %lus, reconnecting ***\n",
                   (unsigned long)((now - start) / 1000));
            clearPings();
            windowCount = 0;
            windowNext = 0;
            connect();
            lines = 0;
            now = to_ms_since_boot(get_absolute_time());
            nextReport = now + kReportIntervalMs;
            nextPing = now;
            continue;
        }

        if ((int32_t)(now - nextPing) >= 0) {
            pingSend();
            nextPing += kPingIntervalMs;
        }

        if ((int32_t)(now - nextReport) >= 0) {
            nextReport += kReportIntervalMs;

            Second sec = {};
            sec.rssiValid = cyw43_wifi_get_rssi(&cyw43_state, &sec.rssi) == 0;
            collectPings(sec);
            window[windowNext] = sec;
            windowNext = (windowNext + 1) % kWindowSeconds;
            if (windowCount < kWindowSeconds) {
                windowCount++;
            }

            if (lines++ % kHeaderEvery == 0) {
                printHeader();
            }
            printSecond((now - start) / 1000, sec);

            // Blink the onboard LED once per report so you can tell it's alive without the screen.
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
            sleep_ms(30);
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);
        }

        sleep_ms(5);
    }
}
