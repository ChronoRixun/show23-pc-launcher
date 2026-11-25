#include "std_include.hpp"
#include "resolution.hpp"

namespace show23
{
    const resolution* find_resolution(std::string_view id)
    {
        const auto found = std::ranges::find(resolutions, id, &resolution::id);
        return found == std::end(resolutions) ? nullptr : &*found;
    }
}
