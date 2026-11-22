#pragma once

#include "show23/setup.hpp"
#include "updater/part_store.hpp"
#include <utils/properties.hpp>
#include <utils/string.hpp>
#include <algorithm>
#include <cstdio>

// Include in a disposable native-test build and dispatch from wmain before
// the normal CLI: if (argc > 1 && std::wstring_view(argv[1]) ==
// L"--m4-runtime-proof") return m4_runtime_proof(argc, argv);
inline int m4_runtime_proof(const int argc, wchar_t** argv)
{
    if (argc != 5 || !*argv[2] || !*argv[3] || !*argv[4]) return 2;
    namespace fs = std::filesystem;
    const fs::path install = argv[2];
    const fs::path bundle_subpath = argv[3];
    const auto launcher = utils::string::path_to_utf8(fs::path(argv[4]));
    const auto portable = utils::properties::get_portable_root();
    if (!parts::ordinary_path(portable / "portable.marker") ||
        !parts::ordinary_path(install / ".m4-runtime-proof") || bundle_subpath.has_root_path() ||
        std::ranges::find(bundle_subpath, fs::path("..")) != bundle_subpath.end()) return 2;

    const auto bundle = bundle_subpath == "." ? portable : portable / bundle_subpath;
    if (show23::install_folder() != install && !show23::remember_install(install)) return 3;
    if (show23::install_folder() != install) return 3;
    if (!parts::stage_bundled_runtime(bundle, install, launcher))
    {
        std::string failure;
        parts::verify_runtime(parts::read_runtime(bundle), launcher, failure);
        std::fprintf(stderr, "Could not stage the bundled runtime. %s\n", failure.c_str());
        return 4;
    }

    const auto runtime = parts::read_runtime(install);
    std::string result;
    if (!parts::verify_runtime(runtime, launcher, result))
    {
        std::fprintf(stderr, "%s\n", result.c_str());
        return 5;
    }
    std::printf("%s Active runtime %s\n", result.c_str(), runtime.root.filename().string().c_str());
    return 0;
}
