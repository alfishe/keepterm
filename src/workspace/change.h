// Change notifications of the workspace model. Core code.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

#include "session/ids.h"
#include "workspace/ids.h"

namespace kt::ws {

enum class ChangeKind : std::uint16_t {
    Reset,                       // whole model replaced (load)
    ProjectAdded, ProjectRemoved, ProjectChanged, ProjectsReordered,
    TabAdded, TabRemoved, TabChanged, TabsReordered, TabMoved /* across projects */,
    TreeChanged /* structure or ratios */, FocusChanged, ZoomChanged,
    PaneAdded, PaneRemoved, PaneChanged,
    WindowOpened, WindowClosed, WindowChanged,
    AttentionCountChanged,
    RecentFoldersChanged, QuickFrameChanged,
};

// Bits of Change::fields for the *Changed kinds (and two flags of PaneRemoved).
namespace field {
// ProjectChanged
inline constexpr std::uint32_t kName = 1u << 0, kColor = 1u << 1, kShell = 1u << 2, kPath = 1u << 3;
// TabChanged
inline constexpr std::uint32_t kTitle = 1u << 0, kPinned = 1u << 1;
// PaneChanged
inline constexpr std::uint32_t kCwd = 1u << 0, kAttention = 1u << 1, kPrivate = 1u << 2, kAgent = 1u << 3,
                               kSession = 1u << 4, kRestore = 1u << 5, kBind = 1u << 6, kPaneShell = 1u << 7,
                               kAgentPending = 1u << 8, kPendingRun = 1u << 9,
                               kReadOnly = 1u << 10;
// PaneRemoved: the shell ended by itself (no kill was sent); kDetached: the pane left the model
// but its session keeps running (preset Replace with "keep sessions", detach_tab).
inline constexpr std::uint32_t kExited = 1u << 0, kDetached = 1u << 1;
// WindowChanged
inline constexpr std::uint32_t kShown = 1u << 0, kActiveTab = 1u << 1, kFrame = 1u << 2, kSidebar = 1u << 3,
                               kKey = 1u << 4;
}  // namespace field

struct Change {
    ChangeKind kind = ChangeKind::Reset;
    ProjectId project{};
    TabId tab{};
    PaneId pane{};
    WindowId window{};
    std::uint32_t fields = 0;
    bool persistent = true;                       // false: transient (focus, Running, restore, bind)
    std::optional<session::SessionId> session;   // PaneAdded, PaneRemoved: the pane's session
};

class Workspace;

// RAII batch. Nested transactions merge into the outermost; the outermost delivers at
// destruction (or commit()). abort() drops the batch: the caller has already rolled back.
class Transaction {
public:
    Transaction() = default;
    explicit Transaction(Workspace* ws);
    Transaction(Transaction&& o) noexcept;
    Transaction& operator=(Transaction&& o) noexcept;
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction();
    void commit();
    void abort();

private:
    Workspace* ws_ = nullptr;
};

// Unsubscribes at destruction. Must not outlive the Workspace.
class Subscription {
public:
    Subscription() = default;
    Subscription(Workspace* ws, std::uint64_t id) : ws_(ws), id_(id) {}
    Subscription(Subscription&& o) noexcept;
    Subscription& operator=(Subscription&& o) noexcept;
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;
    ~Subscription();
    void reset();

private:
    Workspace* ws_ = nullptr;
    std::uint64_t id_ = 0;
};

using ChangeHandler = std::function<void(std::span<const Change>)>;

}  // namespace kt::ws
