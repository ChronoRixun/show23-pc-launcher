#include "std_include.hpp"
#include "check_inputs_cli.hpp"
#include "show23/setup_run.hpp"

#include <cstdio>
#include <cstring>
#include <utils/properties.hpp>
#include <utils/property_keys.hpp>
#include <utils/string.hpp>
#include <rapidjson/writer.h>

namespace
{
    int usage()
    {
        std::fputs("Usage: show23-pc-tests.exe (--check-inputs | --setup) --base <path> [--update <path>] --install <folder> [--unpacker <program>]\n", stderr);
        return 2;
    }

    void print(const show23::finding& finding, const char* stage = nullptr)
    {
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> out(buffer);
        out.StartObject();
        if (stage) { out.Key("stage"); out.String(stage); }
        out.Key("id");
        out.String(finding.id.c_str());
        out.Key("state");
        out.String(show23::state_name(finding.state));
        out.Key("values");
        out.StartObject();
        for (const auto& [key, value] : finding.values)
        {
            out.Key(key.c_str());
            if (const auto number = std::get_if<std::uint64_t>(&value)) out.Uint64(*number);
            else out.String(std::get<std::string>(value).c_str());
        }
        out.EndObject();
        out.EndObject();
        std::puts(buffer.GetString());
    }
}

int run_check_inputs_cli(const std::span<wchar_t*> args)
{
    if (args.empty() || args.size() % 2 != 1) return usage();
    const std::wstring_view mode(args.front());
    if (mode != L"--check-inputs" && mode != L"--setup") return usage();
    show23::setup_request request{show23::source_kind::automatic, {}, std::nullopt, {}};
    for (std::size_t at = 1; at < args.size(); at += 2)
    {
        const std::wstring_view key(args[at]);
        const std::wstring_view path(args[at + 1]);
        if (path.empty() || path.starts_with(L"--")) return usage();
        if (key == L"--base" && request.base.empty()) request.base = path;
        else if (key == L"--install" && request.install.empty()) request.install = path;
        else if (key == L"--update" && !request.update) request.update = show23::fs::path(path);
        else if (mode == L"--setup" && key == L"--unpacker" && request.unpacker.empty()) request.unpacker = path;
        else return usage();
    }
    if (request.base.empty() || request.install.empty()) return usage();
    try
    {
        if (mode == L"--setup")
        {
            auto options = show23::default_setup_options();
            options.require_preview = true;
            if (request.unpacker.empty())
                request.unpacker = utils::string::utf8_to_path(utils::properties::load(property_keys::UNPACKER).value_or(""));
            const auto confirm = [](const char* prompt, const char* answers)
            {
                std::fputs(prompt, stderr);
                std::fflush(stderr);
                char input[32]{};
                if (!std::fgets(input, sizeof(input), stdin) || !std::strchr(input, '\n')) return '\0';
                return input[1] == '\n' && std::strchr(answers, input[0]) ? input[0] : '\0';
            };
            show23::setup_report result;
            for (;;)
            {
                std::uint64_t last{};
                result = show23::run_setup(request, options, [&](const auto& status)
                {
                    if (!status.extraction || status.extraction->elapsed < last + 10) return;
                    const auto& row = *status.extraction;
                    last = row.elapsed;
                    print({"unpack.progress", show23::finding_state::notice,
                        {{"elapsed", row.elapsed}, {"written", row.written}, {"expected", row.expected}}}, "unpack");
                    std::fflush(stdout);
                });
                const auto& unpack = result.stages[static_cast<std::size_t>(show23::setup_stage::unpack)];
                if (unpack.state != show23::stage_state::stopped || unpack.findings.empty()) break;
                const auto& decision = unpack.findings.back();
                if (decision.id == "unpack.existing")
                {
                    const auto choice = confirm("Use existing installed copy or replace it? [k/r, Enter cancels] ", "kr");
                    if (!choice) break;
                    request.existing_copy = choice == 'k' ? "keep" : "replace";
                }
                else if (decision.id == "unpack.consent" || decision.id == "unpack.preview")
                {
                    request.displayed_unpacker = std::get<std::string>(decision.values.at("path"));
                    std::fprintf(stderr, "Unpacker: %s\n", request.displayed_unpacker.c_str());
                    if (decision.id == "unpack.consent")
                    {
                        if (!confirm("This program will run with your permissions. Continue? [y/N] ", "y")) break;
                        request.unpacker_consent = std::get<std::string>(decision.values.at("consent"));
                    }
                }
                else break;
            }
            for (std::size_t i = 0; i < result.stages.size(); ++i)
                for (const auto& row : result.stages[i].findings)
                    print(row, show23::stage_name(static_cast<show23::setup_stage>(i)));
            return result.finished() ? 0 : 1;
        }
        const auto result = show23::check_inputs(request, show23::default_input_check_options());
        for (const auto& row : result.findings) print(row);
        return result.passed() ? 0 : 1;
    }
    catch (const std::exception&)
    {
        print({"check.internal", show23::finding_state::failed, {}}, mode == L"--setup" ? "check" : nullptr);
        return 1;
    }
}
