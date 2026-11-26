// The port's own menu (Switch 2), drawn with the host's OpenGL over the game's window while the game waits in its eglSwapBuffers
// (gl2.c runs it: Plus and Minus held five seconds). These functions run as the host (through nx2_host_call): the game's GL
// context is current on this thread, so what the menu changes in it is saved first and put back after.
#ifdef __SWITCH2__
#include <switch.h>

#include <GLES3/gl3.h>
#include <stdio.h>
#include <string.h>

#include "nx2.h"

#ifndef NX2_NAME
#define NX2_NAME "game"
#endif

extern const u8 nx2_font[];  // tools/font.py's atlas (font.s)

static GLuint program, vao, vbo, texture;
static GLint u_screen, u_color, u_solid;
static u16 font_w, font_h, cell_w, cell_h;
static const u8 *advance;

static struct {
    GLint program, vao, buffer, active, texture, sampler, draw_fbo, read_fbo, viewport[4], unpack, unpack_buffer;
    GLint src_rgb, dst_rgb, src_a, dst_a, eq_rgb, eq_a;
    GLboolean blend, depth, cull, scissor, stencil, mask[4];
    GLfloat clear[4];
} saved;

static GLuint shader(GLenum type, const char *src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[200] = {0};
        glGetShaderInfoLog(s, sizeof log - 1, NULL, log);
        nx2_log("[menu] a shader didn't compile: %s\n", log);
    }
    return s;
}

static void make(void) {
    static const char *vs = "#version 300 es\n"
                            "in vec2 p; in vec2 t; uniform vec2 screen; out vec2 uv;\n"
                            "void main() { uv = t; gl_Position = vec4(p.x / screen.x * 2.0 - 1.0, 1.0 - p.y / screen.y * 2.0, 0.0, 1.0); }\n";
    static const char *fs = "#version 300 es\n"
                            "precision mediump float;\n"
                            "in vec2 uv; uniform sampler2D tex; uniform vec4 color; uniform float solid; out vec4 o;\n"
                            "void main() { o = vec4(color.rgb, color.a * mix(texture(tex, uv).r, 1.0, solid)); }\n";
    program = glCreateProgram();
    glAttachShader(program, shader(GL_VERTEX_SHADER, vs));
    glAttachShader(program, shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(program, 0, "p");
    glBindAttribLocation(program, 1, "t");
    glLinkProgram(program);
    u_screen = glGetUniformLocation(program, "screen");
    u_color = glGetUniformLocation(program, "color");
    u_solid = glGetUniformLocation(program, "solid");
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    memcpy(&font_w, nx2_font, 2), memcpy(&font_h, nx2_font + 2, 2), memcpy(&cell_w, nx2_font + 4, 2), memcpy(&cell_h, nx2_font + 6, 2);
    advance = nx2_font + 8;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, font_w, font_h, 0, GL_RED, GL_UNSIGNED_BYTE, nx2_font + 8 + 224);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    nx2_log("[menu] made (GL error %x)\n", (u64)glGetError());
}

// the game's GL state the menu touches: saved, and the menu's own set
void nx2_menu_begin(void) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &saved.program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved.vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &saved.buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &saved.active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &saved.texture);
    glGetIntegerv(GL_SAMPLER_BINDING, &saved.sampler);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved.draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &saved.read_fbo);
    glGetIntegerv(GL_VIEWPORT, saved.viewport);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &saved.unpack);
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &saved.unpack_buffer);
    glGetIntegerv(GL_BLEND_SRC_RGB, &saved.src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &saved.dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &saved.src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &saved.dst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &saved.eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &saved.eq_a);
    saved.blend = glIsEnabled(GL_BLEND), saved.depth = glIsEnabled(GL_DEPTH_TEST), saved.cull = glIsEnabled(GL_CULL_FACE);
    saved.scissor = glIsEnabled(GL_SCISSOR_TEST), saved.stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, saved.mask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, saved.clear);

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    if (!program) make();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindSampler(0, 0);
    glDisable(GL_DEPTH_TEST), glDisable(GL_CULL_FACE), glDisable(GL_SCISSOR_TEST), glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(1, 1, 1, 1);
    nx2_log("[menu] opened\n");
}

void nx2_menu_end(void) {
    glUseProgram((GLuint)saved.program);
    glBindVertexArray((GLuint)saved.vao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)saved.buffer);
    glBindTexture(GL_TEXTURE_2D, (GLuint)saved.texture);
    glBindSampler(0, (GLuint)saved.sampler);
    glActiveTexture((GLenum)saved.active);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)saved.draw_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)saved.read_fbo);
    glViewport(saved.viewport[0], saved.viewport[1], saved.viewport[2], saved.viewport[3]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, saved.unpack);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (GLuint)saved.unpack_buffer);
    glBlendFuncSeparate((GLenum)saved.src_rgb, (GLenum)saved.dst_rgb, (GLenum)saved.src_a, (GLenum)saved.dst_a);
    glBlendEquationSeparate((GLenum)saved.eq_rgb, (GLenum)saved.eq_a);
    saved.blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    saved.depth ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
    saved.cull ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
    saved.scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
    saved.stencil ? glEnable(GL_STENCIL_TEST) : glDisable(GL_STENCIL_TEST);
    glColorMask(saved.mask[0], saved.mask[1], saved.mask[2], saved.mask[3]);
    glClearColor(saved.clear[0], saved.clear[1], saved.clear[2], saved.clear[3]);
    nx2_log("[menu] closed (GL error %x)\n", (u64)glGetError());
}

static float scale;
static void quads(const float *v, int count, float r, float g, float b, float a, float solid) {
    glUniform4f(u_color, r, g, b, a);
    glUniform1f(u_solid, solid);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(count * 16 * sizeof(float)), v, GL_STREAM_DRAW);
    for (int k = 0; k < count; k++) glDrawArrays(GL_TRIANGLE_STRIP, k * 4, 4);
}
static void rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    x *= scale, y *= scale, w *= scale, h *= scale;
    const float v[16] = {x, y, 0, 0, x + w, y, 0, 0, x, y + h, 0, 0, x + w, y + h, 0, 0};
    quads(v, 1, r, g, b, a, 1);
}
// text at (x, y), in 720ths of the screen's height, a character cell `size` high
static void text(float x, float y, float size, const char *s, float r, float g, float b) {
    static float v[160 * 16];
    int n = 0;
    const float k = size / (float)cell_h * scale;
    float px = x * scale;
    const float py = y * scale;
    for (; *s && n < 160; s++) {
        const u32 c = (u8)*s < 32 ? 0 : (u8)*s - 32;
        if (c >= 224) continue;
        const float u0 = (float)(c % 16 * cell_w) / font_w, v0 = (float)(c / 16 * cell_h) / font_h;
        const float u1 = u0 + (float)cell_w / font_w, v1 = v0 + (float)cell_h / font_h;
        const float w = cell_w * k, h = cell_h * k;
        float *q = v + n * 16;
        q[0] = px, q[1] = py, q[2] = u0, q[3] = v0;
        q[4] = px + w, q[5] = py, q[6] = u1, q[7] = v0;
        q[8] = px, q[9] = py + h, q[10] = u0, q[11] = v1;
        q[12] = px + w, q[13] = py + h, q[14] = u1, q[15] = v1;
        px += advance[c] * k;
        n++;
    }
    quads(v, n, r, g, b, 1, 0);
}

// one picture of the menu (gl2.c swaps the buffers after it)
void nx2_menu_draw(const Nx2Menu *m) {
    glViewport(0, 0, m->screen_w, m->screen_h);
    glClearColor(0.05f, 0.06f, 0.10f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform2f(u_screen, (float)m->screen_w, (float)m->screen_h);
    scale = (float)m->screen_h / 720.0f;

    text(120, 90, 54, "nx2-recomp", 1, 1, 1);
#ifdef NX2_LOADER
    text(124, 150, 30, "(en pausa)", 0.6f, 0.7f, 0.85f);
#else
    text(124, 150, 30, "Juego: " NX2_NAME "   (en pausa)", 0.6f, 0.7f, 0.85f);
#endif
    char res[64];
    snprintf(res, sizeof res, "Resolucion: %dx%d", m->res_h * 16 / 9, m->res_h);
    const char *items[4] = {"Continuar", res, m->debug ? "Registro detallado: encendido" : "Registro detallado: apagado", "Salir del juego"};
    for (int k = 0; k < 4; k++) {
        const float y = 240 + k * 70;
        if (k == m->selected) rect(100, y - 10, 760, 60, 0.20f, 0.45f, 0.85f, 1);
        text(130, y, 40, items[k], 1, 1, 1);
    }
    if (m->selected == 1) text(130, 540, 28, "Izquierda / Derecha: cambiar. Se aplica al volver a abrir el juego.", 0.75f, 0.8f, 0.9f);
    if (m->selected == 2) text(130, 540, 28, "Izquierda / Derecha: cambiar. Anota todo en /nx2/log.txt; el arranque va mas lento.", 0.75f, 0.8f, 0.9f);
    if (m->selected == 3) text(130, 540, 28, "Cierra el juego y vuelve al menu HOME.", 0.75f, 0.8f, 0.9f);
    if (m->res_h != m->current_h) text(130, 580, 28, "Guardado. Ahora mismo el juego corre a otra resolucion.", 0.95f, 0.8f, 0.4f);
    text(120, 640, 28, "Arriba / Abajo: elegir     A: aceptar     B: volver al juego", 0.6f, 0.7f, 0.85f);
}

// the resolution the game is given at its next start
// the long log, on or off: the file that says so at each start (main.c), and from now on in this run too
void nx2_menu_debug(int on) {
    int ok = 1;
    if (on) {
        FILE *f = fopen(NX2_F("debug"), "w");
        ok = f != NULL;
        if (f) fclose(f);
    } else {
        remove(NX2_F("debug"));
    }
    nx2_debug = on;
    nx2_log("[menu] the long log is turned %s (<folder>/debug %s)\n", on ? "on" : "off", !ok ? "couldn't be written" : on ? "written" : "removed");
}
void nx2_menu_save(int res_h) {
    FILE *f = fopen(NX2_F("settings.cfg"), "w");
    if (f) {
        fprintf(f, "res=%d\n", res_h);
        fclose(f);
    }
    nx2_log("[menu] resolution for the next start: %d (%s)\n", (s64)res_h, f ? "saved" : "the file couldn't be written");
}
int nx2_menu_load(void) {
    int res = 1080;
    FILE *f = fopen(NX2_F("settings.cfg"), "r");
    if (f) {
        if (fscanf(f, "res=%d", &res) != 1 || (res != 720 && res != 1080)) res = 1080;
        fclose(f);
    }
    return res;
}

// "Close the game": the process isn't simply ended, which the system shows as an error (a Switch 2, 2026-11-23), nor left the way
// a homebrew program leaves, back into its loader, since the game's threads are still running in its memory. The system is asked to
// close the application, as it is when a game is closed from the HOME menu (the applet session's Exit): the host library does that
// when it gives its applet session up with this mode set. Then a moment for the system to act; ending the process is what is left
// if it doesn't.
extern u32 __nx_applet_exit_mode;
void nx2_menu_quit(void) {
    // (Giving the applet session up with the mode set didn't do it: the program was still there four seconds on, and ending the
    // process then was shown as an error; a Switch 2, 2026-11-23. So the request is made outright, of the session's own
    // controller, and the process ends right after it, as a program that leaves by that request does.)
    Service *self = appletGetServiceSession_SelfController();
    const Result rc = self && serviceIsActive(self) ? serviceDispatch(self, 0) : 1;  // Exit
    nx2_log("[menu] the system is asked to close the program: %x\n", (u64)rc);
    (void)__nx_applet_exit_mode;
    svcSleepThread(300000000ull);
}
#endif
