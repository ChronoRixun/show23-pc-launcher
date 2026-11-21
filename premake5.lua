dependencies = {
    basePath = "./deps"
}

function dependencies.load()
    dir = path.join(dependencies.basePath, "premake/*.lua")
    deps = os.matchfiles(dir)

    for i, dep in pairs(deps) do
        dep = dep:gsub(".lua", "")
        require(dep)
    end
end

function dependencies.imports()
    for i, proj in pairs(dependencies) do
        if type(i) == 'number' then
            proj.import()
        end
    end
end

function dependencies.projects()
    for i, proj in pairs(dependencies) do
        if type(i) == 'number' then
            proj.project()
        end
    end
end

newoption {
    trigger = "copy-to",
    description = "Optional, copy the EXE to a custom folder after build, define the path here if wanted.",
    value = "PATH"
}


local function git_value(command)
    if not (os.isdir(".git") or os.isfile(".git")) then return "" end
    local process = assert(io.popen("git " .. command .. " 2>nul", "r"))
    local value = process:read("*a"):gsub("%s+$", "")
    process:close()
    return value
end

local function version_info()
    local tag = git_value("describe --tags --abbrev=0 --match v[0-9]*")
    local describe = git_value("describe --tags --dirty --always --match v[0-9]*")
    local major, minor, patch = tag:match("^v(%d+)%.(%d+)%.(%d+)")
    local core = table.concat({major or "0", minor or "1", patch or "0"}, ".")
    local revision = tonumber(git_value("rev-list --count HEAD")) or 0
    return tag, describe == "" and core .. "-dev" or describe, core, math.min(revision, 65535)
end

local function write_changed(pathname, content)
    if io.readfile(pathname) == content then return end
    local file = assert(io.open(pathname, "wb"))
    file:write(content)
    file:close()
end

newaction {
    trigger = "version",
    description = "Print this source tree's version.",
    execute = function()
        local _, describe = version_info()
        print(describe)
    end
}

newaction {
    trigger = "generate-buildinfo",
    description = "Generate version headers for the launcher and the Windows resource.",
    execute = function()
        local tag, describe, core, revision = version_info()
        local rc = core:gsub("%.", ",")
        os.mkdir("build/src")
        local header = string.format([[#pragma once
#define GIT_DESCRIBE %q
#define GIT_TAG %q
#define VERSION_PRODUCT_RC %s
#define VERSION_PRODUCT %q
#define VERSION_FILE_RC %s,%d
#define VERSION_FILE %q
#define VERSION GIT_DESCRIBE
#define SHORTVERSION VERSION_PRODUCT
]], describe, tag, rc, core, rc, revision, core .. "." .. revision)
        write_changed("build/src/version.h", header)
        write_changed("build/src/version.hpp", '#pragma once\n#include "version.h"\n')
    end
}

dependencies.load()

workspace "show23-pc"
startproject "show23-pc"
location "./build"
objdir "%{wks.location}/obj"
targetdir "%{wks.location}/bin/%{cfg.platform}/%{cfg.buildcfg}"

configurations {"Debug", "Release"}

language "C++"
cppdialect "C++20"

buildoptions {"/utf-8"}

architecture "x86_64"
platforms "x64"

systemversion "latest"
symbols "On"
staticruntime "On"
editandcontinue "Off"
warnings "Extra"
characterset "ASCII"

if os.getenv("CI") then
    defines {"CI_BUILD"}
end

flags {"NoIncrementalLink", "NoMinimalRebuild", "MultiProcessorCompile", "No64BitChecks"}

filter "platforms:x64"
    defines {"_WINDOWS", "WIN32"}
filter {}

filter "configurations:Release"
    optimize "Full"
    buildoptions {"/GL"}
    linkoptions {"/IGNORE:4702", "/LTCG"}
    defines {"NDEBUG"}
    flags {"FatalCompileWarnings"}
    buildoptions {"/utf-8"}
filter {}

filter "configurations:Debug"
    optimize "Debug"
    defines {"DEBUG", "_DEBUG", "_ITERATOR_DEBUG_LEVEL=0"}
filter {}

project "common"
kind "StaticLib"
language "C++"

buildoptions {"/guard:cf"}

files {"./src/common/**.hpp", "./src/common/**.cpp"}

includedirs {"./src/common", "%{prj.location}/src"}

resincludedirs {"$(ProjectDir)src"}

dependencies.imports()

project "show23-pc"
kind "WindowedApp"
language "C++"

targetname "show23-pc"

pchheader "std_include.hpp"
pchsource "src/launcher/std_include.cpp"

-- /DEPENDENTLOADFLAG:0x800 = LOAD_LIBRARY_SEARCH_SYSTEM32 for all delay-loaded DLLs.
linkoptions {"/IGNORE:4254", "/DYNAMICBASE", "/HIGHENTROPYVA", "/GUARD:CF", "/CETCOMPAT", "/LARGEADDRESSAWARE", "/PDBCompress", "/DEPENDENTLOADFLAG:0x800"}
buildoptions {"/guard:cf"}

files {"./src/launcher/**.rc", "./src/launcher/**.hpp", "./src/launcher/**.cpp", "./src/launcher/**.manifest", "./src/launcher/resource/**.ico"}

includedirs {"./src/launcher", "./src/common", "%{prj.location}/src"}

resincludedirs {"$(ProjectDir)src"}

links {"common"}

prebuildcommands { 'pushd "%{_MAIN_SCRIPT_DIR}"', '"' .. _PREMAKE_COMMAND .. '" generate-buildinfo', "popd" }

if _OPTIONS["copy-to"] then
    postbuildcommands {"copy /y \"$(TargetPath)\" \"" .. _OPTIONS["copy-to"] .. "\""}
end

dependencies.imports()


-- Native unit tests are independent of the browser and exercise synthetic fixtures.
project "show23-pc-tests"
kind "ConsoleApp"
language "C++"
files {"./src/tests/**.hpp", "./src/tests/**.cpp",
       "./src/launcher/updater/swap_retry.cpp", "./src/launcher/updater/release_channel.cpp",
       "./src/launcher/updater/release_resolver.cpp", "./src/launcher/updater/part_manifest.cpp",
       "./src/launcher/support/redact.cpp", "./src/launcher/support/report_rules.cpp",
       "./src/launcher/show23/sfo.cpp", "./src/launcher/show23/package_header.cpp",
       "./src/launcher/show23/input_paths.cpp", "./src/launcher/show23/input_checks.cpp",
       "./src/launcher/show23/setup.cpp", "./src/launcher/show23/setup_verification.cpp",
       "./src/launcher/show23/game_record.cpp", "./src/launcher/show23/setup_run.cpp",
       "./src/launcher/show23/unpacker.cpp", "./src/launcher/show23/child_process.cpp",
       "./src/launcher/show23/play_session.cpp", "./src/launcher/updater/part_store.cpp",
       "./src/launcher/updater/bundled_runtime.cpp"}
includedirs {"./src/common", "./src/tests", "./src/launcher", path.join(dependencies.basePath, "rapidjson/include")}
defines {"RAPIDJSON_HAS_STDSTRING", "NOMINMAX"}
links {"common"}
libtomcrypt.import()
libtommath.import()

if _ACTION == "vs2022" then
    group "Dependencies"
    dependencies.projects()
end
