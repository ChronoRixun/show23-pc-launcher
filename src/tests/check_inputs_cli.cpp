#include "std_include.hpp"
#include "check_inputs_cli.hpp"
#include "show23/setup_run.hpp"

#include <cstdio>
#include <rapidjson/writer.h>

namespace
{
    int usage()
    {
        std::fputs("Usage: show23-pc-tests.exe (--check-inputs | --setup) --base <path> [--update <path>] --install <folder>\n", stderr);
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
        else return usage();
    }
    if (request.base.empty() || request.install.empty()) return usage();
    try
    {
        if (mode == L"--setup")
        {
            const auto result = show23::run_setup(request, show23::default_setup_options());
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
