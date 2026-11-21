#pragma once

#include "release_channel.hpp"

#include <cstdint>
#include <optional>

// A component's manifest/check has a lifetime separate from the release feed cache. Call under
// that component's mutex. A switch invalidates immediately; the old worker retains its slot until
// finish(), which asks its caller to recheck. Even Stable -> Beta -> Stable rejects the old result.
namespace channel_refresh
{
    struct ticket
    {
        std::uint64_t generation{};
        release_channel::channel channel = release_channel::channel::stable;
    };

    class gate
    {
    public:
        void invalidate()
        {
            ++generation_;
            pending_ = true;
            resolved_.reset();
        }

        std::optional<ticket> begin(const release_channel::channel channel)
        {
            if (active_) return std::nullopt;
            active_ = true;
            pending_ = false;
            resolved_.reset();
            return ticket{generation_, channel};
        }

        bool current(const ticket& work) const
        {
            return work.generation == generation_;
        }

        // true: a switch happened while this worker was alive; start another check after unlocking.
        bool finish(const ticket& work, const bool success)
        {
            active_ = false;
            if (current(work) && success) resolved_ = work;
            return !current(work);
        }

        bool ready(const release_channel::channel channel) const
        {
            return !busy() && resolved_ && current(*resolved_) && resolved_->channel == channel;
        }

        bool busy() const
        {
            return active_ || pending_; // keep polling during the handoff from an invalidated worker
        }

    private:
        std::uint64_t generation_{};
        bool pending_{};
        bool active_{};
        std::optional<ticket> resolved_;
    };
}
