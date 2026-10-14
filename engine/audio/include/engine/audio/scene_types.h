// The classes `audio` owns, registered into scene's registry
// (architecture.md §2, rule 3).
#pragma once

#include "engine/core/name_atom.h"
#include "engine/scene/class_registry.h"

namespace engine::audio {

// `Sound`, `AudioGroup` and `AudioService`.
//
// MUST run after `scene::generated::registerClasses`, like every other module's:
// all three extend `Instance`, and a subclass is registered by naming its
// parent's `ClassId`.
void registerSceneTypes(scene::ClassRegistry& classes, core::AtomTable& atoms);

} // namespace engine::audio
