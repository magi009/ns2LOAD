// A Switch 2's graphics for a Switch game that draws with NVN, as far as a game needs it. NVN is the Switch's
// own graphics API: the game asks for each of its functions by name (nvnBootstrapLoader, then nvnDeviceGetProcAddress) and the SDK's
// answer is a driver that speaks to the Switch's GPU through a service a Switch 2 refuses a homebrew program. So the names are
// answered here instead, with functions that do the same work through the host's OpenGL ES (ounce-devkit's, over Vulkan).
//
// What makes that possible for this game, and not yet for most: it uses 105 of NVN's functions, the plain ones (measured, 2026-11-23),
// and it compiles its shaders from GLSL text it carries, at run time, with the compiler in one of its modules (glslcCompile). That
// call is watched for the text, which, changed a little (below), is what the host's OpenGL is given. A game whose shaders come already
// compiled for the Switch's GPU has nothing here to draw with.
//
// How it is laid out. NVN's functions only write things down: they are called on the game's thread, where no host library may run
// (the thread-local region is the SDK's). A command buffer's commands go into a list; nvnQueueSubmitCommands hands the list to one
// function that runs as the host (nx2_host_call) and does all the OpenGL. NVN's objects are the game's memory, of sizes this code
// doesn't know for certain, so little is kept in them: a number that names a record here, or, for the small state objects, their
// few values packed in the 4 or 8 bytes they are known to have.
// Buffers are memory the game gave (a memory pool): an NVN "buffer address" is, here, the pointer itself. Their contents are read
// when the commands run, which is when the GPU would have read them.
#ifdef __SWITCH2__
#include <switch.h>

#include <EGL/egl.h>
#include <GLES3/gl32.h>
// The port's own OpenGL over Vulkan (glvk.c), used in the host's stead when <folder>/vulkan is on the card: every gl function
// called below goes to it then (glvk_route.h), and the few things OpenGL has no word for are said to it by name.
#include "glvk_route.h"
void glvk_start(int w, int h, const char *pack);
void glvk_show(void);
void glvk_flip(int on);
void glvk_depth_zero(int on);
u32 glvk_program(u64 vs_key, u64 fs_key, u32 packed, u32 need[32], u8 units[24][2], u32 unit_kind[24], int *nunits, u32 *storage, u16 *attrs);
void vk_glTextureView(GLuint id, GLenum target, GLuint of, GLenum internal, GLuint level0, GLuint levels, GLuint layer0, GLuint layers);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nx2.h"

#ifndef NX2_NAME
#define NX2_NAME "game"
#endif

#define SAY(n, ...) \
    do { \
        static u32 said_; \
        if (said_ < (n)) said_++, nx2_log(__VA_ARGS__); \
    } while (0)

// ---------------------------------------------------------------- the records
#define NVN_MAGIC 0x324E564Eu  // "NVN2"
typedef struct {
    u8 *mem;
    u64 size;
    u32 flags;
} PoolBuilder;
typedef struct {
    u32 magic, flags;
    u8 *mem;
    u64 size;
} Pool;
typedef struct {
    Pool *pool;
    s64 offset;
    u64 size;
} BufferBuilder;
typedef struct {
    u8 *ptr;
    u64 size;
} Buffer;
typedef struct {
    u32 flags, format, target;
    s32 w, h;
    Pool *pool;
    s64 offset, stride;
    s32 depth, levels;
    s32 block;  // the first level's block height (its log2) as a packaged texture says it, or -1: worked out from the size
    u8 swizzle[4];  // which channel each of red, green, blue and alpha is read from (NVN's numbers: 0 zero, 1 one, 2 to 5 the four)
} TextureBuilder;
typedef struct {
    u32 magic, id;
} Named;  // a texture, a sampler, a program, a command buffer: the place of its record
typedef struct {
    u32 min, mag, ws, wt, wr;
    u32 compare, compare_func;  // (1: the texture's depth is compared with a number, by this comparison; shadows)
    float lod_min, lod_max, lod_bias, border[4];
} SamplerBuilder;
typedef struct {
    u32 count, pad;
    Named *tex[4];
} WindowBuilder;
typedef struct {
    u32 magic, count, tex[4], current;
} Window;

enum { F_RGBA8 = 37, F_DEPTH16 = 50, F_DEPTH24 = 51, F_DEPTH32F = 52, F_DEPTH24_STENCIL8 = 53, F_DEPTH32F_STENCIL8 = 54 };
enum { TEX_DISPLAY = 1, TEX_LINEAR = 0x10 };

// (A game has thousands at once: with room for 4096, every one after those was refused, 5207 of them, and what was drawn
// had no texture; log, 2026-11-23)
#ifdef NX2_GAME_MB  // (a Switch 2 game: never used, see ARENA)
#define TEXTURES 16
#else
#define TEXTURES 32768
#endif
typedef struct {
    int used, w, h, format, flags, window;
    int depth, levels, target;
    int renew;     // the record has been given to another texture: the OpenGL one it had is to go
    u8 *mem;       // where its texels are, in the game's memory pool (a linear texture's rows, stride apart)
    s64 stride;
    volatile int dirty;  // the game has written to its memory since it was last given to OpenGL
    u32 gl;
    u32 gl_target;  // what kind OpenGL has it as (2D, cube, array...)
    int block;      // the first level's block height (log2), for a texture laid out in blocks
    u64 room;       // how much of the game's pool lies from mem on: nothing is read past it
    const Pool *pool;  // the pool its texels are in, and where: the pool's memory may be given it after the texture is made
    u64 offset;
    int drawn_to;   // it has been a render target: its texels are OpenGL's own, the game's memory isn't read
    u8 swizzle[4];  // which channel each of its four is read from
    volatile int swizzle_new;  // it has changed since OpenGL was told
    volatile int written;      // texels given by nvnTextureWriteTexels wait to be handed to OpenGL (texels_written)
    u32 gl_internal, gl_levels;  // the format and the count of levels OpenGL has it with
    u32 view_gl, view_kind, view_base;  // the same texels seen as another kind of texture (texture_as): OpenGL's, its kind, of which texture
    u32 gamma_gl, gamma_base;           // the same texels seen with the other gamma (texture_gamma): OpenGL's, of which texture
    int share;  // which row of `shared` its OpenGL texture is of (+1), or 0: one of its own
    u32 serial;       // (a depth target's) how many times it has been drawn or cleared into
    u32 copy_gl, copy_serial;  // a copy of it for a draw to read while it is the depth drawn against, and which state of it that is
} Texture;
static Texture textures[TEXTURES];
// Two textures a game makes over the same memory, alike in every way, are one texture seen twice: what is drawn into one is read
// from the other. A game draws its picture's glow into one and reads it from its twin; with an OpenGL texture each,
// the twin was never drawn to and held whatever the GPU's memory did, and every scene came out as colored noise (2026-11-24). So:
// the OpenGL texture is of the memory, kept here with how many records have it; it goes when the last of them does.
#define SHARED 4096
static struct {
    const u8 *mem;
    int w, h, depth, format, target, levels;  // (the levels as the game asked for them)
    u32 gl, refs, internal, gl_levels;
    int kept_levels;  // the levels the record that made it was left with
    int drawn;        // it has been a render target (of any of the records)
} shared[SHARED];
static u32 n_shared;
#define SAMPLERS 256
typedef struct {
    int used;
    int pooled;  // made by a pool from a builder (nvnSamplerPoolRegisterSamplerBuilder): one for all that are alike
    SamplerBuilder b;
    u32 gl;
} Sampler;
static Sampler samplers[SAMPLERS];
static u16 texture_of[32768], sampler_of[4096];  // the pools' registrations: a descriptor's number to a record (+1)
// Over Vulkan the lists are run by a thread of the port's own, beside the game: the game hands a picture's lists over and goes on
// with the next picture's work while they are drawn, as it does on a Switch, whose GPU draws after the fact too. What the game
// says that bears on the lists' running (a texture ended, a descriptor given another texture) goes through the same line of
// jobs, in its turn. (Run by the game's own thread as they were handed over, a picture's commands and the game's own work for
// a picture came one after the other: some 11 ms and 13 ms in a game's races, measured 2026-11-24.)
enum { JOB_RUN = 1, JOB_SHOW, JOB_TEXTURE_END, JOB_TEXTURE_AT, JOB_SAMPLER_AT };
#define JOBS 16384
static struct {
    u64 kind, a, b;
} jobs[JOBS];
static volatile u32 jobs_pushed, jobs_done, shows_pushed, shows_done;
static volatile int jobs_busy, async_on;
static int try_noasync;
static void job_push(u64 kind, u64 a, u64 b) {
    while (__atomic_exchange_n(&jobs_busy, 1, __ATOMIC_ACQUIRE)) svcSleepThread(10000);
    while (jobs_pushed - jobs_done >= JOBS - 1) svcSleepThread(100000);
    jobs[jobs_pushed % JOBS].kind = kind, jobs[jobs_pushed % JOBS].a = a, jobs[jobs_pushed % JOBS].b = b;
    __atomic_store_n(&jobs_pushed, jobs_pushed + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&jobs_busy, 0, __ATOMIC_RELEASE);
}
static void jobs_wait(void) {
    while (async_on && jobs_done != jobs_pushed) svcSleepThread(50000);
}
// (A game makes more than a thousand at its start: with room for 1024 the rest were refused; a Switch 2, 2026-11-23)
#ifdef NX2_GAME_MB  // (a Switch 2 game: never used, see ARENA)
#define PROGRAMS 16
#else
#define PROGRAMS 16384
#endif
typedef struct {
    int used, state;  // state: 0 not built yet, 1 built, -1 failed
    const char *src[2];  // vertex, fragment: the GLSL the game compiled, copied
    u32 gl;
    int nsamp;
    struct {
        int unit_uniform, slot, offset;  // the sampler's uniform; and where its texture's handle is: the uniform buffer's slot, the offset in it
    } samp[8];
    // a program whose shaders came compiled for the Switch's GPU: the two shaders' numbers (what the port's translations are filed
    // under), and, once built, which texture bindings its samplers stand for (unit k has the texture bound at tex[k]: stage, index)
    u64 key[2];
    const u8 *raw[2];  // the two compiled shaders as the game gave them, and their sizes (kept to be written out when unknown)
    u32 raw_size[2];
    int ntex;
    u8 tex[24][2];
    u32 tex_kind[24];  // the kind of texture each reads, as OpenGL names it (2D, an array, a cube...)
    int nstorage;  // how many storage buffers its shaders read
    u8 whole[16];  // which attributes its vertex shader has as whole numbers (1 unsigned, 2 signed)
    u16 attrs;     // which attributes its vertex shader reads at all (a bit each)
    u32 gl_other;  // the same program built for the other way round (some draws give an attribute as whole numbers, some as floats)
    u8 whole_other[16];
    u32 need[32];  // a uniform block's size, by the slot it is bound at: the buffer given must be no smaller
} Program;
static Program programs[PROGRAMS];

// the shader text, kept: the game's own copy may be freed after its compile
#ifdef NX2_GAME_MB  // (a Switch 2 game: never used, see ARENA)
static char text[1 << 12];
#else
static char text[8 << 20];
#endif
static u32 text_at;
static const char *pending[2];  // the pair last compiled, for the program made next (the game does the two in one function)

// ---------------------------------------------------------------- the commands, as written down
static int pool_holding(u64 address);
static int pool_holds(const void *from, u64 size);
static u64 pool_left(const void *from);
static void pools_near(u64 address);
enum { OP_END, OP_BLEND, OP_COLOR, OP_DEPTH_STENCIL, OP_POLYGON, OP_PROGRAM, OP_UNIFORMS, OP_ATTRIBS, OP_STREAMS, OP_VERTICES, OP_CLEAR_COLOR,
       OP_CLEAR_DEPTH_STENCIL, OP_DRAW_ARRAYS, OP_DRAW_ELEMENTS, OP_TARGETS, OP_SCISSOR, OP_VIEWPORT, OP_STENCIL_MASK, OP_STENCIL_REF,
       OP_STENCIL_VALUE_MASK, OP_TEXTURE, OP_COPY_TO_TEXTURE, OP_STORAGE, OP_UPDATE, OP_SHOW,
       OP_CHANNEL_MASK, OP_POLYGON_OFFSET, OP_DEPTH_RANGE, OP_RENDER_ENABLE, OP_BLEND_COLOR, OP_CLEAR_TEXTURE };
// Each command buffer of the game's has its own stretch of words: the game records into several at once, from several threads,
// and with one stretch for all of them (as this was) their commands ran into one another. A game's first pictures
// had lists holding draws alone, the programs they were to be drawn with gone into another list (a Switch 2, 2026-11-23).
// A list's handle: which command buffer (from 1, in bits 40 up), where the list begins in its words (20 bits), how many (20 bits).
// A command buffer's words are begun again when the game gives it command memory anew, which is how it says the lists it made
// are done with.
// (How many command buffers a game makes isn't known beforehand: a game makes more than 48, and with 48 kept apart
// the rest shared one and ran into one another again. So: 4096 of them, each with words taken as it needs them from one large
// stretch, twice as many each time it runs out; what it had is copied over and left where it was, since a list being run may still
// be read there.)
#define LIST (1u << 20)
#define RECORDERS 4096
// (A Switch 2 game draws with its own SDK and nothing here is ever started (gl2.c: native), yet the stretches below were part of
// the program all the same, 500 MB of the console's memory that the game then hadn't: one that maps memory into a stretch of its
// own as it runs was refused it over and over in its opening, with nothing left (log, 2026-11-25). Built for such a game they
// are next to nothing.)
#ifdef NX2_GAME_MB
#define ARENA (1u << 12)
#else
#define ARENA (1u << 24)
#endif
typedef struct {
    u64 *words;
    u32 cap, at, start;
    int full, used;
    u32 total;                      // how many words it has recorded in all
    u32 memory_from, control_from;  // what that count was when it was last given command memory, and control memory
    u64 memory_size, control_size;  // how much of each it was given
    u64 memory_at;                  // where in the game's memory its commands go now (0: not said)
    u32 list_from;                  // what the count of words was when the list being recorded was begun
} Recorder;
static Recorder recorders[RECORDERS];
static u32 recorders_made;
static u64 arena[ARENA];
static u32 arena_at;
// room for so many more words in a command buffer's: 1, or 0 when there is none to be had
static int recorder_room(Recorder *r, u32 more) {
    if (r->at + more <= r->cap) return 1;
    u32 cap = r->cap ? r->cap : 512;
    while (cap < r->at + more) cap *= 2;
    const u32 from = cap <= LIST ? __atomic_fetch_add(&arena_at, cap, __ATOMIC_RELAXED) : ARENA;
    if (from >= ARENA || from + cap > ARENA) {
        if (!r->full) r->full = 1, nx2_log("[nvn] a command buffer's commands don't fit what is kept of them (%u words asked): the rest are dropped\n", (u64)cap);
        return 0;
    }
    if (r->at) memcpy(arena + from, r->words, (size_t)r->at * 8);
    r->words = arena + from, r->cap = cap;
    return 1;
}
static void put_to(const Named *c, u64 v) {
    Recorder *r = &recorders[c->id < RECORDERS ? c->id : 0];
    if (recorder_room(r, 1)) r->words[r->at++] = v, r->total++;
}
// A list that is ended is copied to a place of its own, and its handle names it: a game may end a command buffer and use its
// lists afterwards (A game makes one for each thing it draws, ends it, and joins the list to its main one later),
// and the command buffer's words are another's by then. Its first picture came out torn, and the game ended, reading such words
// (a Switch 2, 2026-11-23).
// A game may also keep a list for as long as it runs and use it at every picture, and never says when it is done with one. So:
// each list has a block of words of its own, of a size that is a power of two, taken from one large stretch, and a row in a
// table (where it is, how long, at which pictures it was made and last run). A list run at a later picture than it was made at
// is one the game keeps: it stays while it was run within the last 1800 pictures; one made and run once goes after 120. A list
// let go gives its block back, for the next list of that size. (They were written one after the other into one of two stretches
// of 8 million words, those still in use copied over when one was full: a race of a game keeps some 20000 lists,
// more than the stretch held; the course's own lists had no room, and the course wasn't drawn; 2026-11-24.) A handle: bit 62,
// and the row.
#ifdef NX2_GAME_MB  // (a Switch 2 game: never used, see ARENA)
#define STORE (1u << 12)
#define LISTS (1u << 4)
#else
#define STORE (40u << 20)
#define LISTS (1u << 20)
#endif
static u64 store[STORE];
static u32 store_at, store_free[32];  // (the next word never given; for each size, the first block given back (+1), which holds the next)
static u32 kept_row, kept_swept;
static int kept_busy;
static u32 picture;  // how many have been shown
static struct {
    u32 at, n, used;  // (n 0: a free row)
    u32 made;         // the picture it was ended at
    u32 first;        // the picture it was first run at (~0: never yet)
    u8 again, size;   // it has been run at a later picture than its first; its block is 1 << size words
    u16 region;       // 1: where it lies in the game's command memory is known (`from`, `len`)
    u32 len, serial;  // (serial: how many stretches of command memory the game had given when it was ended)
    u64 from;
    u32 turn;         // how many lists this row has held: a handle names the row and the turn, and an old handle finds nothing
    u32 freed, idle_then;  // (when it was let go, how long unrun by then, and by which rule: said if the game asks for it after)
    u8 why;
} lists[LISTS];
// How long a list lasts. A list lies in command memory the game gave its command buffer, and is good until the game gives that
// memory to a command buffer again, to record over: that is when the game is done with it, and the only way it says so. So each
// list remembers the stretch of the game's memory it took (from what this port told the game its commands take, which is what
// the game counts with), and each stretch of memory the game gives is noted; when the lists are gone through, those lying in
// memory given again since are let go.
// (Lists were let go by how long they had gone unrun: two seconds for one run at a single picture. A game keeps the list of a
// model's coarser form, runs it at one picture, and runs it again when the model is that far away once more: it was gone, or
// its row was another list's by then and that one's commands were run in its stead. A game's karts and drivers
// were drawn with other models' bones and buffers; 2026-11-24. The whole stretch a command buffer was given can't be taken for
// the list's: the game gives "all that is left of the pool" each time, and every list would lie in every later stretch.)
#define GIVEN 8192
static struct {
    u64 from, to;
} given[GIVEN], given_sorted[GIVEN];
static u32 given_count, given_serial, given_swept, given_lost;
static int given_busy;
static int try_oldlists;
static void given_note(u64 from, u64 size) {
    if (!from || !size) return;
    while (__atomic_exchange_n(&given_busy, 1, __ATOMIC_ACQUIRE)) svcSleepThread(10000);
    given_serial++;
    if (given_count < GIVEN) given[given_count].from = from, given[given_count].to = from + size, given_count++;
    else given_lost = 1;  // (more than are kept between two goings-through: the lists of that while last by how long they go unrun)
    __atomic_store_n(&given_busy, 0, __ATOMIC_RELEASE);
}
static int given_by_place(const void *a, const void *b) {
    const u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}
// The lists no longer wanted are let go. One run at one picture only, and not since for two seconds, was made for that picture.
// One never run yet, or run at several pictures, is one the game keeps to run when it needs it, which may be much later (a
// thing's set-up, made as a course is loaded and first run when the thing comes into view): those stay, unless room has run
// out (`hard`), when the ones idle for thirty seconds go too. (They went after two seconds idle if never run twice, and a race
// of a game lost the lists made while it loaded: nothing of the course was drawn; 2026-11-24.)
static void kept_sweep(int hard) {  // (with kept_busy held)
    u32 alive = 0, gone = 0, words = 0;
    // the stretches given since the lists were last gone through, in order of place and run together where they touch
    while (__atomic_exchange_n(&given_busy, 1, __ATOMIC_ACQUIRE)) svcSleepThread(10000);
    u32 stretches = given_count;
    const u32 before = given_swept, now = given_serial;
    const int lost = given_lost;
    memcpy(given_sorted, given, (size_t)stretches * sizeof *given);
    given_count = 0, given_lost = 0, given_swept = now;
    __atomic_store_n(&given_busy, 0, __ATOMIC_RELEASE);
    qsort(given_sorted, stretches, sizeof *given_sorted, given_by_place);
    u32 merged = 0;
    for (u32 k = 0; k < stretches; k++) {
        if (merged && given_sorted[k].from <= given_sorted[merged - 1].to) {
            if (given_sorted[k].to > given_sorted[merged - 1].to) given_sorted[merged - 1].to = given_sorted[k].to;
        } else given_sorted[merged++] = given_sorted[k];
    }
    for (u32 k = 0; k < LISTS; k++) {
        if (!lists[k].n) continue;
        const u32 idle = picture - lists[k].used;
        const int once = lists[k].first != ~0u && !lists[k].again;
        const int timed = try_oldlists || !lists[k].region;  // (where it lies isn't known: by how long it has gone unrun, as before)
        // (A list ended before the last going-through is looked for among the stretches given since; one ended after it may
        // lie in one of those very stretches, and is looked at the next time.)
        int recorded_over = 0;
        if (!timed && !lost && lists[k].serial <= before && merged) {
            u32 lo = 0, hi = merged;
            const u64 from = lists[k].from, to = from + (lists[k].len ? lists[k].len : 1);
            while (lo < hi) {  // (the first stretch that ends after the list begins)
                const u32 mid = (lo + hi) / 2;
                if (given_sorted[mid].to <= from) lo = mid + 1;
                else hi = mid;
            }
            recorded_over = lo < merged && given_sorted[lo].from < to;
            if (recorded_over && lists[k].region == 1) lists[k].region = 2;  // (remembered: the stretches are forgotten once gone through)
        }
        // (Recorded over, by this port's reckoning of where a list lies, and yet the game went on running some: so such a list
        // goes only once it has also gone unrun for two seconds, or a few pictures when room has run out. One not
        // recorded over is the game's to run whenever it will: it stays.)
        if (lists[k].region == 2) recorded_over = idle > (lists[k].n <= 256 ? 900u : hard ? 120u : 600u);  // (a small one costs little to keep longer)
        else if (lists[k].region == 3) recorded_over = 0;
        // (one never run stays thirty seconds: a game ends tens of thousands it never runs, and they filled the room there is)
        if (!timed ? !recorded_over && (!hard || idle <= 36000)
                   : once ? idle <= 120 : lists[k].first == ~0u ? idle <= 1800 : (!hard || idle <= 1800)) {
            alive++, words += 1u << lists[k].size;
            continue;
        }
        store[lists[k].at] = store_free[lists[k].size];
        store_free[lists[k].size] = lists[k].at + 1;
        lists[k].freed = picture, lists[k].idle_then = idle;
        lists[k].why = (u8)((timed ? 1 : lists[k].region == 2 ? 2 : 3) + (hard ? 10 : 0) + (once ? 20 : lists[k].first == ~0u ? 40 : 0));
        lists[k].n = 0, gone++;
    }
    kept_swept = picture;
    static u32 said;
    if (said++ < 40 || hard) nx2_log("[nvn] the lists kept are gone through%s: %u still in use (%u words of %u), %u let go\n", hard ? " (no room was left)" : "", (u64)alive, (u64)words, (u64)STORE, (u64)gone);
}
// a block for so many words: where it is, or STORE when there is none
// The lists kept are moved together to the stretch's beginning, in the order they lie in, which leaves what is free in one
// piece after them (blocks given back are of many sizes, and a large list may find none though half the stretch is free). No
// list is being run meanwhile: the lock the lists are run under is held.
static void gl_take(void);
static void gl_give(void);
static u32 kept_order[LISTS];
static int kept_by_place(const void *a, const void *b) {
    const u32 x = lists[*(const u32 *)a].at, y = lists[*(const u32 *)b].at;
    return x < y ? -1 : x > y;
}
static void kept_pack(void) {  // (with kept_busy held)
    gl_take();
    u32 count = 0, to = 0;
    for (u32 k = 0; k < LISTS; k++)
        if (lists[k].n) kept_order[count++] = k;
    qsort(kept_order, count, sizeof *kept_order, kept_by_place);
    for (u32 i = 0; i < count; i++) {
        const u32 k = kept_order[i];
        if (lists[k].at != to) memmove(store + to, store + lists[k].at, (size_t)lists[k].n * 8);
        lists[k].at = to, to += 1u << lists[k].size;
    }
    store_at = to;
    memset(store_free, 0, sizeof store_free);
    gl_give();
    static u32 said;
    if (said++ < 20) nx2_log("[nvn] the lists kept are moved together: %u of them, %u words of %u\n", (u64)count, (u64)to, (u64)STORE);
}
static u32 kept_block(u32 n, u8 *size) {
    u8 c = 4;
    while ((1u << c) < n) c++;
    *size = c;
    if (store_free[c]) {
        const u32 at = store_free[c] - 1;
        store_free[c] = (u32)store[at];
        return at;
    }
    if (store_at + (1u << c) <= STORE) {
        store_at += 1u << c;
        return store_at - (1u << c);
    }
    // (none of that size, and the stretch all given out: a larger block that was given back is cut in two, and again, down to
    // the size wanted; the halves not taken are blocks of their sizes)
    for (u8 k = (u8)(c + 1); k < 26; k++) {
        if (!store_free[k]) continue;
        const u32 at = store_free[k] - 1;
        store_free[k] = (u32)store[at];
        for (u8 j = k; j > c; j--) {
            const u32 half = at + (1u << (j - 1));
            store[half] = store_free[j - 1];
            store_free[j - 1] = half + 1;
        }
        return at;
    }
    return STORE;
}
static u64 list_keep(const u64 *words, u32 n, u64 from, u32 len) {
    if (!n || n >= STORE / 8) return 0;
    while (__atomic_exchange_n(&kept_busy, 1, __ATOMIC_ACQUIRE)) svcSleepThread(10000);
    if (picture - kept_swept >= 60) kept_sweep(0);
    u32 row = LISTS;
    for (u32 tries = 0; tries < LISTS; tries++) {
        const u32 k = kept_row++ % LISTS;
        if (!lists[k].n) {
            row = k;
            break;
        }
    }
    u8 size = 0;
    u32 at = row < LISTS ? kept_block(n, &size) : STORE;
    if (row < LISTS && at == STORE) {  // (no block left: the lists are gone through now, which may give some back)
        kept_sweep(0);
        at = kept_block(n, &size);
        if (at == STORE) kept_pack(), at = kept_block(n, &size);
        if (at == STORE) kept_sweep(1), kept_pack(), at = kept_block(n, &size);
    }
    u64 handle = 0;
    if (row < LISTS && at < STORE) {
        memcpy(store + at, words, (size_t)n * 8);
        lists[row].at = at, lists[row].used = picture, lists[row].n = n, lists[row].made = picture, lists[row].first = ~0u, lists[row].again = 0, lists[row].size = size;
        lists[row].region = from != 0, lists[row].from = from, lists[row].len = len, lists[row].serial = given_serial, lists[row].turn++;
        handle = 1ull << 62 | (u64)(lists[row].turn & 0xFFFFFFFu) << 20 | row;
    } else {
        static u32 said;
        if (said++ < 4) nx2_log("[nvn] a list of %u words has no room to be kept in: what it draws is left out\n", (u64)n);
    }
    __atomic_store_n(&kept_busy, 0, __ATOMIC_RELEASE);
    return handle;
}
#define put(v) put_to(c, (v))
// a list's words and their count, by its handle (NULL: not a handle of this port's, or one let go); it is marked as used now
static const u64 *list_of(u64 handle, u32 *count) {
    const u32 row = (u32)handle & (LISTS - 1);
    if (!(handle >> 62 & 1) || (handle >> 48 & 0x3FFF) || !lists[row].n || (lists[row].turn & 0xFFFFFFFu) != (u32)(handle >> 20 & 0xFFFFFFFu)) {
        static u32 said;
        if (handle && said++ < 40)
            nx2_log("[nvn] a list is asked for that isn't kept any more (its handle %x): left out. The row: turn %u, %s; let go at picture %u (now %u) after %u unrun, rule %u (1 timed, 2 recorded over, 3 neither; +10 no room; +20 run once, +40 never run); made at %u, %u bytes at %p\n",
                    handle, (u64)lists[row].turn, lists[row].n ? "holding another list" : "free", (u64)lists[row].freed, (u64)picture, (u64)lists[row].idle_then, (u64)lists[row].why,
                    (u64)lists[row].made, (u64)lists[row].len, lists[row].from);
        return NULL;
    }
    // (one taken for recorded over that the game runs all the same wasn't: it stays from here on)
    if (lists[row].region == 2 && picture > lists[row].used + 2) lists[row].region = 3;
    lists[row].used = picture;
    if (lists[row].first == ~0u) lists[row].first = picture;
    else if (picture > lists[row].first + 2) lists[row].again = 1;
    *count = lists[row].n;
    return store + lists[row].at;
}

// ---------------------------------------------------------------- the host's side: OpenGL (these run as the host)
static EGLDisplay display;
static EGLSurface surface;
static int started, gl_w, gl_h;
static u32 vao, vbo[16], ibo, ubo[32], sbo[16], submit_serial;
static u32 sbo_room[16];  // and each storage buffer
static u32 ubo_room[32];  // how large each uniform buffer of OpenGL's has been made
// The draws' uniforms, one after another in a large buffer of OpenGL's: each draw's blocks get a stretch of their own, never
// written again while the picture is drawn, and the next picture goes on in the next of three buffers, begun anew. (They went
// into one buffer for each binding point, written again at every draw: a draw that wrote part of a buffer the draws before it
// were still to read with had those draws read ITS numbers, on the host's driver. A game's karts and drivers,
// each drawn with its bones' places in such a block of 64 KB, came out as blobs with spikes; 2026-11-24.)
#define FLOW_ROOM (32u << 20)
static u32 flow[3], flow_now, flow_at, flow_zero;
static int try_oldubo;
// Where the time of running a picture's commands goes: ticks spent making a draw ready (its program, its uniforms, its textures,
// its vertices), in the draw call itself, in setting targets, and in the rest; with the driver's own counts beside them
// (ouncevk/profile.h: what is handed to the GPU, and the waits for it). Asked for when a game's races ran at 20
// pictures a second, to know which part to make faster; 2026-11-24.
#include <ouncevk/profile.h>
static struct {
    u64 program, uniforms, textures, vertices, draw, targets, clears, all;
    u32 draws, lists;
} spent;
static u64 op_time[32];
static u32 op_count[32];
static int try_nomorebones;
static u32 bones_draws, bones_none, bones_zero, bones_odd;
static int try_bones;  // (bones: said of each draw with bones, the largest bone its vertices name and how much of each large block the game gave)
// A block of the game's that several draws of one list are given (the camera's, a scene's lights, a model's bones over its
// passes) goes into the buffer once: the game's memory is the same for every draw of a list as it is run, but for what the list
// itself writes into it (OP_UPDATE, which forgets what it writes over). Two thirds of the time of running a game's
// races went into giving the same blocks at every draw, and summing the same vertices (measured, 2026-11-24).
#define FLOWN 256
static struct {
    const u8 *ptr;
    u32 have, take, at, turn;
} flown[FLOWN];
static u32 flow_turn = 1;
static struct {
    u32 buf, at, room;
} flow_bound[32];  // what each binding point has now: a block already there isn't bound again
static u32 n_blocks_given, n_blocks_shared, n_blocks_bound;
static u64 n_block_bytes;
static int try_nocache;
static void flow_next(void) {
    if (!flow[0]) glGenBuffers(3, flow);
    flow_now = (flow_now + 1) % 3, flow_at = 0;
    flow_turn++;
    glBindBuffer(GL_UNIFORM_BUFFER, flow[flow_now]);
    glBufferData(GL_UNIFORM_BUFFER, FLOW_ROOM, NULL, GL_STREAM_DRAW);
}
// (A framebuffer for each pair of targets a game draws to, kept. There were 64 kept, and one made anew at every change of targets
// past those: a game has hundreds, made thousands a second, and the picture stood still and the game ended; a
// Switch 2, 2026-11-23. Now 1024, known by the two OpenGL textures as well as the records, and the oldest given up when full.)
#define FBOS 1024
#define NO_TEXTURE (~0u)
static struct {
    u32 n, color[8], color_gl[8], color_where[8];  // (how many colors; each one's record or NO_TEXTURE, OpenGL texture, level and layer)
    u32 depth, depth_gl, depth_where, fbo;
    u16 pack[8];    // for a color that is a compressed texture's: which row of `packed` is drawn to in its stead (+1)
    u32 pack_mask;  // (a bit for each such color)
} fbos[FBOS];
static u32 fbo_count, fbo_next;
// (counts of what has been made of OpenGL's, said with each sixty pictures: what grows without end is seen so)
static u32 n_textures, n_textures_gone, n_framebuffers, n_programs, n_uploads, n_draws, n_lists;
static u64 n_upload_bytes;
static int fbo_last;  // which of them fbo_for gave last (the order they were made in)
static u32 channel_mask = ~0u;  // which of red, green, blue and alpha are written, four bits for each of eight targets
// (frame=N: what is done for the Nth picture after the first model with bones is said, a line each; frameat=N: for the Nth picture)
static int try_frame = -1, try_frameat = -1, framing;
static int try_nosampler;  // (nosampler: no sampler of the game's is used, each texture read as it was made)
static int try_pixel[2];   // (pixel=X,Y: see frame_pixel)
static int try_nomipfilter;  // (nomipfilter: samplers read a texture's first level only)
static int try_noinvariant;  // (noinvariant: vertex shaders aren't told to work a vertex's place out the same way)
static int try_scissorclear;  // (scissorclear: a clear is kept to the scissor's box)
static int try_grid;  // (grid: a picture's dump also says how much of each first color is covered, which takes long)
static int try_frametime;  // (frametime=S: see where pictures are shown)
static int try_atvideo;    // (atvideo: the trail begins when a video is started)
static int try_noflip;     // (noflip: what is drawn isn't turned over for a window whose origin is the upper left)
static u32 n_uniform_misses;  // (draws whose program needed a uniform buffer that wasn't in memory the game gave)
static u32 n_3d_said;      // (the first draws into a 3D texture have what they left said too)
static u32 frame_from = ~0u;  // (the picture the first program with whole-number attributes was built at)
static int origin_upper;   // the window's pictures begin at the upper left corner (nvnDeviceSetWindowOriginMode)
static int depth_zero;     // clip space's depth runs from 0 at the near plane, not from -w (nvnDeviceSetDepthMode)
static int depth_zero_how; // 0 not yet looked at, 1 the host's OpenGL was told so (glClipControl), 2 the vertex shaders are given a line
static int try_nodepthmode;
static u32 window_fbo, window_texture, window_w, window_h;
static u32 window_fbo_gamma, window_texture_gamma;  // the same, drawn into as sRGB (texture_gamma)
static int try_nogamma;

static int astc_given = -1;  // <folder>/astc is on the card (looked for in h_start)
// Switches for finding what a game's trouble comes from, with the port as it is: words in the file <folder>/try, read when OpenGL
// is started (tools/bench/run.py writes it: --try "nomips trail").
//   trail          every command run is said, each line on the card before the next thing (from the first texture past 120)
//   nomips         of a texture, only the first level is given to OpenGL
//   nocompressed   textures of a compressed format are left empty
//   noupload       no texture's texels are given at all
//   nooffscreen    nothing is drawn but to the window
//   noclear        nothing is cleared
//   nodepth        targets are made without their depth
//   nostorage      storage buffers are left unbound
static int try_noarray, try_nolayers;  // (noarray: textures that aren't plain 2D are left empty; nolayers: only their first layer is given)
static u64 try_flat, try_still;  // (flat=HEX: that fragment shader is one plain color instead; still=HEX: that vertex shader puts every vertex at one place)
static int try_proglimit = -1, try_progskip = -1;  // (proglimit=N: no program is built past the Nth; progskip=N: the Nth alone isn't)
static u64 try_key;  // (key=HEX: the trail, from the first draw with a program one of whose shaders has that number)
static int try_trailat = -1;  // (trailat=N: the trail, from the Nth draw asked for, for 600 lines)
static int try_drawlimit = -1, try_drawskip = -1;  // (drawlimit=N: from the Nth draw asked for on, none is done but to the window; drawskip=N: the Nth alone isn't)
static u32 n_asked;  // draws asked for, done or not
static int try_sync = -1;  // (sync=N: from the Nth draw on, each line of the log is on the card before the next thing)
static int try_fbolimit = -1, try_fboskip = -1;  // (fbolimit=N: nothing is drawn to the Nth framebuffer made and those after it; fboskip=N: to the Nth alone)
static int try_trail, try_nomips, try_nocompressed, try_noupload, try_nooffscreen, try_noclear, try_nodepth, try_nostorage;
static void try_read(void) {
    static char text[256];
    FILE *f = fopen(NX2_F("try"), "rb");
    if (!f) return;
    const size_t n = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[n] = 0;
    try_trail = strstr(text, "trail") != NULL, try_nomips = strstr(text, "nomips") != NULL, try_nocompressed = strstr(text, "nocompressed") != NULL;
    try_noupload = strstr(text, "noupload") != NULL, try_nooffscreen = strstr(text, "nooffscreen") != NULL, try_noclear = strstr(text, "noclear") != NULL;
    try_nodepth = strstr(text, "nodepth") != NULL, try_nostorage = strstr(text, "nostorage") != NULL;
    const char *limit = strstr(text, "fbolimit="), *skip = strstr(text, "fboskip=");
    if (limit) try_fbolimit = atoi(limit + 9);
    if (strstr(text, "drawlimit=")) try_drawlimit = atoi(strstr(text, "drawlimit=") + 10);
    if (strstr(text, "drawskip=")) try_drawskip = atoi(strstr(text, "drawskip=") + 9);
    if (strstr(text, "trailat=")) try_trailat = atoi(strstr(text, "trailat=") + 8);
    if (strstr(text, "proglimit=")) try_proglimit = atoi(strstr(text, "proglimit=") + 10);
    if (strstr(text, "frame=")) try_frame = atoi(strstr(text, "frame=") + 6);
    if (strstr(text, "frameat=")) try_frameat = atoi(strstr(text, "frameat=") + 8);
    try_nosampler = strstr(text, "nosampler") != NULL;
    try_noflip = strstr(text, "noflip") != NULL;
    try_noasync = strstr(text, "noasync") != NULL;
    try_oldlists = strstr(text, "oldlists") != NULL;
    try_oldubo = strstr(text, "oldubo") != NULL;
    try_nocache = strstr(text, "nocache") != NULL;
    try_nomorebones = strstr(text, "nomorebones") != NULL;
    try_bones = strstr(text, "bones") != NULL;
    try_nogamma = strstr(text, "nogamma") != NULL;
    try_nodepthmode = strstr(text, "nodepthmode") != NULL;
    if (strstr(text, "depthzero")) depth_zero = 1;
    try_atvideo = strstr(text, "atvideo") != NULL;
    try_grid = strstr(text, "grid") != NULL;
    try_scissorclear = strstr(text, "scissorclear") != NULL;
    if (strstr(text, "frametime=")) try_frametime = atoi(strstr(text, "frametime=") + 10);
    try_noinvariant = strstr(text, "noinvariant") != NULL;
    try_nomipfilter = strstr(text, "nomipfilter") != NULL;
    if (strstr(text, "pixel=")) sscanf(strstr(text, "pixel=") + 6, "%d,%d", &try_pixel[0], &try_pixel[1]);
    if (strstr(text, "progskip=")) try_progskip = atoi(strstr(text, "progskip=") + 9);
    if (strstr(text, "flat=")) try_flat = strtoull(strstr(text, "flat=") + 5, NULL, 16);
    if (strstr(text, "still=")) try_still = strtoull(strstr(text, "still=") + 6, NULL, 16);
    try_noarray = strstr(text, "noarray") != NULL, try_nolayers = strstr(text, "nolayers") != NULL;
    if (strstr(text, "key=")) try_key = strtoull(strstr(text, "key=") + 4, NULL, 16);
    if (strstr(text, "sync=")) try_sync = atoi(strstr(text, "sync=") + 5);
    if (skip) try_fboskip = atoi(skip + 8);
    nx2_log("[nvn] <folder>/try: %s\n", text);
}
static void h_start(int w, int h) {
    if (started) return;
    started = 1;
    {
        FILE *file = fopen(NX2_F("astc"), "rb");
        astc_given = file != NULL;
        if (file) fclose(file);
        nx2_log("[nvn] ASTC textures: %s\n", astc_given ? "given to OpenGL (<folder>/astc is there)" : "left empty (no <folder>/astc)");
        try_read();
    }
    {
        FILE *file = fopen(NX2_F("vulkan"), "rb");
        if (file) {
            fclose(file);
            nx2_log("[nvn] <folder>/vulkan is there: the port's own OpenGL over Vulkan is drawn with, not the host's\n");
            glvk_start(w, h, NX2_F("shaders.spv"));
        }
    }
    if (nx2_vulkan) {
        gl_w = w, gl_h = h;
        if (!try_noasync) {
            static Thread worker;
            extern void nx2_jobs_run(void *);
            // (on a core of its own, if the system allows another: on the game's it would only take turns with the game)
            u64 allowed = 0, now_mask = 0;
            s32 now_core = 0;
            svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
            svcGetThreadCoreMask(&now_core, &now_mask, CUR_THREAD_HANDLE);
            int core = -2;
            for (int k = 7; k >= 0; k--)
                if (allowed >> k & 1) {
                    core = k;
                    break;
                }
            if (R_SUCCEEDED(threadCreate(&worker, nx2_jobs_run, NULL, NULL, 4 << 20, 0x2C, core)) && R_SUCCEEDED(threadStart(&worker))) async_on = 1;
            nx2_log("[nvn] the lists are run by a thread of the port's own, beside the game: %s (cores the system allows %x; this thread is on core %d of mask %x; the port's on %d)\n",
                    async_on ? "yes" : "NO (it couldn't be started)", allowed, (s64)now_core, now_mask, (s64)core);
        }
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(16, vbo);
        glGenBuffers(1, &ibo);
        glGenBuffers(32, ubo);
        glGenBuffers(16, sbo);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, w, h);
        glViewport(0, 0, w, h);
        return;
    }
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0, count = 0;
    eglInitialize(display, &major, &minor);
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint want[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
                                  EGL_RENDERABLE_TYPE, 0x40 /* ES 3 */, EGL_NONE};
    EGLConfig config = 0;
    eglChooseConfig(display, want, &config, 1, &count);
    // (A context whose reads past the end of a buffer give zeros, where the host has such: a translated shader reads its buffers
    // at places it works out as it runs, and one read past an end, with a plain context, is the GPU's fault to take. A game stood still with the driver saying its work for the GPU had failed, at its first draws of models; a Switch 2,
    // 2026-11-23. Asked for in the two ways EGL has; a plain context if neither is to be had.)
    static const EGLint ctx[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    static const EGLint robust_a[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, 0x30BF /* EGL_CONTEXT_OPENGL_ROBUST_ACCESS_EXT */, 1, EGL_NONE};
    static const EGLint robust_b[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, 0x31B2 /* EGL_CONTEXT_OPENGL_ROBUST_ACCESS */, 1, EGL_NONE};
    const char *kind = "one that answers reads past a buffer's end with zeros";
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, robust_a);
    if (context == EGL_NO_CONTEXT) context = eglCreateContext(display, config, EGL_NO_CONTEXT, robust_b);
    if (context == EGL_NO_CONTEXT) context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx), kind = "a plain one (the host has no other)";
    nx2_log("[nvn] the OpenGL context: %s\n", kind);
    nwindowSetDimensions(nwindowGetDefault(), (u32)w, (u32)h);
    surface = eglCreateWindowSurface(display, config, nwindowGetDefault(), NULL);
    const EGLBoolean ok = eglMakeCurrent(display, surface, surface, context);
    gl_w = w, gl_h = h;
    nx2_log("[nvn] the host's OpenGL: EGL %u.%u, %d configs, context %p, surface %p, current %u (error %x); a %ux%u picture\n", (u64)major, (u64)minor,
            (s64)count, (u64)context, (u64)surface, (u64)ok, (u64)eglGetError(), (u64)w, (u64)h);
    nx2_log("[nvn] %s; %s; GLSL %s\n", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER),
            (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(16, vbo);
    glGenBuffers(1, &ibo);
    glGenBuffers(32, ubo);
    glGenBuffers(16, sbo);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, w, h);
    glViewport(0, 0, w, h);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

// The game's GLSL, for the host's OpenGL ES. It is written for desktop OpenGL 4.4 with one of NVIDIA's extensions, by which a texture
// is a 64 bit number in a uniform buffer ("bindless"): uint64_t g_tex0; ... texture2D(sampler2D(g_tex0), uv). OpenGL ES has none of
// that, so: the version line is ES 3.2's, the number becomes two 32 bit ones (the same place and size in the buffer), each gets a
// sampler of its own (g_tex0_s), and where the shader made a sampler of the number it uses that one. The number is still written
// by the game, in its buffer: it is read there at each draw to know which texture the sampler is to have.
static char built[1 << 16];
static int is_name(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
static const char *adapt(const char *src, int fragment, char names[8][48], int *count) {
    u32 n = 0;
    *count = 0;
#define OUT(s) \
    for (const char *q_ = (s); *q_ && n < sizeof built - 1; q_++) built[n++] = *q_
    OUT("#version 320 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n");
    if (!fragment) OUT("#define NX2_VERTEX 1\n");
    // the samplers first: every uint64_t the text declares
    for (const char *p = src; (p = strstr(p, "uint64_t")) != NULL; p += 8) {
        const char *q = p + 8;
        while (*q == ' ' || *q == '\t') q++;
        u32 len = 0;
        while (is_name(q[len]) && len < 40) len++;
        if (!len || *count >= 8) continue;
        memcpy(names[*count], q, len);
        names[*count][len] = 0;
        OUT("uniform sampler2D ");
        OUT(names[*count]);
        OUT("_s;\n");
        (*count)++;
    }
    for (const char *p = src; *p && n < sizeof built - 64;) {
        if (*p == '#' && (!strncmp(p, "#version", 8) || !strncmp(p, "#extension", 10))) {  // (the line is dropped; its end is kept)
            while (*p && *p != '\n') p++;
        } else if (!strncmp(p, "uint64_t", 8) && !is_name(p[8])) {
            OUT("uvec2");
            p += 8;
        } else if (!strncmp(p, "texture2D", 9) && !is_name(p[9])) {
            OUT("texture");
            p += 9;
        } else if (!strncmp(p, "sampler2D", 9) && (p == src || !is_name(p[-1]))) {
            const char *q = p + 9;
            while (*q == ' ' || *q == '\t') q++;
            if (*q != '(') {
                built[n++] = *p++;
                continue;
            }
            q++;
            while (*q == ' ' || *q == '\t') q++;
            u32 len = 0;
            while (is_name(q[len])) len++;
            const char *e = q + len;
            while (*e == ' ' || *e == '\t') e++;
            if (!len || *e != ')') {
                built[n++] = *p++;
                continue;
            }
            for (u32 k = 0; k < len && n < sizeof built - 8; k++) built[n++] = q[k];
            OUT("_s");
            p = e + 1;
        } else {
            built[n++] = *p++;
        }
    }
#undef OUT
    built[n] = 0;
    return built;
}

static u32 compile(u32 kind, const char *src, int id) {
    const u32 s = glCreateShader(kind);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        static char info[1200];
        glGetShaderInfoLog(s, sizeof info - 1, NULL, info);
        nx2_log("[nvn] program %d: its %s shader doesn't compile for the host's OpenGL:\n%s\n", (s64)id, kind == GL_VERTEX_SHADER ? "vertex" : "fragment", info);
        static u32 shown;
        if (shown++ < 3) {  // the text as it was given, in pieces the log takes
            for (const char *p = src; *p;) {
                static char piece[400];
                u32 k = 0;
                while (*p && k < sizeof piece - 1) piece[k++] = *p++;
                piece[k] = 0;
                nx2_log("%s", piece);
            }
            nx2_log("\n[nvn] (end of the shader)\n");
        }
    }
    return s;
}

// The translations of a game's compiled shaders: <folder>/shaders, written when the port is made (tools/shaderpack.py, which
// tools/maxwell.py does the translating for). A table of programs, sorted by their two shaders' numbers, and the GLSL of each.
typedef struct {
    u64 vs, fs, at;
    u32 vs_len, fs_len;
} PackEntry;
static PackEntry *pack;
static u32 pack_count;
static FILE *pack_file;
static void pack_open(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    FILE *f = fopen(NX2_F("shaders.bin"), "rb");
    u32 head[4] = {0};
    if (!f || fread(head, 4, 4, f) != 4 || head[0] != 0x5332584E /* NX2S */ || head[1] != 1) {
        nx2_log("[nvn] the translated shaders (<folder>/shaders.bin): %s\n", f ? "not a file of this port's" : "not there");
        if (f) fclose(f);
        return;
    }
    pack = malloc((size_t)head[2] * sizeof *pack);
    if (pack && fread(pack, sizeof *pack, head[2], f) == head[2]) pack_count = head[2], pack_file = f;
    nx2_log("[nvn] the translated shaders (<folder>/shaders.bin): %u programs\n", (u64)pack_count);
    if (!pack_file) fclose(f);
}
static u32 compile(u32 kind, const char *src, int id);
static const PackEntry *pack_find(u64 vs, u64 fs) {
    for (u32 lo = 0, hi = pack_count; lo < hi;) {
        const u32 mid = (lo + hi) / 2;
        const PackEntry *m = &pack[mid];
        if (m->vs == vs && m->fs == fs) return m;
        if (m->vs < vs || (m->vs == vs && m->fs < fs)) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}
// an entry's two texts, one after the other, each ended by a zero (to be freed), or NULL
static char *pack_text(const PackEntry *e) {
    const size_t n = (size_t)e->vs_len + e->fs_len + 2;
    char *both = malloc(n);
    if (!both) return NULL;
    fseek(pack_file, (long)e->at, SEEK_SET);
    if (fread(both, 1, n, pack_file) != n || both[e->vs_len] || both[n - 1]) {
        free(both);
        return NULL;
    }
    return both;
}
// A vertex shader and a fragment shader the file has apart (a game's archive that doesn't say which goes with which: a game's agl_resource), made to agree: what the fragment shader reads of the vertex shader's ("in vec4 v_N;") and that one
// doesn't declare is declared in it, before its main, and left as it starts. The vertex text, to be freed.
static char *vertex_agreeing(const char *vs, const char *fs) {
    char extra[32 * 20];
    int n = 0;
    for (int k = 0; k < 32; k++) {
        char in[24], out[24];
        snprintf(in, sizeof in, "\nin vec4 v_%d;", k);
        snprintf(out, sizeof out, "out vec4 v_%d;", k);
        if (strstr(fs, in) && !strstr(vs, out)) n += snprintf(extra + n, sizeof extra - (size_t)n, "%s\n", out);
    }
    const char *main_at = strstr(vs, "\nvoid main");
    const size_t len = strlen(vs), head = main_at ? (size_t)(main_at - vs) + 1 : len;
    char *text = malloc(len + (size_t)n + 1);
    if (!text) return NULL;
    memcpy(text, vs, head);
    memcpy(text + head, extra, (size_t)n);
    memcpy(text + head + n, vs + head, len - head + 1);
    return text;
}
static u8 build_whole[16];
static u32 build_packed;  // (which colors of the draw's targets take whole numbers: a bit each; see packed_for)
static int build_translated(Program *p, int id) {
    static u32 missing, built_count;
    static int asked;
    const int nth = asked++;
    if (nx2_debug) nx2_log("[nvn] program %d is the %dth to be built (shaders %x, %x)\n", (s64)id, (s64)nth, p->key[0], p->key[1]);
    if ((try_proglimit >= 0 && nth >= try_proglimit) || nth == try_progskip) return 0;
    if (nx2_vulkan) {
        // (Over Vulkan a program is two of the port's shaders as SPIR-V, by their numbers. What OpenGL's text was changed for is
        // said to the pipeline instead: an attribute given as whole numbers, which way up, where clip space's depth begins.)
        int units = 0;
        u32 storage = 0;
        u16 attrs = 0;
        memset(p->need, 0, sizeof p->need), memset(p->whole, 0, sizeof p->whole);
        p->gl = glvk_program(p->key[0], p->key[1], build_packed, p->need, p->tex, p->tex_kind, &units, &storage, &attrs);
        if (!p->gl) {
            if (missing++ < 8) nx2_log("[nvn] program %d (shaders %x, %x) isn't in the port's file of SPIR-V: nothing is drawn with it\n", (s64)id, p->key[0], p->key[1]);
            return 0;
        }
        n_programs++;
        p->ntex = units, p->nstorage = (int)storage, p->nsamp = 0, p->attrs = 0, p->state = 1;
        if (built_count++ < 6) nx2_log("[nvn] program %d made of the port's SPIR-V: %d textures, attributes %x\n", (s64)id, (s64)units, (u64)attrs);
        return 1;
    }
    pack_open();
    {
        // (<folder>/skip: the numbers of shaders, in hexadecimal, that nothing is to be drawn with. To find which one of a game's
        // makes the GPU give up, without the port being made again.)
        static u64 skip[64];
        static int skips = -1;
        if (skips < 0) {
            skips = 0;
            FILE *f = fopen(NX2_F("skip"), "r");
            unsigned long long v = 0;
            while (f && skips < 64 && fscanf(f, "%llx", &v) == 1) skip[skips++] = v;
            if (f) fclose(f);
            if (skips) nx2_log("[nvn] <folder>/skip names %d shaders: nothing is drawn with them\n", (s64)skips);
        }
        for (int k = 0; k < skips; k++)
            if (skip[k] == p->key[0] || skip[k] == p->key[1]) {
                nx2_log("[nvn] program %d (shaders %x, %x) is one <folder>/skip names: nothing is drawn with it\n", (s64)id, p->key[0], p->key[1]);
                return 0;
            }
    }
    const PackEntry *e = pack_find(p->key[0], p->key[1]);
    char *both = NULL, *apart[2] = {NULL, NULL}, *agreed = NULL;
    const char *vtext = NULL, *ftext = NULL;
    if (e) {
        both = pack_text(e);
        if (both) vtext = both, ftext = both + e->vs_len + 1;
    } else {
        const PackEntry *ev = pack_find(p->key[0], 0), *ef = pack_find(0, p->key[1]);
        if (ev && ef && p->key[0] && p->key[1]) {
            e = ev;
            apart[0] = pack_text(ev), apart[1] = pack_text(ef);
            if (apart[0] && apart[1]) agreed = vertex_agreeing(apart[0], apart[1] + 1);
            if (agreed) vtext = agreed, ftext = apart[1] + 1;
        }
    }
    if (!e) {
        if (missing++ < 8) nx2_log("[nvn] program %d (shaders %x, %x) has no translation in the port's file: nothing is drawn with it\n", (s64)id, p->key[0], p->key[1]);
        // (the first few such are written to the card, <folder>/missing-N-v.bin and -f.bin, to be looked for in the game's files)
        for (int k = 0; missing <= 4 && k < 2; k++) {
            char name[64];
            snprintf(name, sizeof name, NX2_F("missing-%u-%c.bin"), missing, k ? 'f' : 'v');
            FILE *f = p->raw[k] ? fopen(name, "wb") : NULL;
            if (f) {
                fwrite(p->raw[k], 1, p->raw_size[k], f);
                fclose(f);
            }
        }
        return 0;
    }
    if (!vtext) {
        nx2_log("[nvn] program %d: its translation couldn't be read from the port's file\n", (s64)id);
        free(both), free(apart[0]), free(apart[1]), free(agreed);
        return 0;
    }
    if (try_flat && try_flat == p->key[1]) {
        ftext = "#version 320 es\nprecision highp float;\nlayout(location = 0) out vec4 color;\nvoid main() { color = vec4(1.0, 0.0, 1.0, 1.0); }\n";
        nx2_log("[nvn] program %d: its fragment shader is a plain color instead (<folder>/try)\n", (s64)id);
    }
    if (try_still && try_still == p->key[0]) {
        vtext = "#version 320 es\nprecision highp float;\nvoid main() { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); }\n";
        nx2_log("[nvn] program %d: its vertex shader puts every vertex at one place instead (<folder>/try)\n", (s64)id);
    }
    // (A shader's text may be given in a file of its own, <folder>/shader/<its number in hexadecimal>.glsl, in place of the
    // translation's: to try a change to one shader without the whole file of them made again. Looked for only with the long log on.)
    char *instead[2] = {NULL, NULL};
    for (int k = 0; k < 2 && nx2_debug; k++) {
        char name[64];
        snprintf(name, sizeof name, NX2_F("shader/%016llx.glsl"), (unsigned long long)p->key[k]);
        FILE *f = fopen(name, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        instead[k] = n > 0 ? malloc((size_t)n + 1) : NULL;
        if (instead[k] && fread(instead[k], 1, (size_t)n, f) == (size_t)n) {
            instead[k][n] = 0;
            if (k) ftext = instead[k];
            else vtext = instead[k];
            nx2_log("[nvn] program %d: its %s shader's text is taken from %s\n", (s64)id, k ? "fragment" : "vertex", name);
        }
        fclose(f);
    }
    // the attributes the draw has as whole numbers are declared so (the shader takes their bits by U(), which is given two more forms)
    char *declared = malloc(strlen(vtext) + 400);
    memset(p->whole, 0, sizeof p->whole);
    p->attrs = 0;
    for (int k = 0; k < 16; k++) {
        char name[16];
        snprintf(name, sizeof name, " a_%d;", k);
        if (strstr(vtext, name)) p->attrs |= (u16)(1u << k);
    }
    if (declared) {
        strcpy(declared, vtext);
        int any = 0;
        for (int k = 0; k < 16; k++) {
            char was[40];
            if (!build_whole[k]) continue;
            snprintf(was, sizeof was, " in vec4 a_%d;", k);
            char *at = strstr(declared, was);
            if (!at) continue;
            memmove(at + 5, at + 4, strlen(at + 4) + 1);
            at[4] = build_whole[k] == 1 ? 'u' : 'i';
            p->whole[k] = build_whole[k], any = 1;
        }
        char *at = any ? strstr(declared, "\nvoid main()") : NULL;
        if (at) {
            static const char more[] = "\nuint U(uint x) { return x; }\nuint U(int x) { return uint(x); }";
            memmove(at + sizeof more - 1, at, strlen(at) + 1);
            memcpy(at, more, sizeof more - 1);
            vtext = declared;
            if (frame_from == ~0u) frame_from = picture;
            SAY(8, "[nvn] program %d: attributes of whole numbers declared so (%d %d %d %d %d %d %d %d)\n", (s64)id, (s64)p->whole[0], (s64)p->whole[1], (s64)p->whole[2], (s64)p->whole[3], (s64)p->whole[4], (s64)p->whole[5], (s64)p->whole[6], (s64)p->whole[7]);
        } else {
            memset(p->whole, 0, sizeof p->whole);
        }
        // With the window's origin at the upper left (the game's choice, nvnDeviceSetWindowOriginMode), NVN has what a shader
        // puts at the top of clip space in the FIRST row of whatever is drawn to, a texture's as the window's; OpenGL has it in
        // the last. So the vertex shader's place is turned over (and the faces' winding with it, where polygons are set up):
        // what is drawn into a texture and read back by its rows is then where the game reads it. (Without this a game's table of colors had its green axis backwards, white coming out magenta and black green; 2026-11-24.)
        // A vertex's place is worked out the same way by every shader that works it out from the same numbers ("invariant"): a game
        // draws its scene's depth first and then its colors where the depth is EQUAL, with other shaders, and the host's compiler
        // is free to do the sums a little differently in each unless told so. (A game's races had their depth and
        // nothing else: the picture was black behind the race's numbers; 2026-11-24.)
        {
            char *at = strstr(declared, "\nvoid main()");
            static const char more[] = "\ninvariant gl_Position;";
            if (at && !try_noinvariant) {
                memmove(at + sizeof more - 1, at, strlen(at) + 1);
                memcpy(at, more, sizeof more - 1);
                vtext = declared;
            }
        }
        // The depth of clip space from 0 (nvnDeviceSetDepthMode): the host's OpenGL is told so where it can be, and if not the
        // shader's own depth is put where OpenGL's -w..w has it. (A game's shadows are looked up by a depth worked
        // out for 0..1: with OpenGL's the whole race lay in shadow; 2026-11-24.)
        if (depth_zero && !try_nodepthmode && !nx2_vulkan) {
            if (!depth_zero_how) {
                void (*clip)(GLenum, GLenum) = (void (*)(GLenum, GLenum))eglGetProcAddress("glClipControlEXT");
                const char *ext = (const char *)glGetString(GL_EXTENSIONS);
                depth_zero_how = 2;
                if (clip && ext && strstr(ext, "GL_EXT_clip_control")) {
                    while (glGetError()) {}
                    clip(0x8CA1 /* GL_LOWER_LEFT */, 0x935F /* GL_ZERO_TO_ONE */);
                    if (!glGetError()) depth_zero_how = 1;
                }
                nx2_log("[nvn] clip space's depth from 0: %s\n", depth_zero_how == 1 ? "the host's OpenGL is told so (glClipControl)" : "each vertex shader is given a line for it");
            }
            if (depth_zero_how == 2) {
                char *end = strrchr(declared, '}');
                if (end && strstr(declared, "\nvoid main()")) {
                    strcpy(end, "    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;\n}\n");
                    vtext = declared;
                }
            }
        }
        if (origin_upper && !try_noflip) {
            char *end = strrchr(declared, '}');
            if (end && strstr(declared, "\nvoid main()")) {
                strcpy(end, "    gl_Position.y = -gl_Position.y;\n}\n");
                vtext = declared;
            }
        }
    }
    // A color that is drawn as whole numbers (the blocks of a compressed texture, made by the shader: packed_for) is declared
    // so, and given the registers as they are, not as floats.
    char *packs = build_packed ? malloc(strlen(ftext) + 64) : NULL;
    if (packs) {
        strcpy(packs, ftext);
        for (int k = 0; k < 8; k++) {
            char name[16], was[48];
            if (!(build_packed >> k & 1)) continue;
            snprintf(name, sizeof name, k ? "color%d" : "color", k);
            snprintf(was, sizeof was, " out vec4 %s;", name);
            char *at = strstr(packs, was);
            if (!at) continue;
            memmove(at + 6, at + 5, strlen(at + 5) + 1);
            at[5] = 'u';
            snprintf(was, sizeof was, "\n    %s = vec4(", name);
            at = strstr(packs, was);
            char *end = at ? strstr(at, ");") : NULL;
            if (!at || !end) continue;
            char line[160];
            int n = snprintf(line, sizeof line, "\n    %s = uvec4(", name);
            for (const char *c = at + strlen(was); c < end && n < 140;) {  // (each part is F(rN) or 0.0: rN, or 0u)
                if (c[0] == 'F' && c[1] == '(') {
                    c += 2;
                    while (c < end && *c != ')') line[n++] = *c++;
                    c++;
                } else if (c[0] == '0' && c[1] == '.') {
                    line[n++] = '0', line[n++] = 'u';
                    c += 3;
                } else {
                    line[n++] = *c++;
                }
            }
            line[n] = 0;
            memmove(at + n, end, strlen(end) + 1);  // (the new line is never the longer)
            memcpy(at, line, (size_t)n);
            SAY(12, "[nvn] program %d: its color %d is given as whole numbers (a compressed texture's blocks)\n", (s64)id, (s64)k);
        }
        ftext = packs;
    }
    const u32 vs = compile(GL_VERTEX_SHADER, vtext, id), fs = compile(GL_FRAGMENT_SHADER, ftext, id);
    free(instead[0]), free(instead[1]), free(declared), free(packs);
    free(both), free(apart[0]), free(apart[1]), free(agreed);
    p->gl = glCreateProgram();
    n_programs++;
    glAttachShader(p->gl, vs);
    glAttachShader(p->gl, fs);
    glLinkProgram(p->gl);
    GLint ok = 0;
    glGetProgramiv(p->gl, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        static char info[800];
        glGetProgramInfoLog(p->gl, sizeof info - 1, NULL, info);
        SAY(8, "[nvn] program %d (translated) doesn't link: %s\n", (s64)id, info);
        return 0;
    }
    // (its uniform blocks say their own binding points, a stage's sixteen in a row; what is asked here is how large each is)
    GLint blocks = 0;
    glGetProgramiv(p->gl, GL_ACTIVE_UNIFORM_BLOCKS, &blocks);
    memset(p->need, 0, sizeof p->need);
    for (GLint b = 0; b < blocks; b++) {
        GLint at = 0, size = 0;
        glGetActiveUniformBlockiv(p->gl, (u32)b, GL_UNIFORM_BLOCK_BINDING, &at);
        glGetActiveUniformBlockiv(p->gl, (u32)b, GL_UNIFORM_BLOCK_DATA_SIZE, &size);
        if (at >= 0 && at < 32) p->need[at] = (u32)size;
    }
    GLint storage = 0;
    glGetProgramInterfaceiv(p->gl, GL_SHADER_STORAGE_BLOCK, GL_ACTIVE_RESOURCES, &storage);
    p->nstorage = storage;
    // its textures: vtexN and ftexN, N being 8 + 2 x the binding NVN knows the texture by
    glUseProgram(p->gl);
    p->ntex = 0;
    for (int stage = 0; stage < 2; stage++)
        for (int binding = 0; binding < 32 && p->ntex < 24; binding++) {
            char name[16];
            snprintf(name, sizeof name, "%ctex%d", stage ? 'f' : 'v', 8 + 2 * binding);
            const GLint loc = glGetUniformLocation(p->gl, name);
            if (loc < 0) continue;
            glUniform1i(loc, p->ntex);
            p->tex[p->ntex][0] = (u8)stage, p->tex[p->ntex][1] = (u8)binding;
            {
                const char *names[1] = {name};
                GLuint index = 0;
                GLint type = 0;
                glGetUniformIndices(p->gl, 1, names, &index);
                if (index != GL_INVALID_INDEX) glGetActiveUniformsiv(p->gl, 1, &index, GL_UNIFORM_TYPE, &type);
                p->tex_kind[p->ntex] = type == 0x8B5F ? GL_TEXTURE_3D : type == 0x8B60 || type == 0x8DC5 ? GL_TEXTURE_CUBE_MAP :
                                       type == 0x8DC1 || type == 0x8DC4 ? GL_TEXTURE_2D_ARRAY : type == 0x900C || type == 0x900D ? GL_TEXTURE_CUBE_MAP_ARRAY : GL_TEXTURE_2D;
            }
            p->ntex++;
        }
    p->nsamp = 0;
    p->state = 1;
    if (built_count++ < 6) nx2_log("[nvn] program %d built from its translation: %d uniform blocks, %d textures\n", (s64)id, (s64)blocks, (s64)p->ntex);
    return 1;
}

static void build(Program *p, int id);
// A program whose vertex shader has an attribute one way (whole numbers, or floats) and a draw that gives it the other way: the
// program is built again for that way, and both are kept, each used for the draws it fits. (The attribute was left out of such
// a draw: a model with bones drawn with a program first used for one without came out as a blob, a game's own
// kart among them; 2026-11-24.)
static int program_misfit(const Program *p, const u8 *whole, const u32 *attrib) {
    for (int k = 0; k < 16; k++)
        if ((p->attrs >> k & 1) && (attrib[k] & 255) && (whole[k] != 0) != (build_whole[k] != 0)) return 1;
    return 0;
}
static void program_other(Program *p, int id, const u32 *attrib) {
    u8 was[16];
    const u32 gl = p->gl;
    memcpy(was, p->whole, 16);
    if (p->gl_other && !program_misfit(p, p->whole_other, attrib)) {  // (the other one fits: the two change places)
        p->gl = p->gl_other, memcpy(p->whole, p->whole_other, 16);
        p->gl_other = gl, memcpy(p->whole_other, was, 16);
        return;
    }
    if (p->gl_other) glDeleteProgram(p->gl_other);
    p->gl_other = gl, memcpy(p->whole_other, was, 16);
    const u16 attrs = p->attrs;
    p->state = 0;
    build(p, id);
    if (p->state <= 0 || program_misfit(p, p->whole, attrib)) {  // (it can't be had: as it was, and no more is asked)
        if (p->state > 0 && p->gl != gl) glDeleteProgram(p->gl);
        p->gl = gl, memcpy(p->whole, was, 16), p->gl_other = 0, p->state = 1, p->attrs = 0;
        SAY(8, "[nvn] program %d couldn't be built again for a draw's other kind of attributes\n", (s64)id);
        return;
    }
    p->attrs = attrs;
    SAY(8, "[nvn] program %d is built a second time, for draws that give its attributes the other way (whole numbers or floats)\n", (s64)id);
}
static void build(Program *p, int id) {
    p->state = -1;
    if (!p->src[0] || !p->src[1]) {
        if (p->key[0] && p->key[1]) {
            build_translated(p, id);
            return;
        }
        SAY(8, "[nvn] program %d has neither GLSL text nor compiled shaders this port knows: nothing is drawn with it\n", (s64)id);
        return;
    }
    p->ntex = 0, p->nstorage = 0;
    memset(p->need, 0, sizeof p->need);
    char names[2][8][48];
    int counts[2];
    const u32 vs = compile(GL_VERTEX_SHADER, adapt(p->src[0], 0, names[0], &counts[0]), id);
    const u32 fs = compile(GL_FRAGMENT_SHADER, adapt(p->src[1], 1, names[1], &counts[1]), id);
    p->gl = glCreateProgram();
    n_programs++;
    glAttachShader(p->gl, vs);
    glAttachShader(p->gl, fs);
    glLinkProgram(p->gl);
    GLint ok = 0;
    glGetProgramiv(p->gl, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        static char info[800];
        glGetProgramInfoLog(p->gl, sizeof info - 1, NULL, info);
        nx2_log("[nvn] program %d doesn't link: %s\n", (s64)id, info);
        return;
    }
    // A uniform block is bound by stage and number in NVN, and each stage counts from 0; OpenGL has one row of binding points. So
    // a block's point is its stage's sixteen plus the number its declaration gave.
    GLint blocks = 0;
    glGetProgramiv(p->gl, GL_ACTIVE_UNIFORM_BLOCKS, &blocks);
    int block_slot[16];
    for (GLint b = 0; b < blocks && b < 16; b++) {
        GLint declared = 0, in_fs = 0;
        glGetActiveUniformBlockiv(p->gl, (u32)b, GL_UNIFORM_BLOCK_BINDING, &declared);
        // (which stage a block is of is read from the text, by the block's name: asked of OpenGL, "is it used by the fragment
        // shader" came back no for every block, both went to the vertex stage's points, and a texture's handle was looked for in
        // the vertex stage's buffer; a Switch 2, 2026-11-23)
        char block_name[64] = {0};
        glGetActiveUniformBlockName(p->gl, (u32)b, sizeof block_name - 1, NULL, block_name);
        in_fs = block_name[0] && strstr(p->src[1], block_name) && !strstr(p->src[0], block_name);
        block_slot[b] = (in_fs ? 16 : 0) + (declared & 15);
        glUniformBlockBinding(p->gl, (u32)b, (u32)block_slot[b]);
    }
    glUseProgram(p->gl);
    p->nsamp = 0;
    for (int stage = 0; stage < 2; stage++)
        for (int k = 0; k < counts[stage] && p->nsamp < 8; k++) {
            char s_name[56];
            const char *name = names[stage][k];
            u32 len = (u32)strlen(name);
            memcpy(s_name, name, len);
            memcpy(s_name + len, "_s", 3);
            const GLint loc = glGetUniformLocation(p->gl, s_name);
            GLuint index = GL_INVALID_INDEX;
            glGetUniformIndices(p->gl, 1, &name, &index);
            if (loc < 0 || index == GL_INVALID_INDEX) continue;  // (not used by this build of the shader)
            GLint offset = -1, block = -1;
            glGetActiveUniformsiv(p->gl, 1, &index, GL_UNIFORM_OFFSET, &offset);
            glGetActiveUniformsiv(p->gl, 1, &index, GL_UNIFORM_BLOCK_INDEX, &block);
            if (offset < 0 || block < 0 || block >= 16) continue;
            glUniform1i(loc, p->nsamp);
            p->samp[p->nsamp].unit_uniform = loc, p->samp[p->nsamp].slot = block_slot[block], p->samp[p->nsamp].offset = offset;
            p->nsamp++;
        }
    p->state = 1;
    SAY(6, "[nvn] program %d built: %d uniform blocks, %d textures\n", (s64)id, (s64)blocks, (s64)p->nsamp);
    static u32 shown;
    if (nx2_debug && shown < 2) {  // (the first two, as the host's OpenGL was given them: to read beside what they draw)
        shown++;
        for (int stage = 0; stage < 2; stage++) {
            char names2[8][48];
            int count2;
            const char *a = adapt(p->src[stage], stage, names2, &count2);
            nx2_log("[nvn] program %d's %s shader, adapted:\n", (s64)id, stage ? "fragment" : "vertex");
            for (const char *q = a; *q;) {
                static char piece[400];
                u32 k = 0;
                while (*q && k < sizeof piece - 1) piece[k++] = *q++;
                piece[k] = 0;
                nx2_log("%s", piece);
            }
            nx2_log("\n");
        }
        for (GLint b = 0; b < blocks && b < 16; b++) nx2_log("[nvn]   uniform block %d is at binding point %d\n", (s64)b, (s64)block_slot[b]);
    }
}

// A color format of NVN's, for OpenGL: how it is kept, how its texels are given (0: compressed, given as they are), and the size of
// a block of texels (1x1 for a plain format). The numbers are NVN's own list of formats.
// (1 is one byte a texel: a game's sprites that take their colors from a palette are that, and read as four bytes a texel
// they came out as slivers; a Switch 2, 2026-11-23)
typedef struct {
    u8 nvn, bw, bh, bytes;
    u32 internal, given, type;
} Format;
static const Format formats[] = {
    {1, 1, 1, 1, GL_R8, GL_RED, GL_UNSIGNED_BYTE}, {2, 1, 1, 1, GL_R8_SNORM, GL_RED, GL_BYTE},
    {5, 1, 1, 2, GL_R16F, GL_RED, GL_HALF_FLOAT}, {10, 1, 1, 4, GL_R32F, GL_RED, GL_FLOAT},
    {13, 1, 1, 2, GL_RG8, GL_RG, GL_UNSIGNED_BYTE}, {14, 1, 1, 2, GL_RG8_SNORM, GL_RG, GL_BYTE},
    {17, 1, 1, 4, GL_RG16F, GL_RG, GL_HALF_FLOAT}, {22, 1, 1, 8, GL_RG32F, GL_RG, GL_FLOAT},
    {25, 1, 1, 3, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE},
    {37, 1, 1, 4, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE}, {38, 1, 1, 4, GL_RGBA8_SNORM, GL_RGBA, GL_BYTE},
    {41, 1, 1, 8, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT}, {46, 1, 1, 16, GL_RGBA32F, GL_RGBA, GL_FLOAT},
    {55, 1, 1, 4, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE}, {56, 1, 1, 4, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE},
    {57, 1, 1, 2, GL_RGBA4, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4}, {59, 1, 1, 2, GL_RGB5_A1, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1},
    {60, 1, 1, 2, GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5}, {61, 1, 1, 4, GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV},
    {63, 1, 1, 4, GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV},
    // compressed, in blocks of 4x4: DXT1 (four kinds), DXT3, DXT5, then the same for sRGB; RGTC (one and two channels); BPTC
    {65, 4, 4, 8, 0x83F0, 0, 0}, {66, 4, 4, 8, 0x83F1, 0, 0}, {67, 4, 4, 16, 0x83F2, 0, 0}, {68, 4, 4, 16, 0x83F3, 0, 0},
    {69, 4, 4, 8, 0x8C4C, 0, 0}, {70, 4, 4, 8, 0x8C4D, 0, 0}, {71, 4, 4, 16, 0x8C4E, 0, 0}, {72, 4, 4, 16, 0x8C4F, 0, 0},
    {73, 4, 4, 8, 0x8DBB, 0, 0}, {74, 4, 4, 8, 0x8DBC, 0, 0}, {75, 4, 4, 16, 0x8DBD, 0, 0}, {76, 4, 4, 16, 0x8DBE, 0, 0},
    {77, 4, 4, 16, 0x8E8C, 0, 0}, {78, 4, 4, 16, 0x8E8D, 0, 0}, {79, 4, 4, 16, 0x8E8E, 0, 0}, {80, 4, 4, 16, 0x8E8F, 0, 0},
    // ASTC, in blocks of 16 bytes and of these sizes, then the same for sRGB (A game has both)
    {121, 4, 4, 16, 0X93B0, 0, 0}, {122, 5, 4, 16, 0X93B1, 0, 0}, {123, 5, 5, 16, 0X93B2, 0, 0}, {124, 6, 5, 16, 0X93B3, 0, 0}, {125, 6, 6, 16, 0X93B4, 0, 0}, {126, 8, 5, 16, 0X93B5, 0, 0}, {127, 8, 6, 16, 0X93B6, 0, 0}, {128, 8, 8, 16, 0X93B7, 0, 0}, {129, 10, 5, 16, 0X93B8, 0, 0}, {130, 10, 6, 16, 0X93B9, 0, 0}, {131, 10, 8, 16, 0X93BA, 0, 0}, {132, 10, 10, 16, 0X93BB, 0, 0}, {133, 12, 10, 16, 0X93BC, 0, 0}, {134, 12, 12, 16, 0X93BD, 0, 0},
    {135, 4, 4, 16, 0X93D0, 0, 0}, {136, 5, 4, 16, 0X93D1, 0, 0}, {137, 5, 5, 16, 0X93D2, 0, 0}, {138, 6, 5, 16, 0X93D3, 0, 0}, {139, 6, 6, 16, 0X93D4, 0, 0}, {140, 8, 5, 16, 0X93D5, 0, 0}, {141, 8, 6, 16, 0X93D6, 0, 0}, {142, 8, 8, 16, 0X93D7, 0, 0}, {143, 10, 5, 16, 0X93D8, 0, 0}, {144, 10, 6, 16, 0X93D9, 0, 0}, {145, 10, 8, 16, 0X93DA, 0, 0}, {146, 10, 10, 16, 0X93DB, 0, 0}, {147, 12, 10, 16, 0X93DC, 0, 0}, {148, 12, 12, 16, 0X93DD, 0, 0},
};
// (ASTC, 121 to 148, only with a file <folder>/astc on the card: the host's OpenGL takes such textures without a word, and
// soon after the first of them the GPU's work fails ("completion join failed: status 8", the host's own messages; a game, 2026-11-23). Whether the two have to do with one another is what leaving them out is to show: without the file they
// are left empty, as formats this port doesn't know.)
static const Format *format_of(int f) {
    // (the file is looked for when OpenGL is started, by the host: this is asked by the game's threads too, where the C library
    // isn't to be used. Looked for here, as it first was, the game ended before its first picture.)
    if (f >= 121 && f <= 148 && astc_given <= 0) return NULL;
    for (u32 k = 0; k < sizeof formats / sizeof *formats; k++)
        if (formats[k].nvn == f) return &formats[k];
    return NULL;
}
static u32 color_format(int f, u32 *given, u32 *type) {
    const Format *x = format_of(f);
    if (!x || !x->given) return 0;
    *given = x->given, *type = x->type;
    return x->internal;
}
static u32 texel_bytes(int f) {
    const Format *x = format_of(f);
    return x && x->given ? x->bytes : 4;
}

// How the Switch's GPU keeps a texture that isn't "linear": in blocks. Each level's rows of texels (or of compressed blocks) are
// cut into pieces 64 bytes wide and 8 rows high (512 bytes), those are stacked "block height" high, and within a piece the bytes
// go in a fixed zigzag. A level's rows are padded to a multiple of 64 bytes and its height to a whole block; a smaller level
// halves the block height when it would be more than half empty. A texture of several layers has each layer's levels together,
// a layer padded to a whole block of the first level's kind. The game's files hold their textures laid out so, and hand them to
// NVN as they are. (These rules were checked on a PC against a game's texture files: the level offsets worked out
// this way are the file's own in 8963 textures of 8963, and textures read back through it are pictures; 2026-11-23.)
typedef struct {
    u64 at;  // where the level begins within a layer
    u32 pitch, block, tw, th;  // a row's bytes; the block height's log2; the level's size in texels or compressed blocks
} Level;
static u32 block_default(u32 th) {  // (what the driver picks for a first level of th rows, when a packaged texture doesn't say)
    u32 block = 4;
    while (block > 0 && th <= (8u << (block - 1))) block--;
    return block;
}
static u64 texture_layout(const Format *f, int w, int h, int levels, int block, Level *out, u32 *first_block) {
    u64 at = 0;
    for (int l = 0; l < levels && l < 16; l++) {
        const u32 lw = (u32)(w >> l) ? (u32)(w >> l) : 1, lh = (u32)(h >> l) ? (u32)(h >> l) : 1;
        const u32 tw = (lw + f->bw - 1) / f->bw, th = (lh + f->bh - 1) / f->bh;
        if (block < 0) block = (int)block_default(th);
        while (block > 0 && th <= (8u << (block - 1))) block--;
        if (!l && first_block) *first_block = (u32)block;
        const u32 pitch = (tw * f->bytes + 63) & ~63u, rows = (th + (8u << block) - 1) & ~((8u << block) - 1);
        if (out) out[l].at = at, out[l].pitch = pitch, out[l].block = (u32)block, out[l].tw = tw, out[l].th = th;
        at += (u64)pitch * rows;
    }
    return at;
}
static u64 block_address(u32 xb, u32 y, u32 gobs, u32 bh) {  // (xb: a byte's place in its row; gobs: the row's width in 64 byte pieces)
    const u64 gob = (u64)(y / (8 * bh)) * 512 * bh * gobs + (u64)(xb / 64) * 512 * bh + (u64)(y % (8 * bh) / 8) * 512;
    return gob + (xb % 64 / 32) * 256 + (y % 8 / 2) * 64 + (xb % 32 / 16) * 32 + (y % 2) * 16 + xb % 16;
}

static u32 target_of(const Texture *t) {
    if (t->window || ((t->flags & TEX_LINEAR) && t->target != 4)) return GL_TEXTURE_2D;
    return t->target == 2 ? GL_TEXTURE_3D : t->target == 4 ? GL_TEXTURE_2D_ARRAY : t->target == 8 ? GL_TEXTURE_CUBE_MAP :
           t->target == 9 ? GL_TEXTURE_CUBE_MAP_ARRAY : GL_TEXTURE_2D;
}
// one level of one layer, its texels in plain rows, given to OpenGL
static void level_give(const Texture *t, const Format *f, int level, int layer, int x, int y, int w, int h, const void *texels, u32 bytes) {
    const u32 face = t->gl_target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + (u32)layer : t->gl_target;
    if (t->gl_target == GL_TEXTURE_2D || t->gl_target == GL_TEXTURE_CUBE_MAP) {
        if (f->given) glTexSubImage2D(face, level, x, y, w, h, f->given, f->type, texels);
        else glCompressedTexSubImage2D(face, level, x, y, w, h, f->internal, (GLsizei)bytes, texels);
    } else {
        if (f->given) glTexSubImage3D(face, level, x, y, layer, w, h, 1, f->given, f->type, texels);
        else glCompressedTexSubImage3D(face, level, x, y, layer, w, h, 1, f->internal, (GLsizei)bytes, texels);
    }
}
static u8 *flat;
static u64 flat_room;
// A 3D texture's texels (its first level), laid out in blocks that are also so many slices deep: within a block the slices come
// one after the other, each as a 2D texture's piece of 64 bytes by 8 rows is. (They weren't read at all, and such a texture was
// whatever the GPU's memory held: the table a game passes its picture's colors through is one, 8x8x8, and the whole
// scene came out as colored noise; 2026-11-24.)
static void upload_3d(Texture *t, const Format *f, int id) {
    static u32 said;
    const u32 tw = ((u32)t->w + f->bw - 1) / f->bw, th = ((u32)t->h + f->bh - 1) / f->bh, td = t->depth > 0 ? (u32)t->depth : 1;
    const u32 row = tw * f->bytes, gobs = (row + 63) / 64;
    u32 by = 16, bz = 16;  // (how many pieces high a block is, and how many slices deep: halved while the texture is smaller)
    while (by > 1 && th <= 8 * (by / 2)) by /= 2;
    while (bz > 1 && td <= bz / 2) bz /= 2;
    const u64 block = 512ull * by * bz, rob = block * gobs, slice = (u64)((th + 8 * by - 1) / (8 * by)) * rob;
    const u64 total = (u64)((td + bz - 1) / bz) * slice, need = (u64)row * th * td;
    if (!f->given || total > t->room || !pool_holds(t->mem, total)) {
        if (said++ < 12)
            nx2_log("[nvn] texture %d (3D, %dx%dx%d, format %d): %s: left empty\n", (s64)id, (s64)t->w, (s64)t->h, (s64)td, (s64)t->format,
                    !f->given ? "of a format not given as plain texels" : "its texels aren't all in memory the game gave");
        return;
    }
    if (need > flat_room) {
        free(flat);
        flat_room = need + (need >> 2), flat = malloc(flat_room);
        if (!flat) {
            flat_room = 0;
            return;
        }
    }
    for (u32 z = 0; z < td; z++)
        for (u32 y = 0; y < th; y++)
            for (u32 xb = 0; xb < row; xb += 16) {
                const u64 gob = (u64)(z / bz) * slice + (u64)(y / (8 * by)) * rob + (u64)(xb / 64) * block + (u64)(z % bz) * 512 * by + (u64)(y / 8 % by) * 512;
                const u64 at = gob + (xb % 64 / 32) * 256 + (y % 8 / 2) * 64 + (xb % 32 / 16) * 32 + (y % 2) * 16;
                memcpy(flat + ((u64)z * th + y) * row + xb, t->mem + at, row - xb < 16 ? row - xb : 16);
            }
    n_uploads++, n_upload_bytes += total;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, t->w, t->h, (GLsizei)td, f->given, f->type, flat);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    const u32 error = glGetError();
    if (said++ < 12) nx2_log("[nvn] texture %d (3D, %dx%dx%d, format %d) given: blocks %u high, %u deep%s\n", (s64)id, (s64)t->w, (s64)t->h, (s64)td, (s64)t->format, (u64)by, (u64)bz, error ? "; OpenGL REFUSED it" : "");
}
static void upload_blocks(Texture *t, int id) {
    const Format *f = format_of(t->format);
    static u32 said;
    if (try_noupload || (try_nocompressed && f && !f->given) || (try_noarray && t->gl_target != GL_TEXTURE_2D)) return;
    if (f && t->gl_target == GL_TEXTURE_3D) {
        upload_3d(t, f, id);
        return;
    }
    if (!f || t->gl_target == GL_TEXTURE_3D) {
        if (said++ < 12) nx2_log("[nvn] texture %d (format %d, target %d) is of a kind whose texels aren't read here yet: it is left empty\n", (s64)id, (s64)t->format, (s64)t->target);
        return;
    }
    Level levels[16];
    u32 first = 0;
    const int count = t->levels < 16 ? t->levels : 16;
    const u64 one = texture_layout(f, t->w, t->h, count, t->block, levels, &first);
    const u64 layer_size = (one + (512u << first) - 1) & ~((u64)(512u << first) - 1);
    const int layers = t->gl_target == GL_TEXTURE_2D || try_nolayers ? 1 : t->depth;
    for (int layer = 0; layer < layers; layer++)
        for (int l = 0; l < (try_nomips ? 1 : count); l++) {
            const Level *v = &levels[l];
            const u32 row = v->tw * f->bytes, bh = 1u << v->block;
            const u64 base = layer_size * (u64)layer + v->at, span = (u64)v->pitch * ((v->th + 8 * bh - 1) & ~(8 * bh - 1));
            if (base + span > t->room) {
                if (said++ < 12) nx2_log("[nvn] texture %d: level %d of layer %d would lie past the end of the game's pool (%u of %u bytes): left as it is\n", (s64)id, (s64)l, (s64)layer, base + span, t->room);
                return;
            }
            const u64 need = (u64)row * v->th;
            if (need > flat_room) {
                free(flat);
                flat_room = need + (need >> 2), flat = malloc(flat_room);
                if (!flat) {
                    flat_room = 0;
                    return;
                }
            }
            const u8 *src = t->mem + base;
            n_uploads++, n_upload_bytes += span;
            if (try_sync >= 0 && n_draws >= (u32)try_sync) nx2_log("[nvn] texture %d: level %d of layer %d given (%u bytes)\n", (s64)id, (s64)l, (s64)layer, span);
            if (!pool_holds(src, span)) {
                if (said++ < 12) nx2_log("[nvn] texture %d: level %d of layer %d (%u bytes at %p) isn't in memory the game gave: left as it is\n", (s64)id, (s64)l, (s64)layer, span, (u64)src);
                return;
            }
            for (u32 y = 0; y < v->th; y++)
                for (u32 xb = 0; xb < row; xb += 16) memcpy(flat + (u64)y * row + xb, src + block_address(xb, y, v->pitch / 64, bh), row - xb < 16 ? row - xb : 16);
            const int lw = (t->w >> l) ? (t->w >> l) : 1, lh = (t->h >> l) ? (t->h >> l) : 1;
            level_give(t, f, l, layer, 0, 0, lw, lh, flat, (u32)need);
        }
    const u32 error = glGetError();
    if (error && said++ < 12) nx2_log("[nvn] texture %d (%dx%d, format %d, %d levels, target %d): OpenGL refused its texels (error %x)\n", (s64)id, (s64)t->w, (s64)t->h, (s64)t->format, (s64)t->levels, (s64)t->target, (u64)error);
}

// A trail, for a game that ends without a word: from the first texture of a format past 120 on, each command run and each texture
// made ready is said, and the line is on the card before anything else is done (a Switch 2 tells the port of no fault). A game ends right after its first such textures; 2026-11-23. A few thousand lines, then no more.
static int trail;
// (the trail begun from elsewhere, for so many lines: the video player asks for it when a video starts, with "atvideo" in <folder>/try)
void nx2_trail(int lines) {
    if (try_atvideo) trail = lines, nx2_log_wait(1);
}
static void texture_ready_inner(Texture *t, int id);
// a record's OpenGL texture is let go: deleted, unless other records have it too
static void texture_let_go(Texture *t) {
    if (t->gamma_gl) glDeleteTextures(1, &t->gamma_gl), t->gamma_gl = 0;
    if (t->copy_gl) glDeleteTextures(1, &t->copy_gl), t->copy_gl = 0;
    if (t->view_gl) glDeleteTextures(1, &t->view_gl), t->view_gl = 0;
    if (t->share && shared[t->share - 1].gl == t->gl) {
        if (--shared[t->share - 1].refs == 0) glDeleteTextures(1, &t->gl), shared[t->share - 1].gl = 0, shared[t->share - 1].mem = NULL, n_textures_gone++;
    } else if (t->gl) {
        glDeleteTextures(1, &t->gl), n_textures_gone++;
    }
    t->gl = 0, t->share = 0;
}
// an empty texture of a kind (one texel, black), for a sampler that has none of its kind to read
static u32 empty_texture(u32 kind) {
    static u32 made[5];
    const int which = kind == GL_TEXTURE_2D ? 0 : kind == GL_TEXTURE_2D_ARRAY ? 1 : kind == GL_TEXTURE_CUBE_MAP ? 2 : kind == GL_TEXTURE_3D ? 3 : 4;
    if (!made[which]) {
        glGenTextures(1, &made[which]);
        glBindTexture(kind, made[which]);
        static const u8 black[24];
        if (which == 0) glTexStorage2D(kind, 1, GL_RGBA8, 1, 1), glTexSubImage2D(kind, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
        else if (which == 2) {
            glTexStorage2D(kind, 1, GL_RGBA8, 1, 1);
            for (u32 face = 0; face < 6; face++) glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
        } else {
            glTexStorage3D(kind, 1, GL_RGBA8, 1, 1, which == 4 ? 6 : 1);
            glTexSubImage3D(kind, 0, 0, 0, 0, 1, 1, which == 4 ? 6 : 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
        }
        glTexParameteri(kind, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(kind, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    return made[which];
}
// A texture seen as another kind (a cube as an array of cubes, a plain one as an array of one, a cube's faces as layers...): NVN
// has that of a view of a texture, and a shader reads the kind it was written for. OpenGL's own views do it (an extension, asked
// for by name). One view is kept with each texture, the kind last asked for. (A sampler given a texture of another kind had an
// empty one instead: a game's lighting reads its cubes as arrays of cubes, and came out as noise; 2026-11-24.)
static void (*view)(GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint);
static void view_asked(void) {
    static int asked;
    if (!asked) {
        asked = 1;
        view = nx2_vulkan ? vk_glTextureView : (void (*)(GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint))eglGetProcAddress("glTextureViewOES");
        if (!view) view = (void (*)(GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint))eglGetProcAddress("glTextureViewEXT");
        nx2_log("[nvn] a texture seen as another kind: OpenGL's views %s\n", view ? "are there" : "AREN'T there (such a sampler reads an empty texture)");
    }
}
// A texture drawn into with the other gamma: NVN has a view say the format a target is drawn in, and a game draws its picture into
// plain RGBA8 through an sRGB view, so that what its shaders give (linear light) is kept as sRGB, which is what the screen shows.
// OpenGL's views do it. (The view's format was left out, the linear light went to the screen as it was, and a game's
// races were as dark as a night with no lamps; 2026-11-24.)
static u32 gamma_of(u32 base, u32 kind, u32 internal, u32 levels, u32 layers) {
    view_asked();
    const u32 other = internal == GL_RGBA8 ? GL_SRGB8_ALPHA8 : internal == GL_SRGB8_ALPHA8 ? GL_RGBA8 : 0;
    if (!view || !base || !other) return 0;
    u32 gl = 0;
    while (glGetError()) {}
    glGenTextures(1, &gl);
    view(gl, kind, base, other, 0, levels ? levels : 1, 0, layers);
    const u32 error = glGetError();
    SAY(12, "[nvn] a texture (OpenGL's %u, format %x) is drawn into as format %x%s\n", (u64)base, (u64)internal, (u64)other, error ? ": OpenGL REFUSED" : "");
    if (error) {
        glDeleteTextures(1, &gl);
        return 0;
    }
    return gl;
}
static u32 texture_gamma(Texture *t) {
    if (t->gamma_gl && t->gamma_base == t->gl) return t->gamma_gl;
    if (t->gamma_gl) glDeleteTextures(1, &t->gamma_gl), t->gamma_gl = 0;
    const u32 kind = t->gl_target;
    const u32 layers = kind == GL_TEXTURE_2D ? 1 : kind == GL_TEXTURE_CUBE_MAP ? 6 : t->depth > 0 ? (u32)t->depth : 1;
    if (kind == GL_TEXTURE_3D) return 0;
    t->gamma_gl = gamma_of(t->gl, kind, t->gl_internal, t->gl_levels, layers), t->gamma_base = t->gl;
    return t->gamma_gl;
}
static u32 texture_as(Texture *t, u32 kind) {
    view_asked();
    if (!view || !t->gl) return 0;
    if (t->view_gl && t->view_kind == kind && t->view_base == t->gl) return t->view_gl;
    const u32 from = t->gl_target;
    u32 layers = 0;
    if (kind == GL_TEXTURE_2D_ARRAY && from == GL_TEXTURE_2D) layers = 1;
    else if (kind == GL_TEXTURE_2D && (from == GL_TEXTURE_2D_ARRAY || from == GL_TEXTURE_CUBE_MAP)) layers = 1;
    else if (kind == GL_TEXTURE_CUBE_MAP_ARRAY && from == GL_TEXTURE_CUBE_MAP) layers = 6;
    else if (kind == GL_TEXTURE_CUBE_MAP && from == GL_TEXTURE_CUBE_MAP_ARRAY) layers = 6;
    else if (kind == GL_TEXTURE_2D_ARRAY && from == GL_TEXTURE_CUBE_MAP) layers = 6;
    else if (kind == GL_TEXTURE_CUBE_MAP && from == GL_TEXTURE_2D_ARRAY && t->depth >= 6 && t->w == t->h) layers = 6;
    else if (kind == GL_TEXTURE_2D_ARRAY && from == GL_TEXTURE_CUBE_MAP_ARRAY) layers = t->depth > 0 ? (u32)t->depth : 6;
    else return 0;
    if (t->view_gl) glDeleteTextures(1, &t->view_gl), t->view_gl = 0;
    while (glGetError()) {}
    glGenTextures(1, &t->view_gl);
    view(t->view_gl, kind, t->gl, t->gl_internal, 0, t->gl_levels ? t->gl_levels : 1, 0, layers);
    const u32 error = glGetError();
    SAY(40, "[nvn] a texture of kind %x (%dx%d, format %d) is seen as one of kind %x%s\n", (u64)from, (s64)t->w, (s64)t->h, (s64)t->format, (u64)kind, error ? ": OpenGL REFUSED" : "");
    if (error) {
        glDeleteTextures(1, &t->view_gl), t->view_gl = 0;
        return 0;
    }
    static const GLint read_from[6] = {GL_ZERO, GL_ONE, GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    glBindTexture(kind, t->view_gl);
    glTexParameteri(kind, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(kind, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    for (int k = 0; k < 4; k++) glTexParameteri(kind, GL_TEXTURE_SWIZZLE_R + (u32)k, read_from[t->swizzle[k] < 6 ? t->swizzle[k] : 2 + k]);
    t->view_kind = kind, t->view_base = t->gl;
    return t->view_gl;
}
static void texture_ready(Texture *t, int id) {
    static int begun;
    if (!begun && t->format > 120 && nx2_debug && try_trail) begun = 1, trail = 20000, nx2_log_wait(1);
    const int say = trail > 0 && (!t->gl || t->dirty || t->renew);
    if (say) nx2_log("[trail] texture %d: %dx%d, format %d, target %d, %d levels, depth %d, flags %x\n", (s64)id, (s64)t->w, (s64)t->h, (s64)t->format, (s64)t->target, (s64)t->levels, (s64)t->depth, (u64)t->flags);
    texture_ready_inner(t, id);
    if (say) nx2_log("[trail] texture %d ready (OpenGL's error %x)\n", (s64)id, (u64)glGetError());
}
// Texels a game hands over itself (nvnTextureWriteTexels: a box of a level, as plain rows), from any thread: kept until the
// texture is next made ready, on the thread OpenGL is used from. A game gives the table its picture's colors are
// passed through this way (8x8x8, made anew as the scene changes); the call was answered with nothing, the table was whatever
// the GPU's memory held, and the whole scene came out as colored noise (2026-11-24).
#define WRITES 64
static struct {
    int texture, level, box[6];  // (the box: x, y, z, then how wide, high and deep)
    u8 *texels;
} writes[WRITES];
static volatile int writes_busy;
#ifdef NX2_GAME_MB  // (a Switch 2 game: never used, see ARENA)
static u8 writes_room[1u << 12];
#else
static u8 writes_room[8u << 20];  // where the texels wait: begun again once none are waiting
#endif
static u32 writes_at;
static void writes_take(void) {
    while (__atomic_exchange_n(&writes_busy, 1, __ATOMIC_ACQUIRE)) svcSleepThread(20000);
}
static void writes_give(void) { __atomic_store_n(&writes_busy, 0, __ATOMIC_RELEASE); }
static void texels_written(Texture *t, int id) {
    u32 given = GL_RGBA, type = GL_UNSIGNED_BYTE;
    const int known = color_format(t->format, &given, &type) != 0;
    writes_take();
    t->written = 0;
    glBindTexture(t->gl_target, t->gl);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int k = 0; k < WRITES; k++) {
        if (!writes[k].texels || writes[k].texture != id) continue;
        const int *b = writes[k].box;
        if (known) {
            if (t->gl_target == GL_TEXTURE_2D) glTexSubImage2D(GL_TEXTURE_2D, writes[k].level, b[0], b[1], b[3], b[4], given, type, writes[k].texels);
            else if (t->gl_target == GL_TEXTURE_CUBE_MAP)
                glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (u32)(b[2] < 6 ? b[2] : 0), writes[k].level, b[0], b[1], b[3], b[4], given, type, writes[k].texels);
            else glTexSubImage3D(t->gl_target, writes[k].level, b[0], b[1], b[2], b[3], b[4], b[5], given, type, writes[k].texels);
        }
        SAY(12, "[nvn] texture %d: texels the game wrote are given (level %d, at %d,%d,%d, %dx%dx%d)%s\n", (s64)id, (s64)writes[k].level, (s64)b[0], (s64)b[1],
            (s64)b[2], (s64)b[3], (s64)b[4], (s64)b[5], known ? "" : ": NOT, its format isn't one given as plain texels");
        writes[k].texels = NULL;
    }
    int waiting = 0;
    for (int k = 0; k < WRITES; k++) waiting |= writes[k].texels != NULL;
    if (!waiting) writes_at = 0;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    writes_give();
}
static void texture_ready_inner(Texture *t, int id) {
    // (Where its texels are is worked out now, not when it was made: a pool with no memory of its own has another's given it
    // later, and a texture made in between was taken to be at the offset alone, a place that isn't there. A game
    // ended reading one such; a Switch 2, 2026-11-23.)
    if (t->pool) t->mem = t->pool->magic == NVN_MAGIC && t->pool->mem ? t->pool->mem + t->offset : NULL;
    const int depth = t->format >= F_DEPTH16 && t->format <= F_DEPTH32F_STENCIL8;
    const int linear = t->window || (t->flags & TEX_LINEAR);
    if (t->renew && t->gl) texture_let_go(t);
    t->renew = 0;
    const int asked_levels = t->levels;
    if (!t->gl && t->mem && !t->window) {  // (another record over the same memory, alike: its OpenGL texture is this one's too)
        for (u32 k = 0; k < SHARED; k++)
            if (shared[k].gl && shared[k].mem == t->mem && shared[k].w == t->w && shared[k].h == t->h && shared[k].depth == t->depth &&
                shared[k].format == t->format && shared[k].target == t->target && shared[k].levels == asked_levels) {
                shared[k].refs++, n_shared++;
                t->gl = shared[k].gl, t->share = (int)k + 1, t->gl_target = target_of(t), t->gl_internal = shared[k].internal, t->gl_levels = shared[k].gl_levels;
                t->levels = shared[k].kept_levels;
                SAY(40, "[nvn] texture %d (%dx%d, format %d) is over the same memory as another, alike: one OpenGL texture for both (%u records have it)\n", (s64)id,
                    (s64)t->w, (s64)t->h, (s64)t->format, (u64)shared[k].refs);
                break;
            }
    }
    if (!t->gl) {
        glGenTextures(1, &t->gl);
        n_textures++;
        t->gl_target = target_of(t);
        glBindTexture(t->gl_target, t->gl);
        const u32 internal = t->format == F_DEPTH16 ? GL_DEPTH_COMPONENT16 : t->format == F_DEPTH24 ? GL_DEPTH_COMPONENT24 :
                             t->format == F_DEPTH32F ? GL_DEPTH_COMPONENT32F : t->format == F_DEPTH24_STENCIL8 ? GL_DEPTH24_STENCIL8 :
                             t->format == F_DEPTH32F_STENCIL8 ? GL_DEPTH32F_STENCIL8 : GL_RGBA8;
        const Format *f = depth ? NULL : format_of(t->format);
        if (!depth && !f) SAY(8, "[nvn] texture %d has format %d, which isn't known here: taken for RGBA8\n", (s64)id, (s64)t->format);
        int levels = linear || depth ? 1 : t->levels, most = 1;
        while ((t->w >> most) || (t->h >> most)) most++;
        if (levels > most) levels = most;
        if (levels < 1) levels = 1;
        t->levels = linear || depth ? t->levels : levels;
        t->gl_internal = f ? f->internal : internal, t->gl_levels = (u32)levels;
        if (t->gl_target == GL_TEXTURE_2D || t->gl_target == GL_TEXTURE_CUBE_MAP) glTexStorage2D(t->gl_target, levels, f ? f->internal : internal, t->w, t->h);
        else glTexStorage3D(t->gl_target, levels, f ? f->internal : internal, t->w, t->h, t->depth);
        glTexParameteri(t->gl_target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(t->gl_target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        if (f && !f->given && (t->flags & 8)) {
            // (a compressed texture the game means to fill itself, by drawing: black until it does, not what the GPU's memory held)
            static const u8 none[1 << 16];
            for (int l = 0; l < levels; l++) {
                const int lw = (t->w >> l) ? (t->w >> l) : 1, lh = (t->h >> l) ? (t->h >> l) : 1;
                const u32 bytes = (u32)((lw + (int)f->bw - 1) / (int)f->bw) * (u32)((lh + (int)f->bh - 1) / (int)f->bh) * f->bytes;
                const int layers = t->gl_target == GL_TEXTURE_2D ? 1 : t->gl_target == GL_TEXTURE_CUBE_MAP ? 6 : t->depth;
                for (int layer = 0; bytes <= sizeof none && layer < layers; layer++) level_give(t, f, l, layer, 0, 0, lw, lh, none, bytes);
            }
            SAY(20, "[nvn] texture %d (%dx%d, format %d, compressed) is one the game fills by drawing: black until then\n", (s64)id, (s64)t->w, (s64)t->h, (s64)t->format);
        }
        glTexParameteri(t->gl_target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);  // (read with no sampler, it isn't repeated past its edges)
        glTexParameteri(t->gl_target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(t->gl_target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        t->swizzle_new = t->swizzle[0] != 2 || t->swizzle[1] != 3 || t->swizzle[2] != 4 || t->swizzle[3] != 5;
        t->dirty = 1;
        t->share = 0;
        for (u32 k = 0; t->mem && !t->window && k < SHARED; k++)
            if (!shared[k].gl) {
                shared[k].mem = t->mem, shared[k].w = t->w, shared[k].h = t->h, shared[k].depth = t->depth, shared[k].format = t->format, shared[k].target = t->target;
                shared[k].levels = asked_levels, shared[k].kept_levels = t->levels, shared[k].gl = t->gl, shared[k].refs = 1, shared[k].internal = t->gl_internal;
                shared[k].gl_levels = t->gl_levels, shared[k].drawn = 0;
                t->share = (int)k + 1;
                break;
            }
        const u32 error = glGetError();
        SAY(600, "[nvn] texture %d: %dx%d, format %d, flags %x, stride %d, target %d, %d levels, depth %d%s\n", (s64)id, (s64)t->w, (s64)t->h, (s64)t->format,
            (u64)t->flags, (s64)t->stride, (s64)t->target, (s64)levels, (s64)t->depth, error ? ": OpenGL REFUSED it" : "");
    }
    if (t->swizzle_new) {
        static const GLint from[6] = {GL_ZERO, GL_ONE, GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
        t->swizzle_new = 0;
        glBindTexture(t->gl_target, t->gl);
        for (int k = 0; k < 4; k++) glTexParameteri(t->gl_target, GL_TEXTURE_SWIZZLE_R + (u32)k, from[t->swizzle[k] < 6 ? t->swizzle[k] : 2 + k]);
        SAY(6, "[nvn] texture %d: its channels are read from %u %u %u %u (0 zero, 1 one, 2 to 5 red to alpha)\n", (s64)id, (u64)t->swizzle[0], (u64)t->swizzle[1], (u64)t->swizzle[2], (u64)t->swizzle[3]);
    }
    if (t->target == 2 && t->dirty)
        SAY(40, "[nvn] texture %d is a 3D one: %dx%dx%d, format %d, flags %x, stride %d, %d levels; OpenGL has it as %x; its memory %p (%s), written %d, drawn to %d\n", (s64)id,
            (s64)t->w, (s64)t->h, (s64)t->depth, (s64)t->format, (u64)t->flags, (s64)t->stride, (s64)t->levels, (u64)t->gl_target, (u64)t->mem,
            t->mem && pool_holds(t->mem, 64) ? "in a pool" : "in NO pool", (s64)t->written, (s64)t->drawn_to);
    if (t->dirty && t->mem && !depth && !t->drawn_to && !(t->share && shared[t->share - 1].drawn) && !t->window && !(t->flags & 8)) {  // (a render target's texels are OpenGL's own)
        t->dirty = 0;
        glBindTexture(t->gl_target, t->gl);
        if (t->flags & TEX_LINEAR) {
            u32 given = GL_RGBA, type = GL_UNSIGNED_BYTE;
            if (!color_format(t->format, &given, &type)) given = GL_RGBA, type = GL_UNSIGNED_BYTE;
            glPixelStorei(GL_UNPACK_ROW_LENGTH, t->stride > 0 ? (GLint)((u64)t->stride / texel_bytes(t->format)) : 0);
            if (t->gl_target == GL_TEXTURE_2D_ARRAY) {
                const u64 row = t->stride > 0 ? (u64)t->stride : (u64)t->w * texel_bytes(t->format);
                for (int layer = 0; layer < t->depth; layer++)
                    if (pool_holds(t->mem + row * (u64)t->h * (u64)layer, row * (u64)t->h))
                        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, t->w, t->h, 1, given, type, t->mem + row * (u64)t->h * (u64)layer);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t->w, t->h, given, type, t->mem);
            }
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        } else {
            upload_blocks(t, id);
        }
    }
    if (t->written) texels_written(t, id);
}

static void sampler_ready(Sampler *s) {
    if (s->gl) return;
    static const u32 filter[6] = {GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR};
    static const u32 wrap[8] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT, GL_MIRRORED_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_BORDER,
                                GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE};
    const SamplerBuilder *b = &s->b;
    glGenSamplers(1, &s->gl);
    // (the game's textures have one level: a filter that asks for more would show nothing)
    // (The smaller levels are used as the game says: a texture read with none of them, far away or at a slant, glitters. They
    // were left out when the first game drawn here had textures of one level; nomipfilter has it that way again.)
    glSamplerParameteri(s->gl, GL_TEXTURE_MIN_FILTER, (GLint)filter[b->min < 6 ? (try_nomipfilter ? (b->min & 1) : b->min) : 0]);
    glSamplerParameterf(s->gl, GL_TEXTURE_MIN_LOD, b->lod_min);
    glSamplerParameterf(s->gl, GL_TEXTURE_MAX_LOD, b->lod_max);
    glSamplerParameteri(s->gl, GL_TEXTURE_MAG_FILTER, (GLint)filter[b->mag < 2 ? b->mag : 0]);
    glSamplerParameteri(s->gl, GL_TEXTURE_WRAP_S, (GLint)wrap[b->ws & 7]);
    glSamplerParameteri(s->gl, GL_TEXTURE_WRAP_T, (GLint)wrap[b->wt & 7]);
    glSamplerParameteri(s->gl, GL_TEXTURE_WRAP_R, (GLint)wrap[b->wr & 7]);
    glSamplerParameterfv(s->gl, GL_TEXTURE_BORDER_COLOR, b->border);
    // (a shadow: the shader is given whether the depth kept passes the comparison, not the depth)
    if (b->compare == 1) {
        glSamplerParameteri(s->gl, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(s->gl, GL_TEXTURE_COMPARE_FUNC, (GLint)(b->compare_func >= 1 && b->compare_func <= 8 ? GL_NEVER + b->compare_func - 1 : GL_LEQUAL));
    }
}

// A texture as a framebuffer's target. One that isn't a plain 2D texture (an array's layer, a cube's face, a 3D texture's slice)
// is attached by its first layer: which one the game means, its view says, and that isn't followed yet. (Attached as 2D, as all
// were, OpenGL refused 57 of a game's and nothing was drawn to them; the host's own messages, 2026-11-23.)
static void attach(u32 point, Texture *t, u32 where) {
    GLint level = (GLint)(where & 0xFFFF);
    const GLint layer = (GLint)(where >> 16 & 0x7FFF);
    u32 gl = t->gl;
    if (where >> 31 && !try_nogamma) {
        const u32 other = texture_gamma(t);
        if (other) gl = other;
    }
    if (level >= t->levels) level = t->levels > 0 ? t->levels - 1 : 0;
    if (t->gl_target == GL_TEXTURE_2D) glFramebufferTexture2D(GL_FRAMEBUFFER, point, GL_TEXTURE_2D, gl, level);
    else if (t->gl_target == GL_TEXTURE_CUBE_MAP) glFramebufferTexture2D(GL_FRAMEBUFFER, point, GL_TEXTURE_CUBE_MAP_POSITIVE_X + (u32)(layer < 6 ? layer : 0), gl, level);
    else glFramebufferTextureLayer(GL_FRAMEBUFFER, point, gl, level, layer);
}
// Once a picture: the OpenGL textures of the records the game has ended are let go, and the framebuffers whose textures are gone
// or have changed. (They were let go only when a record was given to another texture, and the records are given in turn, all
// 32768 of them: a game makes some 28 textures a picture for what it draws on the way, and ends them, and within
// 200 pictures the GPU's 4 GB were full; the host's own messages, 2026-11-24.)
static u32 texture_top;
// The game's vertices and indices, in buffers of OpenGL's kept from one draw to the next: one for each place in the game's memory
// they are read from, given anew only when what is there has changed (a sum over it says so: all of a small one, a sample of a
// large one). They were given whole at every draw: a scene of models is tens of megabytes a picture so, and the host's driver
// has 64 MB for what one picture hands over; a game's first scene of models lost it the device ("arena status 2",
// the host's own messages, 2026-11-24). Those not used for ten seconds are let go (sweep).
#define KEPT_BUFFERS 8192
static struct {
    const u8 *from;
    u64 sum;
    u32 gl, room, size, used;
    u32 looked;  // the list (submit_serial) it was last summed at: within one list the game's memory is as it is
} buffers_kept[KEPT_BUFFERS];
static u64 n_given_bytes;
static u64 sum_of(const u8 *from, u64 size) {
    u64 h = 0xCBF29CE484222325ull ^ size, w;
    if (size <= 16384) {
        for (u64 i = 0; i + 8 <= size; i += 8) memcpy(&w, from + i, 8), h = (h ^ w) * 0x100000001B3ull;
        for (u64 i = size & ~7ull; i < size; i++) h = (h ^ from[i]) * 0x100000001B3ull;
        return h;
    }
    const u64 step = (size / 1024) & ~7ull;
    for (u64 i = 0; i + 8 <= size; i += step) memcpy(&w, from + i, 8), h = (h ^ w) * 0x100000001B3ull;
    for (u64 i = 0; i < 256; i += 8) memcpy(&w, from + i, 8), h = (h ^ w) * 0x100000001B3ull;
    for (u64 i = size - 256; i + 8 <= size; i += 8) memcpy(&w, from + i, 8), h = (h ^ w) * 0x100000001B3ull;
    return h;
}
// the buffer for so many bytes at a place (at least `room` bytes long, what lies past `size` being zeros), bound to a target
static void buffer_kept(u32 target, const u8 *from, u64 size, u64 room) {
    u32 at = (u32)(((u64)from >> 4) * 0x9E3779B1u) % KEPT_BUFFERS, free_at = KEPT_BUFFERS;
    for (u32 tries = 0; tries < 64; tries++, at = (at + 1) % KEPT_BUFFERS) {
        if (buffers_kept[at].from == from) break;
        if (!buffers_kept[at].gl && free_at == KEPT_BUFFERS) free_at = at;
        if (!buffers_kept[at].from && !buffers_kept[at].gl) break;
    }
    if (buffers_kept[at].from != from) {
        if (free_at == KEPT_BUFFERS) free_at = at;  // (no place free near: the one there is taken over)
        at = free_at;
        if (!buffers_kept[at].gl) glGenBuffers(1, &buffers_kept[at].gl);
        buffers_kept[at].from = from, buffers_kept[at].size = 0, buffers_kept[at].sum = 0;
    }
    if (buffers_kept[at].size > size && pool_holds(from, buffers_kept[at].size)) size = buffers_kept[at].size;  // (as much as was ever read there: one sum for the place)
    if (room < size) room = size;
    glBindBuffer(target, buffers_kept[at].gl);
    if (!try_nocache && buffers_kept[at].looked == submit_serial && buffers_kept[at].size >= size && buffers_kept[at].room >= room) {
        buffers_kept[at].used = picture;
        return;
    }
    buffers_kept[at].looked = submit_serial;
    const u64 sum = sum_of(from, size);
    buffers_kept[at].used = picture;
    if (buffers_kept[at].room < room) {
        static const u8 zeros[65536];
        buffers_kept[at].room = (u32)room;
        glBufferData(target, (GLsizeiptr)room, room <= sizeof zeros ? zeros : NULL, GL_DYNAMIC_DRAW);
    } else if (buffers_kept[at].sum == sum && buffers_kept[at].size >= size) {
        return;
    }
    glBufferSubData(target, 0, (GLsizeiptr)size, from);
    buffers_kept[at].sum = sum, buffers_kept[at].size = (u32)size;
    n_given_bytes += size;
}
static void sweep(void) {
    if (picture % 60 == 0)
        for (u32 k = 0; k < KEPT_BUFFERS; k++)
            if (buffers_kept[k].gl && picture - buffers_kept[k].used > 600)
                glDeleteBuffers(1, &buffers_kept[k].gl), buffers_kept[k].gl = 0, buffers_kept[k].room = 0, buffers_kept[k].from = (const u8 *)1;
    for (u32 k = 0; k < texture_top; k++) {
        Texture *t = &textures[k];
        if (!t->used && t->gl) texture_let_go(t);
    }
    for (u32 k = 0; k < fbo_count; k++) {
        if (!fbos[k].fbo) continue;
        int gone = fbos[k].depth != NO_TEXTURE && textures[fbos[k].depth].gl != fbos[k].depth_gl;
        for (u32 i = 0; i < fbos[k].n; i++)
            if (fbos[k].color[i] != NO_TEXTURE && textures[fbos[k].color[i]].gl != fbos[k].color_gl[i]) gone = 1;
        if (gone) glDeleteFramebuffers(1, &fbos[k].fbo), fbos[k].fbo = 0;
    }
}
// A compressed texture as a target. A game may make such a texture's blocks itself: it draws them as whole numbers, a block to
// a texel, into the texture seen as one of plain numbers a quarter the size each way (A game does, for the cubes its
// reflections are read from: BC3, 128 bits to a block). OpenGL has no such view of a compressed texture, but copies between the
// two kinds bit for bit. So: a texture of whole numbers is drawn to instead, one for each level and layer drawn to, and copied
// into the compressed one when the targets are changed. (Such a framebuffer wasn't complete and nothing was drawn: the cubes
// were whatever the GPU's memory held, and every scene glittered with it.)
#define PACKED 1024
static struct {
    u32 texture, base_gl, level, layer, gl, w, h;
} packed[PACKED];
static u32 packed_for(u32 texture, const Format *f, u32 where) {
    const Texture *t = &textures[texture];
    u32 level = where & 0xFFFF, free_at = PACKED;
    if (level >= (u32)t->levels) level = t->levels > 0 ? (u32)t->levels - 1 : 0;
    for (u32 k = 0; k < PACKED; k++) {
        if (packed[k].gl && textures[packed[k].texture].gl != packed[k].base_gl) glDeleteTextures(1, &packed[k].gl), packed[k].gl = 0;  // (its texture is gone)
        if (packed[k].gl && packed[k].texture == texture && packed[k].level == level && packed[k].layer == (where >> 16 & 0x7FFF)) return k + 1;
        if (!packed[k].gl && free_at == PACKED) free_at = k;
    }
    if (free_at == PACKED || (f->bytes != 16 && f->bytes != 8)) return 0;
    const int lw = (t->w >> level) ? (t->w >> level) : 1, lh = (t->h >> level) ? (t->h >> level) : 1;
    packed[free_at].texture = texture, packed[free_at].base_gl = t->gl, packed[free_at].level = level, packed[free_at].layer = where >> 16 & 0x7FFF;
    packed[free_at].w = (u32)(lw + f->bw - 1) / f->bw, packed[free_at].h = (u32)(lh + f->bh - 1) / f->bh;
    glGenTextures(1, &packed[free_at].gl);
    glBindTexture(GL_TEXTURE_2D, packed[free_at].gl);
    glTexStorage2D(GL_TEXTURE_2D, 1, f->bytes == 16 ? GL_RGBA32UI : GL_RGBA16UI, (GLsizei)packed[free_at].w, (GLsizei)packed[free_at].h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    SAY(12, "[nvn] texture %u (%dx%d, format %d, compressed) is drawn to: level %u, layer %u, by way of %ux%u texels of whole numbers\n", (u64)texture, (s64)t->w,
        (s64)t->h, (s64)t->format, (u64)level, (u64)(where >> 16), (u64)packed[free_at].w, (u64)packed[free_at].h);
    return free_at + 1;
}
// what was drawn in a framebuffer's compressed colors' stead is copied into them
static void packed_copy(u32 at) {
    for (u32 k = 0; k < fbos[at].n && k < 8; k++) {
        const u32 row = fbos[at].pack[k];
        if (!row || !packed[row - 1].gl) continue;
        const Texture *t = &textures[packed[row - 1].texture];
        if (!t->gl || t->gl != packed[row - 1].base_gl) continue;
        glCopyImageSubData(packed[row - 1].gl, GL_TEXTURE_2D, 0, 0, 0, 0, t->gl, t->gl_target, (GLint)packed[row - 1].level, 0, 0,
                           t->gl_target == GL_TEXTURE_2D ? 0 : (GLint)packed[row - 1].layer, (GLsizei)packed[row - 1].w, (GLsizei)packed[row - 1].h, 1);
        static u32 said;
        if (said < 6) {
            const u32 error = glGetError();
            said++, nx2_log("[nvn] the blocks drawn for texture %u (level %u, layer %u) are copied into it%s\n", (u64)packed[row - 1].texture, (u64)packed[row - 1].level,
                            (u64)packed[row - 1].layer, error ? ": OpenGL REFUSED" : "");
        }
    }
}
// The depth a draw is drawn against, for that same draw to read: a copy of it, made anew when the depth has been drawn into since
// the last one. NVN lets a shader read the depth it is tested against (a light's shape drawn where it is nearer than the scene,
// reading how far the scene is); read and tested at once, the host's GPU gave it wrong over a part of the picture, and a
// quarter of a game's races lay dark (2026-11-24).
static u32 depth_copy(Texture *d) {
    if (d->gl_target != GL_TEXTURE_2D || !d->gl) return 0;
    if (!d->copy_gl) {
        glGenTextures(1, &d->copy_gl);
        glBindTexture(GL_TEXTURE_2D, d->copy_gl);
        glTexStorage2D(GL_TEXTURE_2D, 1, d->gl_internal, d->w, d->h);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        d->copy_serial = d->serial - 1;
    }
    if (d->copy_serial != d->serial) {
        glCopyImageSubData(d->gl, GL_TEXTURE_2D, 0, 0, 0, 0, d->copy_gl, GL_TEXTURE_2D, 0, 0, 0, 0, d->w, d->h, 1);
        d->copy_serial = d->serial;
        static u32 said;
        if (said < 4) said++, nx2_log("[nvn] a depth texture (%dx%d) is copied for a draw that reads it while drawing against it%s\n", (s64)d->w, (s64)d->h, glGetError() ? ": OpenGL REFUSED" : "");
    }
    return d->copy_gl;
}
// The framebuffer for a set of targets: so many colors (a fragment shader gives each its own; a place among them may be empty,
// NO_TEXTURE) and a depth, or a depth alone (a shadow's). There was one color to a framebuffer, the first of those the game named:
// what a game draws its scenes into is four at once, and a depth alone for its shadows was taken for the screen.
static u32 fbo_for(u32 n, const u32 *color, const u32 *where, u32 depth, u32 depth_where) {
    u32 gl[8] = {0};
    for (u32 k = 0; k < n; k++)
        if (color[k] != NO_TEXTURE) {
            Texture *c = &textures[color[k]];
            c->drawn_to = 1;
            texture_ready(c, (int)color[k]);
            if (c->share) shared[c->share - 1].drawn = 1;
            gl[k] = c->gl;
        }
    Texture *d = depth != NO_TEXTURE ? &textures[depth] : NULL;
    if (d) {
        d->drawn_to = 1, texture_ready(d, (int)depth);
        if (d->share) shared[d->share - 1].drawn = 1;
    }
    const u32 dgl = d ? d->gl : 0;
    u32 at = FBOS;
    for (u32 k = 0; k < fbo_count; k++)
        if (fbos[k].fbo && fbos[k].n == n && fbos[k].depth == depth && fbos[k].depth_where == depth_where && !memcmp(fbos[k].color, color, n * 4) &&
            !memcmp(fbos[k].color_where, where, n * 4)) {
            if (fbos[k].depth_gl == dgl && !memcmp(fbos[k].color_gl, gl, n * 4)) {
                fbo_last = (int)k;
                return fbos[k].fbo;
            }
            at = k;  // (the same records, with other textures since: made again in its place)
            glDeleteFramebuffers(1, &fbos[at].fbo);
            break;
        }
    for (u32 k = 0; at == FBOS && k < fbo_count; k++)  // (a place whose framebuffer was let go)
        if (!fbos[k].fbo) at = k;
    if (at == FBOS) {
        if (fbo_count < FBOS) at = fbo_count++;
        else at = fbo_next++ % FBOS, glDeleteFramebuffers(1, &fbos[at].fbo);  // (all in use: the oldest is given up)
    }
    u32 f = 0;
    glGenFramebuffers(1, &f);
    n_framebuffers++;
    glBindFramebuffer(GL_FRAMEBUFFER, f);
    GLenum to[8] = {GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_NONE};
    u16 pack[8] = {0};
    u32 pack_mask = 0;
    for (u32 k = 0; k < n; k++)
        if (color[k] != NO_TEXTURE) {
            const Format *cf = format_of(textures[color[k]].format);
            if (cf && !cf->given) pack[k] = (u16)packed_for(color[k], cf, where[k]);
            if (pack[k]) {
                glBindFramebuffer(GL_FRAMEBUFFER, f);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + k, GL_TEXTURE_2D, packed[pack[k] - 1].gl, 0);
                pack_mask |= 1u << k;
            } else {
                attach(GL_COLOR_ATTACHMENT0 + k, &textures[color[k]], where[k]);
            }
            to[k] = GL_COLOR_ATTACHMENT0 + k;
        }
    glDrawBuffers(n ? (GLsizei)n : 1, to);
    if (!n) glReadBuffer(GL_NONE);
    if (d) {
        const int stencil = d->format == F_DEPTH24_STENCIL8 || d->format == F_DEPTH32F_STENCIL8;
        attach(stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, d, depth_where);
    }
    const u32 status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    static u32 said;
    fbo_last = (int)at;
    const Texture *c = n && color[0] != NO_TEXTURE ? &textures[color[0]] : NULL;
    if (said++ < 400 || status != GL_FRAMEBUFFER_COMPLETE)
        SAY(600, "[nvn] framebuffer %u: %u colors, the first texture %d (%dx%d, format %d, target %d, %d levels; level %u, layer %u), depth %d (%dx%d, format %d): status %x%s\n",
            (u64)at, (u64)n, (s64)(c ? (int)color[0] : -1), (s64)(c ? c->w : 0), (s64)(c ? c->h : 0), (s64)(c ? c->format : 0), (s64)(c ? c->target : 0),
            (s64)(c ? c->levels : 0), (u64)(c ? where[0] & 0xFFFF : 0), (u64)(c ? where[0] >> 16 : 0), (s64)(d ? (int)depth : -1), (s64)(d ? d->w : 0),
            (s64)(d ? d->h : 0), (s64)(d ? d->format : 0), (u64)status, status == GL_FRAMEBUFFER_COMPLETE ? "" : " (NOT complete)");
    memset(&fbos[at], 0, sizeof fbos[at]);
    fbos[at].n = n, fbos[at].depth = depth, fbos[at].depth_gl = dgl, fbos[at].depth_where = depth_where, fbos[at].fbo = f;
    memcpy(fbos[at].pack, pack, sizeof pack), fbos[at].pack_mask = pack_mask;
    memcpy(fbos[at].color, color, n * 4), memcpy(fbos[at].color_gl, gl, n * 4), memcpy(fbos[at].color_where, where, n * 4);
    return f;
}
// The window as a target. With its pictures begun at the upper left corner (the game's choice; OpenGL's own window begins at the
// lower left) it is drawn into a texture of the port's, which is copied to the screen upside down when the picture is shown.
static void window_bind(u32 w, u32 h, int gamma) {
    if (!origin_upper) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    if (!window_fbo || window_w != w || window_h != h) {
        if (window_fbo) glDeleteFramebuffers(1, &window_fbo), glDeleteTextures(1, &window_texture);
        if (window_fbo_gamma) glDeleteFramebuffers(1, &window_fbo_gamma), window_fbo_gamma = 0;
        if (window_texture_gamma) glDeleteTextures(1, &window_texture_gamma), window_texture_gamma = 0;
        glGenTextures(1, &window_texture);
        glBindTexture(GL_TEXTURE_2D, window_texture);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, (GLsizei)w, (GLsizei)h);
        glGenFramebuffers(1, &window_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, window_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, window_texture, 0);
        window_w = w, window_h = h;
        nx2_log("[nvn] the window is drawn into a texture of the port's (%ux%u) and shown turned over: status %x\n", (u64)w, (u64)h,
                (u64)glCheckFramebufferStatus(GL_FRAMEBUFFER));
    }
    if (gamma && !try_nogamma && !window_fbo_gamma) {
        window_texture_gamma = gamma_of(window_texture, GL_TEXTURE_2D, GL_RGBA8, 1, 1);
        if (window_texture_gamma) {
            glGenFramebuffers(1, &window_fbo_gamma);
            glBindFramebuffer(GL_FRAMEBUFFER, window_fbo_gamma);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, window_texture_gamma, 0);
            nx2_log("[nvn] the window is drawn into as sRGB: status %x\n", (u64)glCheckFramebufferStatus(GL_FRAMEBUFFER));
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, gamma && !try_nogamma && window_fbo_gamma ? window_fbo_gamma : window_fbo);
}
static void window_show(void) {
    if (!origin_upper || !window_fbo) return;
    GLint was = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &was);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, window_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (nx2_vulkan) glBlitFramebuffer(0, 0, (GLint)window_w, (GLint)window_h, 0, 0, gl_w ? gl_w : (GLint)window_w, gl_h ? gl_h : (GLint)window_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    else glBlitFramebuffer(0, 0, (GLint)window_w, (GLint)window_h, 0, gl_h ? gl_h : (GLint)window_h, gl_w ? gl_w : (GLint)window_w, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, (u32)was);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

// what a vertex attribute's NVN format is, to OpenGL: how many components, their type, whether they are scaled to 0..1
// (*whole: 0 when the shader is given it as a float, 1 as an unsigned whole number, 2 as a signed one)
static int attrib_format(u32 f, GLint *n, u32 *type, GLboolean *norm, int *whole) {
    // NVN's formats 1 to 48 are twelve kinds, of one to four parts: R8, R8SN, R8UI, R8I, R16F, R16, R16SN, R16UI, R16I, R32F, R32UI, R32I
    static const struct {
        u32 type;
        u8 norm, whole;
    } kinds[12] = {{GL_UNSIGNED_BYTE, 1, 0}, {GL_BYTE, 1, 0}, {GL_UNSIGNED_BYTE, 0, 1}, {GL_BYTE, 0, 2}, {GL_HALF_FLOAT, 0, 0}, {GL_UNSIGNED_SHORT, 1, 0},
                   {GL_SHORT, 1, 0}, {GL_UNSIGNED_SHORT, 0, 1}, {GL_SHORT, 0, 2}, {GL_FLOAT, 0, 0}, {GL_UNSIGNED_INT, 0, 1}, {GL_INT, 0, 2}};
    *norm = GL_FALSE, *whole = 0;
    // (61 RGB10A2, 62 the same as whole numbers, 105 its signed kind, 107 and 108 those two as plain numbers; 81 to 104 are whole
    // numbers given to the shader as floats of the same worth: six kinds, of one to four parts)
    if (f == 61 || f == 62 || f == 105 || f == 107 || f == 108) {
        *n = 4, *type = f == 105 || f == 108 ? GL_INT_2_10_10_10_REV : GL_UNSIGNED_INT_2_10_10_10_REV, *norm = f == 61 || f == 105, *whole = f == 62;
        return 1;
    }
    if (f >= 81 && f <= 104) {
        static const u32 types[6] = {GL_UNSIGNED_BYTE, GL_BYTE, GL_UNSIGNED_SHORT, GL_SHORT, GL_UNSIGNED_INT, GL_INT};
        *n = (GLint)((f - 81) / 6 + 1), *type = types[(f - 81) % 6];
        return 1;
    }
    if (f < 1 || f > 48) return 0;
    *n = (GLint)((f - 1) / 12 + 1), *type = kinds[(f - 1) % 12].type, *norm = kinds[(f - 1) % 12].norm, *whole = kinds[(f - 1) % 12].whole;
    return 1;
}
// Which of a draw's attributes are whole numbers, for the program about to be built from its translation: a translated shader has
// every attribute a vec4 whose bits it takes, and one of whole numbers (the bones a vertex hangs from) has to be declared so.
// A game's models with bones had theirs left out as a format not known: the shader read 1.0's bits as a bone's
// number, far past the end of the table of bones, and the GPU's work failed ("completion join failed"; 2026-11-24).
static void whole_of(const u32 *attrib, u32 count) {
    for (u32 k = 0; k < 16; k++) {
        GLint parts;
        u32 type;
        GLboolean norm;
        int whole = 0;
        if (k < count && (attrib[k] & 255)) attrib_format(attrib[k] & 255, &parts, &type, &norm, &whole);
        build_whole[k] = (u8)whole;
    }
}
// (frame=N: what a draw goes by, as OpenGL has it: the attributes' formats, blending, depth, the faces culled, the viewport)
static void frame_draw(const u32 *attrib, u32 count) {
    char line[200];
    int at = 0;
    for (u32 k = 0; k < count && k < 16 && at < 180; k++)
        if (attrib[k] & 255) at += snprintf(line + at, sizeof line - (size_t)at, " %u:%u/%u+%u", k, attrib[k] & 255, attrib[k] >> 8 & 15, attrib[k] >> 13 & 0xFFFF);
    line[at] = 0;
    GLint v[4] = {0}, src = 0, dst = 0, srca = 0, dsta = 0, eq = 0, func = 0, face = 0, front = 0;
    GLboolean write = 0, mask[4] = {0};
    glGetIntegerv(GL_VIEWPORT, v), glGetIntegerv(GL_BLEND_SRC_RGB, &src), glGetIntegerv(GL_BLEND_DST_RGB, &dst), glGetIntegerv(GL_BLEND_SRC_ALPHA, &srca);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &dsta), glGetIntegerv(GL_BLEND_EQUATION_RGB, &eq), glGetIntegerv(GL_DEPTH_FUNC, &func);
    glGetIntegerv(GL_CULL_FACE_MODE, &face), glGetIntegerv(GL_FRONT_FACE, &front), glGetBooleanv(GL_DEPTH_WRITEMASK, &write), glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    nx2_log("[frame]   attributes (format/stream+offset):%s\n", line);
    nx2_log("[frame]   viewport %d,%d %dx%d; blending %u (%x %x, %x %x; %x); depth test %u (%x) write %u; cull %u (%x), front %x; scissor %u; mask %u%u%u%u\n", (s64)v[0],
            (s64)v[1], (s64)v[2], (s64)v[3], (u64)glIsEnabled(GL_BLEND), (u64)src, (u64)dst, (u64)srca, (u64)dsta, (u64)eq, (u64)glIsEnabled(GL_DEPTH_TEST), (u64)func,
            (u64)write, (u64)glIsEnabled(GL_CULL_FACE), (u64)face, (u64)front, (u64)glIsEnabled(GL_SCISSOR_TEST), (u64)mask[0], (u64)mask[1], (u64)mask[2], (u64)mask[3]);
}
// (frame=N: the color at one place of what is being drawn to, read back after a draw or a clear: pixel=X,Y names the place, as
// of a picture 1920 wide and 1080 high counted from the first row kept; the middle when not given. What a draw leaves is seen so.)
static int try_pixel[2] = {960, 540};
static void frame_pixel(const char *after, int colors) {
    if (colors <= 0) return;
    GLint v[4] = {0};
    glGetIntegerv(GL_VIEWPORT, v);
    const GLint x = v[0] + try_pixel[0] * v[2] / 1920, y = v[1] + try_pixel[1] * v[3] / 1080;
    char line[240];
    int at = 0;
    for (int k = 0; k < colors && k < 8 && at < 200; k++) {  // (each of the colors drawn to at once)
        GLint kind = 0;
        if (colors > 1) glReadBuffer(GL_COLOR_ATTACHMENT0 + (u32)k);
        glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &kind);
        if (kind == GL_FLOAT || kind == GL_HALF_FLOAT || kind == GL_UNSIGNED_INT_10F_11F_11F_REV) {
            float c[4] = {0};
            glReadPixels(x, y, 1, 1, GL_RGBA, GL_FLOAT, c);
            at += snprintf(line + at, sizeof line - (size_t)at, " [%d %d %d %d]", (int)(c[0] * 1000), (int)(c[1] * 1000), (int)(c[2] * 1000), (int)(c[3] * 1000));
        } else {
            u8 c[4] = {0};
            glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
            at += snprintf(line + at, sizeof line - (size_t)at, " (%u %u %u %u)", c[0], c[1], c[2], c[3]);
        }
    }
    if (colors > 1) glReadBuffer(GL_COLOR_ATTACHMENT0);
    // (and of the first color, at 45 places spread over the viewport: how many aren't all zero, which says whether anything is there)
    int lit = 0;
    for (int gy = 0; try_grid && gy < 5; gy++)
        for (int gx = 0; gx < 9; gx++) {
            u32 c[4] = {0};
            glReadPixels(v[0] + (2 * gx + 1) * v[2] / 18, v[1] + (2 * gy + 1) * v[3] / 10, 1, 1, GL_RGBA, GL_FLOAT, c);
            if (glGetError()) glReadPixels(v[0] + (2 * gx + 1) * v[2] / 18, v[1] + (2 * gy + 1) * v[3] / 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
            lit += (c[0] | c[1] | c[2]) != 0;
        }
    GLint box[4] = {0};
    glGetIntegerv(GL_SCISSOR_BOX, box);
    while (glGetError()) {}
    nx2_log("[frame]   %s, at %d,%d:%s   ([..] thousandths, (..) of 255); lit %d of 45; scissor %d,%d %dx%d\n", after, (s64)x, (s64)y, line, (s64)lit, (s64)box[0], (s64)box[1],
            (s64)box[2], (s64)box[3]);
}
static u32 blend_factor(u32 f) {
    static const u32 to[20] = {GL_ONE, GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA,
                               GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE, GL_ONE, GL_ONE, GL_ONE, GL_ONE,
                               GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR, GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA};
    return f < 20 ? to[f] : GL_ONE;
}
static u32 stencil_op(u32 o) {
    static const u32 to[9] = {GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
    return o < 9 ? to[o] : GL_KEEP;
}
static u32 gl_face(u32 f) { return f == 1 ? GL_FRONT : f == 2 ? GL_BACK : GL_FRONT_AND_BACK; }

#define QUADS 16384
static u32 quad_index[QUADS * 6];

// The commands of one list, done. What the draws need (the program, its buffers, the vertex layout) is kept as it is set, and
// given to OpenGL at each draw.
static void h_run(const u64 *c, u32 count) {
    static struct {
        Program *program;
        int program_id;
        u32 attrib[16], nattrib;
        u64 stream[16];
        struct {
            const u8 *ptr;
            u64 size;
        } vertices[16], uniforms[32];
        struct {
            const u8 *ptr;
            u64 size;
        } storage[16];  // storage buffers (a stage's eight in a row), which a translated shader reads as whole words
        u64 tex[2][32];  // the textures bound by number (nvnCommandBufferBindTexture), a stage's in a row
        struct {
            const u8 *ptr;
            u64 size;
            u32 serial;
        } loaded[16];
        u32 stencil_ref[2], stencil_vmask[2], stencil_func[2], draws, skipped;
        int offscreen;  // the targets aren't the window
        int render_off;  // the game has said nothing is to be drawn for now (nvnCommandBufferSetRenderEnable)
        int colors;      // how many colors the targets are
        int depth_test;  // the depth test is on
        int depth_write; // and what passes it is written
        int reads_depth; // a texture the draw reads is the targets' depth
        int undepthed;   // the draw goes to the targets without their depth (see PREPARE), to be put back after it
        int into_3d;     // the first of them is a 3D texture's slice
        int fbo;        // which framebuffer they are, by the order made in (-1: the window)
    } s;
    submit_serial++;
    const u64 *end = c + count;
    if (nx2_vulkan) glvk_flip(origin_upper && !try_noflip), glvk_depth_zero(depth_zero && !try_nodepthmode);
    const u64 tick_run = armGetSystemTick();
    spent.lists++;
    flow_turn++;
    if (framing) nx2_log("[frame] a list begins: %u words at %p\n", (u64)count, (u64)c);
#define PREPARE(last_vertex) \
    n_asked++; \
    const u64 tick0_ = armGetSystemTick(); \
    if (framing) \
        nx2_log("[frame] draw: program %d (%x, %x; %s), framebuffer %d, %u vertices, %s\n", (s64)s.program_id, s.program ? s.program->key[0] : 0, \
                s.program ? s.program->key[1] : 0, !s.program ? "none" : s.program->state > 0 ? "built" : s.program->state < 0 ? "NOT built" : "new", (s64)s.fbo, \
                (u64)(last_vertex), s.render_off ? "drawing off" : "drawing on"); \
    if (s.render_off) { \
        s.skipped++; \
        break; \
    } \
    if (try_trailat >= 0 && n_asked == (u32)try_trailat + 1) trail = 600, nx2_log_wait(1); \
    if (try_key && trail <= 0 && s.program && (s.program->key[0] == try_key || s.program->key[1] == try_key)) try_key = 0, trail = 400, nx2_log_wait(1); \
    if (trail > 0) { \
        nx2_log("[trail] draw %u asked for: program %d, framebuffer %d, %u attributes\n", (u64)n_asked - 1, (s64)s.program_id, (s64)s.fbo, (u64)s.nattrib); \
        for (u32 k = 0; k < 16; k++) \
            if (s.storage[k].ptr) nx2_log("[trail]   storage %u: %u bytes at %p (%s)\n", (u64)k, s.storage[k].size, (u64)s.storage[k].ptr, pool_holds(s.storage[k].ptr, s.storage[k].size) ? "in a pool" : "NOT in a pool"); \
        for (u32 k = 0; k < s.nattrib && k < 16; k++) \
            if (s.attrib[k] & 255) nx2_log("[trail]   attribute %u: format %u, stream %u, offset %u; the stream %u bytes at %p, stride %u\n", (u64)k, (u64)(s.attrib[k] & 255), \
                    (u64)(s.attrib[k] >> 8 & 15), (u64)(s.attrib[k] >> 13 & 0xFFFF), s.vertices[s.attrib[k] >> 8 & 15].size, (u64)s.vertices[s.attrib[k] >> 8 & 15].ptr, \
                    (u64)(u32)s.stream[s.attrib[k] >> 8 & 15]); \
    } \
    if ((try_drawlimit >= 0 && n_asked > (u32)try_drawlimit && s.offscreen) || (try_drawskip >= 0 && n_asked == (u32)try_drawskip + 1)) { \
        static u32 said_limit; \
        if (said_limit++ < 3) \
            nx2_log("[nvn] draw %u asked for isn't done (<folder>/try): program %d (shaders %x, %x), framebuffer %d, %d textures\n", (u64)n_asked - 1, \
                    (s64)s.program_id, s.program ? s.program->key[0] : 0, s.program ? s.program->key[1] : 0, (s64)s.fbo, (s64)(s.program ? s.program->ntex : 0)); \
        s.skipped++; \
        break; \
    } \
    if (s.offscreen && ((try_fbolimit >= 0 && s.fbo >= try_fbolimit) || s.fbo == try_fboskip)) { \
        s.skipped++; \
        break; \
    } \
    if (try_nooffscreen && s.offscreen) { \
        s.skipped++; \
        break; \
    } \
    if (!s.program || s.program->state < 0) { \
        static u32 said_skip; \
        if (nx2_debug && said_skip++ < 12) \
            nx2_log("[nvn] a draw left out: program %d %s (its shaders' numbers %x, %x)\n", (s64)s.program_id, \
                    !s.program ? "isn't one made here" : "couldn't be built", s.program ? s.program->key[0] : 0, s.program ? s.program->key[1] : 0); \
        s.skipped++; \
        break; \
    } \
    if (!s.program->state) { \
        build_packed = s.offscreen && s.fbo >= 0 ? fbos[s.fbo].pack_mask : 0; \
        whole_of(s.attrib, s.nattrib), build(s.program, s.program_id); \
    } \
    if (s.program->state < 0) { \
        s.skipped++; \
        break; \
    } \
    if (s.program->attrs) { \
        whole_of(s.attrib, s.nattrib); \
        if (program_misfit(s.program, s.program->whole, s.attrib)) { \
            build_packed = s.offscreen && s.fbo >= 0 ? fbos[s.fbo].pack_mask : 0; \
            program_other(s.program, s.program_id, s.attrib); \
        } \
    } \
    glUseProgram(s.program->gl); \
    if (framing) frame_draw(s.attrib, s.nattrib); \
    if (s.into_3d) { \
        static u32 told_draw; \
        if (told_draw++ < 12) { \
            nx2_log("[nvn] a draw into a 3D texture: program %d (%x, %x), %u vertices, channel masks %x\n", (s64)s.program_id, s.program->key[0], s.program->key[1], (u64)(last_vertex), (u64)channel_mask); \
            frame_draw(s.attrib, s.nattrib); \
        } \
    } \
    if (trail > 0) nx2_log("[trail]   program in use (%d textures, %d by handle; its shaders' numbers %x, %x)\n", (s64)s.program->ntex, (s64)s.program->nsamp, s.program->key[0], s.program->key[1]); \
    if (nx2_debug && s.draws < 12) { \
        GLint fb = 0, vp[4] = {0}; \
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb), glGetIntegerv(GL_VIEWPORT, vp); \
        nx2_log("[nvn] draw %u: program %d, framebuffer %d, viewport %d,%d %dx%d, %u attributes, blending %u, depth test %u\n", (u64)s.draws, \
                (s64)s.program_id, (s64)fb, (s64)vp[0], (s64)vp[1], (s64)vp[2], (s64)vp[3], (u64)s.nattrib, (u64)glIsEnabled(GL_BLEND), \
                (u64)glIsEnabled(GL_DEPTH_TEST)); \
        for (u32 k = 0; k < 32; k++) \
            if (s.uniforms[k].ptr) { \
                const u64 *w = (const u64 *)s.uniforms[k].ptr; \
                const u32 n = (u32)(s.uniforms[k].size / 8); \
                nx2_log("[nvn]   uniforms %u at %p: %u bytes: %x %x %x %x %x %x %x %x %x %x\n", (u64)k, (u64)w, s.uniforms[k].size, n > 0 ? w[0] : 0, n > 1 ? w[1] : 0, \
                        n > 2 ? w[2] : 0, n > 3 ? w[3] : 0, n > 4 ? w[4] : 0, n > 5 ? w[5] : 0, n > 6 ? w[6] : 0, n > 7 ? w[7] : 0, n > 8 ? w[8] : 0, \
                        n > 9 ? w[9] : 0); \
                u32 first = 0; \
                while (first < n && !w[first]) first++; \
                if (first && first < n) \
                    nx2_log("[nvn]     its first number that isn't zero is at byte %u: %x %x %x %x\n", (u64)first * 8, w[first], first + 1 < n ? w[first + 1] : 0, \
                            first + 2 < n ? w[first + 2] : 0, first + 3 < n ? w[first + 3] : 0); \
                else if (first) nx2_log("[nvn]     all of it is zero (%s)\n", pool_holding((u64)w) ? "in a pool" : "in NO pool known"); \
            } \
        if (s.vertices[0].ptr && s.vertices[0].size >= 64) { \
            const u32 *v = (const u32 *)s.vertices[0].ptr; \
            nx2_log("[nvn]   the vertices begin: %x %x %x %x  %x %x %x %x  %x %x %x %x\n", (u64)v[0], (u64)v[1], (u64)v[2], (u64)v[3], (u64)v[4], (u64)v[5], \
                    (u64)v[6], (u64)v[7], (u64)v[8], (u64)v[9], (u64)v[10], (u64)v[11]); \
        } \
        for (u32 k = 0; k < 2; k++) \
            for (u32 j = 0; j < 32; j++) \
                if (s.tex[k][j]) { \
                    const u32 ti = texture_of[s.tex[k][j] & 32767]; \
                    nx2_log("[nvn]   texture bound for stage %u, number %u: handle %x, which is texture %d (%ux%u, format %u)\n", (u64)k, (u64)j, s.tex[k][j], \
                            (s64)ti - 1, ti ? (u64)textures[ti - 1].w : 0, ti ? (u64)textures[ti - 1].h : 0, ti ? (u64)textures[ti - 1].format : 0); \
                } \
        for (u32 k = 0; k < s.nattrib; k++) \
            nx2_log("[nvn]   attribute %u: format %u, stream %u, offset %u; the stream: %p, %u bytes, stride %u\n", (u64)k, (u64)(s.attrib[k] & 255), \
                    (u64)(s.attrib[k] >> 8 & 15), (u64)(s.attrib[k] >> 13 & 0xFFFF), (u64)s.vertices[s.attrib[k] >> 8 & 15].ptr, \
                    s.vertices[s.attrib[k] >> 8 & 15].size, (u64)(u32)s.stream[s.attrib[k] >> 8 & 15]); \
    } \
    const u64 tick1_ = armGetSystemTick(); \
    spent.program += tick1_ - tick0_; \
    for (u32 k = 0; k < 32; k++) { \
        const u64 need = s.program->need[k]; \
        u64 have = s.uniforms[k].ptr ? s.uniforms[k].size : 0; \
        if (have && !pool_holds(s.uniforms[k].ptr, have)) { \
            SAY(12, "[nvn] a draw's uniform buffer (%u bytes at %p) isn't in memory the game gave: taken for none\n", have, (u64)s.uniforms[k].ptr); \
            if (need) { \
                n_uniform_misses++; \
                static u32 said_needed; \
                if (said_needed++ < 6) { \
                    nx2_log("[nvn] program %d needs %u bytes of uniform buffer %u, and what the game bound (%u bytes at %p) isn't in memory it gave\n", (s64)s.program_id, need, (u64)k, have, (u64)s.uniforms[k].ptr); \
                    pools_near((u64)s.uniforms[k].ptr); \
                } \
            } \
            have = 0; \
        } \
        /* (A translated program says how large each of its blocks is, and no more than that is given: a game may bind 64 KB to  \
           every program it uses, as a game's engine does, and given whole at each draw the host's OpenGL was made a \
           new 64 KB buffer hundreds of times a picture. The game ended inside this step; a Switch 2, 2026-11-23.) */ \
        const int exact = s.program->key[0] && !s.program->src[0]; \
        if (exact ? !need : !have && !need) continue; \
        if (exact && have > need) have = need;         /* (A block read by a number worked out as the shader runs, a model's bones, is given some way past what the game bound:            a game draws a model's coarser forms with bones past the count it binds, which lie in its memory right            after, and the console reads them there. Given only what was bound, the player's own kart and driver were drawn with            whatever lay after in the port's buffer: a blob, spikes, or shapes across the whole screen; 2026-11-24.) */         if (exact && need == 65536 && have && have < need && !try_nomorebones) {             u64 more = ((have + 4095) & ~4095ull) + 4096;             if (more > need) more = need;             if (!pool_holds(s.uniforms[k].ptr, more)) { const u64 left_ = pool_left(s.uniforms[k].ptr); more = left_ > have ? (left_ < more ? left_ : more) : have; }             have = more;         } \
        if (trail > 0) nx2_log("[trail]   uniforms %u: %u bytes of the game's at %p, %u wanted\n", (u64)k, have, (u64)s.uniforms[k].ptr, need); \
        /* (The buffer is made once, as large as has been wanted of it, and written into at each draw: made anew at each, as it  \
           was, a program whose block is 64 KB (one whose shader reads its block by a number worked out as it runs) had the      \
           host's OpenGL make a 64 KB buffer at every draw, and the game ended in that step again.) */ \
        const u64 room = need > have ? need : have; \
        if (!try_oldubo && room <= (4u << 20)) { \
            if (!have) { /* (a block the program has and the game gave nothing for: zeros) */ \
                if (!flow_zero) { \
                    static const u8 zeros[65536]; \
                    glGenBuffers(1, &flow_zero); \
                    glBindBuffer(GL_UNIFORM_BUFFER, flow_zero); \
                    glBufferData(GL_UNIFORM_BUFFER, sizeof zeros, zeros, GL_STATIC_DRAW); \
                } \
                if (room <= 65536) { \
                    glBindBufferRange(GL_UNIFORM_BUFFER, k, flow_zero, 0, (GLsizeiptr)room); \
                    flow_bound[k].buf = 0; \
                    continue; \
                } \
            } \
            const u32 take = (u32)((room + 255) & ~255ull); \
            const u32 row_ = (u32)((((u64)s.uniforms[k].ptr >> 4) * 0x9E3779B1u) >> 24) & (FLOWN - 1); \
            if (have && !try_nocache && flown[row_].ptr == s.uniforms[k].ptr && flown[row_].turn == flow_turn && flown[row_].have >= have && flown[row_].take >= room) { \
                n_blocks_shared++; \
                if (flow_bound[k].buf != flow[flow_now] || flow_bound[k].at != flown[row_].at || flow_bound[k].room != (u32)room) { \
                    glBindBufferRange(GL_UNIFORM_BUFFER, k, flow[flow_now], (GLintptr)flown[row_].at, (GLsizeiptr)room); \
                    flow_bound[k].buf = flow[flow_now], flow_bound[k].at = flown[row_].at, flow_bound[k].room = (u32)room, n_blocks_bound++; \
                } \
                continue; \
            } \
            if (!flow[0] || flow_at + take > FLOW_ROOM) flow_next(); \
            glBindBuffer(GL_UNIFORM_BUFFER, flow[flow_now]); \
            if (have) glBufferSubData(GL_UNIFORM_BUFFER, (GLintptr)flow_at, (GLsizeiptr)have, s.uniforms[k].ptr); \
            glBindBufferRange(GL_UNIFORM_BUFFER, k, flow[flow_now], (GLintptr)flow_at, (GLsizeiptr)room); \
            flow_bound[k].buf = flow[flow_now], flow_bound[k].at = flow_at, flow_bound[k].room = (u32)room, n_blocks_given++, n_blocks_bound++, n_block_bytes += have; \
            if (have) flown[row_].ptr = s.uniforms[k].ptr, flown[row_].have = (u32)have, flown[row_].take = take, flown[row_].at = flow_at, flown[row_].turn = flow_turn; \
            flow_at += take; \
            continue; \
        } \
        glBindBuffer(GL_UNIFORM_BUFFER, ubo[k]); \
        if (ubo_room[k] < room) { \
            ubo_room[k] = (u32)(room < 4096 ? 4096 : room); \
            glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)ubo_room[k], NULL, GL_DYNAMIC_DRAW); \
        } \
        if (have) glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)have, s.uniforms[k].ptr); \
        glBindBufferBase(GL_UNIFORM_BUFFER, k, ubo[k]); \
    } \
    if (trail > 0) nx2_log("[trail]   uniforms given\n"); \
    const u64 tick2_ = armGetSystemTick(); \
    spent.uniforms += tick2_ - tick1_; \
    /* (A storage buffer is bound at every place, the game's or an empty one: a shader that reads one with none bound has the GPU  \
       read nowhere, and its work fails. A game's text is drawn so, and with its first line of text the driver said \
       "completion join failed" and the game ended; found by leaving draws out one at a time, 2026-11-24.) */ \
    for (u32 k = 0; k < 16 && s.program->nstorage; k++) { \
        const int have = !try_nostorage && s.storage[k].ptr && s.storage[k].size && pool_holds(s.storage[k].ptr, s.storage[k].size); \
        const u64 room = have ? s.storage[k].size : 4096; \
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, sbo[k]); \
        if (sbo_room[k] < room) { \
            sbo_room[k] = (u32)(room < 65536 ? 65536 : room); \
            glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)sbo_room[k], NULL, GL_DYNAMIC_DRAW); \
            static const u8 nothing_there[65536]; \
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof nothing_there, nothing_there); \
        } \
        if (have) glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)s.storage[k].size, s.storage[k].ptr); \
        else SAY(6, "[nvn] a program reads storage buffer %u, and the game has bound none there: given an empty one\n", (u64)k); \
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, k, sbo[k]); \
    } \
    s.reads_depth = 0; \
    for (int k = 0; k < s.program->ntex; k++) { \
        const u64 handle = s.tex[s.program->tex[k][0]][s.program->tex[k][1]]; \
        const u32 ti = texture_of[handle & 32767], si = sampler_of[handle >> 32 & 4095]; \
        glActiveTexture(GL_TEXTURE0 + (u32)k); \
        if (trail > 0) \
            nx2_log("[trail]   sampler %d (stage %d, number %d): handle %x, texture %d: %dx%d, format %d, target %d, %d levels, depth %d\n", (s64)k, (s64)s.program->tex[k][0], \
                    (s64)s.program->tex[k][1], handle, (s64)ti - 1, (s64)(ti ? textures[ti - 1].w : 0), (s64)(ti ? textures[ti - 1].h : 0), (s64)(ti ? textures[ti - 1].format : 0), \
                    (s64)(ti ? textures[ti - 1].target : 0), (s64)(ti ? textures[ti - 1].levels : 0), (s64)(ti ? textures[ti - 1].depth : 0)); \
        /* (A sampler is given a texture of the kind it reads, or an empty one of that kind: a shader that reads an array, given a \
           plain texture, has the GPU's work fail. A game's text is drawn from an array of glyphs, which this had as  \
           a plain texture: found by leaving that program out, 2026-11-24.) */ \
        const u32 kind = s.program->tex_kind[k] ? s.program->tex_kind[k] : GL_TEXTURE_2D; \
        if (ti) texture_ready(&textures[ti - 1], (int)ti - 1); \
        if (ti && s.offscreen && s.fbo >= 0 && fbos[s.fbo].depth_gl && textures[ti - 1].gl == fbos[s.fbo].depth_gl && kind == GL_TEXTURE_2D) { \
            const u32 copy = depth_copy(&textures[fbos[s.fbo].depth]); \
            if (copy) { \
                glActiveTexture(GL_TEXTURE0 + (u32)k); \
                glBindTexture(GL_TEXTURE_2D, copy); \
                if (si) sampler_ready(&samplers[si - 1]); \
                glBindSampler((u32)k, si && !try_nosampler ? samplers[si - 1].gl : 0); \
                continue; \
            } \
            s.reads_depth = 1; \
        } \
        const u32 seen_as = ti && textures[ti - 1].gl_target != kind ? texture_as(&textures[ti - 1], kind) : 0; \
        if (ti && textures[ti - 1].gl_target == kind) { \
            glBindTexture(kind, textures[ti - 1].gl); \
        } else if (seen_as) { \
            glBindTexture(kind, seen_as); \
        } else { \
            if (ti) SAY(12, "[nvn] a sampler reads a texture of kind %x and the game's texture %d is of kind %x: given an empty one\n", (u64)kind, (s64)ti - 1, (u64)textures[ti - 1].gl_target); \
            glBindTexture(kind, empty_texture(kind)); \
        } \
        if (si) sampler_ready(&samplers[si - 1]); \
        glBindSampler((u32)k, si && !try_nosampler ? samplers[si - 1].gl : 0); \
        if (framing) \
            nx2_log("[frame]   texture unit %u: handle %x, texture %d (%dx%d, format %d, target %d, %d levels, channels from %u%u%u%u, OpenGL's %u), sampler %d (filters %u %u, compared %u)\n", \
                    (u64)k, handle, (s64)ti - 1, (s64)(ti ? textures[ti - 1].w : 0), (s64)(ti ? textures[ti - 1].h : 0), (s64)(ti ? textures[ti - 1].format : 0), \
                    (s64)(ti ? textures[ti - 1].target : 0), (s64)(ti ? textures[ti - 1].levels : 0), (u64)(ti ? textures[ti - 1].swizzle[0] : 0), \
                    (u64)(ti ? textures[ti - 1].swizzle[1] : 0), (u64)(ti ? textures[ti - 1].swizzle[2] : 0), (u64)(ti ? textures[ti - 1].swizzle[3] : 0), \
                    (u64)(ti ? textures[ti - 1].gl : 0), (s64)si - 1, (u64)(si ? samplers[si - 1].b.min : 0), (u64)(si ? samplers[si - 1].b.mag : 0), \
                    (u64)(si ? samplers[si - 1].b.compare : 0)); \
    } \
    if (trail > 0) nx2_log("[trail]   textures bound\n"); \
    for (int k = 0; k < s.program->nsamp; k++) { \
        const int slot = s.program->samp[k].slot, at = s.program->samp[k].offset; \
        u64 handle = 0; \
        if (s.uniforms[slot].ptr && (u64)at + 8 <= s.uniforms[slot].size) memcpy(&handle, s.uniforms[slot].ptr + at, 8); \
        const u32 ti = texture_of[handle & 32767], si = sampler_of[handle >> 32 & 4095]; \
        if (nx2_debug && s.draws < 12) \
            nx2_log("[nvn]   draw %u: sampler %d reads its handle at %d of slot %d (%p, %u bytes): %x -> texture %d, sampler %d\n", (u64)s.draws, (s64)k, \
                    (s64)at, (s64)slot, (u64)s.uniforms[slot].ptr, s.uniforms[slot].size, handle, (s64)ti - 1, (s64)si - 1); \
        glActiveTexture(GL_TEXTURE0 + (u32)k); \
        if (ti) { \
            texture_ready(&textures[ti - 1], (int)ti - 1); \
            if (s.offscreen && s.fbo >= 0 && fbos[s.fbo].depth_gl && textures[ti - 1].gl == fbos[s.fbo].depth_gl) s.reads_depth = 1; \
            glBindTexture(textures[ti - 1].gl_target, textures[ti - 1].gl); \
        } else { \
            SAY(6, "[nvn] a draw's texture handle %x names no registered texture\n", handle); \
            glBindTexture(GL_TEXTURE_2D, 0); \
        } \
        if (si) sampler_ready(&samplers[si - 1]); \
        glBindSampler((u32)k, si && !try_nosampler ? samplers[si - 1].gl : 0); \
        if (framing) \
            nx2_log("[frame]   texture unit %u: handle %x, texture %d (%dx%d, format %d, target %d, %d levels, channels from %u%u%u%u, OpenGL's %u), sampler %d (filters %u %u, compared %u)\n", \
                    (u64)k, handle, (s64)ti - 1, (s64)(ti ? textures[ti - 1].w : 0), (s64)(ti ? textures[ti - 1].h : 0), (s64)(ti ? textures[ti - 1].format : 0), \
                    (s64)(ti ? textures[ti - 1].target : 0), (s64)(ti ? textures[ti - 1].levels : 0), (u64)(ti ? textures[ti - 1].swizzle[0] : 0), \
                    (u64)(ti ? textures[ti - 1].swizzle[1] : 0), (u64)(ti ? textures[ti - 1].swizzle[2] : 0), (u64)(ti ? textures[ti - 1].swizzle[3] : 0), \
                    (u64)(ti ? textures[ti - 1].gl : 0), (s64)si - 1, (u64)(si ? samplers[si - 1].b.min : 0), (u64)(si ? samplers[si - 1].b.mag : 0), \
                    (u64)(si ? samplers[si - 1].b.compare : 0)); \
    } \
    if (trail > 0) nx2_log("[trail]   textures by handle bound\n"); \
    const u64 tick3_ = armGetSystemTick(); \
    spent.textures += tick3_ - tick2_; \
    /* (A draw that reads the depth it is also given as a target, and doesn't test against it, is drawn to the same colors with \
       no depth: read and drawn to at once, the host's GPU had the depth wrong over a part of the picture. A game \
       works out how far each pixel is so, and a quarter of its picture lay in shadow; 2026-11-24.) */ \
    if (s.depth_test && s.depth_write && s.offscreen && s.fbo >= 0 && fbos[s.fbo].depth != NO_TEXTURE) textures[fbos[s.fbo].depth].serial++; \
    if (s.reads_depth && !s.depth_test && s.offscreen && s.fbo >= 0 && fbos[s.fbo].n) { \
        const int last = fbo_last; \
        u32 colors_[8], where_[8]; \
        const u32 n_ = fbos[s.fbo].n; \
        memcpy(colors_, fbos[s.fbo].color, sizeof colors_), memcpy(where_, fbos[s.fbo].color_where, sizeof where_); \
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_for(n_, colors_, where_, NO_TEXTURE, 0)); \
        fbo_last = last, s.undepthed = 1; \
    } else if (s.reads_depth) { \
        SAY(6, "[nvn] a draw reads the depth it also tests against (program %d): not followed, it may come out wrong\n", (s64)s.program_id); \
    } \
    for (u32 k = 0; k < 16; k++) { \
        const u32 a = k < s.nattrib ? s.attrib[k] : 0, format = a & 255, stream = a >> 8 & 15, offset = a >> 13 & 0xFFFF; \
        GLint parts = 0; /* (not "n": that is the draw's count of vertices, which this macro is given by name) */ \
        u32 type = 0; \
        GLboolean norm = 0; \
        int whole = 0; \
        const int wants = s.program->whole[k]; /* (how the program built has it: whole numbers, or not) */ \
        if (!format || !attrib_format(format, &parts, &type, &norm, &whole) || !s.vertices[stream].ptr) { \
            if (format && s.vertices[stream].ptr) SAY(6, "[nvn] a vertex attribute has format %u, which isn't known here: left out\n", (u64)format); \
            glDisableVertexAttribArray(k); \
            if (wants) glVertexAttribI4ui(k, 0, 0, 0, 1); \
            continue; \
        } \
        const u32 stride = (u32)s.stream[stream]; \
        /* (What OpenGL is given is never shorter than what the draw reads: a stream the game bound with fewer bytes than its     \
           vertices take is filled out with zeros. A game draws four vertices from streams of sixteen bytes, one     \
           vertex's worth; given as they were, the GPU read past them and its work failed, the game ending with the driver's      \
           "completion join failed". Found with the trail, 2026-11-24.) */ \
        const u64 reads = (s.stream[stream] >> 32) || (u64)(last_vertex) * stride <= s.vertices[stream].size ? 0 : (u64)(last_vertex) * stride + 32; \
        u64 need = (u64)(last_vertex) * stride; \
        if (!need || need > s.vertices[stream].size) need = s.vertices[stream].size; \
        if (!pool_holds(s.vertices[stream].ptr, need)) { \
            SAY(12, "[nvn] a draw's vertices (%u bytes at %p) aren't in memory the game gave: left out\n", need, (u64)s.vertices[stream].ptr); \
            glDisableVertexAttribArray(k); \
            if (wants) glVertexAttribI4ui(k, 0, 0, 0, 1); \
            continue; \
        } \
        if (reads > need) SAY(6, "[nvn] a draw reads %u bytes of a vertex stream the game bound with %u: filled out\n", reads, s.vertices[stream].size); \
        buffer_kept(GL_ARRAY_BUFFER, s.vertices[stream].ptr, need, reads); \
        if (!nx2_vulkan && (wants != 0) != (whole != 0)) { \
            /* (the program was built for a draw that had this attribute the other way: not followed, the attribute is left out) */ \
            SAY(8, "[nvn] program %d: attribute %u is %s here, and the program was built with it %s: left out\n", (s64)s.program_id, (u64)k, whole ? "whole numbers" : "floats", wants ? "whole numbers" : "floats"); \
            glDisableVertexAttribArray(k); \
            if (wants) glVertexAttribI4ui(k, 0, 0, 0, 1); \
            continue; \
        } \
        glEnableVertexAttribArray(k); \
        if (whole) glVertexAttribIPointer(k, parts, type, (GLsizei)stride, (const void *)(u64)offset); \
        else glVertexAttribPointer(k, parts, type, norm, (GLsizei)stride, (const void *)(u64)offset); \
        glVertexAttribDivisor(k, (u32)(s.stream[stream] >> 32)); \
    } \
    if (try_bones && s.nattrib > 6 && (s.attrib[6] & 255)) { \
        for (u32 k = 0; k < 16; k++) \
            if (s.program->need[k] == 65536) { \
                const u64 have_ = s.uniforms[k].ptr ? s.uniforms[k].size : 0; \
                bones_draws++; \
                if (!have_ || !pool_holds(s.uniforms[k].ptr, have_)) bones_none++; \
                else if (have_ >= 48) { \
                    float m[12]; \
                    int zero = 1, odd = 0; \
                    for (u64 at_ = 0; at_ + 48 <= have_; at_ += 48) { \
                        memcpy(m, s.uniforms[k].ptr + at_, 48); \
                        int z = 1; \
                        for (int i = 0; i < 12; i++) { \
                            if (m[i] != 0.0f) z = 0; \
                            if (m[i] != m[i] || m[i] > 1e7f || m[i] < -1e7f) odd = 1; \
                        } \
                        if (!z) zero = 0; \
                        else if (at_) odd = 1; /* (a bone that is all zero among others that aren't) */ \
                    } \
                    if (zero) bones_zero++; \
                    else if (odd) { \
                        bones_odd++; \
                        static u32 said_odd; \
                        if (said_odd++ < 24) { \
                            memcpy(m, s.uniforms[k].ptr, 48); \
                            nx2_log("[bones] program %d (%x), %u vertices: block %u (%u bytes at %p) has bones that are zero or wild; the first: %d %d %d %d, %d %d %d %d (thousandths)\n", \
                                    (s64)s.program_id, s.program->key[0], (u64)(last_vertex), (u64)k, have_, (u64)s.uniforms[k].ptr, (s64)(m[0] * 1000), (s64)(m[1] * 1000), \
                                    (s64)(m[2] * 1000), (s64)(m[3] * 1000), (s64)(m[4] * 1000), (s64)(m[5] * 1000), (s64)(m[6] * 1000), (s64)(m[7] * 1000)); \
                        } \
                    } \
                } \
            } \
        static const u8 *told[96]; \
        static u32 told_count; \
        const u32 a = s.attrib[6], format = a & 255, stream = a >> 8 & 15, offset = a >> 13 & 0xFFFF, stride = (u32)s.stream[stream]; \
        const u32 parts = format == 3 ? 1 : format == 15 ? 2 : format == 27 ? 3 : format == 39 ? 4 : 0; \
        const u8 *v = s.vertices[stream].ptr; \
        int known = 0; \
        for (u32 i = 0; i < told_count; i++) known |= told[i] == v; \
        if (parts && v && !known && told_count < 96 && stride && pool_holds(v, (u64)(last_vertex) * stride)) { \
            told[told_count++] = v; \
            u32 top = 0; \
            for (u32 i = 0; i < (u32)(last_vertex); i++) \
                for (u32 j = 0; j < parts; j++) \
                    if (v[(u64)i * stride + offset + j] > top) top = v[(u64)i * stride + offset + j]; \
            nx2_log("[bones] program %d (%x): %u vertices, bones up to %u (of %u parts); blocks:", (s64)s.program_id, s.program->key[0], (u64)(last_vertex), (u64)top, (u64)parts); \
            for (u32 k = 0; k < 16; k++) \
                if (s.program->need[k]) nx2_log(" %u: %u of %u at %p;", (u64)k, s.uniforms[k].ptr ? s.uniforms[k].size : 0, (u64)s.program->need[k], (u64)s.uniforms[k].ptr); \
            nx2_log("\n"); \
        } \
    }
    u32 op_before = 0;
    u64 op_tick = armGetSystemTick();
    while (c < end) {
        const u32 op = (u32)*c++;
        {  // (each kind of command's time, for the line said every 600 pictures)
            const u64 now_ = armGetSystemTick();
            op_time[op_before & 31] += now_ - op_tick, op_count[op_before & 31]++, op_tick = now_, op_before = op;
        }
        if (trail > 0) {
            if (!--trail) nx2_log_wait(0);
            nx2_log("[trail] command %u (program %d)\n", (u64)op, (s64)s.program_id);
        }
        switch (op) {
        case OP_BLEND: {  // (of one target: each has its own way of blending)
            static const u32 how[6] = {GL_FUNC_ADD, GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX};
            const u64 b = *c++;
            const u32 target = (u32)(b & 7), ec = (u32)(b >> 40 & 255), ea = (u32)(b >> 48 & 255);
            glBlendFuncSeparatei(target, blend_factor(b >> 8 & 255), blend_factor(b >> 16 & 255), blend_factor(b >> 24 & 255), blend_factor(b >> 32 & 255));
            glBlendEquationSeparatei(target, how[ec < 6 ? ec : 1], how[ea < 6 ? ea : 1]);
            SAY(6, "[nvn] blending set: %x\n", b);
            break;
        }
        case OP_COLOR: {
            const u32 on = (u32)*c++;
            for (u32 k = 0; k < 8; k++) {
                if (on >> k & 1) glEnablei(GL_BLEND, k);
                else glDisablei(GL_BLEND, k);
            }
            break;
        }
        case OP_DEPTH_STENCIL: {
            const u64 d = *c++;
            s.depth_test = (int)(d & 1), s.depth_write = (int)(d >> 1 & 1);
            if (d & 1) glEnable(GL_DEPTH_TEST);
            else glDisable(GL_DEPTH_TEST);
            glDepthMask(d >> 1 & 1);
            const u32 func = (u32)(d >> 2 & 15);
            glDepthFunc(func >= 1 && func <= 8 ? GL_NEVER + func - 1 : GL_LESS);
            if (d >> 6 & 1) glEnable(GL_STENCIL_TEST);
            else glDisable(GL_STENCIL_TEST);
            for (u32 face = 0; face < 2; face++) {
                const u32 f = (u32)(d >> (8 + face * 4) & 15);
                s.stencil_func[face] = f >= 1 && f <= 8 ? GL_NEVER + f - 1 : GL_ALWAYS;
                glStencilFuncSeparate(face ? GL_BACK : GL_FRONT, s.stencil_func[face], (GLint)s.stencil_ref[face], s.stencil_vmask[face]);
                const u32 o = (u32)(d >> (16 + face * 9));
                glStencilOpSeparate(face ? GL_BACK : GL_FRONT, stencil_op(o & 7), stencil_op(o >> 3 & 7), stencil_op(o >> 6 & 7));
            }
            break;
        }
        case OP_POLYGON: {
            const u32 p = (u32)*c++, cull = p & 3;
            SAY(6, "[nvn] polygons: face culled %u, front face %u\n", (u64)cull, (u64)(p >> 2 & 1));
            if (cull) glEnable(GL_CULL_FACE), glCullFace(gl_face(cull));
            else glDisable(GL_CULL_FACE);
            glFrontFace((int)(p >> 2 & 1) != (origin_upper && !try_noflip) ? GL_CW : GL_CCW);  // (turned over with the picture: see build_translated)
            if (p >> 5 & 1) glEnable(GL_POLYGON_OFFSET_FILL);
            else glDisable(GL_POLYGON_OFFSET_FILL);
            break;
        }
        case OP_PROGRAM: {
            const u32 id = (u32)*c++;
            s.program = id < PROGRAMS && programs[id].used ? &programs[id] : NULL, s.program_id = (int)id;
            break;
        }
        case OP_UPDATE: {  // bytes written into a buffer, in the commands' order: the address, how many, then the bytes themselves
            u8 *to = (u8 *)*c++;
            const u64 size = *c++;
            if (pool_holding((u64)to) && pool_holding((u64)to + size - 1)) memcpy(to, c, size);
            for (u32 k = 0; k < FLOWN; k++)  // (what was given of the bytes written over is given anew when next drawn with)
                if (flown[k].turn == flow_turn && flown[k].ptr < to + size && to < flown[k].ptr + flown[k].have) flown[k].turn = 0;
            c += (size + 7) / 8;
            break;
        }
        case OP_UNIFORMS: {
            const u32 slot = (u32)*c++ & 31;
            s.uniforms[slot].ptr = (const u8 *)*c++;
            s.uniforms[slot].size = *c++;
            if (framing && slot == 4) nx2_log("[frame] bones' block bound: %u bytes at %p\n", s.uniforms[slot].size, (u64)s.uniforms[slot].ptr);
            break;
        }
        case OP_COPY_TO_TEXTURE: {
            const u8 *from = (const u8 *)*c++;
            const u32 ti = (u32)*c++, where = (u32)*c++;
            const int x = (int)(s32)*c++, y = (int)(s32)*c++, z = (int)(s32)*c++, w = (int)(s32)*c++, h = (int)(s32)*c++, d = (int)(s32)*c++;
            Texture *t = &textures[ti];
            const Format *f = format_of(t->format);
            static u32 said;
            if (!from || !f || !t->used) {
                if (said++ < 8) nx2_log("[nvn] a copy into texture %u (format %d) isn't done: its format isn't known here\n", (u64)ti, (s64)t->format);
                break;
            }
            t->dirty = 0;  // (what it is given now is its contents: the memory it was made over isn't read after)
            texture_ready(t, (int)ti);
            t->dirty = 0, t->drawn_to = 1;
            if (t->share) shared[t->share - 1].drawn = 1;
            glBindTexture(t->gl_target, t->gl);
            const u32 bytes = (u32)((w + f->bw - 1) / f->bw) * (u32)((h + f->bh - 1) / f->bh) * f->bytes;
            for (int k = 0; k < (d > 0 ? d : 1); k++) level_give(t, f, (int)(where & 255), (int)(where >> 8) + z + k, x, y, w, h, from + (u64)bytes * (u32)k, bytes);
            const u32 error = glGetError();
            if (said < 300) said++, nx2_log("[nvn] a copy into texture %u: level %u, at %d,%d,%d, %dx%dx%d, format %d%s\n", (u64)ti, (u64)(where & 255), (s64)x, (s64)y, (s64)z, (s64)w, (s64)h, (s64)d, (s64)t->format, error ? ": OpenGL REFUSED it" : "");
            break;
        }
        case OP_STORAGE: {
            const u32 slot = (u32)*c++ & 15;
            s.storage[slot].ptr = (const u8 *)*c++;
            s.storage[slot].size = *c++;
            break;
        }
        case OP_TEXTURE: {
            const u32 at = (u32)*c++;
            s.tex[at >> 5 & 1][at & 31] = *c++;
            break;
        }
        case OP_ATTRIBS: {
            s.nattrib = (u32)*c++ & 31;
            for (u32 k = 0; k < s.nattrib; k++) {
                const u32 a = (u32)*c++;
                if (k < 16) s.attrib[k] = a;
            }
            if (s.nattrib > 16) s.nattrib = 16;
            break;
        }
        case OP_STREAMS: {
            const u32 n = (u32)*c++ & 31;
            for (u32 k = 0; k < n; k++) {
                const u64 v = *c++;
                if (k < 16) s.stream[k] = v;
            }
            break;
        }
        case OP_VERTICES: {
            const u32 k = (u32)*c++ & 15;
            s.vertices[k].ptr = (const u8 *)*c++;
            s.vertices[k].size = *c++;
            break;
        }
        case OP_CLEAR_COLOR: {
            const u32 index = (u32)*c++, mask = (u32)*c++;
            float color[4];
            memcpy(color, c, 16);
            c += 2;
            if (framing) nx2_log("[frame] color %u to be cleared (mask %x)%s\n", (u64)index, (u64)mask, s.render_off ? ": NOT, drawing is off" : "");
            if (try_noclear || s.render_off || index > 7) break;
            glColorMaski(index, mask & 1, mask >> 1 & 1, mask >> 2 & 1, mask >> 3 & 1);
            // (All of the target is cleared, whatever the scissor is: a game clears a 1440x810 target with the
            // scissor still at its 1024x1024 shadows', which OpenGL would clear only that much of. "scissorclear" has it OpenGL's way.)
            if (!try_scissorclear) glDisable(GL_SCISSOR_TEST);
            glClearBufferfv(GL_COLOR, (GLint)index, color);
            if (!try_scissorclear) glEnable(GL_SCISSOR_TEST);
            glColorMaski(index, channel_mask >> (index * 4) & 1, channel_mask >> (index * 4 + 1) & 1, channel_mask >> (index * 4 + 2) & 1, channel_mask >> (index * 4 + 3) & 1);
            if (framing) {
                nx2_log("[frame] color %u cleared to %d %d %d %d (thousandths)\n", (u64)index, (s64)(color[0] * 1000), (s64)(color[1] * 1000), (s64)(color[2] * 1000), (s64)(color[3] * 1000));
                if (!index) frame_pixel("cleared", s.colors);
            }
            break;
        }
        case OP_CLEAR_DEPTH_STENCIL: {
            float depth;
            memcpy(&depth, c++, 4);
            const u32 depth_mask = (u32)*c++, value = (u32)*c++, mask = (u32)*c++;
            if (try_noclear || s.render_off) break;
            if (framing) nx2_log("[frame] depth cleared (%u), stencil (%x)\n", (u64)depth_mask, (u64)mask);
            GLboolean was = 1;
            glGetBooleanv(GL_DEPTH_WRITEMASK, &was);
            if (!try_scissorclear) glDisable(GL_SCISSOR_TEST);
            if (s.offscreen && s.fbo >= 0 && fbos[s.fbo].depth != NO_TEXTURE) textures[fbos[s.fbo].depth].serial++;
            if (depth_mask) glDepthMask(1), glClearBufferfv(GL_DEPTH, 0, &depth);
            if (mask) {
                const GLint v = (GLint)value;
                glStencilMask(mask);
                glClearBufferiv(GL_STENCIL, 0, &v);
            }
            if (!try_scissorclear) glEnable(GL_SCISSOR_TEST);
            glDepthMask(was);
            break;
        }
        case OP_DRAW_ARRAYS: {
            const u32 kind = (u32)*c++, first = (u32)*c++, n = (u32)*c++;
            const u32 prim = kind & 255, instances = kind >> 8 ? kind >> 8 : 1;
            if ((nx2_debug && s.draws < 12) || prim > 8) SAY(24, "[nvn] draw %u is of kind %u: %u vertices from %u\n", (u64)s.draws, (u64)prim, (u64)n, (u64)first);
            if (trail > 0) nx2_log("[trail]   %u vertices from %u, kind %u\n", (u64)n, (u64)first, (u64)prim);
            PREPARE(first + n)
            const u64 tick4_ = armGetSystemTick();
            spent.vertices += tick4_ - tick3_;
            if (trail > 0) nx2_log("[trail]   vertices given: to be drawn\n");
            if (prim == 7) {  // quads, which OpenGL ES hasn't: each is drawn as its two triangles
                u32 quads = n / 4;
                if (quads > QUADS) quads = QUADS;
                for (u32 q = 0; q < quads; q++) {
                    const u32 v = first + q * 4;
                    u32 *t = quad_index + q * 6;
                    t[0] = v, t[1] = v + 1, t[2] = v + 2, t[3] = v, t[4] = v + 2, t[5] = v + 3;
                }
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(quads * 24), quad_index, GL_STREAM_DRAW);
                glDrawElements(GL_TRIANGLES, (GLsizei)(quads * 6), GL_UNSIGNED_INT, NULL);
            } else {
                glDrawArraysInstanced(prim < 7 ? prim : prim == 8 ? GL_TRIANGLE_STRIP : GL_TRIANGLES, (GLint)first, (GLsizei)n, (GLsizei)instances);
            }
            if (trail > 0) nx2_log("[trail]   drawn (OpenGL's error %x)\n", (u64)glGetError());
            s.draws++, n_draws++;
            spent.draw += armGetSystemTick() - tick4_, spent.draws++;
            if (s.undepthed) s.undepthed = 0, glBindFramebuffer(GL_FRAMEBUFFER, fbos[s.fbo].fbo);
            if (framing || (s.into_3d && n_3d_said++ < 12)) frame_pixel("drawn", s.colors);
            if (try_sync >= 0 && n_draws == (u32)try_sync) nx2_log_wait(1), nx2_log("[nvn] from here each line is on the card before the next thing\n");
            break;
        }
        case OP_DRAW_ELEMENTS: {
            // (The vertex the indices count from, "base vertex": a model's parts share one buffer of vertices, each part's indices
            // counting from where its own begin. It was left out, every part drawn with the buffer's first vertices: a game's kart and driver came out as a blob with spikes whenever their finer or coarser forms were drawn; 2026-11-24.)
            const u64 kind64 = *c++;
            const u32 kind = (u32)kind64, type = (u32)*c++, n = (u32)*c++;
            const s32 base_vertex = (s32)(kind64 >> 32);
            const u32 prim = kind & 255, instances = kind >> 8 ? kind >> 8 : 1;
            const u8 *indices = (const u8 *)*c++;
            if (base_vertex) SAY(4, "[nvn] a draw counts its vertices from %d (base vertex), %u indices\n", (s64)base_vertex, (u64)n);
            const u32 size = type == 0 ? 1 : type == 1 ? 2 : 4;
            if (trail > 0) nx2_log("[trail]   %u indices of %u bytes at %p, kind %u\n", (u64)n, (u64)size, (u64)indices, (u64)prim);
            if (!pool_holds(indices, (u64)n * size)) {
                SAY(12, "[nvn] a draw's indices (%u of %u bytes at %p) aren't in memory the game gave: the draw is left out\n", (u64)n, (u64)size, (u64)indices);
                pools_near((u64)indices);
                s.skipped++;
                break;
            }
            u32 top = 0;  // the last vertex the indices name: as much of the vertex buffers is given to OpenGL
            for (u32 k = 0; k < n; k++) {
                const u32 v = size == 1 ? indices[k] : size == 2 ? ((const u16 *)indices)[k] : ((const u32 *)indices)[k];
                if (v > top && v < 0xFFFFFF) top = v;
            }
            if ((nx2_debug && s.draws < 12) || prim > 8) SAY(24, "[nvn] draw %u is of kind %u: %u indices of %u bytes\n", (u64)s.draws, (u64)prim, (u64)n, (u64)size);
            PREPARE(top + 1 + (base_vertex > 0 ? (u32)base_vertex : 0))
            const u64 tick4_ = armGetSystemTick();
            spent.vertices += tick4_ - tick3_;
            if (try_bones && s.nattrib > 6 && (s.attrib[6] & 255) && prim != 7) {
                // (the largest bone the draw's own vertices name, against how many bones the game gave it)
                const u32 a6 = s.attrib[6], f6 = a6 & 255, st6 = a6 >> 8 & 15, off6 = a6 >> 13 & 0xFFFF, stride6 = (u32)s.stream[st6];
                const u32 parts6 = f6 == 3 ? 1 : f6 == 15 ? 2 : f6 == 27 ? 3 : f6 == 39 ? 4 : 0;
                const u8 *v6 = s.vertices[st6].ptr;
                u64 bones_have = 0;
                u32 bones_block = 99;
                for (u32 k = 0; k < 16; k++)
                    if (s.program->need[k] == 65536) bones_have = s.uniforms[k].ptr ? s.uniforms[k].size : 0, bones_block = k;
                if (parts6 && v6 && stride6 && bones_block != 99) {
                    u32 most = 0, beyond = 0;
                    for (u32 k = 0; k < n; k++) {
                        const u32 vi = (size == 1 ? indices[k] : size == 2 ? ((const u16 *)indices)[k] : ((const u32 *)indices)[k]) + (u32)base_vertex;
                        if ((u64)vi * stride6 + off6 + parts6 > s.vertices[st6].size) {
                            beyond++;
                            continue;
                        }
                        for (u32 j = 0; j < parts6; j++)
                            if (v6[(u64)vi * stride6 + off6 + j] > most) most = v6[(u64)vi * stride6 + off6 + j];
                    }
                    static u32 said_most, said_fine;
                    const int bad = (u64)(most + 1) * 48 > bones_have || beyond;
                    if (bad ? said_most++ < 40 : ((n > 3000 || base_vertex) && said_fine++ < 60))
                        nx2_log("[bones] %s: program %d (%x), %u indices from vertex %d: bones up to %u, the block (%u) has %u bytes (%u bones); %u indices past the stream (%u bytes, stride %u, offset %u)\n",
                                bad ? "TOO FEW" : "fine", (s64)s.program_id, s.program->key[0], (u64)n, (s64)base_vertex, (u64)most, (u64)bones_block, bones_have, bones_have / 48,
                                (u64)beyond, s.vertices[st6].size, (u64)stride6, (u64)off6);
                }
            }
            if (trail > 0) nx2_log("[trail]   vertices given: to be drawn\n");
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
            if (prim == 7) {  // quads: see above
                u32 quads = n / 4;
                if (quads > QUADS) quads = QUADS;
                for (u32 q = 0; q < quads; q++) {
                    u32 v[4];
                    for (u32 k = 0; k < 4; k++)
                        v[k] = size == 1 ? indices[q * 4 + k] : size == 2 ? ((const u16 *)indices)[q * 4 + k] : ((const u32 *)indices)[q * 4 + k];
                    u32 *t = quad_index + q * 6;
                    t[0] = v[0], t[1] = v[1], t[2] = v[2], t[3] = v[0], t[4] = v[2], t[5] = v[3];
                }
                glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(quads * 24), quad_index, GL_STREAM_DRAW);
                glDrawElements(GL_TRIANGLES, (GLsizei)(quads * 6), GL_UNSIGNED_INT, NULL);
            } else {
                buffer_kept(GL_ELEMENT_ARRAY_BUFFER, indices, (u64)n * size, 0);
                if (base_vertex)
                    glDrawElementsInstancedBaseVertex(prim < 7 ? prim : prim == 8 ? GL_TRIANGLE_STRIP : GL_TRIANGLES, (GLsizei)n,
                               size == 1 ? GL_UNSIGNED_BYTE : size == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, NULL, (GLsizei)instances, base_vertex);
                else
                    glDrawElementsInstanced(prim < 7 ? prim : prim == 8 ? GL_TRIANGLE_STRIP : GL_TRIANGLES, (GLsizei)n,
                               size == 1 ? GL_UNSIGNED_BYTE : size == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, NULL, (GLsizei)instances);
            }
            if (trail > 0) nx2_log("[trail]   drawn (OpenGL's error %x)\n", (u64)glGetError());
            s.draws++, n_draws++;
            spent.draw += armGetSystemTick() - tick4_, spent.draws++;
            if (s.undepthed) s.undepthed = 0, glBindFramebuffer(GL_FRAMEBUFFER, fbos[s.fbo].fbo);
            if (framing || (s.into_3d && n_3d_said++ < 12)) frame_pixel("drawn", s.colors);
            if (try_sync >= 0 && n_draws == (u32)try_sync) nx2_log_wait(1), nx2_log("[nvn] from here each line is on the card before the next thing\n");
            break;
        }
        case OP_SHOW:
            if (framing) nx2_log("[frame] the picture is shown\n");
            if (!try_oldubo) flow_next();
            {
                static u32 shown;
                if (++shown % 600 == 0) {
                    const u64 n = 600;
#define US(t) (armTicksToNs((t) / n) / 1000)
                    nx2_log("[time] a picture's commands: %u us in all (%u lists, %u draws). Making draws ready: program %u, uniforms %u, textures %u, vertices %u; the draw calls %u; targets %u; the rest %u\n",
                            US(spent.all), (u64)(spent.lists / n), (u64)(spent.draws / n), US(spent.program), US(spent.uniforms), US(spent.textures), US(spent.vertices), US(spent.draw), US(spent.targets),
                            US(spent.all - spent.program - spent.uniforms - spent.textures - spent.vertices - spent.draw - spent.targets));
#undef US
                    memset(&spent, 0, sizeof spent);
                    for (int k = 0; k < 32; k++)
                        if (op_time[k] / n > armNsToTicks(150000)) nx2_log("[time]   commands of kind %d: %u a picture, %u us\n", (s64)k, (u64)(op_count[k] / n), armTicksToNs(op_time[k] / n) / 1000);
                    memset(op_time, 0, sizeof op_time), memset(op_count, 0, sizeof op_count);
                    nx2_log("[time] uniform blocks, a picture: %u given (%u bytes), %u found given already, %u bound\n", (u64)(n_blocks_given / n), n_block_bytes / n, (u64)(n_blocks_shared / n), (u64)(n_blocks_bound / n));
                    n_blocks_given = n_blocks_shared = n_blocks_bound = 0, n_block_bytes = 0;
                    struct ouncevk_profile vk;
                    if (ouncevk_profile_read(&vk, true)) {
                        nx2_log("[time] the driver, a picture: %u batches to the GPU (%u bytes), %u flushes, %u syncs; most in flight %u\n", vk.execs / n, vk.exec_bytes / n, vk.flushes / n, vk.syncs / n,
                                (u64)vk.peak_inflight_submissions);
                        for (int k = 0; k < OUNCEVK_PROFILE_TIMER_COUNT; k++)
                            nx2_log("[time]   %s: %u times a picture, %u us a picture (the longest %u us)\n", ouncevk_profile_timer_name((enum ouncevk_profile_timer)k), vk.timers[k].count / n,
                                    vk.timers[k].total_us / n, vk.timers[k].max_us);
                    } else nx2_log("[time] the driver's own counts can't be read\n");
                }
            }
            framing = (try_frame >= 0 && frame_from != ~0u && picture + 1 == frame_from + (u32)try_frame) || (try_frameat >= 0 && picture + 1 == (u32)try_frameat);
            if (try_frametime > 0 && !framing) {  // (frametime=S: the first picture so many seconds after the first one)
                static u64 first_tick;
                static int done;
                if (!first_tick) first_tick = armGetSystemTick();
                if (!done && armTicksToNs(armGetSystemTick() - first_tick) / 1000000000ull >= (u64)try_frametime) done = 1, framing = 1;
            }
            if (framing) nx2_log("[frame] picture %u, the %dth after the first model with bones:\n", (u64)picture + 1, (s64)try_frame);
            window_show();
            if (nx2_vulkan) glvk_show();
            sweep();
            picture++;
            break;
        case OP_TARGETS: {
            // how many colors, then each one's record (+1; 0: none) and its level (16 bits) and layer; then the depth's the same way
            const u32 n = (u32)*c++;
            u32 color[8], where[8];
            for (u32 k = 0; k < n && k < 8; k++) {
                const u32 v = (u32)*c++;
                color[k] = v ? v - 1 : NO_TEXTURE, where[k] = (u32)*c++;
            }
            const u32 dv = (u32)*c++, depth_where = (u32)*c++;
            const u32 depth = dv && !try_nodepth ? dv - 1 : NO_TEXTURE;
            const int window = n && color[0] != NO_TEXTURE && textures[color[0]].window;
            if (s.offscreen && s.fbo >= 0 && fbos[s.fbo].pack_mask) packed_copy((u32)s.fbo);  // (what was drawn for compressed textures goes into them)
            if (window) window_bind((u32)textures[color[0]].w, (u32)textures[color[0]].h, (int)(where[0] >> 31) != (textures[color[0]].format == 56 || textures[color[0]].format == 55));  // (the window's textures are the screen)
            else if (!n && depth == NO_TEXTURE) glBindFramebuffer(GL_FRAMEBUFFER, 0);
            else {
                const u64 tick_t = armGetSystemTick();
                glBindFramebuffer(GL_FRAMEBUFFER, fbo_for(n, color, where, depth, depth_where));
                spent.targets += armGetSystemTick() - tick_t;
            }
            s.offscreen = !window && (n || depth != NO_TEXTURE);
            s.colors = (int)n;
            s.into_3d = n && color[0] != NO_TEXTURE && textures[color[0]].target == 2;
            if (s.into_3d) {
                static u32 told;
                if (told++ < 12) {
                    nx2_log("[nvn] the targets are a 3D texture's: %u colors, texture %u (%dx%dx%d, format %d), framebuffer %d, status %x\n", (u64)n, (u64)color[0],
                            (s64)textures[color[0]].w, (s64)textures[color[0]].h, (s64)textures[color[0]].depth, (s64)textures[color[0]].format, (s64)s.fbo,
                            (u64)glCheckFramebufferStatus(GL_FRAMEBUFFER));
                    for (u32 k = 0; k < n && k < 8; k++) nx2_log("[nvn]   color %u: texture %d, level %u, layer %u\n", (u64)k, (s64)(int)color[k], (u64)(where[k] & 0xFFFF), (u64)(where[k] >> 16));
                }
            }
            s.fbo = s.offscreen ? fbo_last : -1;
            SAY(8, "[nvn] render targets: %u colors, the first %d%s, depth %d\n", (u64)n, (s64)(n ? (int)color[0] : -1), window ? " (the window)" : "", (s64)(int)depth);
            if (framing) {
                nx2_log("[frame] targets: %u colors, depth %d, framebuffer %d%s\n", (u64)n, (s64)(int)depth, (s64)s.fbo, window ? " (the window)" : "");
                for (u32 k = 0; k < n && k < 8; k++)
                    if (color[k] != NO_TEXTURE)
                        nx2_log("[frame]   color %u: texture %u, %dx%d, format %d, target %d, level %u, layer %u\n", (u64)k, (u64)color[k], (s64)textures[color[k]].w,
                                (s64)textures[color[k]].h, (s64)textures[color[k]].format, (s64)textures[color[k]].target, (u64)(where[k] & 0xFFFF), (u64)(where[k] >> 16));
                if (depth != NO_TEXTURE)
                    nx2_log("[frame]   depth: texture %u, %dx%d, format %d, target %d\n", (u64)depth, (s64)textures[depth].w, (s64)textures[depth].h,
                            (s64)textures[depth].format, (s64)textures[depth].target);
            }
            break;
        }
        case OP_CHANNEL_MASK: {
            channel_mask = (u32)*c++;
            if (framing) nx2_log("[frame] channel masks: %x\n", (u64)channel_mask);
            for (u32 k = 0; k < 8; k++) glColorMaski(k, channel_mask >> (k * 4) & 1, channel_mask >> (k * 4 + 1) & 1, channel_mask >> (k * 4 + 2) & 1, channel_mask >> (k * 4 + 3) & 1);
            break;
        }
        case OP_POLYGON_OFFSET: {
            float two[2];
            memcpy(two, c++, 8);
            glPolygonOffset(two[0], two[1]);
            break;
        }
        case OP_DEPTH_RANGE: {
            float two[2];
            memcpy(two, c++, 8);
            glDepthRangef(two[0], two[1]);
            break;
        }
        case OP_RENDER_ENABLE:
            s.render_off = !*c++;
            if (framing) nx2_log("[frame] drawing is turned %s\n", s.render_off ? "off" : "on");
            break;
        case OP_BLEND_COLOR: {
            float four[4];
            memcpy(four, c, 16);
            c += 2;
            glBlendColor(four[0], four[1], four[2], four[3]);
            break;
        }
        case OP_CLEAR_TEXTURE: {  // a texture's level filled with a color, whatever the targets are
            const u32 texture = (u32)*c++, where = (u32)*c++, mask = (u32)*c++;
            const s32 x = (s32)*c++, y = (s32)*c++, w = (s32)*c++, h = (s32)*c++;
            float color[4];
            memcpy(color, c, 16);
            c += 2;
            if (try_noclear || !texture || textures[texture - 1].window) break;
            GLint was = 0, box[4] = {0};
            const int last = fbo_last;
            const u32 one = texture - 1;
            const GLboolean cut = glIsEnabled(GL_SCISSOR_TEST);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &was);
            glGetIntegerv(GL_SCISSOR_BOX, box);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo_for(1, &one, &where, NO_TEXTURE, 0));
            glEnable(GL_SCISSOR_TEST);
            glScissor(x, y, w, h);
            glColorMaski(0, mask & 1, mask >> 1 & 1, mask >> 2 & 1, mask >> 3 & 1);
            glClearBufferfv(GL_COLOR, 0, color);
            glColorMaski(0, channel_mask & 1, channel_mask >> 1 & 1, channel_mask >> 2 & 1, channel_mask >> 3 & 1);
            glScissor(box[0], box[1], box[2], box[3]);
            if (!cut) glDisable(GL_SCISSOR_TEST);
            glBindFramebuffer(GL_FRAMEBUFFER, (u32)was);
            fbo_last = last;
            if (framing) nx2_log("[frame] texture %u filled with a color (%d,%d %dx%d)\n", (u64)one, (s64)x, (s64)y, (s64)w, (s64)h);
            break;
        }
        case OP_SCISSOR: {
            const s32 x = (s32)*c++, y = (s32)*c++, w = (s32)*c++, h = (s32)*c++;
            glScissor(x, y, w, h);
            break;
        }
        case OP_VIEWPORT: {
            const s32 x = (s32)*c++, y = (s32)*c++, w = (s32)*c++, h = (s32)*c++;
            glViewport(x, y, w, h);
            SAY(4, "[nvn] viewport %d, %d, %dx%d\n", (s64)x, (s64)y, (s64)w, (s64)h);
            break;
        }
        case OP_STENCIL_MASK: {
            const u32 face = (u32)*c++, v = (u32)*c++;
            glStencilMaskSeparate(gl_face(face), v);
            break;
        }
        case OP_STENCIL_REF:
        case OP_STENCIL_VALUE_MASK: {
            const u32 face = (u32)*c++, v = (u32)*c++;
            for (u32 f = 0; f < 2; f++)
                if (face & (1u << f)) {
                    if (op == OP_STENCIL_REF) s.stencil_ref[f] = v;
                    else s.stencil_vmask[f] = v;
                    glStencilFuncSeparate(f ? GL_BACK : GL_FRONT, s.stencil_func[f] ? s.stencil_func[f] : GL_ALWAYS, (GLint)s.stencil_ref[f], s.stencil_vmask[f]);
                }
            break;
        }
        default:
            nx2_log("[nvn] a command of an unknown kind (%u) in the list: the rest of it is left\n", (u64)op);
            c = end;
        }
    }
    op_time[op_before & 31] += armGetSystemTick() - op_tick, op_count[op_before & 31]++;
#undef PREPARE
    spent.all += armGetSystemTick() - tick_run;
    const u32 err = glGetError();
    static u32 runs;
    if (runs < 6 || err || runs % 1800 == 0)
        nx2_log("[nvn] list %u: %u words, %u draws so far (%u left out); OpenGL's error: %x\n", (u64)runs, (u64)count, (u64)s.draws, (u64)s.skipped, (u64)err);
    runs++;
}

// ---------------------------------------------------------------- NVN's functions (these run as the game: nothing of the host's here)
static void (*host_start)(int, int);
static void (*host_run)(const u64 *, u32);

static u32 texture_index(const Named *t) { return t && t->magic == NVN_MAGIC && t->id < TEXTURES && textures[t->id].used ? t->id + 1 : 0; }

static u32 texture_top;  // one past the highest texture record given so far
static void nothing(void) {}
static u64 yes(void) { return 1; }
static u64 zero(void) { return 0; }

static void device_get_integer(void *device, u32 name, int *value) {
    (void)device;
    static const int known[24] = {53, 1000, 14, 65536, 256, 8, 16, 4, 16, 32, 4, 4, 32, 4, 16, 32, 32, 256, 256, 4, 8, 0x200, 0x100, 0};
    *value = name < 24 ? known[name] : 0;
    // (57 is an alignment: a game rounds a buffer's size up to it, and told 0 it made buffers of no size and was
    // ended soon after; a Switch 2, 2026-11-23. Which alignment isn't known: a page is a multiple of any it could be.)
    if (name == 57) *value = 0x1000;
    static u8 said[128];
    if (name < 128 && said[name]++ < 2) nx2_log("[nvn] the device is asked for value %u: told %d%s\n", (u64)name, (s64)*value, name < 23 ? "" : " (not a value known here)");
}
static u64 device_get_texture_handle(void *device, u32 texture_id, u32 sampler_id) {
    (void)device;
    return (u64)sampler_id << 32 | texture_id;
}

static void pool_builder_defaults(PoolBuilder *b) { b->mem = NULL, b->size = 0, b->flags = 0; }
static void pool_builder_flags(PoolBuilder *b, u32 flags) { b->flags = flags; }
static void pool_builder_storage(PoolBuilder *b, void *mem, u64 size) { b->mem = mem, b->size = size; }
static struct {
    u64 from, to;
} pools_known[4096];
static u32 pools_count;
static int pool_holding(u64 address) {
    for (u32 k = 0; k < pools_count; k++)
        if (address >= pools_known[k].from && address < pools_known[k].to) return 1;
    return 0;
}
// the whole of a stretch lies in memory the game has given NVN (what is read of the game's is read only there)
// (The pools, in order of where they begin, for looking one up: every draw asks of each of its buffers whether it lies in one,
// a game has thousands of pools, and going through them all each time was a third of the time of drawing a picture; measured,
// 2026-11-24. The order is made anew when a pool has come or gone, by whoever asks next; while it is being made, and for
// anyone asking meanwhile, the pools are gone through as before.)
static struct {
    u64 from, to;
} pools_sorted[4096];
static u32 pools_sorted_count;
static u64 pools_longest;
static volatile int pools_changed = 1, pools_sorting;
static int pools_by_place(const void *a, const void *b) {
    const u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}
static int pool_holds(const void *from, u64 size) {
    const u64 a = (u64)from;
    if (!size) return 0;
    if (pools_changed && !__atomic_exchange_n(&pools_sorting, 1, __ATOMIC_ACQUIRE)) {
        pools_changed = 0;
        u32 n = 0;
        u64 longest = 0;
        for (u32 k = 0; k < pools_count; k++)
            if (pools_known[k].to) {
                pools_sorted[n].from = pools_known[k].from, pools_sorted[n].to = pools_known[k].to, n++;
                if (pools_known[k].to - pools_known[k].from > longest) longest = pools_known[k].to - pools_known[k].from;
            }
        qsort(pools_sorted, n, sizeof *pools_sorted, pools_by_place);
        pools_sorted_count = n, pools_longest = longest;
        __atomic_store_n(&pools_sorting, 0, __ATOMIC_RELEASE);
    }
    if (!pools_changed && !pools_sorting) {
        u32 lo = 0, hi = pools_sorted_count;
        while (lo < hi) {  // (the first pool that begins after the address)
            const u32 mid = (lo + hi) / 2;
            if (pools_sorted[mid].from <= a) lo = mid + 1;
            else hi = mid;
        }
        for (u32 k = lo; k-- > 0 && a - pools_sorted[k].from < pools_longest;)
            if (a + size <= pools_sorted[k].to) return 1;
        if (!pools_changed && !pools_sorting) return 0;  // (the order held while it was looked through)
    }
    for (u32 k = 0; k < pools_count; k++)
        if (a >= pools_known[k].from && a + size <= pools_known[k].to) return 1;
    return 0;
}
// how far the pool an address lies in goes on from it (0: it lies in none)
static u64 pool_left(const void *from) {
    const u64 a = (u64)from;
    for (u32 k = 0; k < pools_count; k++)
        if (a >= pools_known[k].from && a < pools_known[k].to) return pools_known[k].to - a;
    return 0;
}
static struct {
    u64 from, to;
} pools_virtual[64];
static u32 pools_virtual_count;
static void queue_run_now(void);
// (said with a draw left out for where its indices lie: which pools are nearest that place)
static void pools_near(u64 address) {
    static u32 said_near;
    if (said_near++ >= 9) return;
    u64 below = 0, below_to = 0, above = ~0ull;
    for (u32 k = 0; k < pools_count; k++) {
        if (!pools_known[k].to) continue;
        if (pools_known[k].from <= address && pools_known[k].from >= below) below = pools_known[k].from, below_to = pools_known[k].to;
        if (pools_known[k].from > address && pools_known[k].from < above) above = pools_known[k].from;
    }
    nx2_log("[nvn]   %u pools' places are kept; the nearest below is %p to %p, the nearest above begins at %p\n", (u64)pools_count, below, below_to, above);
    for (u32 k = 0; k < pools_virtual_count; k++)
        if (address >= pools_virtual[k].from && address < pools_virtual[k].to)
            nx2_log("[nvn]   the place is %x into a virtual pool taken to lie at %p to %p\n", address - pools_virtual[k].from, pools_virtual[k].from, pools_virtual[k].to);
}
static void pool_finalize(Pool *p) {
    // (One place in the list is let go, not every one with this memory: a game may make a pool over memory another pool of its
    // has, and end the older one after. Both were let go, and what was drawn from the newer one's memory was left out, as not in
    // memory the game gave: a game's passes over the whole picture among them, the table its colors go through too.)
    if (p->magic != NVN_MAGIC) return;
    // (The lists the game has handed over are run first: they are run later than they are handed over, and a game ends a pool
    // once it has handed over the last list that draws from it. A game's passes over the whole picture have their
    // four vertices in pools ended so; they were left out as "not in memory the game gave", the table its colors are passed
    // through was never drawn, and the scene came out as noise; 2026-11-24.)
    queue_run_now();
    for (u32 k = 0; p->mem && k < pools_count; k++)
        if (pools_known[k].from == (u64)p->mem && pools_known[k].to == (u64)p->mem + p->size) {
            pools_known[k].from = pools_known[k].to = 0;
            pools_changed = 1;
            break;
        }
    SAY(8, "[nvn] a memory pool is ended: %x bytes at %p\n", p->size, (u64)p->mem);
    p->magic = 0;
}
// nvnMemoryPoolMapVirtual: a pool with no memory of its own is given another's, piece by piece. What is in such a pool is where
// the other's memory is: taken from the first piece (the game's pools of this kind are mapped whole, as far as seen).
typedef struct {
    Pool *physical;
    s64 physical_offset, virtual_offset;
    u64 size;
    u32 storage_class;
} Mapping;
static u64 pool_map_virtual(Pool *p, int count, const Mapping *m) {
    static u32 said;
    {
        static u32 said_all;
        for (int k = 0; k < count && m && said_all < 60; k++, said_all++)
            nx2_log("[nvn] a virtual pool (%p, %x bytes, at %p) is given piece %d of %d: %x bytes at %x of it, from %x of the pool at %p (%x bytes)\n", (u64)p, p->size, (u64)p->mem,
                    (s64)k, (s64)count, m[k].size, (u64)m[k].virtual_offset, (u64)m[k].physical_offset, (u64)(m[k].physical ? m[k].physical->mem : NULL),
                    m[k].physical ? m[k].physical->size : 0);
    }
    if (count > 0 && m && m[0].physical && m[0].physical->mem && !p->mem) p->mem = m[0].physical->mem + m[0].physical_offset - m[0].virtual_offset;
    if (p->mem && pools_virtual_count < 64) {
        u32 at = 0;
        while (at < pools_virtual_count && pools_virtual[at].from != (u64)p->mem) at++;
        pools_virtual[at].from = (u64)p->mem, pools_virtual[at].to = (u64)p->mem + p->size;
        if (at == pools_virtual_count) pools_virtual_count++;
    }
    if (nx2_debug && said++ < 6)
        nx2_log("[nvn] a pool with no memory of its own is given another's: %d pieces; the first, %x bytes of it at %x from %x of the other; it is at %p now\n",
                (s64)count, count > 0 && m ? m[0].size : 0, count > 0 && m ? (u64)m[0].virtual_offset : 0, count > 0 && m ? (u64)m[0].physical_offset : 0, (u64)p->mem);
    return 1;
}
static u64 pool_initialize(Pool *p, const PoolBuilder *b) {
    p->magic = NVN_MAGIC, p->flags = b->flags, p->mem = b->mem, p->size = b->size;
    if (b->mem) {
        u32 at = 0;
        while (at < pools_count && pools_known[at].to) at++;
        if (at < 4096) {
            pools_known[at].from = (u64)b->mem, pools_known[at].to = (u64)b->mem + b->size;
            pools_changed = 1;
            if (at == pools_count) pools_count++;
        } else {
            SAY(4, "[nvn] no room to keep another memory pool's place (%u are kept): what is drawn from it is left out\n", (u64)pools_count);
        }
    }
    SAY(12, "[nvn] a memory pool: %x bytes at %p, flags %x\n", b->size, (u64)b->mem, (u64)b->flags);
    return 1;
}
// the game has written to a stretch of a pool: the textures whose texels are there are to be given to OpenGL again
static void pool_flush(Pool *p, s64 offset, u64 size) {
    const u8 *a = p->mem + offset, *b = a + size;
    for (u32 k = 0; k < texture_top; k++) {  // (as far as records have been given, not all the room there is)
        Texture *t = &textures[k];
        if (t->used && t->mem && t->mem < b && t->mem + (u64)(t->stride > 0 ? t->stride : t->w * (s64)texel_bytes(t->format)) * (u64)t->h > a) t->dirty = 1;
    }
}

static void buffer_builder_defaults(BufferBuilder *b) { b->pool = NULL, b->offset = 0, b->size = 0; }
static void buffer_builder_storage(BufferBuilder *b, Pool *pool, s64 offset, u64 size) { b->pool = pool, b->offset = offset, b->size = size; }
static u64 buffer_initialize(Buffer *buf, const BufferBuilder *b) {
    buf->ptr = b->pool ? b->pool->mem + b->offset : NULL, buf->size = b->size;
    return buf->ptr != NULL;
}
static void *buffer_map(const Buffer *buf) { return buf->ptr; }
static u64 buffer_address(const Buffer *buf) { return (u64)buf->ptr; }

static void texture_builder_defaults(TextureBuilder *b) { memset(b, 0, sizeof *b), b->target = 1, b->format = F_RGBA8, b->depth = 1, b->levels = 1, b->block = -1, b->swizzle[0] = 2, b->swizzle[1] = 3, b->swizzle[2] = 4, b->swizzle[3] = 5; }
// (a texture from the game's files says how it is laid out: eight bytes, of which the first's low bits are the block height's log2)
static void texture_builder_packaged(TextureBuilder *b, const u8 *layout) { b->block = layout ? layout[0] & 7 : -1; }
static void texture_builder_width(TextureBuilder *b, s32 v) { b->w = v; }
// (A game's glyphs are a texture of one channel read as alpha: without this its text was white boxes)
static void texture_builder_swizzle(TextureBuilder *b, u32 r, u32 g, u32 bl, u32 a) { b->swizzle[0] = (u8)r, b->swizzle[1] = (u8)g, b->swizzle[2] = (u8)bl, b->swizzle[3] = (u8)a; }
static void texture_builder_height(TextureBuilder *b, s32 v) { b->h = v; }
static void texture_builder_depth(TextureBuilder *b, s32 v) { b->depth = v; }
static void texture_builder_levels(TextureBuilder *b, s32 v) { b->levels = v; }
static void texture_builder_size1(TextureBuilder *b, s32 w) { b->w = w, b->h = 1; }
static void texture_builder_size3(TextureBuilder *b, s32 w, s32 h, s32 d) { b->w = w, b->h = h, b->depth = d; }
static u32 texture_builder_get_flags(const TextureBuilder *b) { return b->flags; }
static u32 texture_builder_get_target(const TextureBuilder *b) { return b->target; }
static Pool *texture_builder_get_pool(const TextureBuilder *b) { return b->pool; }
static void texture_builder_flags(TextureBuilder *b, u32 v) { b->flags = v; }
static void texture_builder_format(TextureBuilder *b, u32 v) { b->format = v; }
static void texture_builder_target(TextureBuilder *b, u32 v) { b->target = v; }
static void texture_builder_size(TextureBuilder *b, s32 w, s32 h) { b->w = w, b->h = h; }
static void texture_builder_storage(TextureBuilder *b, Pool *pool, s64 offset) { b->pool = pool, b->offset = offset; }
static void texture_builder_stride(TextureBuilder *b, s64 stride) { b->stride = stride; }
// (A linear texture's is its rows; any other's is worked out as the GPU lays it out, a layer padded to whole blocks. A kind this
// doesn't lay out, a 3D one or an unknown format, is given a size that is surely enough.)
static u64 texture_builder_storage_size(const TextureBuilder *b) {
    const u64 w = b->w > 0 ? (u64)b->w : 1, h = b->h > 0 ? (u64)b->h : 1, d = b->depth > 0 ? (u64)b->depth : 1;
    const Format *f = format_of((int)b->format);
    if ((b->flags & TEX_LINEAR) || !f || b->target == 2) {
        const u64 row = b->stride > 0 ? (u64)b->stride : w * texel_bytes((int)b->format);
        u64 size = row * h;
        if (d > 1 || b->levels > 1 || b->target != 1) size = (size * d * (b->target == 8 || b->target == 9 ? 6 : 1) * 4 / 3 + 0xFFFF) & ~0xFFFFull;
        return (size + 0xFFF) & ~0xFFFull;
    }
    u32 first = 0;
    const u64 one = texture_layout(f, (int)w, (int)h, b->levels > 0 ? b->levels : 1, b->block, NULL, &first);
    const u64 layer = (one + (512u << first) - 1) & ~((u64)(512u << first) - 1);
    return (layer * (b->target == 1 ? 1 : d) + 0xFFF) & ~0xFFFull;
}
static u64 texture_builder_alignment(const TextureBuilder *b) {
    (void)b;
    return 0x1000;
}
static u64 texture_initialize(Named *t, const TextureBuilder *b) {
    static u32 next;  // (where the search for a free record starts: after the last one given)
    for (u32 tries = 0; tries < TEXTURES; tries++) {
        const u32 k = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED) % TEXTURES;
        if (!__atomic_exchange_n(&textures[k].used, 1, __ATOMIC_ACQ_REL)) {
            Texture *x = &textures[k];
            x->renew = 1;
            if (k >= texture_top) texture_top = k + 1;
            x->w = b->w, x->h = b->h, x->format = (int)b->format, x->flags = (int)b->flags, x->window = 0, x->stride = b->stride;
            x->depth = b->depth > 0 ? b->depth : 1, x->levels = b->levels > 0 ? b->levels : 1, x->target = (int)b->target;
            x->mem = b->pool ? b->pool->mem + b->offset : NULL;
            x->pool = b->pool, x->offset = (u64)b->offset;
            x->room = b->pool && (u64)b->offset < b->pool->size ? b->pool->size - (u64)b->offset : 0;
            x->block = b->block, x->drawn_to = 0;
            memcpy(x->swizzle, b->swizzle, 4);
            x->dirty = 1;
            t->magic = NVN_MAGIC, t->id = k;
            return 1;
        }
    }
    SAY(4, "[nvn] no room for another texture\n");
    return 0;
}
static void texture_ended(u32 id) { textures[id].mem = NULL, textures[id].dirty = 0, textures[id].window = 0, textures[id].used = 0; }
static void texture_finalize(Named *t) {
    // (its OpenGL texture is deleted by the host when the record is next used: texture_ready)
    if (texture_index(t)) {
        if (async_on) job_push(JOB_TEXTURE_END, t->id, 0);  // (after the lists handed over so far, which may draw with it)
        else texture_ended(t->id);
    }
    t->magic = 0;
}
static Texture *texture_record(const Named *t) { return texture_index(t) ? &textures[t->id] : NULL; }
static s32 texture_get_width(const Named *t) { return texture_record(t) ? texture_record(t)->w : 0; }
static s32 texture_get_height(const Named *t) { return texture_record(t) ? texture_record(t)->h : 0; }
static s32 texture_get_depth(const Named *t) { return texture_record(t) ? texture_record(t)->depth : 0; }
static s32 texture_get_levels(const Named *t) { return texture_record(t) ? texture_record(t)->levels : 0; }
static s32 texture_get_format(const Named *t) { return texture_record(t) ? texture_record(t)->format : 0; }
static s32 texture_get_target(const Named *t) { return texture_record(t) ? texture_record(t)->target : 0; }
static s32 texture_get_flags(const Named *t) { return texture_record(t) ? texture_record(t)->flags : 0; }
static void texture_touched(const Named *t) {
    if (texture_record(t)) texture_record(t)->dirty = 1;
}
static void *pool_map(const Pool *p) { return p->mem; }
static u64 pool_address(const Pool *p) { return (u64)p->mem; }
static u64 pool_size(const Pool *p) { return p->size; }
static u32 pool_flags(const Pool *p) { return p->flags; }
static u64 big(void) { return 0x1000000; }
static u64 zcull(void) { return 0x10000; }
static u64 clock_now(void) { return armTicksToNs(armGetSystemTick()); }

static u64 texture_pool_initialize(void *pool, const Pool *memory, s64 offset, int count) {
    (void)pool, (void)memory, (void)offset;
    nx2_log("[nvn] a texture pool of %d descriptors\n", (s64)count);
    return 1;
}
typedef struct {
    u32 magic;
    u16 level, layer;
    u8 swizzle[4], swizzled;  // which channel each of the four is read from, when the view says (NVN's numbers)
    u8 target;                // the kind of texture it is to be seen as (NVN's number + 1), or 0: its own
    u16 format;               // the format it is to be seen in (NVN's number), or 0: its own
    u16 levels, layers;       // how many levels and layers it was said to have (0: not said)
    u8 said;                  // which of them the game has set: 1 levels, 2 layers
} View;
// (said once for each pair: which formats the game's views see its textures in)
static void view_said(const Named *t, const View *v, const char *where) {
    static u32 pairs[48], count;
    if (!v || v->magic != NVN_MAGIC || (!v->format && !v->target) || !texture_index(t)) return;
    const Texture *x = &textures[t->id];
    const u32 pair = (u32)x->format | (u32)v->format << 10 | (u32)v->target << 20 | (u32)x->target << 24;
    for (u32 k = 0; k < count; k++)
        if (pairs[k] == pair) return;
    if (count >= 48) return;
    pairs[count++] = pair;
    nx2_log("[nvn] a view (%s) sees a texture of format %d, target %d as format %d, target %d (0: its own)\n", where, (s64)x->format, (s64)x->target, (s64)v->format, (s64)v->target - 1);
}
static void texture_pool_register(void *pool, u32 id, const Named *t, const void *view) {
    (void)pool;
    if (id < 32768) {
        if (async_on) job_push(JOB_TEXTURE_AT, id, texture_index(t));
        else texture_of[id] = (u16)texture_index(t);
    }
    // (A view's swizzle is taken for the texture's own: OpenGL has it of a texture, not of a view of one, and a texture seen two
    // ways at once isn't followed. A game's glyphs are one channel, seen as alpha through their view.)
    const View *v = view;
    view_said(t, v, "read");
    if (v && v->magic == NVN_MAGIC && v->swizzled && texture_index(t)) {
        Texture *x = &textures[t->id];
        if (memcmp(x->swizzle, v->swizzle, 4)) memcpy(x->swizzle, v->swizzle, 4), x->swizzle_new = 1;
    }
    SAY(8, "[nvn] descriptor %u is texture %d\n", (u64)id, (s64)texture_index(t) - 1);
}
static void sampler_builder_defaults(SamplerBuilder *b) {
    memset(b, 0, sizeof *b);
    b->ws = b->wt = b->wr = 7, b->compare_func = 2, b->lod_max = 1000.0f;
}
static void sampler_builder_filter(SamplerBuilder *b, u32 min, u32 mag) { b->min = min, b->mag = mag; }
static void sampler_builder_wrap(SamplerBuilder *b, u32 s, u32 t, u32 r) { b->ws = s, b->wt = t, b->wr = r; }
static void sampler_builder_compare(SamplerBuilder *b, u32 mode, u32 func) { b->compare = mode, b->compare_func = func; }
static void sampler_builder_lod_clamp(SamplerBuilder *b, float min, float max) { b->lod_min = min, b->lod_max = max; }
static void sampler_builder_lod_bias(SamplerBuilder *b, float bias) { b->lod_bias = bias; }
static void sampler_builder_border(SamplerBuilder *b, const float *color) {
    if (color) memcpy(b->border, color, 16);
}
static u64 sampler_initialize(Named *s, const SamplerBuilder *b) {
    // (One sampler for all that say the same, and none is ever let go: a game makes them by the thousand, of a few dozen kinds.
    // Each had a place of its own, the 256 places ran out, and a texture read with a sampler past those was read with none:
    // nearest texel, and repeated past its edges. A game's table of colors was read so; 2026-11-24.)
    for (u32 k = 0; k < SAMPLERS; k++)
        if (samplers[k].used && !memcmp(&samplers[k].b, b, sizeof *b)) {
            s->magic = NVN_MAGIC, s->id = k;
            return 1;
        }
    for (u32 k = 0; k < SAMPLERS; k++)
        if (!__atomic_exchange_n(&samplers[k].used, 1, __ATOMIC_ACQ_REL)) {
            samplers[k].b = *b, samplers[k].pooled = 0;
            s->magic = NVN_MAGIC, s->id = k;
            SAY(8, "[nvn] a sampler: filters %u, %u; wrap %u, %u; compared %u (%u)\n", (u64)b->min, (u64)b->mag, (u64)b->ws, (u64)b->wt, (u64)b->compare, (u64)b->compare_func);
            return 1;
        }
    static int said;
    if (!said++) nx2_log("[nvn] no room for another sampler (%d are kept)\n", (s64)SAMPLERS);
    return 0;
}
static void sampler_pool_register(void *pool, u32 id, const Named *s) {
    (void)pool;
    if (id < 4096) {
        const u16 record = s && s->magic == NVN_MAGIC && s->id < SAMPLERS ? (u16)(s->id + 1) : 0;
        if (async_on) job_push(JOB_SAMPLER_AT, id, record);
        else sampler_of[id] = record;
    }
}
static void sampler_pool_register_builder(void *pool, u32 id, const SamplerBuilder *b) {  // (a sampler made on the spot, of the pool's own)
    Named made = {0, 0};
    // (one sampler for all the builders that say the same: a game registers them anew as it goes, and there aren't that many kinds)
    for (u32 k = 0; k < SAMPLERS; k++)
        if (samplers[k].used && samplers[k].pooled && !memcmp(&samplers[k].b, b, sizeof *b)) {
            made.magic = NVN_MAGIC, made.id = k;
            sampler_pool_register(pool, id, &made);
            return;
        }
    if (sampler_initialize(&made, b)) samplers[made.id].pooled = 1, sampler_pool_register(pool, id, &made);
}

static u64 program_initialize(Named *p, void *device) {
    (void)device;
    for (u32 k = 0; k < PROGRAMS; k++)
        if (!__atomic_exchange_n(&programs[k].used, 1, __ATOMIC_ACQ_REL)) {
            programs[k].state = 0, programs[k].src[0] = programs[k].src[1] = NULL, programs[k].gl = 0, programs[k].key[0] = programs[k].key[1] = 0, programs[k].ntex = programs[k].nsamp = 0;
            p->magic = NVN_MAGIC, p->id = k;
            return 1;
        }
    nx2_log("[nvn] no room for another program\n");
    return 0;
}
// A compiled shader's number: a sum over all of its data, as tools/maxwell.py does it (fnv), eight bytes at a time.
static u64 shader_key(const u8 *data, u32 size) {
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = 0; i + 8 <= size; i += 8) {
        u64 w;
        memcpy(&w, data + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
    }
    return h;
}
// What a program is given: for each stage, where the compiled shader's data is (a buffer's address: here, a pointer) and its
// control section, which says the data's size at 0x704. The data: 0x30 bytes of header, then the shader's own header, whose
// first word says the stage (bits 10 to 13: 1 vertex, 5 fragment), then the code.
typedef struct {
    const u8 *data, *control;
} ShaderData;
static u64 program_set_shaders(Named *p, int count, const ShaderData *data) {
    if (p->magic == NVN_MAGIC && p->id < PROGRAMS) {
        Program *g = &programs[p->id];
        g->src[0] = pending[0], g->src[1] = pending[1], g->state = 0, g->key[0] = g->key[1] = 0;
        for (int k = 0; data && k < count && k < 6 && !(pending[0] && pending[1]); k++) {
            const u8 *d = data[k].data, *ctl = data[k].control;
            u32 magic = 0, ctl_magic = 0, size = 0, head = 0;
            if (!d || !ctl) continue;
            memcpy(&magic, d, 4), memcpy(&ctl_magic, ctl, 4);
            if (magic != 0x12345678 && ctl_magic == 0x98761234 && pool_holding((u64)d - 0x80)) {
                // (the address may be that of the code itself, 0x80 bytes into the data: a game's programs made of
                // the archives in its own code are given so; a Switch 2, 2026-11-23)
                u32 before = 0;
                memcpy(&before, d - 0x80, 4);
                if (before == 0x12345678) d -= 0x80, magic = before;
            }
            if (magic != 0x12345678 || ctl_magic != 0x98761234) {
                SAY(4, "[nvn] a program's shader isn't laid out as this port knows compiled ones (%x, %x)\n", (u64)magic, (u64)ctl_magic);
                static u32 shown;
                if (shown++ < 6 && ctl_magic == 0x98761234) {  // (what lies around it: asked of a game, 2026-11-23)
                    u32 w[12] = {0}, cs[2] = {0};
                    const int before = pool_holding((u64)d - 0x80);
                    if (before) memcpy(w, d - 0x80, 16), memcpy(w + 4, d - 0x50, 16);
                    memcpy(w + 8, d, 16), memcpy(cs, ctl + 0x700, 8);
                    nx2_log("[nvn]   shader %d of %d at %p (%s; 0x80 before it %s): there %x %x %x %x; 0x80 before %x %x %x %x; 0x50 before %x %x %x %x; its code ends at %x of %x\n",
                            (s64)k, (s64)count, (u64)d, pool_holding((u64)d) ? "in a pool" : "in NO pool known", before ? "too" : "isn't", (u64)w[8], (u64)w[9],
                            (u64)w[10], (u64)w[11], (u64)w[0], (u64)w[1], (u64)w[2], (u64)w[3], (u64)w[4], (u64)w[5], (u64)w[6], (u64)w[7], (u64)cs[0], (u64)cs[1]);
                }
                continue;
            }
            memcpy(&size, ctl + 0x704, 4), memcpy(&head, d + 0x30, 4);
            const u32 stage = head >> 10 & 15;
            if (size < 0x80 || size > (4u << 20) || (stage != 1 && stage != 5)) continue;
            g->key[stage == 5] = shader_key(d, size);
            g->raw[stage == 5] = d, g->raw_size[stage == 5] = size;
        }
        SAY(6, "[nvn] program %u is given its shaders (%d stages): %s (%x, %x)\n", (u64)p->id, (s64)count,
            pending[0] && pending[1] ? "GLSL seen at its compile, kept" : g->key[0] && g->key[1] ? "compiled ones, known by number" : "NOT known",
            g->key[0], g->key[1]);
    }
    pending[0] = pending[1] = NULL;
    return 1;
}

// The game's compile of a pair of shaders (glslcCompile, in one of its modules): the text is copied on its way in. Where the text is
// in the compile's record is this SDK's layout, read from the game's own code: the sources' array at 168, their stages' at 176, the
// count at 184.
static u8 (*real_compile)(u8 *);
static u8 compile_watched(u8 *object) {
    const char *const *sources = *(const char *const **)(object + 168);
    const u32 *stages = *(const u32 **)(object + 176);
    const u32 count = object[184];
    pending[0] = pending[1] = NULL;
    for (u32 k = 0; sources && stages && k < count && k < 6; k++) {
        const char *s = sources[k];
        if (!s || stages[k] > 1) continue;
        const u32 len = (u32)strlen(s);
        if (text_at + len + 1 > sizeof text) {
            SAY(1, "[nvn] no room left to keep the shaders' text\n");
            break;
        }
        memcpy(text + text_at, s, len + 1);
        pending[stages[k]] = text + text_at;
        text_at += len + 1;
    }
    SAY(4, "[nvn] the game compiles %u shaders: vertex text %s, fragment text %s\n", (u64)count, pending[0] ? "kept" : "none", pending[1] ? "kept" : "none");
    return real_compile ? real_compile(object) : 1;
}

static void window_builder_defaults(WindowBuilder *b) { memset(b, 0, sizeof *b); }
static void window_builder_textures(WindowBuilder *b, u32 count, Named *const *t) {
    b->count = count < 4 ? count : 4;
    for (u32 k = 0; k < b->count; k++) b->tex[k] = t[k];
}
static u64 window_initialize(Window *w, const WindowBuilder *b) {
    w->magic = NVN_MAGIC, w->count = b->count, w->current = 0;
    int pw = 1280, ph = 720;
    for (u32 k = 0; k < b->count; k++) {
        const u32 i = texture_index(b->tex[k]);
        w->tex[k] = i;
        if (i) textures[i - 1].window = 1, pw = textures[i - 1].w, ph = textures[i - 1].h;
    }
    nx2_log("[nvn] the window: %u textures of %dx%d\n", (u64)b->count, (s64)pw, (s64)ph);
    host_start(pw, ph);
    return 1;
}
static u32 queue_acquire(void *queue, Window *w, int *index) {
    (void)queue;
    if (w->count) w->current = (w->current + 1) % w->count;
    *index = (int)w->current;
    return 0;
}
// Where a picture's time goes, said every so often: the time from one picture to the next, and of that, the time the commands took
// to run and the time showing the picture took (which includes waiting for the screen). A game that is steady at 60 pictures a
// second has 16.7 ms between them.
static struct {
    u64 last, run, show, between, worst;
    u32 frames;
    u32 steady, slow, slower;  // how many of them came within 18 ms of the one before, within 34, and later still
} pace;
// (the picture is shown on a stack of the port's too: the same thread's 16 KB, and the game ended at its first picture shown, once
// out of two; a Switch 2, 2026-11-23. 1 MB, apart from the lists' so that neither waits for the other.)
void nx2_on_stack(const u64 *c, u32 count, void (*fn)(const u64 *, u32), void *top);
static void present_here(const u64 *c, u32 count) {
    (void)c, (void)count;
    nx2_present(display, surface);
}
static void present_roomy(void) {
    if (nx2_vulkan) return;  // (shown already, as the list's last step was run: glvk_show)
    enum { ROOM = 1 << 20 };
    static u8 stack[ROOM] __attribute__((aligned(16)));  // (not asked of the host's heap: this runs as the game)
    static int inside;
    if (__atomic_exchange_n(&inside, 1, __ATOMIC_ACQUIRE)) {
        nx2_present(display, surface);
        return;
    }
    nx2_on_stack(NULL, 0, present_here, stack + ROOM - 64);
    __atomic_store_n(&inside, 0, __ATOMIC_RELEASE);
}
static void queue_run(void);
static void gl_take(void);
static void gl_give(void);
static void queue_present(void *queue, Window *w, int index) {
    (void)queue, (void)w, (void)index;
    const u64 t0 = armGetSystemTick();
    if (async_on) {
        // (one picture at most is on its way: the last one's drawing is waited for, then this one's is handed over)
        while (shows_pushed != shows_done) svcSleepThread(50000);
        queue_run();
        job_push(JOB_SHOW, 0, 0);
        __atomic_store_n(&shows_pushed, shows_pushed + 1, __ATOMIC_RELEASE);
    } else {
        queue_run();
        gl_take();
        {
            static const u64 show[1] = {OP_SHOW};
            host_run(show, 1);
        }
        present_roomy();
        gl_give();
    }
    const u64 t1 = armGetSystemTick();
    pace.show += t1 - t0;
    if (pace.last) {
        const u64 d = t1 - pace.last;
        pace.between += d;
        if (d > pace.worst) pace.worst = d;
        const u64 us = armTicksToNs(d) / 1000;
        if (us <= 18000) pace.steady++;
        else if (us <= 34000) pace.slow++;
        else pace.slower++;
    }
    pace.last = t1;
    static u32 total;
    total++;
    if (++pace.frames >= (total <= 600 ? 5u : 600u)) {
        const u64 n = pace.frames;
        nx2_log("[nvn] pictures %u to %u: %u us apart on average (the worst %u), %u us running their commands, %u us showing them\n", (u64)(total - pace.frames),
                (u64)total, armTicksToNs(pace.between / n) / 1000, armTicksToNs(pace.worst) / 1000, armTicksToNs(pace.run / n) / 1000,
                armTicksToNs(pace.show / n) / 1000);
        nx2_log("[nvn]   of those pictures, %u came within 18 ms of the one before (sixty a second), %u within 34 ms, %u later\n", (u64)pace.steady, (u64)pace.slow, (u64)pace.slower);
        pace.steady = pace.slow = pace.slower = 0;
        nx2_log("[nvn]   made so far: %u textures (%u let go), %u framebuffers, %u programs; %u levels of texels given (%u KB); %u draws of %u asked for\n",
                (u64)n_textures, (u64)n_textures_gone, (u64)n_framebuffers, (u64)n_programs, (u64)n_uploads, n_upload_bytes >> 10, (u64)n_draws, (u64)n_asked);
        nx2_log("[nvn]   vertices and indices given so far: %u KB; draws that lacked a uniform buffer they needed: %u\n", n_given_bytes >> 10, (u64)n_uniform_misses);
        if (try_bones) nx2_log("[bones] draws with bones so far: %u; with no block of bones %u, with one all zero %u, with zero or wild bones in it %u\n", (u64)bones_draws, (u64)bones_none, (u64)bones_zero, (u64)bones_odd);
        pace.frames = 0, pace.run = pace.show = pace.between = pace.worst = 0;
    }
}
// Lists submitted are kept, and run when the queue is flushed or finished, or a picture is shown or asked for: not at once. NVN
// only queues at a submit, and a game may still write what its commands read before it flushes: a game's first
// pictures were drawn with a uniform buffer all zero (64 KB of them) at the submit (a Switch 2, 2026-11-23; whether it is filled
// later is what this is to show).
static u64 waiting[256];
static u32 waiting_count;
// (One thread at a time: the lists are run with the host's OpenGL, which is one context, and by whichever thread flushes, shows a
// picture or submits. Two ran the same list at once: a game, 2026-11-23.)
static int gl_busy;
static void gl_take(void) {
    while (__atomic_exchange_n(&gl_busy, 1, __ATOMIC_ACQUIRE)) svcSleepThread(50000);
}
static void gl_give(void) { __atomic_store_n(&gl_busy, 0, __ATOMIC_RELEASE); }
static void h_run(const u64 *c, u32 count);
// the port's own thread: each job in its turn
void nx2_jobs_run(void *unused) {
    (void)unused;
    for (;;) {
        if (jobs_done == jobs_pushed) {
            svcSleepThread(30000);
            continue;
        }
        const u64 kind = jobs[jobs_done % JOBS].kind, a = jobs[jobs_done % JOBS].a, b = jobs[jobs_done % JOBS].b;
        gl_take();
        if (kind == JOB_RUN) {
            u32 n = 0;
            const u64 *words = list_of(a, &n);
            if (words) h_run(words, n);
        } else if (kind == JOB_SHOW) {
            static const u64 show[1] = {OP_SHOW};
            h_run(show, 1);
        } else if (kind == JOB_TEXTURE_END) {
            texture_ended((u32)a);
        } else if (kind == JOB_TEXTURE_AT) {
            texture_of[a] = (u16)b;
        } else if (kind == JOB_SAMPLER_AT) {
            sampler_of[a] = (u16)b;
        }
        gl_give();
        if (kind == JOB_SHOW) __atomic_store_n(&shows_done, shows_done + 1, __ATOMIC_RELEASE);
        __atomic_store_n(&jobs_done, jobs_done + 1, __ATOMIC_RELEASE);
    }
}
static void queue_run(void) {
    if (async_on) {  // (handed to the port's thread, in the order they were submitted)
        u64 mine[256];
        gl_take();
        const u32 count = waiting_count;
        memcpy(mine, waiting, count * sizeof *mine);
        waiting_count = 0;
        gl_give();
        for (u32 k = 0; k < count; k++) job_push(JOB_RUN, mine[k], 0);
        return;
    }
    gl_take();
    const u64 t0 = armGetSystemTick();
    const u32 count = waiting_count;
    for (u32 k = 0; k < count; k++) {
        u32 n = 0;
        const u64 *words = list_of(waiting[k], &n);
        if (words) host_run(words, n);
    }
    waiting_count = 0;
    pace.run += armGetSystemTick() - t0;
    gl_give();
}
static void queue_run_now(void) {
    if (waiting_count) queue_run();
    jobs_wait();
}
static void queue_submit(void *queue, int count, const u64 *handles) {
    (void)queue;
    for (int k = 0; k < count; k++) {
        if (waiting_count == sizeof waiting / sizeof *waiting) queue_run();
        gl_take();
        waiting[waiting_count++] = handles[k];
        gl_give();
        static u32 said;
        if (nx2_debug && said++ < 40) {
            u32 n = 0;
            const u64 *words = list_of(handles[k], &n);
            nx2_log("[nvn] a list submitted: row %u, %u words%s\n", handles[k] & (LISTS - 1), (u64)n, words ? "" : " (NOT a list this port can find)");
        }
    }
}
static void queue_flush(void *queue) {
    (void)queue;
    queue_run();
}
static void queue_finish(void *queue) {  // (nvnQueueFinish: until all that was handed over has been drawn)
    (void)queue;
    queue_run();
    jobs_wait();
}

static u64 command_initialize(Named *c, void *device) {
    (void)device;
    // (one that a command buffer since ended had is taken first: a game may make and end them all the time)
    const u32 made = __atomic_fetch_add(&recorders_made, 1, __ATOMIC_RELAXED);
    u32 n = RECORDERS;
    for (u32 k = 0; k < RECORDERS; k++)
        if (!__atomic_exchange_n(&recorders[k].used, 1, __ATOMIC_ACQ_REL)) {
            n = k;
            break;
        }
    c->magic = NVN_MAGIC, c->id = n < RECORDERS ? n : RECORDERS - 1;
    if (n < RECORDERS) recorders[n].at = recorders[n].start = 0, recorders[n].full = 0, recorders[n].total = recorders[n].memory_from = recorders[n].control_from = 0, recorders[n].memory_size = recorders[n].control_size = 0;
    else SAY(2, "[nvn] the game has %u command buffers at once: more than are kept apart here, the rest share one\n", (u64)RECORDERS);
    if (nx2_debug && (made == 63 || made == 255 || made == 1023 || made == 4095)) nx2_log("[nvn] %u command buffers made so far\n", (u64)made + 1);
    return 1;
}
static Recorder *recorder_of(const Named *c) { return &recorders[c->id < RECORDERS ? c->id : 0]; }
static void command_finalize(Named *c) {
    if (c->magic != NVN_MAGIC || c->id >= RECORDERS) return;
    c->magic = 0;
    __atomic_store_n(&recorders[c->id].used, 0, __ATOMIC_RELEASE);
}
static void command_begin(Named *c) {
    Recorder *r = recorder_of(c);
    r->at = r->start = 0, r->full = 0;
    r->list_from = r->total;
}
static u64 command_end(Named *c) {
    Recorder *r = recorder_of(c);
    // (where the list lies in the game's memory, by the count the game is told its commands take: see command_memory_used)
    u64 from = 0, to = 0;
    if (r->memory_at && r->memory_size) {
        from = (u64)(u32)(r->list_from - r->memory_from) * 4, to = (u64)(u32)(r->total - r->memory_from) * 4;
        if (from > r->memory_size) from = r->memory_size;
        if (to > r->memory_size) to = r->memory_size;
        from += r->memory_at, to += r->memory_at;
    }
    const u64 handle = list_keep(r->words, r->at, from, (u32)(to - from));
    r->at = 0;
    return handle;
}
// nvnCommandBufferAddCommandMemory: the lists made so far are done with (those still waiting are run first), and its words begin again
static void queue_run_now(void);
static void command_memory(Named *c, void *pool, s64 offset, u64 size) {
    Recorder *r = recorder_of(c);
    r->memory_from = r->total, r->memory_size = size;
    // (the lists recorded in this memory before are done with from here on: those handed over and not yet run are run first)
    const Pool *p = pool;
    if (!try_oldlists && p && p->magic == NVN_MAGIC && p->mem && offset >= 0) {
        // (the lists are copies of the port's own once ended: with a thread of the port's running them, nothing is waited for here)
        if (!async_on) queue_run_now();
        r->memory_at = (u64)p->mem + (u64)offset;
        given_note(r->memory_at, size);
    }
}
static void command_control_memory(Named *c, void *memory, u64 size) {
    (void)memory;
    Recorder *r = recorder_of(c);
    r->control_from = r->total, r->control_size = size;
}
// How much of the memory it was given a command buffer has used. A game asks to know whether anything was recorded and how much
// room is left: answered 0, as this did, a game took every one of its lists for empty and joined none to the list it
// submits, which then held the last step alone (a Switch 2, 2026-11-23). The count is of this port's words, 4 bytes for each:
// near what NVN's own commands take, and never more than was given.
static u64 command_memory_used(const Named *c) {
    const Recorder *r = recorder_of(c);
    const u64 used = (u64)(u32)(r->total - r->memory_from) * 4;
    return r->memory_size && used > r->memory_size ? r->memory_size : used;
}
static u64 command_control_used(const Named *c) {
    const Recorder *r = recorder_of(c);
    const u64 used = (u64)(u32)(r->total - r->control_from);
    return r->control_size && used > r->control_size ? r->control_size : used;
}
static u64 command_memory_size(const Named *c) { return recorder_of(c)->memory_size; }
static u64 command_memory_free(const Named *c) { return recorder_of(c)->memory_size - command_memory_used(c); }
static u64 command_control_free(const Named *c) { return recorder_of(c)->control_size - command_control_used(c); }
static void command_blend(Named *c, const u64 *state) { (void)c, put(OP_BLEND), put(*state); }
static void command_color(Named *c, const u32 *state) { (void)c, put(OP_COLOR), put(*state); }
static void command_depth_stencil(Named *c, const u64 *state) { (void)c, put(OP_DEPTH_STENCIL), put(*state); }
static void command_polygon(Named *c, const u32 *state) { (void)c, put(OP_POLYGON), put(*state); }
static void command_program(Named *c, const Named *p, int stages) {
    (void)c, (void)stages;
    if (!p || p->magic != NVN_MAGIC) {
        static u32 said;
        if (nx2_debug && said++ < 8)
            nx2_log("[nvn] a program bound that isn't one made here: %p%s, stages %x; it begins %x %x\n", (u64)p, p ? "" : " (none)", (u64)(u32)stages,
                    p ? *(const u64 *)p : 0, p ? ((const u64 *)p)[1] : 0);
    }
    put(OP_PROGRAM), put(p && p->magic == NVN_MAGIC ? p->id : ~0u);
}
static void command_uniforms(Named *c, u32 stage, u32 index, u64 address, u64 size) {
    (void)c;
    if (stage > 1 || index > 15) {
        SAY(4, "[nvn] a uniform buffer for stage %u, number %u: beyond what is kept here\n", (u64)stage, (u64)index);
        return;
    }
    static u32 said;
    static u32 said_big;
    if (nx2_debug && (said++ < 24 || (size >= 0x10000 && said_big++ < 12))) {  // (what is bound, and what lies there as the command is recorded: asked of a game, 2026-11-23)
        const u64 *w = (const u64 *)address;
        const int in_pool = pool_holding(address);
        nx2_log("[nvn] uniform buffer bound: stage %u, number %u, at %p (%s), %u bytes; there now: %x %x %x %x %x %x\n", (u64)stage, (u64)index, address,
                in_pool ? "in a pool" : "in NO pool known", size, in_pool ? w[0] : 0, in_pool ? w[1] : 0, in_pool ? w[2] : 0, in_pool ? w[3] : 0,
                in_pool ? w[4] : 0, in_pool ? w[5] : 0);
    }
    put(OP_UNIFORMS), put(stage * 16 + index), put(address), put(size);
}
// nvnCommandBufferUpdateUniformBuffer: a part of a uniform buffer written from the commands themselves, in their order (so one
// buffer serves many draws, each with its own numbers). A game's engine hands every loose uniform over so: without
// this its shaders read a buffer that is all zero, and its first pictures were drawn to nothing (a Switch 2, 2026-11-23). The
// bytes are copied into the list, and written to the buffer when the list is run.
static void command_update_uniforms(Named *c, u64 buffer, u64 buffer_size, s64 offset, u64 size, const void *data) {
    (void)c;
    static u32 said;
    if (nx2_debug && said++ < 6) {
        const u32 *w = data;
        nx2_log("[nvn] a uniform buffer written from the commands: %u bytes at %d of the %u at %p: %x %x %x %x\n", size, offset, buffer_size, buffer,
                size >= 4 ? (u64)w[0] : 0, size >= 8 ? (u64)w[1] : 0, size >= 12 ? (u64)w[2] : 0, size >= 16 ? (u64)w[3] : 0);
    }
    Recorder *r = recorder_of(c);
    if (!data || !size || size > 0x10000 || offset < 0 || (u64)offset + size > buffer_size || !recorder_room(r, 3 + (u32)((size + 7) / 8))) return;
    put(OP_UPDATE), put(buffer + (u64)offset), put(size);
    r->words[r->at + (size + 7) / 8 - 1] = 0;
    memcpy(r->words + r->at, data, size);
    r->at += (u32)((size + 7) / 8), r->total += (u32)((size + 7) / 8);
}
// a texture bound by number (what a compiled shader's samplers read): stage 0 is the vertex shader's, 1 the fragment's
static void command_texture(Named *c, u32 stage, u32 index, u64 handle) {
    (void)c;
    if (stage < 2 && index < 32) put(OP_TEXTURE), put(stage << 5 | index), put(handle);
}
static void command_storage(Named *c, u32 stage, u32 index, u64 address, u64 size) {
    (void)c;
    if (stage < 2 && index < 8) put(OP_STORAGE), put(stage * 8 + index), put(address), put(size);
}
static void command_storage_many(Named *c, u32 stage, u32 first, u32 count, const u64 *address, const u64 *size) {
    for (u32 k = 0; k < count; k++) command_storage(c, stage, first + k, address[k], size[k]);
}
static void command_textures(Named *c, u32 stage, u32 first, u32 count, const u64 *handles) {
    for (u32 k = 0; k < count; k++) command_texture(c, stage, first + k, handles[k]);
}
// A view of a texture, as the game makes one to say which level or layer it means. The record is the game's memory; what this
// keeps in it: a mark, the first level, the first layer.
static void view_defaults(View *v) { v->magic = NVN_MAGIC, v->level = v->layer = 0, v->swizzled = 0, v->format = 0, v->target = 0, v->levels = v->layers = 0, v->said = 0; }
// what a view was told, asked back (each answers whether it was told at all): a game asks
static int view_get_levels(const View *v, int *base, int *count) {
    if (!v || v->magic != NVN_MAGIC || !(v->said & 1)) return 0;
    if (base) *base = v->level;
    if (count) *count = v->levels;
    return 1;
}
static int view_get_layers(const View *v, int *first, int *count) {
    if (!v || v->magic != NVN_MAGIC || !(v->said & 2)) return 0;
    if (first) *first = v->layer;
    if (count) *count = v->layers;
    return 1;
}
static int view_get_target(const View *v, int *target) {
    if (!v || v->magic != NVN_MAGIC || !v->target) return 0;
    if (target) *target = v->target - 1;
    return 1;
}
static int view_get_format(const View *v, int *format) {
    if (!v || v->magic != NVN_MAGIC || !v->format) return 0;
    if (format) *format = v->format;
    return 1;
}
static void view_format(View *v, u32 format) {
    if (v->magic != NVN_MAGIC) view_defaults(v);
    v->format = (u16)format;
}
static void view_target(View *v, u32 target) {
    if (v->magic != NVN_MAGIC) view_defaults(v);
    v->target = (u8)(target + 1);
}
static void view_swizzle(View *v, u32 r, u32 g, u32 b, u32 a) {
    if (v->magic != NVN_MAGIC) view_defaults(v);
    v->swizzle[0] = (u8)r, v->swizzle[1] = (u8)g, v->swizzle[2] = (u8)b, v->swizzle[3] = (u8)a, v->swizzled = 1;
}
static void view_levels(View *v, int base, int count) {
    if (v->magic != NVN_MAGIC) view_defaults(v);
    v->level = (u16)base, v->levels = (u16)count, v->said |= 1;
}
static void view_layers(View *v, int first, int count) {
    if (v->magic != NVN_MAGIC) view_defaults(v);
    v->layer = (u16)first, v->layers = (u16)count, v->said |= 2;
}
// texels in a buffer (plain rows, or compressed blocks in rows), copied into part of a texture
typedef struct {
    s32 x, y, z, w, h, d;
} Region;
static void texture_write(const Named *texture, const View *view, const Region *r, const void *texels) {
    if (!texture_index(texture) || !r || !texels) return;
    Texture *t = &textures[texture->id];
    const u64 bytes = (u64)r->w * (u64)r->h * (u64)(r->d > 0 ? r->d : 1) * texel_bytes(t->format);
    if (r->w <= 0 || r->h <= 0 || !bytes || bytes > (64u << 20)) return;
    writes_take();
    if (writes_at + bytes > sizeof writes_room) {
        writes_give();
        SAY(4, "[nvn] texels a game wrote (%u bytes) are let go: too many wait to be given\n", bytes);
        return;
    }
    u8 *copy = writes_room + writes_at;
    writes_at += (u32)((bytes + 15) & ~15ull);
    memcpy(copy, texels, bytes);
    int at = -1;
    for (int k = 0; k < WRITES; k++) {  // (the same box of the same texture written again takes the earlier one's place)
        if (writes[k].texels && writes[k].texture == (int)texture->id && writes[k].box[0] == r->x && writes[k].box[1] == r->y && writes[k].box[2] == r->z &&
            writes[k].box[3] == r->w && writes[k].box[4] == r->h) {
            at = k;
            break;
        }
        if (!writes[k].texels && at < 0) at = k;
    }
    if (at < 0) at = 0;  // (all taken: the first is given up)
    writes[at].texels = copy, writes[at].texture = (int)texture->id, writes[at].level = view && view->magic == NVN_MAGIC ? view->level : 0;
    writes[at].box[0] = r->x, writes[at].box[1] = r->y, writes[at].box[2] = r->z, writes[at].box[3] = r->w, writes[at].box[4] = r->h, writes[at].box[5] = r->d > 0 ? r->d : 1;
    t->written = 1;
    if (t->target == 2 || r->d > 1) SAY(20, "[nvn] texture %u (target %d) is written by the game: at %d,%d,%d, %dx%dx%d\n", (u64)texture->id, (s64)t->target, (s64)r->x, (s64)r->y, (s64)r->z, (s64)r->w, (s64)r->h, (s64)r->d);
    writes_give();
}
static void command_copy_to_texture(Named *c, u64 buffer, const Named *texture, const View *view, const Region *r, int flags) {
    (void)c, (void)flags;
    const u32 ti = texture_index(texture);
    if (!ti || !r) return;
    const u32 where = view && view->magic == NVN_MAGIC ? (u32)(view->level & 255) | (u32)view->layer << 8 : 0;
    put(OP_COPY_TO_TEXTURE), put(buffer), put(ti - 1), put(where);
    put((u64)(s64)r->x), put((u64)(s64)r->y), put((u64)(s64)r->z), put((u64)(s64)r->w), put((u64)(s64)r->h), put((u64)(s64)r->d);
}
static void command_clear_texture(Named *c, const Named *texture, const View *view, const Region *r, const float *color, u32 mask) {
    (void)c;
    u64 two[2];
    memcpy(two, color, 16);
    const Texture *t = texture_index(texture) ? &textures[texture->id] : NULL;
    put(OP_CLEAR_TEXTURE), put(texture_index(texture)), put(view && view->magic == NVN_MAGIC ? (u64)view->level | (u64)view->layer << 16 : 0), put(mask);
    put((u64)(r ? r->x : 0)), put((u64)(r ? r->y : 0)), put((u64)(r ? r->w : t ? t->w : 0)), put((u64)(r ? r->h : t ? t->h : 0));
    put(two[0]), put(two[1]);
}
static void command_attribs(Named *c, u32 count, const u32 *states) {
    (void)c;
    put(OP_ATTRIBS), put(count & 31);
    for (u32 k = 0; k < (count & 31); k++) put(states[k]);
}
static void command_streams(Named *c, u32 count, const u64 *states) {
    (void)c;
    put(OP_STREAMS), put(count & 31);
    for (u32 k = 0; k < (count & 31); k++) put(states[k]);
}
static void command_vertices(Named *c, u32 index, u64 address, u64 size) { (void)c, put(OP_VERTICES), put(index), put(address), put(size); }
static void command_clear_color(Named *c, u32 index, const float *color, u32 mask) {
    (void)c;
    u64 two[2];
    memcpy(two, color, 16);
    put(OP_CLEAR_COLOR), put(index), put(mask), put(two[0]), put(two[1]);
}
static void command_clear_depth_stencil(Named *c, float depth, u32 depth_mask, u32 stencil, u32 stencil_mask) {
    (void)c;
    u64 d = 0;
    memcpy(&d, &depth, 4);
    put(OP_CLEAR_DEPTH_STENCIL), put(d), put(depth_mask & 1), put(stencil), put(stencil_mask);
}
static void command_draw_arrays(Named *c, u32 prim, u32 first, u32 count) { (void)c, put(OP_DRAW_ARRAYS), put(prim), put(first), put(count); }
static void command_draw_elements_from(Named *c, u64 prim, u32 type, u32 count, u64 indices) {
    (void)c, put(OP_DRAW_ELEMENTS), put(prim), put(type), put(count), put(indices);
}
static void command_draw_elements(Named *c, u32 prim, u32 type, u32 count, u64 indices) { command_draw_elements_from(c, prim, type, count, indices); }
static void command_targets(Named *c, int colors, const Named *const *color, const void *views, const Named *depth, const void *depth_view) {
    // (a target's view says which level and which layer of the texture is drawn to: none, the first of each)
    const View *const *color_views = views;
    const View *cv = colors > 0 && color_views ? color_views[0] : NULL, *dv = depth_view;
    const u64 cw = cv && cv->magic == NVN_MAGIC ? (u64)cv->level | (u64)cv->layer << 16 : 0;
    const u64 dw = dv && dv->magic == NVN_MAGIC ? (u64)dv->level | (u64)dv->layer << 16 : 0;
    (void)cw;
    const int n = colors > 0 && color ? (colors < 8 ? colors : 8) : 0;
    put(OP_TARGETS), put((u64)n);
    for (int k = 0; k < n; k++) {
        const View *v = color_views ? color_views[k] : NULL;
        if (color[k]) view_said(color[k], v, "drawn to");
        u64 gamma = 0;
        if (color[k] && v && v->magic == NVN_MAGIC && v->format && texture_index(color[k])) {
            const int own = textures[color[k]->id].format, seen = v->format;
            gamma = (own == 37 && (seen == 56 || seen == 55)) || ((own == 56 || own == 55) && seen == 37);
        }
        put(color[k] ? texture_index(color[k]) : 0), put(v && v->magic == NVN_MAGIC ? (u64)v->level | (u64)(v->layer & 0x7FFF) << 16 | gamma << 31 : 0);
    }
    put(texture_index(depth)), put(dw);
}
static void command_channel_mask(Named *c, const u32 *state) { (void)c, put(OP_CHANNEL_MASK), put(*state); }
static void command_polygon_offset(Named *c, float factor, float units, float clamp) {
    (void)c, (void)clamp;
    const float two[2] = {factor, units};
    u64 w = 0;
    memcpy(&w, two, 8);
    put(OP_POLYGON_OFFSET), put(w);
}
static void command_depth_range(Named *c, float near, float far) {
    (void)c;
    const float two[2] = {near, far};
    u64 w = 0;
    memcpy(&w, two, 8);
    put(OP_DEPTH_RANGE), put(w);
}
// nvnCommandBufferReportCounter: a count the GPU keeps (how many samples of what was drawn since the count was begun passed the
// depth test, most of all) is written into the game's buffer, with the time: 16 bytes. Nothing is counted here; the game is
// told a large count, at once. (A game draws a thing's box with nothing shown, asks how much of it was seen, and leaves the thing
// out when none was: with no answer written, the buffer said none, and in a game's races nearly nothing of the
// course was drawn; 2026-11-24.)
static void command_report_counter(Named *c, u32 counter, u64 address) {
    (void)c;
    static u32 said;
    if (said++ < 4) nx2_log("[nvn] a count is asked for (kind %u) at %p: told a large one (nothing is counted here)\n", (u64)counter, address);
    if (!pool_holds((const void *)address, 16)) return;
    u64 answer[2] = {counter == 0 ? armTicksToNs(armGetSystemTick()) : 0x100000, armTicksToNs(armGetSystemTick())};
    memcpy((void *)address, answer, 16);
}
static void command_render_enable(Named *c, u32 on) { (void)c, put(OP_RENDER_ENABLE), put(on & 1); }
static void command_render_enable_if(Named *c) { (void)c, put(OP_RENDER_ENABLE), put(1); }  // (by a count of what was seen: taken as yes)
static void command_blend_color(Named *c, const float *color) {
    (void)c;
    u64 two[2];
    memcpy(two, color, 16);
    put(OP_BLEND_COLOR), put(two[0]), put(two[1]);
}
static void command_scissor(Named *c, s32 x, s32 y, s32 w, s32 h) { (void)c, put(OP_SCISSOR), put((u64)x), put((u64)y), put((u64)w), put((u64)h); }
static void command_viewport(Named *c, s32 x, s32 y, s32 w, s32 h) { (void)c, put(OP_VIEWPORT), put((u64)x), put((u64)y), put((u64)w), put((u64)h); }
// the forms that set several at once, or draw with more said: done as the plain ones (instances and a base vertex aren't yet)
static void command_uniforms_many(Named *c, u32 stage, u32 first, u32 count, const u64 *address, const u64 *size) {
    for (u32 k = 0; k < count; k++) command_uniforms(c, stage, first + k, address[k], size[k]);
}
static void command_vertices_many(Named *c, u32 first, u32 count, const u64 *address, const u64 *size) {
    for (u32 k = 0; k < count; k++) command_vertices(c, first + k, address[k], size[k]);
}
static void command_draw_arrays_instanced(Named *c, u32 prim, u32 first, u32 count, u32 base, u32 instances) {
    (void)base;
    command_draw_arrays(c, prim | (instances > 1 ? instances << 8 : 0), first, count);
}
static void command_draw_elements_instanced(Named *c, u32 prim, u32 type, u32 count, u64 indices, u32 base_vertex, u32 base, u32 instances) {
    (void)base;
    command_draw_elements_from(c, (u64)(prim | (instances > 1 ? instances << 8 : 0)) | (u64)base_vertex << 32, type, count, indices);
}
static void command_draw_elements_base(Named *c, u32 prim, u32 type, u32 count, u64 indices, u32 base_vertex) {
    command_draw_elements_from(c, (u64)prim | (u64)base_vertex << 32, type, count, indices);
}
static void command_viewports(Named *c, u32 first, u32 count, const float *v) {
    if (!first && count) command_viewport(c, (s32)v[0], (s32)v[1], (s32)v[2], (s32)v[3]);
}
static void command_scissors(Named *c, u32 first, u32 count, const s32 *v) {
    if (!first && count) command_scissor(c, v[0], v[1], v[2], v[3]);
}
// another list's commands, as part of this one: copied in
static void command_call(Named *c, int count, const u64 *handles) {
    for (int k = 0; k < count; k++) {
        u32 n = 0;
        const u64 *words = list_of(handles[k], &n);
        for (u32 i = 0; words && i < n; i++) put(words[i]);
    }
}
static u32 window_acquire(Window *w, int *index) { return queue_acquire(NULL, w, index); }

static void command_stencil_mask(Named *c, u32 face, u32 v) { (void)c, put(OP_STENCIL_MASK), put(face), put(v); }
static void command_stencil_ref(Named *c, u32 face, u32 v) { (void)c, put(OP_STENCIL_REF), put(face), put(v); }
static void command_stencil_value_mask(Named *c, u32 face, u32 v) { (void)c, put(OP_STENCIL_VALUE_MASK), put(face), put(v); }

// The small state objects, their values packed in the object itself.
//   blend (8 bytes): the target, then the four factors (color source and destination, alpha source and destination), a byte each
//   color (4): bit 0, blending on for target 0
//   depth and stencil (8): bit 0 test, 1 write, 2-5 the depth comparison, 6 stencil test; from bit 8, each face's comparison (4 bits);
//     from bit 16, each face's three operations (3 bits each)
//   polygon (4): bits 0-1 the face culled, bit 2 the front face
//   a vertex attribute (4): bits 0-7 format, 8-12 stream, 13-28 offset; a vertex stream (8): its stride, and above it its divisor
//   (blend, further: bits 40-47 and 48-55, how the color and the alpha are put together: 1 added, 2 and 3 subtracted, 4 the least, 5 the most)
//   channel mask (4): four bits for each of eight targets, red green blue alpha; polygon, further: bits 3-5, offset on for points, lines, faces
static void blend_defaults(u64 *s) { *s = 2ull << 8 | 1ull << 16 | 2ull << 24 | 1ull << 32 | 1ull << 40 | 1ull << 48; }
static void blend_target(u64 *s, u32 target) { *s = (*s & ~255ull) | (target & 7); }
static void blend_equation(u64 *s, u32 color, u32 alpha) { *s = (*s & ~(0xFFFFull << 40)) | (u64)(color & 255) << 40 | (u64)(alpha & 255) << 48; }
static void mask_defaults(u32 *s) { *s = ~0u; }
static void mask_set(u32 *s, u32 index, u32 r, u32 g, u32 b, u32 a) {
    const u32 at = (index & 7) * 4;
    *s = (*s & ~(15u << at)) | ((r & 1) | (g & 1) << 1 | (b & 1) << 2 | (a & 1) << 3) << at;
}
static void polygon_offset_on(u32 *s, u32 kinds) { *s = (*s & ~0x38u) | (kinds & 7) << 3; }
static void blend_func(u64 *s, u32 src, u32 dst, u32 src_a, u32 dst_a) {
    *s = (*s & 255) | (u64)(src & 255) << 8 | (u64)(dst & 255) << 16 | (u64)(src_a & 255) << 24 | (u64)(dst_a & 255) << 32;
}
static void color_defaults(u32 *s) { *s = 0; }
static void color_blend_enable(u32 *s, u32 index, u32 on) { *s = (*s & ~(1u << (index & 7))) | (on & 1) << (index & 7); }
static void ds_defaults(u64 *s) { *s = 2 | 2ull << 2 | 8ull << 8 | 8ull << 12 | 0x49ull << 16 | 0x49ull << 25; }  // (write on, LESS; ALWAYS; KEEP)
static void ds_depth_func(u64 *s, u32 f) { *s = (*s & ~(15ull << 2)) | (u64)(f & 15) << 2; }
static void ds_depth_test(u64 *s, u32 on) { *s = (*s & ~1ull) | (on & 1); }
static void ds_depth_write(u64 *s, u32 on) { *s = (*s & ~2ull) | (u64)(on & 1) << 1; }
static void ds_stencil_test(u64 *s, u32 on) { *s = (*s & ~(1ull << 6)) | (u64)(on & 1) << 6; }
static void ds_stencil_func(u64 *s, u32 faces, u32 f) {
    for (u32 k = 0; k < 2; k++)
        if (faces & (1u << k)) *s = (*s & ~(15ull << (8 + k * 4))) | (u64)(f & 15) << (8 + k * 4);
}
static void ds_stencil_op(u64 *s, u32 faces, u32 fail, u32 depth_fail, u32 pass) {
    const u64 ops = (fail & 7) | (depth_fail & 7) << 3 | (pass & 7) << 6;
    for (u32 k = 0; k < 2; k++)
        if (faces & (1u << k)) *s = (*s & ~(0x1FFull << (16 + k * 9))) | ops << (16 + k * 9);
}
static void polygon_defaults(u32 *s) { *s = 0; }
static void polygon_cull(u32 *s, u32 face) { *s = (*s & ~3u) | (face & 3); }
static void polygon_front(u32 *s, u32 face) { *s = (*s & ~4u) | (face == 0 ? 4u : 0); }  // (0: clockwise; the default is the other)
static void attrib_stream(u32 *s, u32 stream) { *s = (*s & ~0x1F00u) | (stream & 31) << 8; }
static void stream_divisor(u64 *s, u32 divisor) { *s = (*s & 0xFFFFFFFFull) | (u64)divisor << 32; }
static void attrib_defaults(u32 *s) { *s = 0; }
static void attrib_format_set(u32 *s, u32 format, u64 offset) { *s = (*s & 0x1F00) | (format & 255) | (u32)(offset & 0xFFFF) << 13; }
static void stream_defaults(u64 *s) { *s = 0; }
static void stream_stride(u64 *s, s64 stride) { *s = (*s & ~0xFFFFFFFFull) | (u32)stride; }

// which corner a window's pictures begin at (window_bind)
static void device_origin(void *device, int mode) {
    (void)device;
    origin_upper = mode == 1;
    nx2_log("[nvn] the window's origin mode is set to %d (0: the lower left corner, 1: the upper left)\n", (s64)mode);
}
static void device_depth_mode(void *device, int mode) {
    (void)device;
    depth_zero = mode == 1;
    nx2_log("[nvn] the depth mode is set to %d (0: clip space's depth from -w, 1: from 0)\n", (s64)mode);
}
static void *get_proc(void *device, const char *name);
static const struct {
    const char *name;
    void *fn;
} functions[] = {
    {"nvnDeviceGetProcAddress", get_proc},
    {"nvnDeviceBuilderSetDefaults", nothing}, {"nvnDeviceBuilderSetFlags", nothing}, {"nvnDeviceInitialize", yes}, {"nvnDeviceFinalize", nothing},
    {"nvnDeviceGetInteger", device_get_integer}, {"nvnDeviceSetWindowOriginMode", device_origin}, {"nvnDeviceSetDepthMode", device_depth_mode}, {"nvnDeviceGetTextureHandle", device_get_texture_handle},
    {"nvnQueueBuilderSetDefaults", nothing}, {"nvnQueueBuilderSetDevice", nothing}, {"nvnQueueInitialize", yes}, {"nvnQueueFinalize", nothing},
    {"nvnQueueSubmitCommands", queue_submit}, {"nvnQueueFlush", queue_flush}, {"nvnQueueFinish", queue_finish}, {"nvnQueuePresentTexture", queue_present},
    {"nvnQueueAcquireTexture", queue_acquire}, {"nvnQueueFenceSync", nothing}, {"nvnQueueSetTimesliceNVX", nothing},
    {"nvnSyncInitialize", yes}, {"nvnSyncFinalize", nothing}, {"nvnSyncWait", zero},
    {"nvnWindowBuilderSetDefaults", window_builder_defaults}, {"nvnWindowBuilderSetDevice", nothing}, {"nvnWindowBuilderSetNativeWindow", nothing},
    {"nvnWindowBuilderSetTextures", window_builder_textures}, {"nvnWindowInitialize", window_initialize}, {"nvnWindowFinalize", nothing},
    {"nvnMemoryPoolBuilderSetDefaults", pool_builder_defaults}, {"nvnMemoryPoolBuilderSetDevice", nothing}, {"nvnMemoryPoolBuilderSetFlags", pool_builder_flags},
    {"nvnMemoryPoolBuilderSetStorage", pool_builder_storage}, {"nvnMemoryPoolInitialize", pool_initialize}, {"nvnMemoryPoolFinalize", pool_finalize},
    {"nvnMemoryPoolFlushMappedRange", pool_flush}, {"nvnMemoryPoolSetDebugLabel", nothing},
    {"nvnBufferBuilderSetDefaults", buffer_builder_defaults}, {"nvnBufferBuilderSetDevice", nothing}, {"nvnBufferBuilderSetStorage", buffer_builder_storage},
    {"nvnBufferInitialize", buffer_initialize}, {"nvnBufferFinalize", nothing}, {"nvnBufferMap", buffer_map}, {"nvnBufferGetAddress", buffer_address},
    {"nvnTextureBuilderSetDefaults", texture_builder_defaults}, {"nvnTextureBuilderSetDevice", nothing}, {"nvnTextureBuilderSetFlags", texture_builder_flags},
    {"nvnTextureBuilderSetFormat", texture_builder_format}, {"nvnTextureBuilderSetTarget", texture_builder_target},
    {"nvnTextureBuilderSetSize2D", texture_builder_size}, {"nvnTextureBuilderSetStorage", texture_builder_storage},
    {"nvnTextureBuilderSetStride", texture_builder_stride}, {"nvnTextureBuilderGetStorageSize", texture_builder_storage_size},
    {"nvnTextureBuilderGetStorageAlignment", texture_builder_alignment}, {"nvnTextureInitialize", texture_initialize}, {"nvnTextureFinalize", texture_finalize},
    {"nvnTexturePoolInitialize", texture_pool_initialize}, {"nvnTexturePoolFinalize", nothing}, {"nvnTexturePoolRegisterTexture", texture_pool_register},
    {"nvnSamplerPoolInitialize", texture_pool_initialize}, {"nvnSamplerPoolFinalize", nothing}, {"nvnSamplerPoolRegisterSampler", sampler_pool_register},
    {"nvnSamplerBuilderSetDefaults", sampler_builder_defaults}, {"nvnSamplerBuilderSetDevice", nothing}, {"nvnSamplerBuilderSetMinMagFilter", sampler_builder_filter},
    {"nvnSamplerBuilderSetWrapMode", sampler_builder_wrap}, {"nvnSamplerBuilderSetCompare", sampler_builder_compare},
    {"nvnSamplerBuilderSetLodClamp", sampler_builder_lod_clamp}, {"nvnSamplerBuilderSetLodBias", sampler_builder_lod_bias}, {"nvnSamplerBuilderSetBorderColor", sampler_builder_border}, {"nvnSamplerInitialize", sampler_initialize}, {"nvnSamplerFinalize", nothing},
    {"nvnProgramInitialize", program_initialize}, {"nvnProgramFinalize", nothing}, {"nvnProgramSetShaders", program_set_shaders},
    {"nvnCommandBufferInitialize", command_initialize}, {"nvnCommandBufferFinalize", command_finalize}, {"nvnCommandBufferAddCommandMemory", command_memory},
    {"nvnCommandBufferAddControlMemory", command_control_memory}, {"nvnCommandBufferGetCommandMemoryUsed", command_memory_used}, {"nvnCommandBufferGetControlMemoryUsed", command_control_used}, {"nvnCommandBufferBeginRecording", command_begin}, {"nvnCommandBufferEndRecording", command_end},
    {"nvnCommandBufferBarrier", nothing}, {"nvnCommandBufferBindBlendState", command_blend}, {"nvnCommandBufferBindColorState", command_color},
    {"nvnCommandBufferBindDepthStencilState", command_depth_stencil}, {"nvnCommandBufferBindPolygonState", command_polygon},
    {"nvnCommandBufferBindProgram", command_program}, {"nvnCommandBufferUpdateUniformBuffer", command_update_uniforms}, {"nvnCommandBufferBindUniformBuffer", command_uniforms},
    {"nvnCommandBufferBindVertexAttribState", command_attribs}, {"nvnCommandBufferBindVertexStreamState", command_streams},
    {"nvnCommandBufferBindVertexBuffer", command_vertices}, {"nvnCommandBufferClearColor", command_clear_color},
    {"nvnCommandBufferClearDepthStencil", command_clear_depth_stencil}, {"nvnCommandBufferDrawArrays", command_draw_arrays},
    {"nvnCommandBufferDrawElements", command_draw_elements}, {"nvnCommandBufferSetRenderTargets", command_targets},
    {"nvnCommandBufferSetSamplerPool", nothing}, {"nvnCommandBufferSetTexturePool", nothing}, {"nvnCommandBufferSetScissor", command_scissor},
    {"nvnCommandBufferSetViewport", command_viewport}, {"nvnCommandBufferSetStencilMask", command_stencil_mask},
    {"nvnCommandBufferSetStencilRef", command_stencil_ref}, {"nvnCommandBufferSetStencilValueMask", command_stencil_value_mask},
    {"nvnTextureBuilderSetSwizzle", texture_builder_swizzle}, {"nvnTextureBuilderSetWidth", texture_builder_width}, {"nvnTextureBuilderSetHeight", texture_builder_height},
    {"nvnTextureBuilderSetDepth", texture_builder_depth}, {"nvnTextureBuilderSetLevels", texture_builder_levels},
    {"nvnTextureBuilderSetSize1D", texture_builder_size1}, {"nvnTextureBuilderSetSize3D", texture_builder_size3},
    {"nvnTextureBuilderGetFlags", texture_builder_get_flags}, {"nvnTextureBuilderGetTarget", texture_builder_get_target},
    {"nvnTextureBuilderGetMemoryPool", texture_builder_get_pool},
    {"nvnTextureGetWidth", texture_get_width}, {"nvnTextureGetHeight", texture_get_height}, {"nvnTextureGetDepth", texture_get_depth},
    {"nvnTextureGetLevels", texture_get_levels}, {"nvnTextureGetFormat", texture_get_format}, {"nvnTextureGetTarget", texture_get_target},
    {"nvnTextureGetFlags", texture_get_flags}, {"nvnTextureFlushTexels", texture_touched}, {"nvnTextureInvalidateTexels", texture_touched},
    {"nvnMemoryPoolMap", pool_map}, {"nvnMemoryPoolGetBufferAddress", pool_address}, {"nvnMemoryPoolGetSize", pool_size},
    {"nvnMemoryPoolGetFlags", pool_flags}, {"nvnMemoryPoolMapVirtual", pool_map_virtual},
    // (what a texture's depth culling data would take: a size, not zero, for a game that sets as much aside. A game was ended right after being told 0; a Switch 2, 2026-11-23)
    {"nvnTextureGetZCullStorageSize", zcull}, {"nvnTextureBuilderGetZCullStorageSize", zcull},
    {"nvnQueueBuilderGetQueueMemorySize", big}, {"nvnCommandBufferGetCommandMemorySize", command_memory_size}, {"nvnCommandBufferGetCommandMemoryFree", command_memory_free},
    {"nvnCommandBufferGetControlMemoryFree", command_control_free},
    {"nvnDeviceGetCurrentTimestampInNanoseconds", clock_now}, {"nvnDeviceGetTimestampInNanoseconds", clock_now},
    {"nvnDeviceGetTexelFetchHandle", device_get_texture_handle}, {"nvnDeviceGetImageHandle", device_get_texture_handle},
    {"nvnSyncInitializeFromFencedGLSync", yes}, {"nvnSyncInitializeFromNvRmSyncNVX", yes},
    {"nvnWindowAcquireTexture", window_acquire}, {"nvnProgramSetShadersExt", program_set_shaders},
    {"nvnSamplerPoolRegisterSamplerBuilder", sampler_pool_register_builder},
    {"nvnPolygonStateSetFrontFace", polygon_front}, {"nvnVertexAttribStateSetStreamIndex", attrib_stream},
    {"nvnVertexStreamStateSetDivisor", stream_divisor},
    {"nvnCommandBufferBindUniformBuffers", command_uniforms_many}, {"nvnCommandBufferBindVertexBuffers", command_vertices_many},
    {"nvnCommandBufferDrawArraysInstanced", command_draw_arrays_instanced}, {"nvnCommandBufferDrawElementsInstanced", command_draw_elements_instanced},
    {"nvnCommandBufferDrawElementsBaseVertex", command_draw_elements_base}, {"nvnCommandBufferSetViewports", command_viewports},
    {"nvnCommandBufferSetScissors", command_scissors}, {"nvnCommandBufferCallCommands", command_call}, {"nvnCommandBufferCopyCommands", command_call},
    {"nvnTextureBuilderSetPackagedTextureLayout", texture_builder_packaged},
    {"nvnTextureViewSetSwizzle", view_swizzle}, {"nvnTextureViewSetDefaults", view_defaults}, {"nvnTextureViewGetLevels", view_get_levels}, {"nvnTextureViewGetLayers", view_get_layers},
    {"nvnTextureViewGetTarget", view_get_target}, {"nvnTextureViewGetFormat", view_get_format}, {"nvnTextureViewSetFormat", view_format}, {"nvnTextureViewSetTarget", view_target}, {"nvnTextureWriteTexels", texture_write}, {"nvnTextureWriteTexelsStrided", texture_write}, {"nvnTextureViewSetLevels", view_levels}, {"nvnTextureViewSetLayers", view_layers},
    {"nvnCommandBufferCopyBufferToTexture", command_copy_to_texture},
    {"nvnCommandBufferBindStorageBuffer", command_storage}, {"nvnCommandBufferBindStorageBuffers", command_storage_many},
    {"nvnCommandBufferBindTexture", command_texture}, {"nvnCommandBufferBindTextures", command_textures},
    {"nvnBlendStateSetDefaults", blend_defaults}, {"nvnBlendStateSetBlendTarget", blend_target}, {"nvnBlendStateSetBlendEquation", blend_equation},
    {"nvnChannelMaskStateSetDefaults", mask_defaults}, {"nvnChannelMaskStateSetChannelMask", mask_set}, {"nvnCommandBufferBindChannelMaskState", command_channel_mask},
    {"nvnPolygonStateSetPolygonOffsetEnables", polygon_offset_on}, {"nvnCommandBufferSetPolygonOffsetClamp", command_polygon_offset},
    {"nvnCommandBufferSetDepthRange", command_depth_range}, {"nvnCommandBufferSetRenderEnable", command_render_enable},
    {"nvnCommandBufferSetRenderEnableConditional", command_render_enable_if}, {"nvnCommandBufferSetBlendColor", command_blend_color},
    {"nvnCommandBufferClearTexture", command_clear_texture}, {"nvnCommandBufferReportCounter", command_report_counter},
    {"nvnCommandBufferResetCounter", nothing}, {"nvnBlendStateSetBlendFunc", blend_func}, {"nvnColorStateSetDefaults", color_defaults},
    {"nvnColorStateSetBlendEnable", color_blend_enable}, {"nvnDepthStencilStateSetDefaults", ds_defaults}, {"nvnDepthStencilStateSetDepthFunc", ds_depth_func},
    {"nvnDepthStencilStateSetDepthTestEnable", ds_depth_test}, {"nvnDepthStencilStateSetDepthWriteEnable", ds_depth_write},
    {"nvnDepthStencilStateSetStencilTestEnable", ds_stencil_test}, {"nvnDepthStencilStateSetStencilFunc", ds_stencil_func},
    {"nvnDepthStencilStateSetStencilOp", ds_stencil_op}, {"nvnPolygonStateSetDefaults", polygon_defaults}, {"nvnPolygonStateSetCullFace", polygon_cull},
    {"nvnVertexAttribStateSetDefaults", attrib_defaults}, {"nvnVertexAttribStateSetFormat", attrib_format_set},
    {"nvnVertexStreamStateSetDefaults", stream_defaults}, {"nvnVertexStreamStateSetStride", stream_stride},
};

// A function of NVN's, by name. One this file has is the game's through a slot that says its first call in the log; one it hasn't
// is a function that does nothing and returns 0, whose first call is said the same way: that is how a missing one is known.
// A function of NVN's that this port doesn't have: the game is given one that answers 0 and says so, the first time it is called.
// (Each has its own, so that the name can be said: the log's own way of saying a first call runs out of room in a large game, and
// A game called two such for a day unseen: the one its uniforms go through, and the one it joins its lists with.)
static const char *absent_name[400];
static u8 absent_said[400];
static u32 absent_count;
static u64 absent(u32 k) {
    if (!__atomic_exchange_n(&absent_said[k], 1, __ATOMIC_RELAXED)) nx2_log("[nvn] %s is called, which this port doesn't have: answered 0\n", absent_name[k]);
    return 0;
}
#define S(k) static u64 absent_##k(void) { return absent(k); }
S(0)
S(1)
S(2)
S(3)
S(4)
S(5)
S(6)
S(7)
S(8)
S(9)
S(10)
S(11)
S(12)
S(13)
S(14)
S(15)
S(16)
S(17)
S(18)
S(19)
S(20)
S(21)
S(22)
S(23)
S(24)
S(25)
S(26)
S(27)
S(28)
S(29)
S(30)
S(31)
S(32)
S(33)
S(34)
S(35)
S(36)
S(37)
S(38)
S(39)
S(40)
S(41)
S(42)
S(43)
S(44)
S(45)
S(46)
S(47)
S(48)
S(49)
S(50)
S(51)
S(52)
S(53)
S(54)
S(55)
S(56)
S(57)
S(58)
S(59)
S(60)
S(61)
S(62)
S(63)
S(64)
S(65)
S(66)
S(67)
S(68)
S(69)
S(70)
S(71)
S(72)
S(73)
S(74)
S(75)
S(76)
S(77)
S(78)
S(79)
S(80)
S(81)
S(82)
S(83)
S(84)
S(85)
S(86)
S(87)
S(88)
S(89)
S(90)
S(91)
S(92)
S(93)
S(94)
S(95)
S(96)
S(97)
S(98)
S(99)
S(100)
S(101)
S(102)
S(103)
S(104)
S(105)
S(106)
S(107)
S(108)
S(109)
S(110)
S(111)
S(112)
S(113)
S(114)
S(115)
S(116)
S(117)
S(118)
S(119)
S(120)
S(121)
S(122)
S(123)
S(124)
S(125)
S(126)
S(127)
S(128)
S(129)
S(130)
S(131)
S(132)
S(133)
S(134)
S(135)
S(136)
S(137)
S(138)
S(139)
S(140)
S(141)
S(142)
S(143)
S(144)
S(145)
S(146)
S(147)
S(148)
S(149)
S(150)
S(151)
S(152)
S(153)
S(154)
S(155)
S(156)
S(157)
S(158)
S(159)
S(160)
S(161)
S(162)
S(163)
S(164)
S(165)
S(166)
S(167)
S(168)
S(169)
S(170)
S(171)
S(172)
S(173)
S(174)
S(175)
S(176)
S(177)
S(178)
S(179)
S(180)
S(181)
S(182)
S(183)
S(184)
S(185)
S(186)
S(187)
S(188)
S(189)
S(190)
S(191)
S(192)
S(193)
S(194)
S(195)
S(196)
S(197)
S(198)
S(199)
S(200)
S(201)
S(202)
S(203)
S(204)
S(205)
S(206)
S(207)
S(208)
S(209)
S(210)
S(211)
S(212)
S(213)
S(214)
S(215)
S(216)
S(217)
S(218)
S(219)
S(220)
S(221)
S(222)
S(223)
S(224)
S(225)
S(226)
S(227)
S(228)
S(229)
S(230)
S(231)
S(232)
S(233)
S(234)
S(235)
S(236)
S(237)
S(238)
S(239)
S(240)
S(241)
S(242)
S(243)
S(244)
S(245)
S(246)
S(247)
S(248)
S(249)
S(250)
S(251)
S(252)
S(253)
S(254)
S(255)
S(256)
S(257)
S(258)
S(259)
S(260)
S(261)
S(262)
S(263)
S(264)
S(265)
S(266)
S(267)
S(268)
S(269)
S(270)
S(271)
S(272)
S(273)
S(274)
S(275)
S(276)
S(277)
S(278)
S(279)
S(280)
S(281)
S(282)
S(283)
S(284)
S(285)
S(286)
S(287)
S(288)
S(289)
S(290)
S(291)
S(292)
S(293)
S(294)
S(295)
S(296)
S(297)
S(298)
S(299)
S(300)
S(301)
S(302)
S(303)
S(304)
S(305)
S(306)
S(307)
S(308)
S(309)
S(310)
S(311)
S(312)
S(313)
S(314)
S(315)
S(316)
S(317)
S(318)
S(319)
S(320)
S(321)
S(322)
S(323)
S(324)
S(325)
S(326)
S(327)
S(328)
S(329)
S(330)
S(331)
S(332)
S(333)
S(334)
S(335)
S(336)
S(337)
S(338)
S(339)
S(340)
S(341)
S(342)
S(343)
S(344)
S(345)
S(346)
S(347)
S(348)
S(349)
S(350)
S(351)
S(352)
S(353)
S(354)
S(355)
S(356)
S(357)
S(358)
S(359)
S(360)
S(361)
S(362)
S(363)
S(364)
S(365)
S(366)
S(367)
S(368)
S(369)
S(370)
S(371)
S(372)
S(373)
S(374)
S(375)
S(376)
S(377)
S(378)
S(379)
S(380)
S(381)
S(382)
S(383)
S(384)
S(385)
S(386)
S(387)
S(388)
S(389)
S(390)
S(391)
S(392)
S(393)
S(394)
S(395)
S(396)
S(397)
S(398)
S(399)
#undef S
#define S(k) absent_##k,
static u64 (*const absent_fn[400])(void) = {
S(0)
S(1)
S(2)
S(3)
S(4)
S(5)
S(6)
S(7)
S(8)
S(9)
S(10)
S(11)
S(12)
S(13)
S(14)
S(15)
S(16)
S(17)
S(18)
S(19)
S(20)
S(21)
S(22)
S(23)
S(24)
S(25)
S(26)
S(27)
S(28)
S(29)
S(30)
S(31)
S(32)
S(33)
S(34)
S(35)
S(36)
S(37)
S(38)
S(39)
S(40)
S(41)
S(42)
S(43)
S(44)
S(45)
S(46)
S(47)
S(48)
S(49)
S(50)
S(51)
S(52)
S(53)
S(54)
S(55)
S(56)
S(57)
S(58)
S(59)
S(60)
S(61)
S(62)
S(63)
S(64)
S(65)
S(66)
S(67)
S(68)
S(69)
S(70)
S(71)
S(72)
S(73)
S(74)
S(75)
S(76)
S(77)
S(78)
S(79)
S(80)
S(81)
S(82)
S(83)
S(84)
S(85)
S(86)
S(87)
S(88)
S(89)
S(90)
S(91)
S(92)
S(93)
S(94)
S(95)
S(96)
S(97)
S(98)
S(99)
S(100)
S(101)
S(102)
S(103)
S(104)
S(105)
S(106)
S(107)
S(108)
S(109)
S(110)
S(111)
S(112)
S(113)
S(114)
S(115)
S(116)
S(117)
S(118)
S(119)
S(120)
S(121)
S(122)
S(123)
S(124)
S(125)
S(126)
S(127)
S(128)
S(129)
S(130)
S(131)
S(132)
S(133)
S(134)
S(135)
S(136)
S(137)
S(138)
S(139)
S(140)
S(141)
S(142)
S(143)
S(144)
S(145)
S(146)
S(147)
S(148)
S(149)
S(150)
S(151)
S(152)
S(153)
S(154)
S(155)
S(156)
S(157)
S(158)
S(159)
S(160)
S(161)
S(162)
S(163)
S(164)
S(165)
S(166)
S(167)
S(168)
S(169)
S(170)
S(171)
S(172)
S(173)
S(174)
S(175)
S(176)
S(177)
S(178)
S(179)
S(180)
S(181)
S(182)
S(183)
S(184)
S(185)
S(186)
S(187)
S(188)
S(189)
S(190)
S(191)
S(192)
S(193)
S(194)
S(195)
S(196)
S(197)
S(198)
S(199)
S(200)
S(201)
S(202)
S(203)
S(204)
S(205)
S(206)
S(207)
S(208)
S(209)
S(210)
S(211)
S(212)
S(213)
S(214)
S(215)
S(216)
S(217)
S(218)
S(219)
S(220)
S(221)
S(222)
S(223)
S(224)
S(225)
S(226)
S(227)
S(228)
S(229)
S(230)
S(231)
S(232)
S(233)
S(234)
S(235)
S(236)
S(237)
S(238)
S(239)
S(240)
S(241)
S(242)
S(243)
S(244)
S(245)
S(246)
S(247)
S(248)
S(249)
S(250)
S(251)
S(252)
S(253)
S(254)
S(255)
S(256)
S(257)
S(258)
S(259)
S(260)
S(261)
S(262)
S(263)
S(264)
S(265)
S(266)
S(267)
S(268)
S(269)
S(270)
S(271)
S(272)
S(273)
S(274)
S(275)
S(276)
S(277)
S(278)
S(279)
S(280)
S(281)
S(282)
S(283)
S(284)
S(285)
S(286)
S(287)
S(288)
S(289)
S(290)
S(291)
S(292)
S(293)
S(294)
S(295)
S(296)
S(297)
S(298)
S(299)
S(300)
S(301)
S(302)
S(303)
S(304)
S(305)
S(306)
S(307)
S(308)
S(309)
S(310)
S(311)
S(312)
S(313)
S(314)
S(315)
S(316)
S(317)
S(318)
S(319)
S(320)
S(321)
S(322)
S(323)
S(324)
S(325)
S(326)
S(327)
S(328)
S(329)
S(330)
S(331)
S(332)
S(333)
S(334)
S(335)
S(336)
S(337)
S(338)
S(339)
S(340)
S(341)
S(342)
S(343)
S(344)
S(345)
S(346)
S(347)
S(348)
S(349)
S(350)
S(351)
S(352)
S(353)
S(354)
S(355)
S(356)
S(357)
S(358)
S(359)
S(360)
S(361)
S(362)
S(363)
S(364)
S(365)
S(366)
S(367)
S(368)
S(369)
S(370)
S(371)
S(372)
S(373)
S(374)
S(375)
S(376)
S(377)
S(378)
S(379)
S(380)
S(381)
S(382)
S(383)
S(384)
S(385)
S(386)
S(387)
S(388)
S(389)
S(390)
S(391)
S(392)
S(393)
S(394)
S(395)
S(396)
S(397)
S(398)
S(399)
};
#undef S
static void *get_proc(void *device, const char *name) {
    (void)device;
    for (u32 k = 0; k < sizeof functions / sizeof *functions; k++)
        if (!strcmp(functions[k].name, name)) return functions[k].fn;
    for (u32 k = 0; k < absent_count && k < 400; k++)
        if (!strcmp(absent_name[k], name)) return absent_fn[k];
    const u32 k = __atomic_fetch_add(&absent_count, 1, __ATOMIC_RELAXED);
    if (k >= 400) return zero;
    absent_name[k] = name;  // (the game's own text, which stays where it is)
    return absent_fn[k];
}
static void *bootstrap(const char *name) {
    SAY(4, "[nvn] the game asks NVN's loader for %s: the port's own NVN, over the host's OpenGL\n", name);
    return get_proc(NULL, name);
}

// A list run on a stack of the port's own. The host's OpenGL runs on the stack of whichever of the game's threads submits, and
// A game's "Presentation Thread" has 16 KB: the game ended, with an error of the system's, the moment the first
// translated shader was to be compiled there (a Switch 2, 2026-11-23; the cause is taken to be that stack, which the compiler of
// shaders overruns: not seen, a Switch 2 hands the port no faults). 4 MB, one list at a time.
void nx2_on_stack(const u64 *c, u32 count, void (*fn)(const u64 *, u32), void *top);
__asm__(".text\n.global nx2_on_stack\n.type nx2_on_stack, %function\nnx2_on_stack:\n"
        "    stp x29, x30, [sp, #-32]!\n    mov x29, sp\n    str x19, [sp, #16]\n    mov x19, sp\n    mov sp, x3\n    blr x2\n"
        "    mov sp, x19\n    ldr x19, [sp, #16]\n    ldp x29, x30, [sp], #32\n    ret\n");
static void h_run_roomy(const u64 *c, u32 count) {
    static u8 *stack;
    static int inside;
    enum { ROOM = 4 << 20 };
    if (!stack) {
        stack = malloc(ROOM);
        nx2_log("[nvn] the stack lists are run on: %u MB of the port's%s\n", (u64)(ROOM >> 20), stack ? "" : " (not to be had: the thread's own)");
    }
    if (!stack || __atomic_exchange_n(&inside, 1, __ATOMIC_ACQUIRE)) {  // (none, or one is being run on it: the thread's own stack)
        h_run(c, count);
        return;
    }
    nx2_on_stack(c, count, h_run, stack + ROOM - 64);
    __atomic_store_n(&inside, 0, __ATOMIC_RELEASE);
}

void *nx2_nvn_override(const char *name) {
    if (!host_start) {
        host_start = (void (*)(int, int))nx2_through_host(h_start);
        host_run = (void (*)(const u64 *, u32))nx2_through_host(h_run_roomy);
    }
    if (!strcmp(name, "nvnBootstrapLoader")) return bootstrap;
    if (!strcmp(name, "glslcCompile")) {
        real_compile = (u8 (*)(u8 *))nx2_symbol("glslcCompile");
        return compile_watched;
    }
    return NULL;
}
#endif
