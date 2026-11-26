// A look at the console's own error reports (<folder>/erpt): on a Switch 2 a fault ends the program at once and nothing of it
// reaches the program (no exception is handed to it), but the system writes a report of every program it closes "because of an
// error", with where it was (the registers, the callers). This asks the reports' service for its list and copies the reports to
// <folder>/reports/, for tools/bench to read. It only reads: nothing of the system's is changed or removed.
// (Whether a homebrew program is let open the service at all is the first thing this says.)
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>

#include "nx2.h"

// The console's own record of a program it closed for an error between two moments, in words: what tells a run that ended in a
// fault from one the user closed, when neither left a word (log.c). Of the newest reports, the newest one of a program's fault
// (their codes begin 2168-) written from `from` to `to_time` (the console's clock, seconds; 0: no bound); only its time, which
// the list has, and its code, the first thing in a report, are read: nothing else of a report is copied anywhere.
// 1: there is one, and `to` says which; 0: none; -1: the reports can't be asked.
#ifdef __SWITCH2__
int nx2_erpt_last(char *to, size_t size, s64 from, s64 to_time) {
    Service r, mgr;
    int found = -1;
    if (size) to[0] = 0;
    if (R_FAILED(smGetService(&r, "erpt:r"))) return -1;
    if (R_FAILED(serviceDispatch(&r, 1, .out_num_objects = 1, .out_objects = &mgr))) {
        serviceClose(&r);
        return -1;
    }
    static u8 list[0x15E8];
    const u32 type = 2;
    if (R_SUCCEEDED(serviceDispatchIn(&mgr, 0, type, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out}, .buffers = {{list, sizeof list}}))) {
        const u32 count = *(const u32 *)list;
        found = 0;
        for (u32 k = 0; k < count && k < 12 && !to[0]; k++) {
            s64 when = 0;  // (when the report was written, by the clock the user sets: the newest come first)
            memcpy(&when, list + 8 + k * 0x70 + 0x40, 8);
            if (from && when < from - 5) break;
            if (to_time && when > to_time + 90) continue;
            Service rep;
            if (R_FAILED(serviceDispatch(&r, 0, .out_num_objects = 1, .out_objects = &rep))) break;
            struct {
                u8 id[20];
            } in;
            memcpy(in.id, list + 8 + k * 0x70 + 4, 20);
            static u8 head[0x400];
            u32 got = 0;
            if (R_SUCCEEDED(serviceDispatchIn(&rep, 0, in)) &&
                R_SUCCEEDED(serviceDispatchOut(&rep, 1, got, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out}, .buffers = {{head, sizeof head}}))) {
                for (u32 at = 0; at + 24 < got; at++) {
                    if (memcmp(head + at, "ErrorCode", 9) || (head[at + 9] & 0xE0) != 0xA0) continue;
                    const u32 len = head[at + 9] & 31;
                    char code[32] = {0};
                    memcpy(code, head + at + 10, len < 31 ? len : 31);
                    if (strncmp(code, "2168-", 5)) break;
                    const char *what = !strcmp(code, "2168-0000") ? "an instruction the processor doesn't know" : !strcmp(code, "2168-0001") ? "a jump to a place with no code (instruction abort)"
                                     : !strcmp(code, "2168-0002") ? "memory read or written where there is none (data abort)" : !strcmp(code, "2168-0003") ? "a misaligned access"
                                     : !strcmp(code, "2168-0006") ? "the program ended itself (a break)" : !strcmp(code, "2168-0008") ? "a system call the kernel didn't allow"
                                     : "a fault";
                    snprintf(to, size, "error %s: %s", code, what);
                    found = 1;
                    break;
                }
            }
            serviceDispatch(&rep, 4);
            serviceClose(&rep);
        }
    }
    serviceClose(&mgr);
    serviceClose(&r);
    return found;
}
#endif

void nx2_erpt_look(void) {
    static const char *const names[] = {"erpt:r", "erpt:c", "fatal:u"};  // (the other two: only to know what is open to this program)
    Service s[3];
    Result rc[3];
    for (int k = 0; k < 3; k++) {
        rc[k] = smGetService(&s[k], names[k]);
        nx2_log("[erpt] the service %s: %s (%x)\n", names[k], R_SUCCEEDED(rc[k]) ? "opened" : "not opened", (u64)rc[k]);
        if (k && R_SUCCEEDED(rc[k])) serviceClose(&s[k]);
    }
    nx2_log_flush();
    if (R_FAILED(rc[0])) return;
    Service mgr;
    Result r = serviceDispatch(&s[0], 1, .out_num_objects = 1, .out_objects = &mgr);
    nx2_log("[erpt] its manager: %x\n", (u64)r);
    if (R_FAILED(r)) return;
    static u8 list[0x4000];
    static const u32 sizes[] = {0x15E8, 0x15E0, 0x15E4, 0x2000, 0x4000};  // (the list's size is checked by the service: a Switch's first)
    u32 size = 0;
    for (unsigned k = 0; k < sizeof sizes / sizeof *sizes; k++) {
        const u32 type = 2;  // (any kind)
        memset(list, 0, sizeof list);
        r = serviceDispatchIn(&mgr, 0, type, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out}, .buffers = {{list, sizes[k]}});
        nx2_log("[erpt] the list, asked with %x bytes: %x\n", (u64)sizes[k], (u64)r);
        if (R_SUCCEEDED(r)) {
            size = sizes[k];
            break;
        }
    }
    nx2_log_flush();
    if (!size) return;
    mkdir(NX2_F("reports"), 0777);
    FILE *f = fopen(NX2_F("reports/list.bin"), "wb");
    if (f) fwrite(list, 1, size, f), fclose(f);
    const u32 count = *(const u32 *)list;
    nx2_log("[erpt] %u reports in the list\n", (u64)count);
    static u8 buf[0x10000];
    for (u32 k = 0; k < count && k < 50; k++) {
        const u8 *info = list + 8 + k * 0x70;
        Service rep;
        r = serviceDispatch(&s[0], 0, .out_num_objects = 1, .out_objects = &rep);
        if (R_FAILED(r)) {
            nx2_log("[erpt] report %u: not made (%x)\n", (u64)k, (u64)r);
            break;
        }
        struct {
            u8 id[20];
        } in;
        memcpy(in.id, info + 4, 20);
        r = serviceDispatchIn(&rep, 0, in);
        s64 length = 0;
        const Result r2 = R_SUCCEEDED(r) ? serviceDispatchOut(&rep, 5, length) : r;
        nx2_log("[erpt] report %u: opened %x, its size %x: %d bytes\n", (u64)k, (u64)r, (u64)r2, length);
        if (R_SUCCEEDED(r)) {
            char name[64];
            snprintf(name, sizeof name, NX2_F("reports/%02u.bin"), (unsigned)k);
            f = fopen(name, "wb");
            for (s64 done = 0; f && done < 0x100000;) {
                u32 got = 0;
                const Result r3 = serviceDispatchOut(&rep, 1, got, .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out}, .buffers = {{buf, sizeof buf}});
                if (R_FAILED(r3) || !got) break;
                fwrite(buf, 1, got, f);
                done += got;
            }
            if (f) fclose(f);
            serviceDispatch(&rep, 4);
        }
        serviceClose(&rep);
    }
    serviceClose(&mgr);
    serviceClose(&s[0]);
    nx2_log_flush();
}
