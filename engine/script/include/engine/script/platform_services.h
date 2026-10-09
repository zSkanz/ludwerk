#pragma once
#include <span>

#include "engine/script/instance_binding.h"
namespace engine::script {
[[nodiscard]] std::span<const InstanceMethodBinding> platformServiceMethods();
}
