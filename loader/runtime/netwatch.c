// Where each of the game's threads is, sent to a computer as the game runs (<folder>/netwatch). A Switch 2 ends a program at a
// fault with no word to it, and the log, written to the card, says only what was known some moments before. So: this port listens
// on a TCP port, a computer connects (tools/bench/netwatch.py), and from then on a thread of the port's stops each of the game's
// threads for an instant, a few hundred times a second, and sends where it was: its program counter, link register and stack
// pointer. What arrives last is the nearest there is to where the game ended.
#ifdef __SWITCH2__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <switch.h>
#include <unistd.h>

#include "nx2.h"
void nx2_log(const char *fmt, ...);
extern u32 nx2_threads[96];
extern u32 nx2_main_handle;

static Thread net_thread;
static int net_peer = -1;

static void net_watch(void *arg) {
    (void)arg;
    static u8 packet[8 + 97 * (26 + 29 * 13 + 1 + 12 * 8)];
    static u32 ring_out;
    for (u32 seq = 0;; seq++) {
        // (the driver's calls begun and ended since the last look, first and at once: number 0xFFFFFFFF, how many, 40 bytes each)
        for (int round = 0; round < 8; round++) {
            const u32 in = __atomic_load_n(&nx2_nvn_ring_in, __ATOMIC_RELAXED);
            if (in - ring_out > NX2_NVN_RING) ring_out = in - NX2_NVN_RING;
            u32 n = in - ring_out;
            if (n > 1024) n = 1024;
            if (n) {
                static u8 marks[8 + 1024 * 40];
                const u32 kind = 0xFFFFFFFFu;
                memcpy(marks, &kind, 4), memcpy(marks + 4, &n, 4);
                for (u32 k = 0; k < n; k++) memcpy(marks + 8 + 40 * k, &nx2_nvn_ring[(ring_out + k) % NX2_NVN_RING], 40);
                ring_out += n;
                const u8 *at = marks;
                for (size_t left = 8 + 40 * (size_t)n; left;) {
                    const ssize_t sent = send(net_peer, at, left, 0);
                    if (sent <= 0) return;
                    at += sent, left -= (size_t)sent;
                }
            }
            if (round < 7) svcSleepThread(200000);
        }
        u32 count = 0;
        u8 *p = packet + 8;
        for (int k = -1; k < 96; k++) {
            const u32 h = k < 0 ? nx2_main_handle : nx2_threads[k];
            if (!h || R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) continue;
            ThreadContext c;
            const Result rc = svcGetThreadContext3(&c, h);
            svcSetThreadActivity(h, ThreadActivity_Runnable);
            if (R_FAILED(rc)) continue;
            // (a thread in the game's own code: its registers too, and of each whether it is an address that can be read or written
            // and how far the memory it is in goes on from there)
            const char *module = "?";
            u64 off = 0;
            // (2: not in the game's code but not merely waiting either: in the port's, for the game; its callers are sent too)
            int more = nx2_where(c.pc.x, &module, &off);
            if (!more && nx2_where(c.lr, &module, &off) == 0 && c.lr >= 0x8000000 && c.lr < 0x30000000) {
                extern char nx2_svc_entry[];
                const u64 lr = c.lr;
                if (lr < (u64)nx2_svc_entry || lr > (u64)nx2_svc_entry + 0x200) more = 2;
            }
            p[0] = (u8)(k + 1), p[1] = (u8)more;
            memcpy(p + 2, &c.pc.x, 8), memcpy(p + 10, &c.lr, 8), memcpy(p + 18, &c.sp, 8);
            p += 26, count++;
            for (int g = 0; more == 1 && g < 29; g++) {
                const u64 v = c.cpu_gprs[g].x;
                MemoryInfo mi;
                u32 pi;
                u8 perm = 0xFF;
                u32 left = 0;
                if (v >= 0x10000 && v < (1ull << 39) && R_SUCCEEDED(svcQueryMemory(&mi, &pi, v))) {
                    perm = (u8)mi.perm;
                    const u64 room = mi.addr + mi.size - v;
                    left = room > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)room;
                }
                memcpy(p, &v, 8), p[8] = perm, memcpy(p + 9, &left, 4);
                p += 13;
            }
            if (more) {  // (who called, by the frames, each looked at only if it is memory that can be read)
                u8 *n = p++;
                *n = 0;
                u64 frame = c.fp;
                for (int f = 0; f < 12 && frame && !(frame & 15); f++) {
                    MemoryInfo mi;
                    u32 pi;
                    if (R_FAILED(svcQueryMemory(&mi, &pi, frame)) || !(mi.perm & Perm_R) || frame + 16 > mi.addr + mi.size) break;
                    const u64 up = ((const u64 *)frame)[0], from = ((const u64 *)frame)[1];
                    memcpy(p, &from, 8), p += 8, (*n)++;
                    if (up <= frame) break;
                    frame = up;
                }
            }
        }
        memcpy(packet, &seq, 4), memcpy(packet + 4, &count, 4);
        const u8 *at = packet;
        for (size_t left = (size_t)(p - packet); left;) {
            const ssize_t n = send(net_peer, at, left, 0);
            if (n <= 0) return;
            at += n, left -= (size_t)n;
        }
    }
}

void nx2_net_watch(void) {
    FILE *f = fopen(NX2_F("netwatch"), "rb");
    if (!f) return;
    fclose(f);
    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        nx2_log("[net] the sockets couldn't be started (%x): no one is told where the threads are\n", (u64)rc);
        return;
    }
    const int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET, a.sin_port = htons(28400), a.sin_addr.s_addr = INADDR_ANY;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 1)) {
        nx2_log("[net] the port 28400 couldn't be listened on\n");
        return;
    }
    nx2_log("[net] waiting for a computer to connect to port 28400 (tools/bench/netwatch.py)\n");
    net_peer = accept(s, NULL, NULL);
    if (net_peer < 0) {
        nx2_log("[net] no one connected\n");
        return;
    }
    int nodelay = 1;
    setsockopt(net_peer, IPPROTO_TCP, 1 /* TCP_NODELAY */, &nodelay, sizeof nodelay);
    nx2_log("[net] connected: where the game's threads are is sent from now on\n");
    {
        extern int nx2_net_on;  // (loader.c: every call between modules goes in the ring too)
        nx2_net_on = 1;
    }
    if (R_SUCCEEDED(threadCreate(&net_thread, net_watch, NULL, NULL, 0x8000, 0x2B, -2))) {
        svcSetThreadCoreMask(net_thread.handle, 3, 0x38);  // (cores 3 to 5: not the game's first three)
        threadStart(&net_thread);
    }
}
#endif
