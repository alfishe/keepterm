// The workspace module's API for the UI, the commands and the control CLI.
//
// Include this one header. Pieces:
//   Workspace         model.h, workspace.h: projects, tabs, split trees, windows; queries,
//                     operations (WsResult), the change bus (subscribe / Transaction).
//   SplitTree         split_tree.h: pure tree algorithms, layout() for the view.
//   SessionBinder     binder.h: pane <-> session host; the view reports link events to it.
//   PersistScheduler  persist.h: debounced atomic saves of workspace.json.
//   UsageStore        usage.h: palette_usage.json.
//   WorkspaceRuntime  runtime.h: all of the above wired to the disk and real session hosts.
//
// Threading: everything on the UI thread. Async results come back through the Poster.
//
// Usage (the UI's start-up and a few operations):
//
//   auto rt = kt::ws::WorkspaceRuntime::create({.post = [&](auto fn) { loop.post(std::move(fn)); },
//                                                .config = &cfg});
//   kt::ws::Workspace& ws = rt->model();
//   if (!rt->load_outcome().notice.empty()) show_notice(rt->load_outcome().notice);
//
//   auto sub = ws.subscribe([&](std::span<const kt::ws::Change> batch) {
//       for (const auto& c : batch) {
//           switch (c.kind) {
//               case kt::ws::ChangeKind::Reset: rebuild_everything(); break;
//               case kt::ws::ChangeKind::TreeChanged: relayout(c.tab); break;
//               case kt::ws::ChangeKind::PaneChanged:
//                   if (c.fields & kt::ws::field::kBind) {
//                       const kt::ws::Pane* p = ws.pane(c.pane);
//                       if (p && p->bind == kt::ws::BindState::Ready) attach_link(p->id, p->endpoint);
//                   }
//                   break;
//               case kt::ws::ChangeKind::PaneRemoved: close_link(c.pane); break;
//               default: break;
//           }
//       }
//   });
//   for (const auto& w : ws.windows()) create_window(w);   // state() holds the loaded windows
//   rt->start();                                          // probes / spawns every pane's host
//
//   // A command handler (`pane.split_right`):
//   if (auto r = ws.split_pane(focused, kt::ws::Direction::Right); !r) report(r.error().message);
//   // Close with confirmation (`Busy` -> ask, then force):
//   if (auto r = ws.close_tab(tab, false); !r && r.error().code == kt::ws::WsErrc::Busy) {
//       if (confirm()) (void)ws.close_tab(tab, true);
//   }
//   // The view's link events:
//   rt->binder().on_first_snapshot(pane);   // the first KTSN of an attached link
//   ws.set_pane_cwd(pane, path, kt::ws::cwd_source_from_string(src).value_or(kt::ws::CwdSource::LastKnown));
//   rt->binder().on_exit(pane, code);       // EXIT_STATUS
//   rt->binder().on_host_lost(pane);        // disconnected without EXIT_STATUS
//   // Layout for the view (DIPs):
//   for (const auto& f : ws.tab(t)->tree.layout(area, 1.0)) place(f.pane, f.rect);
//   // About once a second (palette usage saves; the state file has its own timer):
//   rt->tick();
//   // Quit:
//   rt->flush();
#pragma once

#include "workspace/binder.h"
#include "workspace/change.h"
#include "workspace/error.h"
#include "workspace/ids.h"
#include "workspace/model.h"
#include "workspace/paths.h"
#include "workspace/persist.h"
#include "workspace/runtime.h"
#include "workspace/split_tree.h"
#include "workspace/state.h"
#include "workspace/usage.h"
#include "workspace/workspace.h"
