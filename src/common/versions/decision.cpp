#include "decision.hpp"

#include <algorithm>
#include <utility>

namespace versions
{
    namespace
    {
        const verification* lookup(const disk_view& view, std::string_view name)
        {
            const auto it = view.folders.find(name);
            return it == view.folders.end() ? nullptr : &it->second;
        }

        bool good(const disk_view& view, std::string_view name)
        {
            const auto* item = lookup(view, name);
            return item && static_cast<bool>(*item);
        }

        bool confirmed(const disk_view& view, std::string_view name)
        {
            return good(view, name) && view.confirmed_folders.contains(name);
        }

        bool allows_arrival(const disk_view& view, std::string_view name)
        {
            return !view.stable_only || folder_version(name).find('-') == std::string_view::npos;
        }

        void clear_failed(pointer& state, const std::string& folder)
        {
            std::erase_if(state.failed, [&](const auto& item) { return item.folder == folder; });
        }

        void mark_failed(pointer& state, const std::string& folder, const verification* result, std::string reason)
        {
            const auto fingerprint = result && !result->fingerprint.empty()
                ? result->fingerprint : std::string(64, '0');
            for (auto& c : reason) if (c < 32 || c > 126) c = '?';
            reason.resize(std::min<std::size_t>(reason.size(), 256));
            clear_failed(state, folder);
            state.failed.insert(state.failed.begin(), {folder, fingerprint, std::move(reason)});
            if (state.failed.size() > 8) state.failed.pop_back();
        }
    }

    int compare_slots(std::string_view a, std::string_view b)
    {
        if (const auto order = compare(a, b)) return order;
        if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
        return a == b ? 0 : a < b ? -1 : 1;
    }

    std::string previous_for_dialog(const disk_view& view, std::string_view attempted)
    {
        return attempted != view.state.previous && confirmed(view, view.state.previous)
            ? view.state.previous : std::string{};
    }

    decision failed_commit(const disk_view& view)
    {
        if (view.where == step::confirmed) return {action::wait};
        const auto& fallback = confirmed(view, view.state.current) ? view.state.current : view.state.previous;
        if (confirmed(view, fallback))
        {
            if (view.prepare) return {action::prepared, {}, fallback};
            return view.launches < launch_limit
                ? decision{action::launch_current, {}, fallback} : decision{action::dialog_current};
        }
        return {action::dialog_write};
    }

    decision decide(const disk_view& view)
    {
        auto next = view.state;
        switch (view.where)
        {
        case step::validate_names:
        {
            if (const auto* current = lookup(view, next.current); current && current->retryable)
            {
                const auto other = previous_for_dialog(view, next.current);
                if (!other.empty()) return {view.prepare ? action::prepared : action::launch_current, {}, other};
                return {action::dialog_none};
            }

            // These bytes may be valid, but the current folder name cannot confirm a repack.
            std::string changed_current;
            for (auto* name : {&next.current, &next.previous, &next.trial})
            {
                if (name->empty() || good(view, *name)) continue;
                const auto* bad = lookup(view, *name);
                if (bad && bad->retryable) continue;
                if (bad && bad->needs_confirmation) { name->clear(); continue; }
                mark_failed(next, *name, bad, "damaged:" + (bad ? bad->bad_file : std::string("version.txt")));
                name->clear();
            }
            if (!next.current.empty() && !confirmed(view, next.current))
                changed_current = std::exchange(next.current, {});
            const auto* previous = lookup(view, next.previous);
            if (!next.previous.empty() && !confirmed(view, next.previous) && !(previous && previous->retryable))
                next.previous.clear();
            if (next.current.empty() && confirmed(view, next.previous)) next.current = std::exchange(next.previous, {});
            if (next.current == next.trial) next.trial.clear();
            if (next.trial.empty())
            {
                next.starts = 0;
                if (!changed_current.empty())
                {
                    const auto* replacement = lookup(view, changed_current);
                    const auto prior_failure = std::ranges::any_of(next.failed, [&](const auto& failure) {
                        return failure.folder == changed_current && replacement && failure.fingerprint == replacement->fingerprint;
                    });
                    if (!prior_failure) next.trial = std::move(changed_current);
                }
            }
            if (next != view.state) return {action::next, next};
            return {action::next};
        }

        case step::process_request:
            if (!view.request_present) return {action::next};
            if (view.incoming && view.incoming->id == next.last_request) return {action::next, {}, {}, true};
            if (view.incoming)
            {
                const auto* target = lookup(view, view.incoming->trial);
                if (target && target->retryable) return {action::next};
            }
            if (!view.incoming || !good(view, view.incoming->trial) ||
                view.incoming->trial == next.current ||
                (view.incoming->reason == "switch-to-stable" &&
                    folder_version(view.incoming->trial).find('-') != std::string_view::npos))
                return {action::next, {}, {}, true, true};
            next.trial = view.incoming->trial;
            next.starts = 0;
            next.last_request = view.incoming->id;
            clear_failed(next, next.trial);
            return {action::next, next, {}, true};

        case step::arrival:
        {
            if (!next.trial.empty() && view.explicit_request) return {action::next};
            const auto pending = lookup(view, next.trial);
            std::string candidate;
            for (const auto& [name, result] : view.folders)
            {
                const auto recovering = next.trial.empty() && next.current.empty() && name == view.recovered;
                if (!next.trial.empty() &&
                    (!view.published_folders.contains(name) || folder_version(name) != folder_version(next.trial) ||
                     compare_slots(name, next.trial) <= 0 || (pending && pending->fingerprint == result.fingerprint)))
                    continue;
                if (!valid_folder(name) || (!recovering && !allows_arrival(view, name)) ||
                    view.rejected_arrivals.contains(name) || name == next.current || name == next.previous ||
                    (!next.current.empty() && compare(name, next.current) <= 0) ||
                    std::ranges::any_of(next.failed, [&](const auto& failed)
                        { return failed.folder == name && failed.fingerprint == result.fingerprint; })) continue;
                if (recovering)
                {
                    candidate = name;
                    break;
                }
                if (candidate.empty() || compare_slots(candidate, name) < 0) candidate = name;
            }
            if (candidate.empty()) return {action::next};
            if (candidate != view.checked_arrival) return {action::scan, {}, candidate};
            if (view.arrival_result)
            {
                next.trial = candidate;
                next.starts = 0;
                clear_failed(next, candidate);
            }
            else if (view.arrival_result.retryable) return {action::next};
            else mark_failed(next, candidate, &view.arrival_result, "damaged:" + view.arrival_result.bad_file);
            return {action::next, next};
        }

        case step::choose:
            if (view.prepare)
            {
                if (confirmed(view, next.current)) return {action::prepared, {}, next.current};
                if (good(view, next.trial) && next.starts < trial_starts) return {action::prepared, {}, next.trial};
                return {action::dialog_none};
            }
            if (const auto* trial = lookup(view, next.trial); trial && trial->retryable)
            {
                if (confirmed(view, next.current)) return {action::launch_current, {}, next.current};
                return {action::dialog_none};
            }
            if (!next.trial.empty() && next.starts >= trial_starts)
            {
                mark_failed(next, next.trial, lookup(view, next.trial),
                    view.last_reason.empty() ? "interrupted-start" : view.last_reason);
                next.trial.clear();
                next.starts = 0;
                return {action::next, next};
            }
            if (view.launches >= launch_limit)
                return {confirmed(view, next.current) ? action::dialog_current : action::dialog_none};
            if (view.launches + 1 >= launch_limit && confirmed(view, next.current))
                return {action::launch_current, {}, next.current};
            if (!next.trial.empty())
            {
                ++next.starts;
                return {action::launch_trial, next, next.trial};
            }
            if (confirmed(view, next.current)) return {action::launch_current, {}, next.current};
            return {action::dialog_none};

        case step::confirmed:
            if (next.trial.empty()) return {action::wait};
            next.previous = next.current;
            next.current = std::exchange(next.trial, {});
            next.starts = 0;
            return {action::wait, next};

        case step::retry:
            for (const auto& failed : next.failed)
                if (failed.folder != next.current && good(view, failed.folder))
                    return {action::next, {}, failed.folder};
            return {action::dialog_none};
        }
        return {action::dialog_none};
    }
}
