#include "render/damage.h"

namespace kt::render {

namespace {

// Rectangles of a minus b (up to 4).
void subtract(IRect a, IRect b, std::vector<IRect>& out) {
    const IRect i = intersect(a, b);
    if (i.empty()) {
        out.push_back(a);
        return;
    }
    if (i.y > a.y) out.push_back(IRect{a.x, a.y, a.w, i.y - a.y});
    if (i.bottom() < a.bottom()) out.push_back(IRect{a.x, i.bottom(), a.w, a.bottom() - i.bottom()});
    if (i.x > a.x) out.push_back(IRect{a.x, i.y, i.x - a.x, i.h});
    if (i.right() < a.right()) out.push_back(IRect{i.right(), i.y, a.right() - i.right(), i.h});
}

}  // namespace

std::vector<IRect> make_disjoint(const std::vector<IRect>& in) {
    std::vector<IRect> out, pending, next;
    for (const IRect& r : in) {
        pending.assign(1, r);
        for (const IRect& d : out) {
            next.clear();
            for (const IRect& p : pending) subtract(p, d, next);
            pending.swap(next);
            if (pending.empty()) break;
        }
        for (const IRect& p : pending) {
            if (!p.empty()) out.push_back(p);
        }
    }
    return out;
}

std::vector<IRect> normalize_damage(const Damage& damage, PxSize size) {
    const IRect surface{0, 0, size.w, size.h};
    std::vector<IRect> rects;
    if (surface.empty()) return rects;
    if (damage.full) {
        rects.push_back(surface);
        return rects;
    }
    for (const IRect& r : damage.rects) {
        const IRect c = intersect(r, surface);
        if (!c.empty()) rects.push_back(c);
    }
    if (rects.size() > kMaxDamageRects) {
        IRect b{};
        for (const IRect& r : rects) b = unite(b, r);
        rects.assign(1, b);
    }
    return make_disjoint(rects);
}

bool clip_copy(IRect src, int32_t dst_x, int32_t dst_y, PxSize size, IRect& src_out, IRect& dst_out) {
    const IRect surface{0, 0, size.w, size.h};
    IRect sr = intersect(src, surface);
    const int32_t dx = dst_x + (sr.x - src.x), dy = dst_y + (sr.y - src.y);
    const IRect dr = intersect(IRect{dx, dy, sr.w, sr.h}, surface);
    if (sr.empty() || dr.empty()) return false;
    sr = IRect{sr.x + (dr.x - dx), sr.y + (dr.y - dy), dr.w, dr.h};
    src_out = sr;
    dst_out = dr;
    return true;
}

}  // namespace kt::render
