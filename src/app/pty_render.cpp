#include "app/pty_render.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app/png_writer.h"
#include "pty/pty.h"
#include "render/software/presenter.h"
#include "render/software/software_backend.h"
#include "text/font.h"
#include "text/font_source.h"
#include "view/terminal/atlas_textures.h"
#include "view/terminal/terminal_view.h"
#include "vt/terminal.h"

namespace kt::app {

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: keepterm --pty-render <out.png> [--size COLSxROWS] [--font-size PT] [--timeout SEC]"
                 " -- <program> [args...]\n");
    return 2;
}

bool parse_size(std::string_view s, uint16_t& cols, uint16_t& rows) {
    const size_t x = s.find('x');
    if (x == std::string_view::npos) return false;
    const long c = std::strtol(std::string(s.substr(0, x)).c_str(), nullptr, 10);
    const long r = std::strtol(std::string(s.substr(x + 1)).c_str(), nullptr, 10);
    if (c < 1 || c > 1000 || r < 1 || r > 1000) return false;
    cols = static_cast<uint16_t>(c);
    rows = static_cast<uint16_t>(r);
    return true;
}

// Terminal replies (DA, DSR, CPR, ...) go back to the program.
struct ReplySink final : vt::TerminalSink {
    pty::IPty* pty = nullptr;
    void reply(std::span<const std::byte> bytes) override {
        if (pty) (void)pty->write(bytes);
    }
};

}  // namespace

int pty_render(int argc, char** argv, int i) {
    if (i >= argc) return usage();
    const std::string out_path = argv[i++];
    uint16_t cols = 80, rows = 24;
    float font_pt = 12.0f;
    double timeout_s = 10.0;
    for (; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--") {
            ++i;
            break;
        }
        if (i + 1 >= argc) return usage();
        if (a == "--size") {
            if (!parse_size(argv[++i], cols, rows)) return usage();
        } else if (a == "--font-size") {
            font_pt = std::strtof(argv[++i], nullptr);
            if (!(font_pt >= 4.0f && font_pt <= 96.0f)) return usage();
        } else if (a == "--timeout") {
            timeout_s = std::strtod(argv[++i], nullptr);
            if (!(timeout_s > 0.0 && timeout_s <= 3600.0)) return usage();
        } else {
            return usage();
        }
    }
    if (i >= argc) return usage();

    // Fonts: the OS default monospace family plus system fallback.
    auto fonts = text::FontSystem::create();
    if (!fonts) {
        std::fprintf(stderr, "keepterm: %s\n", fonts.error().what.c_str());
        return 1;
    }
    std::unique_ptr<text::IFontSource> source = text::create_font_source();
    text::FontSpec spec;
    spec.size_pt = font_pt;
    auto files = text::resolve_font_files(*source, spec);
    if (!files) {
        std::fprintf(stderr, "keepterm: no monospace font: %s\n", files.error().what.c_str());
        return 1;
    }
    (*fonts)->set_font_source(source.get());
    (*fonts)->atlas().set_raster_budget(std::chrono::microseconds(0));   // no budget: one complete frame
    auto set = (*fonts)->font_set(spec, *files, 1.0f, text::RenderMode::gray);
    if (!set) {
        std::fprintf(stderr, "keepterm: %s\n", set.error().what.c_str());
        return 1;
    }
    const text::CellMetrics& m = (*set)->metrics();

    // Terminal and PTY.
    ReplySink sink;
    const vt::GridSize grid{cols, rows, static_cast<uint16_t>(m.cell_w), static_cast<uint16_t>(m.cell_h)};
    auto term = vt::Terminal::create(grid, vt::TerminalOptions{}, &sink);
    if (!term) {
        std::fprintf(stderr, "keepterm: %s\n", term.error().what.c_str());
        return 1;
    }
    vt::FramePublisher& frames = (*term)->frames();

    pty::SpawnSpec ss;
    for (; i < argc; ++i) ss.argv.emplace_back(argv[i]);
    ss.size = pty::GridSize{cols, rows, static_cast<uint16_t>(cols * m.cell_w), static_cast<uint16_t>(rows * m.cell_h)};
    ss.env_edits.push_back({"TERM", std::string("xterm-256color")});
    ss.env_edits.push_back({"COLORTERM", std::string("truecolor")});
    auto p = pty::spawn(ss);
    if (!p) {
        std::fprintf(stderr, "keepterm: %s\n", p.error().what.c_str());
        return 127;
    }
    pty::IPty& pt = **p;
    sink.pty = &pt;

    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    std::atomic<bool> timed_out{false};
    std::thread timer([&] {
        std::unique_lock lk(mu);
        if (!cv.wait_for(lk, std::chrono::duration<double>(timeout_s), [&] { return done; })) {
            timed_out = true;
            pt.wake();
        }
    });
    std::vector<std::byte> buf(64 * 1024);
    int rc = 0;
    for (;;) {
        auto r = pt.read(buf);
        if (!r) {
            std::fprintf(stderr, "keepterm: %s\n", r.error().what.c_str());
            rc = 1;
            break;
        }
        if (r->bytes) (*term)->feed(std::span(buf.data(), r->bytes));
        if (r->eof || (r->woken && timed_out)) break;
    }
    {
        std::lock_guard lk(mu);
        done = true;
    }
    cv.notify_all();
    timer.join();
    if (timed_out) std::fprintf(stderr, "keepterm: timeout, rendering the screen as it is\n");
    pt.close(std::chrono::milliseconds(500));
    (*term)->publish();

    // Render.
    const vt::Frame* frame = frames.acquire();
    if (!frame) {
        std::fprintf(stderr, "keepterm: no frame\n");
        return 1;
    }
    const render::PxSize px{cols * m.cell_w, rows * m.cell_h};
    render::sw::SoftwareRenderer renderer;
    if (auto a = renderer.attach_presenter(std::make_unique<render::sw::MemoryPresenter>(render::PixelOrder::rgba), px, 1.0f); !a) {
        std::fprintf(stderr, "keepterm: %s\n", a.error().what.c_str());
        return 1;
    }
    {
        view::AtlasTextures textures((*fonts)->atlas(), renderer);
        view::TerminalView tv(**set, textures);
        tv.set_bounds(render::IRect{0, 0, px.w, px.h});
        render::DrawList dl;
        view::BuildResult br;
        const view::TerminalScrollback sb(**term);
        tv.build(*frame, dl, br, &sb);
        frames.release(frame);
        (void)renderer.begin_frame(render::Damage{br.damage, true});
        renderer.submit(dl);
        (void)renderer.end_frame();
        (void)renderer.present();
        auto img = renderer.read_back(render::IRect{0, 0, px.w, px.h});
        if (!img) {
            std::fprintf(stderr, "keepterm: %s\n", img.error().what.c_str());
            return 1;
        }
        if (auto w = write_png_rgb(out_path, *img); !w) {
            std::fprintf(stderr, "keepterm: %s\n", w.error().what.c_str());
            return 1;
        }
    }
    std::fprintf(stderr, "keepterm: %s %dx%d px, %ux%u cells, font %s %.1fpt\n", out_path.c_str(), px.w, px.h,
                 unsigned{cols}, unsigned{rows}, files->family.c_str(), static_cast<double>(font_pt));
    return rc;
}

}  // namespace kt::app
