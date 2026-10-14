// The classes `ui` owns, registered into scene's registry
// (architecture.md §2, rule 3).
//
// The third module to hand its descriptors down, after `render` and `input`,
// and the first at L5. Everything behind this call is generated from
// `api/defs/*.api.luau`.
#pragma once

#include "engine/core/name_atom.h"
#include "engine/scene/class_registry.h"

namespace engine::ui {

// `ScreenGui`, `UIObject` and its nine descendants, the three layout modifiers,
// and `UIService`.
//
// MUST run after `scene::generated::registerClasses`, like the other two: every
// one of these extends `Instance`, and a subclass is registered by naming its
// parent's `ClassId`. `app` orders the calls (engine/app/src/world_host.cpp).
void registerSceneTypes(scene::ClassRegistry& classes, core::AtomTable& atoms);

} // namespace engine::ui
