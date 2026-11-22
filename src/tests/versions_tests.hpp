#pragma once

#include <optional>
#include <span>

std::optional<int> fake_bootstrap_child(std::span<wchar_t*> args);
