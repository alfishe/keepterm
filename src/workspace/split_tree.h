// The split tree of one tab. Core code.
//
// A binary tree in flat index storage: copying a tree (snapshots) is one vector copy. Leaves hold
// pane ids; branches hold an axis and the share of their first child. All geometry is in
// device-independent units; the view snaps to pixels.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "workspace/error.h"
#include "workspace/ids.h"
#include "workspace/json.h"

namespace kt::ws {

enum class Axis : std::uint8_t {
    Horizontal,   // first | second, left-right
    Vertical,     // first over second, top-bottom
};
enum class Direction : std::uint8_t { Left, Right, Up, Down };

inline Axis axis_of(Direction d) { return d == Direction::Left || d == Direction::Right ? Axis::Horizontal : Axis::Vertical; }
inline bool is_forward(Direction d) { return d == Direction::Right || d == Direction::Down; }

struct RectDip {
    double x = 0, y = 0, w = 0, h = 0;
    double cx() const { return x + w / 2; }
    double cy() const { return y + h / 2; }
    bool operator==(const RectDip&) const = default;
};

inline constexpr double kMinRatio = 0.15, kMaxRatio = 0.85;   // keyboard and CLI clamp (Macterm)
inline constexpr double kResizeStep = 0.03;                    // one key press
inline constexpr std::size_t kFocusHistory = 16;               // per-tab MRU of focused panes

struct PaneFrame {
    PaneId pane;
    RectDip rect;
};

// A branch of the tree, for divider drags. Valid until the next structural change.
struct NodeRef {
    std::uint32_t index = 0xFFFFFFFF;
    bool operator==(const NodeRef&) const = default;
};

struct DividerFrame {
    NodeRef branch;
    Axis axis = Axis::Horizontal;
    RectDip rect;      // the divider line (thickness = divider_dip)
    RectDip area;      // the branch's whole area: ratio = (pointer - area.x) / area.w
};

// "Split automatically": Right when the pane is at least as wide as tall, Down otherwise; a pane
// never laid out (w or h zero) splits Right.
Direction auto_split_direction(const RectDip& pane_rect);

class SplitTree {
public:
    static constexpr std::size_t kMaxDepth = 48;   // JSON nesting stays inside the parser's limit

    SplitTree() = default;                 // empty; only for containers and from_json
    explicit SplitTree(PaneId root_pane);

    // Queries
    std::vector<PaneId> panes() const;    // tree order (depth-first, first before second)
    bool contains(PaneId) const;
    std::size_t size() const;             // panes
    bool empty() const { return root_ == kNil; }
    std::vector<PaneFrame> layout(RectDip area, double divider_dip = 0) const;
    std::vector<DividerFrame> dividers(RectDip area, double divider_dip = 0) const;
    // Geometric neighbor. `mru` (most recent first) breaks ties within 1e-6.
    std::optional<PaneId> neighbor(PaneId from, Direction, std::span<const PaneId> mru = {}) const;
    // The leaf nearest to the center of `rect` (unit-square layout) among `candidates`.
    std::optional<PaneId> nearest_to(const RectDip& rect, std::span<const PaneId> candidates) const;
    std::size_t depth() const;            // 1 for a single pane
    // The ratio of the nearest ancestor of `pane` on `axis`; nullopt when there is none.
    std::optional<double> ratio_of(PaneId pane, Axis axis) const;

    // Mutations. Each returns false (and changes nothing) for an unknown pane.
    // split: the new pane goes to the side named by `dir` (Left/Up -> first child). `ratio` is
    // the share of the first child, (0, 1).
    bool split(PaneId target, Direction dir, PaneId new_pane, double ratio = 0.5);
    // close: the sibling subtree takes the parent's place. Returns the pane to focus: the most
    // recent of `mru` inside the sibling subtree, else the sibling leaf nearest to the closed
    // pane's center. LastPane when size() == 1.
    WsResult<PaneId> close(PaneId target, std::span<const PaneId> mru = {});
    bool resize(PaneId target, Direction dir, double delta = kResizeStep);
    bool set_ratio(PaneId target, Axis axis, double ratio);   // nearest ancestor on `axis`, clamped
    // Divider drag: any ratio inside [min_ratio, max_ratio] (the view computes them from the cell
    // minimums), and always inside (0.01, 0.99).
    bool set_branch_ratio(NodeRef branch, double ratio, double min_ratio = 0.01, double max_ratio = 0.99);
    void equalize();
    bool swap(PaneId a, PaneId b);
    bool move(PaneId target, Direction dir, std::span<const PaneId> mru = {});   // swap with neighbor
    // Detach a pane (to move it to another tab); same rules as close().
    WsResult<PaneId> detach(PaneId target, std::span<const PaneId> mru = {});
    // Insert an existing pane next to `target` (drag and drop). Same sides as split().
    bool insert(PaneId target, Direction side, PaneId pane, double ratio = 0.5);
    // Replace a leaf's pane id (ids are unique per tree).
    bool replace(PaneId old_pane, PaneId new_pane);

    // Serialization: {"pane": <leaf(pane)>} or {"split": {axis, ratio, first, second}}.
    json::Value to_json(const std::function<json::Value(PaneId)>& leaf) const;
    // `leaf` decodes a pane object and returns its id, or nullopt (structural error).
    // Ratios outside (0, 1) are clamped to [0.05, 0.95] and counted in *repaired.
    static WsResult<SplitTree> from_json(const json::Value&,
                                         const std::function<std::optional<PaneId>(const json::Value&)>& leaf,
                                         int* repaired = nullptr);

    // Structure and ratios equal (tests, change detection).
    bool same_shape(const SplitTree& o) const;
    // Every invariant holds (parent links, one leaf per pane, ratios in (0,1)). Tests.
    bool valid() const;

private:
    static constexpr std::uint32_t kNil = 0xFFFFFFFF;
    struct Node {
        enum class Kind : std::uint8_t { Leaf, Branch, Free } kind = Kind::Free;
        Axis axis{};
        double ratio = 0.5;
        std::uint32_t first = kNil, second = kNil, parent = kNil;
        PaneId pane{};
    };

    std::uint32_t alloc(Node n);
    void release(std::uint32_t i);
    std::uint32_t find_leaf(PaneId) const;
    void collect(std::uint32_t n, std::vector<PaneId>& out) const;
    void layout_rec(std::uint32_t n, RectDip r, double div, std::vector<PaneFrame>* panes,
                    std::vector<DividerFrame>* divs) const;
    std::uint32_t nearest_ancestor(std::uint32_t leaf, Axis axis) const;
    std::uint32_t unlink_leaf(std::uint32_t leaf);   // returns the sibling subtree root
    int units(std::uint32_t n, Axis axis) const;
    void equalize_rec(std::uint32_t n);
    std::size_t depth_rec(std::uint32_t n) const;
    bool same_rec(std::uint32_t a, const SplitTree& o, std::uint32_t b) const;
    json::Value to_json_rec(std::uint32_t n, const std::function<json::Value(PaneId)>& leaf) const;
    bool from_json_rec(const json::Value& v, std::uint32_t parent, std::uint32_t& out,
                       const std::function<std::optional<PaneId>(const json::Value&)>& leaf, int depth, int* repaired);

    std::vector<Node> nodes_;
    std::uint32_t root_ = kNil;
    std::vector<std::uint32_t> free_;
};

}  // namespace kt::ws
