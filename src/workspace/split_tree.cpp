#include "workspace/split_tree.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace kt::ws {

namespace {
constexpr double kTieEps = 1e-6;
constexpr int kMaxTreeDepth = static_cast<int>(SplitTree::kMaxDepth);

double clamp(double v, double lo, double hi) { return std::min(std::max(v, lo), hi); }

std::size_t mru_rank(std::span<const PaneId> mru, PaneId p) {
    for (std::size_t i = 0; i < mru.size(); ++i)
        if (mru[i] == p) return i;
    return std::numeric_limits<std::size_t>::max();
}
}  // namespace

Direction auto_split_direction(const RectDip& r) {
    if (r.w <= 0 || r.h <= 0) return Direction::Right;
    return r.w >= r.h ? Direction::Right : Direction::Down;
}

SplitTree::SplitTree(PaneId root_pane) {
    Node n;
    n.kind = Node::Kind::Leaf;
    n.pane = root_pane;
    root_ = alloc(n);
}

std::uint32_t SplitTree::alloc(Node n) {
    if (!free_.empty()) {
        const std::uint32_t i = free_.back();
        free_.pop_back();
        nodes_[i] = n;
        return i;
    }
    nodes_.push_back(n);
    return static_cast<std::uint32_t>(nodes_.size() - 1);
}

void SplitTree::release(std::uint32_t i) {
    nodes_[i] = Node{};
    free_.push_back(i);
}

std::uint32_t SplitTree::find_leaf(PaneId p) const {
    for (std::uint32_t i = 0; i < nodes_.size(); ++i)
        if (nodes_[i].kind == Node::Kind::Leaf && nodes_[i].pane == p) return i;
    return kNil;
}

void SplitTree::collect(std::uint32_t n, std::vector<PaneId>& out) const {
    if (n == kNil) return;
    const Node& node = nodes_[n];
    if (node.kind == Node::Kind::Leaf) {
        out.push_back(node.pane);
        return;
    }
    collect(node.first, out);
    collect(node.second, out);
}

std::vector<PaneId> SplitTree::panes() const {
    std::vector<PaneId> out;
    collect(root_, out);
    return out;
}

bool SplitTree::contains(PaneId p) const { return find_leaf(p) != kNil; }

std::size_t SplitTree::size() const {
    std::size_t n = 0;
    for (const Node& node : nodes_)
        if (node.kind == Node::Kind::Leaf) ++n;
    return n;
}

void SplitTree::layout_rec(std::uint32_t n, RectDip r, double div, std::vector<PaneFrame>* panes,
                           std::vector<DividerFrame>* divs) const {
    const Node& node = nodes_[n];
    if (node.kind == Node::Kind::Leaf) {
        if (panes) panes->push_back({node.pane, r});
        return;
    }
    const double ratio = clamp(node.ratio, 0.0, 1.0);
    if (node.axis == Axis::Horizontal) {
        const double avail = std::max(0.0, r.w - div);
        const double w1 = avail * ratio;
        const RectDip a{r.x, r.y, w1, r.h};
        const RectDip d{r.x + w1, r.y, std::min(div, r.w), r.h};
        const RectDip b{r.x + w1 + std::min(div, r.w), r.y, avail - w1, r.h};
        if (divs) divs->push_back({NodeRef{n}, node.axis, d, r});
        layout_rec(node.first, a, div, panes, divs);
        layout_rec(node.second, b, div, panes, divs);
    } else {
        const double avail = std::max(0.0, r.h - div);
        const double h1 = avail * ratio;
        const RectDip a{r.x, r.y, r.w, h1};
        const RectDip d{r.x, r.y + h1, r.w, std::min(div, r.h)};
        const RectDip b{r.x, r.y + h1 + std::min(div, r.h), r.w, avail - h1};
        if (divs) divs->push_back({NodeRef{n}, node.axis, d, r});
        layout_rec(node.first, a, div, panes, divs);
        layout_rec(node.second, b, div, panes, divs);
    }
}

std::vector<PaneFrame> SplitTree::layout(RectDip area, double divider_dip) const {
    std::vector<PaneFrame> out;
    if (root_ != kNil) layout_rec(root_, area, std::max(0.0, divider_dip), &out, nullptr);
    return out;
}

std::vector<DividerFrame> SplitTree::dividers(RectDip area, double divider_dip) const {
    std::vector<DividerFrame> out;
    if (root_ != kNil) layout_rec(root_, area, std::max(0.0, divider_dip), nullptr, &out);
    return out;
}

std::optional<PaneId> SplitTree::neighbor(PaneId from, Direction dir, std::span<const PaneId> mru) const {
    const auto frames = layout({0, 0, 1, 1}, 0);
    const PaneFrame* cur = nullptr;
    for (const auto& f : frames)
        if (f.pane == from) cur = &f;
    if (!cur) return std::nullopt;
    const RectDip c = cur->rect;
    std::optional<PaneId> best;
    double best_dist = std::numeric_limits<double>::max();
    std::size_t best_rank = std::numeric_limits<std::size_t>::max();
    for (const auto& f : frames) {   // tree order
        if (f.pane == from) continue;
        const RectDip& r = f.rect;
        bool cand = false;
        switch (dir) {
            case Direction::Left: cand = r.cx() < c.cx() && r.y + r.h > c.y && r.y < c.y + c.h; break;
            case Direction::Right: cand = r.cx() > c.cx() && r.y + r.h > c.y && r.y < c.y + c.h; break;
            case Direction::Up: cand = r.cy() < c.cy() && r.x + r.w > c.x && r.x < c.x + c.w; break;
            case Direction::Down: cand = r.cy() > c.cy() && r.x + r.w > c.x && r.x < c.x + c.w; break;
        }
        if (!cand) continue;
        const double dist = axis_of(dir) == Axis::Horizontal ? std::abs(c.cx() - r.cx()) : std::abs(c.cy() - r.cy());
        const std::size_t rank = mru_rank(mru, f.pane);
        if (dist < best_dist - kTieEps) {
            best = f.pane;
            best_dist = dist;
            best_rank = rank;
        } else if (std::abs(dist - best_dist) <= kTieEps && rank < best_rank) {
            best = f.pane;
            best_rank = rank;
        }
    }
    return best;
}

std::optional<PaneId> SplitTree::nearest_to(const RectDip& rect, std::span<const PaneId> candidates) const {
    std::optional<PaneId> best;
    double best_d = std::numeric_limits<double>::max();
    for (const auto& f : layout({0, 0, 1, 1}, 0)) {
        if (std::find(candidates.begin(), candidates.end(), f.pane) == candidates.end()) continue;
        const double dx = f.rect.cx() - rect.cx(), dy = f.rect.cy() - rect.cy();
        const double d = dx * dx + dy * dy;
        if (d < best_d - kTieEps) {
            best_d = d;
            best = f.pane;
        }
    }
    return best;
}

std::size_t SplitTree::depth_rec(std::uint32_t n) const {
    if (n == kNil) return 0;
    const Node& node = nodes_[n];
    if (node.kind == Node::Kind::Leaf) return 1;
    return 1 + std::max(depth_rec(node.first), depth_rec(node.second));
}

std::size_t SplitTree::depth() const { return depth_rec(root_); }

std::uint32_t SplitTree::nearest_ancestor(std::uint32_t leaf, Axis axis) const {
    for (std::uint32_t p = nodes_[leaf].parent; p != kNil; p = nodes_[p].parent)
        if (nodes_[p].axis == axis) return p;
    return kNil;
}

std::optional<double> SplitTree::ratio_of(PaneId pane, Axis axis) const {
    const std::uint32_t leaf = find_leaf(pane);
    if (leaf == kNil) return std::nullopt;
    const std::uint32_t a = nearest_ancestor(leaf, axis);
    if (a == kNil) return std::nullopt;
    return nodes_[a].ratio;
}

bool SplitTree::split(PaneId target, Direction dir, PaneId new_pane, double ratio) {
    const std::uint32_t leaf = find_leaf(target);
    if (leaf == kNil || contains(new_pane) || target == new_pane) return false;
    if (!(ratio > 0.0 && ratio < 1.0)) return false;
    if (depth() >= static_cast<std::size_t>(kMaxTreeDepth)) return false;
    Node nl;
    nl.kind = Node::Kind::Leaf;
    nl.pane = new_pane;
    const std::uint32_t added = alloc(nl);
    Node br;
    br.kind = Node::Kind::Branch;
    br.axis = axis_of(dir);
    br.ratio = ratio;
    br.parent = nodes_[leaf].parent;
    const std::uint32_t b = alloc(br);
    const std::uint32_t parent = nodes_[b].parent;
    if (parent == kNil) {
        root_ = b;
    } else if (nodes_[parent].first == leaf) {
        nodes_[parent].first = b;
    } else {
        nodes_[parent].second = b;
    }
    if (is_forward(dir)) {
        nodes_[b].first = leaf;
        nodes_[b].second = added;
    } else {
        nodes_[b].first = added;
        nodes_[b].second = leaf;
    }
    nodes_[leaf].parent = b;
    nodes_[added].parent = b;
    return true;
}

std::uint32_t SplitTree::unlink_leaf(std::uint32_t leaf) {
    const std::uint32_t p = nodes_[leaf].parent;
    const std::uint32_t sib = nodes_[p].first == leaf ? nodes_[p].second : nodes_[p].first;
    const std::uint32_t gp = nodes_[p].parent;
    nodes_[sib].parent = gp;
    if (gp == kNil) {
        root_ = sib;
    } else if (nodes_[gp].first == p) {
        nodes_[gp].first = sib;
    } else {
        nodes_[gp].second = sib;
    }
    release(p);
    release(leaf);
    return sib;
}

WsResult<PaneId> SplitTree::close(PaneId target, std::span<const PaneId> mru) {
    const std::uint32_t leaf = find_leaf(target);
    if (leaf == kNil) return ws_error(WsErrc::NotFound, "no such pane in the tab");
    if (nodes_[leaf].parent == kNil) return ws_error(WsErrc::LastPane, "the last pane of a tab cannot be removed");
    RectDip closed{};
    const auto before = layout({0, 0, 1, 1}, 0);
    for (const auto& f : before)
        if (f.pane == target) closed = f.rect;
    const std::uint32_t sib = unlink_leaf(leaf);
    std::vector<PaneId> cands;
    collect(sib, cands);
    for (PaneId m : mru)
        if (m != target && std::find(cands.begin(), cands.end(), m) != cands.end()) return m;
    // Nearest in the layout the user saw (before the sibling grew).
    std::optional<PaneId> best;
    double best_d = std::numeric_limits<double>::max();
    for (const auto& f : before) {
        if (std::find(cands.begin(), cands.end(), f.pane) == cands.end()) continue;
        const double dx = f.rect.cx() - closed.cx(), dy = f.rect.cy() - closed.cy();
        const double d = dx * dx + dy * dy;
        if (d < best_d - kTieEps) {
            best_d = d;
            best = f.pane;
        }
    }
    return best ? *best : cands.front();
}

WsResult<PaneId> SplitTree::detach(PaneId target, std::span<const PaneId> mru) { return close(target, mru); }

bool SplitTree::insert(PaneId target, Direction side, PaneId pane, double ratio) {
    return split(target, side, pane, ratio);
}

bool SplitTree::replace(PaneId old_pane, PaneId new_pane) {
    const std::uint32_t leaf = find_leaf(old_pane);
    if (leaf == kNil || (old_pane != new_pane && contains(new_pane))) return false;
    nodes_[leaf].pane = new_pane;
    return true;
}

bool SplitTree::resize(PaneId target, Direction dir, double delta) {
    const std::uint32_t leaf = find_leaf(target);
    if (leaf == kNil || !std::isfinite(delta)) return false;
    const std::uint32_t a = nearest_ancestor(leaf, axis_of(dir));
    if (a == kNil) return false;
    const double next = nodes_[a].ratio + (is_forward(dir) ? delta : -delta);
    nodes_[a].ratio = clamp(next, kMinRatio, kMaxRatio);
    return true;
}

bool SplitTree::set_ratio(PaneId target, Axis axis, double ratio) {
    const std::uint32_t leaf = find_leaf(target);
    if (leaf == kNil || !std::isfinite(ratio)) return false;
    const std::uint32_t a = nearest_ancestor(leaf, axis);
    if (a == kNil) return false;
    nodes_[a].ratio = clamp(ratio, kMinRatio, kMaxRatio);
    return true;
}

bool SplitTree::set_branch_ratio(NodeRef branch, double ratio, double min_ratio, double max_ratio) {
    if (branch.index >= nodes_.size() || nodes_[branch.index].kind != Node::Kind::Branch) return false;
    if (!std::isfinite(ratio)) return false;
    double lo = std::max(0.01, std::isfinite(min_ratio) ? min_ratio : 0.01);
    double hi = std::min(0.99, std::isfinite(max_ratio) ? max_ratio : 0.99);
    if (lo > hi) lo = hi = (lo + hi) / 2;   // the window is too small for both minimums
    nodes_[branch.index].ratio = clamp(ratio, lo, hi);
    return true;
}

int SplitTree::units(std::uint32_t n, Axis axis) const {
    const Node& node = nodes_[n];
    if (node.kind == Node::Kind::Leaf) return 1;
    if (node.axis != axis) return 1;
    return units(node.first, axis) + units(node.second, axis);
}

void SplitTree::equalize_rec(std::uint32_t n) {
    if (n == kNil || nodes_[n].kind != Node::Kind::Branch) return;
    const int a = units(nodes_[n].first, nodes_[n].axis);
    const int b = units(nodes_[n].second, nodes_[n].axis);
    if (a + b > 0) nodes_[n].ratio = static_cast<double>(a) / static_cast<double>(a + b);
    equalize_rec(nodes_[n].first);
    equalize_rec(nodes_[n].second);
}

void SplitTree::equalize() { equalize_rec(root_); }

bool SplitTree::swap(PaneId a, PaneId b) {
    if (a == b) return false;
    const std::uint32_t la = find_leaf(a), lb = find_leaf(b);
    if (la == kNil || lb == kNil) return false;
    std::swap(nodes_[la].pane, nodes_[lb].pane);
    return true;
}

bool SplitTree::move(PaneId target, Direction dir, std::span<const PaneId> mru) {
    const auto n = neighbor(target, dir, mru);
    return n && swap(target, *n);
}

json::Value SplitTree::to_json_rec(std::uint32_t n, const std::function<json::Value(PaneId)>& leaf) const {
    const Node& node = nodes_[n];
    json::Value out = json::Value::object();
    if (node.kind == Node::Kind::Leaf) {
        out.set("pane", leaf(node.pane));
        return out;
    }
    json::Value s = json::Value::object();
    s.set("axis", json::Value::string(node.axis == Axis::Horizontal ? "horizontal" : "vertical"));
    s.set("ratio", json::Value::number(node.ratio));
    s.set("first", to_json_rec(node.first, leaf));
    s.set("second", to_json_rec(node.second, leaf));
    out.set("split", std::move(s));
    return out;
}

json::Value SplitTree::to_json(const std::function<json::Value(PaneId)>& leaf) const {
    if (root_ == kNil) return json::Value();
    return to_json_rec(root_, leaf);
}

bool SplitTree::from_json_rec(const json::Value& v, std::uint32_t parent, std::uint32_t& out,
                              const std::function<std::optional<PaneId>(const json::Value&)>& leaf, int depth,
                              int* repaired) {
    if (depth > kMaxTreeDepth || !v.is_object()) return false;
    if (const json::Value* p = v.get("pane")) {
        auto id = leaf(*p);
        if (!id || contains(*id)) return false;
        Node n;
        n.kind = Node::Kind::Leaf;
        n.pane = *id;
        n.parent = parent;
        out = alloc(n);
        return true;
    }
    const json::Value* s = v.get("split");
    if (!s || !s->is_object()) return false;
    const json::Value* first = s->get("first");
    const json::Value* second = s->get("second");
    if (!first || !second) return false;
    Node n;
    n.kind = Node::Kind::Branch;
    n.parent = parent;
    const json::Value* axis = s->get("axis");
    if (axis && axis->as_string() && *axis->as_string() == "vertical") n.axis = Axis::Vertical;
    else if (axis && axis->as_string() && *axis->as_string() == "horizontal") n.axis = Axis::Horizontal;
    else return false;
    double ratio = 0.5;
    if (const json::Value* r = s->get("ratio")) {
        auto d = r->as_double();
        if (!d) return false;
        ratio = *d;
    }
    if (!(ratio > 0.0 && ratio < 1.0)) {
        ratio = std::isfinite(ratio) ? clamp(ratio, 0.05, 0.95) : 0.5;
        if (repaired) ++*repaired;
    }
    n.ratio = ratio;
    const std::uint32_t me = alloc(n);
    std::uint32_t a = kNil, b = kNil;
    if (!from_json_rec(*first, me, a, leaf, depth + 1, repaired)) return false;
    if (!from_json_rec(*second, me, b, leaf, depth + 1, repaired)) return false;
    nodes_[me].first = a;
    nodes_[me].second = b;
    out = me;
    return true;
}

WsResult<SplitTree> SplitTree::from_json(const json::Value& v,
                                         const std::function<std::optional<PaneId>(const json::Value&)>& leaf,
                                         int* repaired) {
    SplitTree t;
    std::uint32_t root = kNil;
    if (!t.from_json_rec(v, kNil, root, leaf, 0, repaired))
        return ws_error(WsErrc::StateCorrupt, "bad split tree");
    t.root_ = root;
    return t;
}

bool SplitTree::same_rec(std::uint32_t a, const SplitTree& o, std::uint32_t b) const {
    if ((a == kNil) != (b == kNil)) return false;
    if (a == kNil) return true;
    const Node& x = nodes_[a];
    const Node& y = o.nodes_[b];
    if (x.kind != y.kind) return false;
    if (x.kind == Node::Kind::Leaf) return x.pane == y.pane;
    return x.axis == y.axis && x.ratio == y.ratio && same_rec(x.first, o, y.first) && same_rec(x.second, o, y.second);
}

bool SplitTree::same_shape(const SplitTree& o) const { return same_rec(root_, o, o.root_); }

bool SplitTree::valid() const {
    if (root_ == kNil) return true;
    if (root_ >= nodes_.size() || nodes_[root_].parent != kNil) return false;
    std::set<PaneId> seen;
    std::size_t reached = 0;
    std::vector<std::uint32_t> stack{root_};
    while (!stack.empty()) {
        const std::uint32_t n = stack.back();
        stack.pop_back();
        if (n >= nodes_.size()) return false;
        ++reached;
        const Node& node = nodes_[n];
        if (node.kind == Node::Kind::Free) return false;
        if (node.kind == Node::Kind::Leaf) {
            if (!seen.insert(node.pane).second) return false;
            continue;
        }
        if (!(node.ratio > 0 && node.ratio < 1)) return false;
        if (node.first == kNil || node.second == kNil) return false;
        if (nodes_[node.first].parent != n || nodes_[node.second].parent != n) return false;
        stack.push_back(node.first);
        stack.push_back(node.second);
    }
    return reached + free_.size() == nodes_.size();
}

}  // namespace kt::ws
