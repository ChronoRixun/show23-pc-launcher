#include "std_include.hpp"
#include "setup_verification.hpp"
#include "input_paths.hpp"

namespace show23
{
    const folder_reference& reference_folder(const bool update)
    {
        static constexpr folder_reference base{{53'634'165'210, 2'522'885'828, 78'289'835, 93'674'489}, {{
            {1'374'546, "999d75729730fb63b9d751d9cbb74070ec69066263102b35416199b5212855a1"},
            {306'016, "93626fc102571a5e32a78337b336f7cfc251e75dcc94facbbaef0fc21f5c40d4"},
            {457'105, "6d65cc5efefc7cbb1eb3b162dfbcc88d28058ebd9e036a51781555df0f992d74"},
            {24'552, "934cbffd30c0579081cb47e6365b6178a599d944a810b599dd62705c961a43f2"},
            {352'641, "67412b11eefb06dc36cbd1935cca9545896c4a0567c2ed11d60a3e12c1ffcfb3"}
        }}};
        static constexpr folder_reference patch{{26'535'887'124, 796'250'185, 40'251'784, 71'352'930}, {{
            {1'374'546, "f3b35e234b07148fca5811e3d0c890b8125639cc14e8d8783ba91c5d1c76fe42"},
            {306'016, "531a97fff1f229c1f1f066d62cbed88004fd98fcdef72162e0402c6fda045944"},
            {457'105, "031446b80a2c74665a552d4aa76b7549d52f6e38048ef42409c89613ca3c9a71"},
            {24'552, "9acbd9d0cc1047d00c2d3272bf5784cec1d05f52cdc67b592e64b1dcace2c2cb"},
            {352'641, "43a28ea85d5774352bfae2313aedd42d0263d82193d84bd8e0e79d65718735fb"}
        }}};
        return update ? patch : base;
    }

    bool findings_pass(const std::vector<finding>& findings)
    {
        return !findings.empty() && std::ranges::none_of(findings, [](const auto& row)
        {
            return row.state == finding_state::failed;
        });
    }

    game_folder verify_folder(const fs::path& root, const bool update, const executable_reference& executable,
                              const folder_reference& reference, std::vector<finding>& findings)
    {
        using enum finding_state;
        const std::string part = update ? "update" : "base";
        const auto add = [&](const std::string& kind, finding_state state, finding_values values = {})
        {
            values.emplace("part", part);
            findings.push_back({"verify." + kind, state, std::move(values)});
        };
        game_folder record{root, update ? "1.21" : "1.00"};
        const auto folder = input_paths::inspect(root);
        if (folder.error != input_paths::path_error::none || !folder.directory)
        {
            add("tree", failed);
            return record;
        }
        record.path = folder.path;
        const auto entry = [&](const char* name, const bool directory, const bool read = false)
        {
            auto item = input_paths::inspect(folder.path / name, false, read);
            if (!item.present || item.error != input_paths::path_error::none || item.directory != directory)
                add("entry", failed, {{"name", std::string(name)}});
            return item;
        };
        const auto binary = entry("eboot.bin", false, true);
        if (binary.present && !binary.directory && binary.error == input_paths::path_error::none)
        {
            const auto digest = binary.length == executable.size ? input_paths::sha256(binary) : std::nullopt;
            const auto known = digest && *digest == executable.sha256;
            add("executable", known ? passed : failed);
            if (known)
            {
                record.executable_size = binary.length;
                record.executable_sha256 = *digest;
            }
        }
        entry("sce_sys/param.sfo", false, true);
        entry(update ? "PATCHLIST.BIN" : "FILELIST.BIN", false, true);
        if (!update) entry("BOOT", true);
        entry("sce_module", true);
        for (std::size_t i = 0; i < module_names.size(); ++i)
        {
            const std::string name = module_names[i];
            const auto module = entry(("sce_module/" + name).c_str(), false, true);
            if (!module.present || module.directory || module.error != input_paths::path_error::none) continue;
            const auto& expected = reference.modules[i];
            if (module.length != expected.size)
            {
                add("moduleSize", failed, {{"name", name}, {"actual", module.length}, {"expected", expected.size}});
                continue;
            }
            const auto digest = input_paths::sha256(module);
            if (!digest) add("entry", failed, {{"name", name}});
            else if (*digest != expected.sha256) add("moduleDigest", notice, {{"name", name}});
        }
        for (std::size_t i = 0; i < directory_names.size(); ++i)
        {
            const std::string name = directory_names[i];
            const auto directory = entry(directory_names[i], true);
            if (!directory.directory || directory.error != input_paths::path_error::none) continue;
            const auto size = input_paths::folder_size(directory.path);
            if (!size)
            {
                add("entry", failed, {{"name", name}});
                continue;
            }
            record.directories[i] = *size;
            const auto expected = reference.directories[i];
            const auto difference = *size < expected ? expected - *size : *size - expected;
            add("directory", *size < expected ? failed : *size > expected ? notice : passed,
                {{"name", name}, {"difference", difference}, {"actual", *size}});
        }
        if (!input_paths::folder_size(folder.path)) add("tree", failed);
        return record;
    }

    std::vector<finding> verify_recorded_game(const game_record& record)
    {
        const input_check_options options;
        std::vector<finding> findings;
        verify_folder(record.base.path, false, options.base, reference_folder(false), findings);
        if (record.update) verify_folder(record.update->path, true, options.update, reference_folder(true), findings);
        return findings;
    }
}
