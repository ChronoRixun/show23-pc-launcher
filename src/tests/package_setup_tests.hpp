#pragma once
#include <span>
#include <optional>

std::optional<int> fake_unpacker(std::span<wchar_t*> args);
