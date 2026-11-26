// A Switch 2's sound for a Switch game that plays through nn::audio's AudioOut: for now, none. The service behind it (audout:u) is
// refused a homebrew program on a Switch 2 (sm: result 1015; log, 2026-11-22), and the SDK ends the game when it can't have it. So
// the game's AudioOut imports come here: an output that takes the game's buffers and gives each back when it would have been played
// (so the game's mixing keeps its pace), and plays nothing. (The resampler functions stay the SDK's: they are arithmetic.)
// These run on the game's threads: no host library in them.
#ifdef __SWITCH2__
#include <switch.h>

#include <string.h>

#include "nx2.h"

// the SDK's AudioOutBuffer, as far as these functions use it (they are the only ones that look inside)
typedef struct Buffer {
    struct Buffer *next;
    void *data;
    u64 size, data_size;
    u64 due;  // (the SDK's has an offset here) the tick it has been played by
} Buffer;

static struct {
    u32 lock;  // (a spin lock: the host library's mutex names its owner by the thread's own record, which a game's thread hasn't)
    Buffer *first, *last;
    u64 end;  // the tick the queue is played by
    int rate, channels;  // what the game asked for when it opened the output (48000 and 2 where it asked for none)
    int started;
    void *event;  // the game's SystemEvent for released buffers, if it gave one
    u32 appended;
} out;
static void (*signal_event)(void *);

static void lock(void) {
    while (__atomic_exchange_n(&out.lock, 1, __ATOMIC_ACQUIRE)) svcSleepThread(0);
}
static void unlock(void) { __atomic_store_n(&out.lock, 0, __ATOMIC_RELEASE); }

static u64 ticks_of(u64 bytes) {  // 16 bit samples, of as many channels and at the rate the output was opened with
    const u64 frame = 2 * (u64)(out.channels > 0 ? out.channels : 2), rate = out.rate > 0 ? (u64)out.rate : 48000;
    return bytes / frame * armGetSystemTickFreq() / rate;
}
// The SDK's AudioOutParameter: the rate, then the channels, 0 in either for the output's own. A game that asks for six channels
// and is then told the output has two takes it for a fault of its own and ends (an assertion right after the output is opened;
// report, 2026-11-25): the silent output has what was asked for.
static void asked(const void *parameter) {
    const s32 rate = parameter ? *(const s32 *)parameter : 0;
    const u16 channels = parameter ? *(const u16 *)((const u8 *)parameter + 4) : 0;
    out.rate = rate > 0 ? rate : 48000;
    out.channels = channels >= 1 && channels <= 8 ? channels : 2;
}

static int list_outs(char *info, int count) {
    if (count < 1) return 0;
    memset(info, 0, 256);
    memcpy(info, "DeviceOut", 10);
    return 1;
}
static void init_parameter(u32 *p) { p[0] = 0, p[1] = 0; }
static u32 open_out(void *o) {
    (void)o;
    nx2_log("[audio] the game opens an audio output: a silent one (a Switch 2 refuses a homebrew program audout:u)\n");
    return 0;
}
static u32 open_named(void *o, const char *name, const void *p) {
    (void)name;
    asked(p);
    return open_out(o);
}
static u32 open_default(void *o, const void *p) {
    asked(p);
    return open_out(o);
}
static u32 open_default_event(void *o, void *event, const void *p) {
    asked(p);
    void (*create)(void *, int, int) = (void (*)(void *, int, int))nx2_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    signal_event = (void (*)(void *))nx2_symbol("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE");
    if (create && signal_event) create(event, 1, 0), out.event = event;  // (1: cleared by the wait that sees it)
    nx2_log("[audio] ... with an event for its buffers (%s)\n", out.event ? "made" : "the SDK has no CreateSystemEvent: not made");
    return open_out(o);
}
static u32 start_out(void *o) {
    (void)o;
    out.started = 1;
    return 0;
}
static void stop_out(void *o) {
    (void)o;
    out.started = 0;
}
static void close_out(void *o) { (void)o; }
static int sample_rate(const void *o) {
    (void)o;
    return out.rate > 0 ? out.rate : 48000;
}
static int channel_count(const void *o) {
    (void)o;
    return out.channels > 0 ? out.channels : 2;
}
static int sample_format(const void *o) {
    (void)o;
    return 2;  // 16 bit
}
static int out_state(const void *o) {
    (void)o;
    return out.started ? 0 : 1;
}
static const char *out_name(const void *o) {
    (void)o;
    return "DeviceOut";
}
static void set_buffer(Buffer *b, void *data, u64 size, u64 data_size) { b->next = NULL, b->data = data, b->size = size, b->data_size = data_size, b->due = 0; }
static void *buffer_pointer(const Buffer *b) { return b->data; }
static u64 buffer_size(const Buffer *b) { return b->data_size; }

static int append(void *o, Buffer *b) {
    (void)o;
    lock();
    const u64 now = armGetSystemTick();
    if (out.end < now) out.end = now;
    out.end += ticks_of(b->data_size);
    b->due = out.end, b->next = NULL;
    if (out.last) out.last->next = b;
    else out.first = b;
    out.last = b;
    if (out.appended++ < 3) nx2_log("[audio] a buffer of %u bytes appended\n", b->data_size);
    unlock();
    // (The event a game gives at the opening is made, and never signalled: a game destroys its own right after the opening
    // and asks for released buffers by itself; signalling the destroyed one ended the game. Log, 2026-11-22.)
    return 1;
}
static Buffer *released(void *o) {
    (void)o;
    lock();
    Buffer *b = out.first;
    if (b && armGetSystemTick() >= b->due) {
        out.first = b->next;
        if (!out.first) out.last = NULL;
    } else {
        b = NULL;
    }
    const int more = out.first != NULL;
    unlock();
    (void)more;
    return b;
}

// The sound that plays: a native Switch 2 program reaches the same audio output manager through another service (aud:u), which main.c
// opens. With it, the SDK's own AudioOut is left as it is, and its request for audout:u is answered with a session of that manager
// (svc.c); the silent output above is for when it couldn't be opened.
u32 nx2_audio_master;
u32 nx2_audren_master;  // the same for the audio renderer's manager (audren:u)

static void *silent_out(const char *name) {
    static const struct {
        const char *name;
        void *fn;
    } table[] = {
        {"_ZN2nn5audio13ListAudioOutsEPNS0_12AudioOutInfoEi", list_outs},
        {"_ZN2nn5audio27InitializeAudioOutParameterEPNS0_17AudioOutParameterE", init_parameter},
        {"_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPKcRKNS0_17AudioOutParameterE", open_named},
        {"_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutERKNS0_17AudioOutParameterE", open_default},
        {"_ZN2nn5audio19OpenDefaultAudioOutEPNS0_8AudioOutEPNS_2os11SystemEventERKNS0_17AudioOutParameterE", open_default_event},
        {"_ZN2nn5audio13StartAudioOutEPNS0_8AudioOutE", start_out},
        {"_ZN2nn5audio12StopAudioOutEPNS0_8AudioOutE", stop_out},
        {"_ZN2nn5audio13CloseAudioOutEPNS0_8AudioOutE", close_out},
        {"_ZN2nn5audio21GetAudioOutSampleRateEPKNS0_8AudioOutE", sample_rate},
        {"_ZN2nn5audio23GetAudioOutChannelCountEPKNS0_8AudioOutE", channel_count},
        {"_ZN2nn5audio23GetAudioOutSampleFormatEPKNS0_8AudioOutE", sample_format},
        {"_ZN2nn5audio16GetAudioOutStateEPKNS0_8AudioOutE", out_state},
        {"_ZN2nn5audio15GetAudioOutNameEPKNS0_8AudioOutE", out_name},
        {"_ZN2nn5audio21SetAudioOutBufferInfoEPNS0_14AudioOutBufferEPvmm", set_buffer},
        {"_ZN2nn5audio28GetAudioOutBufferDataPointerEPKNS0_14AudioOutBufferE", buffer_pointer},
        {"_ZN2nn5audio25GetAudioOutBufferDataSizeEPKNS0_14AudioOutBufferE", buffer_size},
        {"_ZN2nn5audio20AppendAudioOutBufferEPNS0_8AudioOutEPNS0_14AudioOutBufferE", append},
        {"_ZN2nn5audio25GetReleasedAudioOutBufferEPNS0_8AudioOutE", released},
    };
    for (u32 k = 0; k < sizeof table / sizeof *table; k++)
        if (!strcmp(table[k].name, name)) return table[k].fn;
    return NULL;
}
void *nx2_audio_override(const char *name) {
    return nx2_audio_master ? NULL : silent_out(name);
}
// (is this one of the SDK's AudioOut functions, the ones the silent output stands in for: gl2.c says so once in the log)
int nx2_audio_out_name(const char *name) {
    return silent_out(name) != NULL;
}
#endif
