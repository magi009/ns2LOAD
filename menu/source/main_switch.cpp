// SPDX-License-Identifier: GPL-2.0-or-later

// The menu on the console: a software framebuffer for its canvas, the pad for what was
// pressed, and the homebrew loader's "start this next" to hand over to the game loader.

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <switch.h>

#include "app.h"
#include "platform.h"
#include "transfer.h"

// ---------------------------------------------------------------------------------------------
// The network: only when asked (covers downloaded or received, files from a computer)

namespace Platform {
namespace {

bool g_sockets = false;

bool Sockets() {
    if (!g_sockets) {
        // Room for the buffers the file receiver may ask for (menu.cfg's transfer_rcvbuf_kb goes
        // up to 2048 KB) and for sending a file back at a good pace; the defaults if that is refused.
        SocketInitConfig config = *socketGetDefaultInitConfig();
        config.tcp_tx_buf_size = 0x40000;
        config.tcp_rx_buf_size = 0x40000;
        config.tcp_tx_buf_max_size = 0x100000;
        config.tcp_rx_buf_max_size = 0x200000;
        g_sockets = R_SUCCEEDED(socketInitialize(&config)) || R_SUCCEEDED(socketInitializeDefault());
    }
    return g_sockets;
}

/// A connection that is plain or, for https, the system's own TLS over the same socket.
struct Line {
    int sock{-1};
    bool tls{};
    SslContext context{};
    SslConnection connection{};
    bool context_open{}, connection_open{};

    bool Send(const char* data, std::size_t size) {
        while (size != 0) {
            u32 done = 0;
            if (tls) {
                if (R_FAILED(sslConnectionWrite(&connection, data, static_cast<u32>(size), &done)) || done == 0) {
                    return false;
                }
            } else {
                const ssize_t n = send(sock, data, size, 0);
                if (n <= 0) {
                    return false;
                }
                done = static_cast<u32>(n);
            }
            data += done;
            size -= done;
        }
        return true;
    }

    /// Bytes read; 0 at the end; -1 on an error.
    int Receive(char* to, std::size_t size) {
        if (tls) {
            u32 done = 0;
            return R_FAILED(sslConnectionRead(&connection, to, static_cast<u32>(size), &done)) ? -1 : static_cast<int>(done);
        }
        return static_cast<int>(recv(sock, to, size, 0));
    }

    void Close() {
        if (connection_open) {
            sslConnectionClose(&connection); // (the socket is the connection's from when it was given)
            sock = -1;
        }
        if (context_open) {
            sslContextClose(&context);
        }
        if (sock >= 0) {
            close(sock);
        }
        sock = -1;
        connection_open = context_open = false;
    }
};

bool Open(Line& line, const std::string& host, bool tls, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), tls ? "443" : "80", &hints, &found) != 0 || !found) {
        error = "the site's address wasn't found";
        return false;
    }
    line.sock = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
    const bool connected = line.sock >= 0 && connect(line.sock, found->ai_addr, found->ai_addrlen) == 0;
    freeaddrinfo(found);
    if (!connected) {
        error = "the site didn't answer";
        line.Close();
        return false;
    }
    if (!tls) {
        return true;
    }
    static bool ssl_ready = false;
    if (!ssl_ready) {
        ssl_ready = R_SUCCEEDED(sslInitialize(3));
    }
    int taken = -1;
    if (!ssl_ready || R_FAILED(sslCreateContext(&line.context, SslVersion_Auto))) {
        error = "no secure connection could be made";
        line.Close();
        return false;
    }
    line.context_open = true;
    if (R_FAILED(sslContextCreateConnection(&line.context, &line.connection))) {
        error = "no secure connection could be made";
        line.Close();
        return false;
    }
    line.connection_open = true;
    if (R_FAILED(sslConnectionSetSocketDescriptor(&line.connection, line.sock, &taken)) ||
        R_FAILED(sslConnectionSetHostName(&line.connection, host.c_str(), static_cast<u32>(host.size()))) ||
        R_FAILED(sslConnectionDoHandshake(&line.connection, nullptr, nullptr, nullptr, 0))) {
        error = "the secure connection was refused";
        line.Close();
        return false;
    }
    line.tls = true;
    return true;
}

/// One GET. 1: the body is in the file; 0: the site says no (404 and the like); -1: an error.
/// `moved` is where the site sends us instead, when it does.
int Get(const std::string& url, const std::string& to_file, std::string& moved, std::string& error) {
    const bool tls = url.rfind("https://", 0) == 0;
    const std::size_t start = url.find("://");
    if (start == std::string::npos) {
        return -1;
    }
    const std::size_t slash = url.find('/', start + 3);
    const std::string host = url.substr(start + 3, slash == std::string::npos ? slash : slash - start - 3);
    const std::string path = slash == std::string::npos ? "/" : url.substr(slash);
    Line line;
    if (!Open(line, host, tls, error)) {
        return -1;
    }
    const std::string request = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nUser-Agent: covers/1.0\r\nConnection: close\r\n\r\n";
    if (!line.Send(request.data(), request.size())) {
        error = "the request couldn't be sent";
        line.Close();
        return -1;
    }
    std::string head;
    std::FILE* out = nullptr;
    std::size_t body = 0;
    int status = 0, result = -1;
    char block[0x4000];
    for (;;) {
        const int n = line.Receive(block, sizeof block);
        if (n < 0) {
            error = "the answer was cut short";
            break;
        }
        if (n == 0) {
            result = out ? 1 : (status >= 300 && status < 500 ? 0 : -1);
            break;
        }
        const char* data = block;
        std::size_t size = static_cast<std::size_t>(n);
        if (!out && status == 0) {
            head.append(data, size);
            const std::size_t end = head.find("\r\n\r\n");
            if (end == std::string::npos) {
                if (head.size() > 0x4000) {
                    error = "not an answer this reads";
                    break;
                }
                continue;
            }
            std::sscanf(head.c_str(), "HTTP/%*s %d", &status);
            if (status >= 300 && status < 400) {
                std::size_t at = head.find("\r\nLocation: ");
                if (at == std::string::npos) {
                    at = head.find("\r\nlocation: ");
                }
                if (at != std::string::npos) {
                    moved = head.substr(at + 12, head.find("\r\n", at + 12) - at - 12);
                }
            }
            if (status != 200) {
                result = status >= 300 && status < 500 ? 0 : -1;
                if (result < 0) {
                    error = "the site answered " + std::to_string(status);
                }
                break;
            }
            out = std::fopen((to_file + ".part").c_str(), "wb");
            if (!out) {
                error = "the card couldn't be written to";
                break;
            }
            const std::string rest = head.substr(end + 4);
            std::fwrite(rest.data(), 1, rest.size(), out);
            body += rest.size();
            continue;
        }
        if (out) {
            std::fwrite(data, 1, size, out);
            body += size;
        }
    }
    line.Close();
    if (out) {
        std::fclose(out);
        // (a picture is thousands of bytes: less is a page that says there is none)
        if (result == 1 && body > 2000) {
            std::remove(to_file.c_str());
            std::rename((to_file + ".part").c_str(), to_file.c_str());
        } else {
            std::remove((to_file + ".part").c_str());
            result = result == 1 ? 0 : result;
        }
    }
    return result;
}

} // namespace

bool HasNetwork() {
    static bool nifm_ready = false;
    if (!nifm_ready) {
        nifm_ready = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    }
    NifmInternetConnectionType type{};
    u32 strength = 0;
    NifmInternetConnectionStatus status{};
    return nifm_ready && R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status)) &&
           status == NifmInternetConnectionStatus_Connected && Sockets();
}

bool Download(const std::string& url, const std::string& to_file, std::string& error) {
    if (!Sockets()) {
        error = "no network";
        return false;
    }
    std::string at = url;
    for (int hop = 0; hop < 4; ++hop) {
        std::string moved;
        int result = Get(at, to_file, moved, error);
        if (result < 0 && at.rfind("https://", 0) == 0) {
            // (a console that can't make the secure connection may still be let have the picture plainly)
            error.clear();
            result = Get("http://" + at.substr(8), to_file, moved, error);
        }
        if (result == 1) {
            return true;
        }
        if (moved.empty() || moved.find("://") == std::string::npos) {
            return false;
        }
        at = moved;
    }
    return false;
}

bool StartNetwork() {
    return Sockets();
}

std::string Address() {
    if (!Sockets()) {
        return {};
    }
    in_addr address{};
    address.s_addr = gethostid();
    if (address.s_addr == 0 || address.s_addr == htonl(INADDR_LOOPBACK)) {
        return {};
    }
    return inet_ntoa(address);
}

void KeepAwake(bool on) {
    appletSetAutoSleepDisabled(on);
}

bool AskText(const std::string& what_for, const std::string& initial, std::string& out) {
    SwkbdConfig keyboard;
    if (R_FAILED(swkbdCreate(&keyboard, 0))) {
        return false;
    }
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetHeaderText(&keyboard, what_for.c_str());
    swkbdConfigSetGuideText(&keyboard, what_for.c_str());
    swkbdConfigSetInitialText(&keyboard, initial.c_str());
    swkbdConfigSetStringLenMax(&keyboard, 32);
    char text[200] = {};
    const Result result = swkbdShow(&keyboard, text, sizeof text);
    swkbdClose(&keyboard);
    if (R_FAILED(result) || text[0] == 0) {
        return false;
    }
    out = text;
    return true;
}

} // namespace Platform

// ---------------------------------------------------------------------------------------------

namespace {

using namespace SwitchFrontend::Ui;

/// A direction held repeats: after a pause, then steadily.
struct Repeat {
    float held{};
    float next{};

    bool Step(bool down, bool is_held, float dt) {
        if (down) {
            held = 0.0f;
            next = 0.38f;
            return true;
        }
        if (!is_held) {
            held = 0.0f;
            return false;
        }
        held += dt;
        if (held >= next) {
            next += 0.09f;
            return true;
        }
        return false;
    }
};

void Present(Framebuffer& fb, Canvas& canvas) {
    u32 pitch = 0;
    auto* base = static_cast<u8*>(framebufferBegin(&fb, &pitch));
    const int stride = static_cast<int>(pitch / sizeof(u32));
    auto* dst = reinterpret_cast<u32*>(base);
    const u32* src = canvas.Data();
    for (int y = 0; y < canvas.Height(); ++y) {
        std::memcpy(dst + y * stride, src + y * canvas.Width(), static_cast<std::size_t>(canvas.Width()) * 4);
    }
    framebufferEnd(&fb);
}

/// A sentence on an otherwise empty screen, until a button: for what can't go on.
void Say(Framebuffer& fb, Canvas& canvas, PadState& pad, const std::string& text) {
    Font font;
    font.InitLibrary();
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) != 0) {
            break;
        }
        canvas.Clear(kColBg);
        int y = canvas.Height() / 2 - 20;
        for (const std::string& line : WrapLines(font, text, 22, canvas.Width() - 200)) {
            font.Draw(canvas, (canvas.Width() - font.Measure(line, 22)) / 2, y, line, 22, kColText);
            y += 30;
        }
        const char* hint = "Press any button";
        font.Draw(canvas, (canvas.Width() - font.Measure(hint, 17)) / 2, y + 24, hint, 17, kColTextDim);
        Present(fb, canvas);
    }
}

} // namespace

int main(int, char**) {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    Framebuffer fb;
    framebufferCreate(&fb, nwindowGetDefault(), kPanelW, kPanelH, PIXEL_FORMAT_RGBA_8888, 2);
    framebufferMakeLinear(&fb);

    Flow::SetCardRoot("sdmc:");
    auto canvas = std::make_unique<Canvas>();
    auto app = std::make_unique<Flow::App>();
    app->Start();

    Repeat up, down, left, right;
    u64 last = armGetSystemTick();
    while (appletMainLoop()) {
        const u64 now = armGetSystemTick();
        const float dt = std::min(0.1f, static_cast<float>(armTicksToNs(now - last)) / 1e9f);
        last = now;

        padUpdate(&pad);
        const u64 pressed = padGetButtonsDown(&pad), held = padGetButtons(&pad);
        Flow::Input in;
        in.a = pressed & HidNpadButton_A;
        in.b = pressed & HidNpadButton_B;
        in.x = pressed & HidNpadButton_X;
        in.y = pressed & HidNpadButton_Y;
        in.l = pressed & HidNpadButton_L;
        in.r = pressed & HidNpadButton_R;
        in.zl = pressed & HidNpadButton_ZL;
        in.zr = pressed & HidNpadButton_ZR;
        in.plus = pressed & HidNpadButton_Plus;
        in.minus = pressed & HidNpadButton_Minus;
        in.up = up.Step(pressed & HidNpadButton_AnyUp, held & HidNpadButton_AnyUp, dt);
        in.down = down.Step(pressed & HidNpadButton_AnyDown, held & HidNpadButton_AnyDown, dt);
        in.left = left.Step(pressed & HidNpadButton_AnyLeft, held & HidNpadButton_AnyLeft, dt);
        in.right = right.Step(pressed & HidNpadButton_AnyRight, held & HidNpadButton_AnyRight, dt);

        const Flow::Request request = app->Update(in, dt);
        if (request.kind == Flow::Request::Kind::Exit) {
            break;
        }
        if (request.kind == Flow::Request::Kind::Launch) {
            // The game loader, started next by the homebrew loader with the container to run,
            // this menu to come back to and, when the game's settings have one, the room the
            // loader is to keep (docs/layout.md). Each argument in quotes: a file's name has spaces.
            const std::string loader = Flow::OnCard(Flow::kGameNro);
            std::string args = "\"" + loader + "\" \"" + Flow::OnCard(request.nup) + "\" \"menu=" + Flow::OnCard(Flow::kMenuNro) + "\"";
            if (!request.room.empty()) {
                args += " \"room=" + request.room + "\"";
            }
            if (!envHasNextLoad() || R_FAILED(envSetNextLoad(loader.c_str(), args.c_str()))) {
                Say(fb, *canvas, pad, "This homebrew loader can't start another program after this one, so the game can't be started from here.");
                continue;
            }
            break;
        }
        app->Draw(*canvas);
        Present(fb, *canvas);
        // While a file is moving the screen is redrawn a few times a second: this thread comes
        // before the ones that move it, so what it doesn't take is theirs.
        if (const float pause = app->FramePause(); pause > 0.0f) {
            svcSleepThread(static_cast<s64>(pause * 1e9f));
        }
    }
    app->Finish();
    Transfer::Stop();
    framebufferClose(&fb);
    return 0;
}
