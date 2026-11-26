// The game's own name, version and icon, asked of the console (<folder>/control): for a port made from a dump that doesn't have
// them. A Switch game's NSP has its control data, and tools/nsp.py reads it; a Switch 2 game's NUP container of the newer kind has
// them too; but the older kind of container and a folder of loose files have only the program and its files, and the port then had
// the dump's file name and no icon in the homebrew menu. The console keeps these for every game it has (the same data its own menu
// shows), and gives them to a program that asks by the game's number.
//
// The file <folder>/control holds that number, sixteen hexadecimal digits, written by tools/control.py; with it there, this run
// does nothing but ask, write what it is given to <folder>/control-<number>.bin (the control data's 0x4000 bytes, then the icon, a
// JPEG) and go back to where it was started from. tools/control.py fetches the file and the port is built again with what it says.
// It only reads: nothing of the console's is changed.
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "nx2.h"

void nx2_control_look(void) {
    char text[32] = {0};
    FILE *f = fopen(NX2_F("control"), "rb");
    if (!f) return;
    const size_t got = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    u64 id = 0;
    for (size_t k = 0; k < got; k++) {
        const char c = text[k];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break;
        id = id << 4 | (u64)d;
    }
    Result rc = nsInitialize();
    nx2_log("[control] the game %x's name, version and icon asked of the console; its service for them: %x\n", id, (u64)rc);
    if (R_FAILED(rc) || !id) {
        nx2_log_flush();
        return;
    }
    // (room for more than a Switch's: with that much, 0x24000 bytes, a Switch 2 said 5010 for a game it has, 2026-11-25)
    static union {
        NsApplicationControlData d;
        u8 room[0x4000 + (4u << 20)];
    } all;
#define data all.d
    u64 size = 0;
    u32 unknown = 0;
    // (the newer request first, and of it the Switch 2 edition's data before the plain: a Switch 2 game's is that one)
    rc = nsGetApplicationControlData2(NsApplicationControlSource_Storage, id, &data, sizeof all, 0, 1, &size, &unknown);
    nx2_log("[control] the newer request, the Switch 2 edition's data: %x, %u bytes\n", (u64)rc, size);
    if (R_FAILED(rc) || size <= sizeof data.nacp) {
        size = 0;
        rc = nsGetApplicationControlData2(NsApplicationControlSource_Storage, id, &data, sizeof all, 0, 0, &size, &unknown);
        nx2_log("[control] the newer request, the plain data: %x, %u bytes\n", (u64)rc, size);
    }
    if (R_FAILED(rc) || size <= sizeof data.nacp) {
        size = 0;
        rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, id, &data, sizeof all, &size);
        nx2_log("[control] the older request: %x, %u bytes\n", (u64)rc, size);
    }
    nsExit();
    if (R_SUCCEEDED(rc) && size >= sizeof data.nacp && size <= sizeof all) {
        char name[64];
        snprintf(name, sizeof name, NX2_F("control-%016llx.bin"), (unsigned long long)id);
        f = fopen(name, "wb");
        if (f) {
            fwrite(&data, 1, size, f);
            fclose(f);
            nx2_log("[control] written to %s: %u bytes (the icon is %u of them)\n", name, size, size - sizeof data.nacp);
        }
    } else {
        nx2_log("[control] the console gave nothing for that number: is the game on this console?\n");
    }
    nx2_log_flush();
}
