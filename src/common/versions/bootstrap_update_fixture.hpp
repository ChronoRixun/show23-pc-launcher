#pragma once

#include "records.hpp"

#ifdef SHOW23_UPDATE_FIXTURE
namespace versions::fixture
{
    bool commit_pointer(const fs::path& root, const pointer& old, const pointer& next, std::string_view step);
}
#endif
