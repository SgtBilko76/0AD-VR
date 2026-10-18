local semver = require("semver")

print("Premake version: " .. _PREMAKE_VERSION)
if (semver(_PREMAKE_VERSION) < semver("5.0.0-beta5")) then
	print("Requires Premake 5.0.0-beta5 or later")
	print("Aborting")
	os.exit(1)
end

local function print_options()
	local options = ""
	local tkeys = {}
	for key,_  in pairs(_OPTIONS) do table.insert(tkeys, key) end
	table.sort(tkeys)
	for _,key in pairs(tkeys) do
		local value = _OPTIONS[key]
		options = options .. " --" .. key
		if (value and value ~= "") then
			options = options .. "=" .. value
		end
	end
	print("Premake options:" .. options)
end
print_options()
print("")

print("--------------------------------------------------------------------------------")
print("Environment")
print("")
print("AR         : " .. (os.getenv("AR") or "unset"))
print("CC         : " .. (os.getenv("CC") or "unset"))
print("CXX        : " .. (os.getenv("CXX") or "unset"))
print("HOSTTYPE   : " .. (os.getenv("HOSTTYPE") or "unset"))
print("PKG_CONFIG : " .. (os.getenv("PKG_CONFIG") or "unset"))
print("")
print("CFLAGS     : " .. (os.getenv("CFLAGS") or "unset"))
print("CXXFLAGS   : " .. (os.getenv("CXXFLAGS") or "unset"))
print("LDFLAGS    : " .. (os.getenv("LDFLAGS") or "unset"))
print("--------------------------------------------------------------------------------")
print("")

newoption { category = "Pyrogenesis", trigger = "android", description = "Cross-compile for Android arm64 (Meta Quest), see build/quest" }
newoption { category = "Pyrogenesis", trigger = "coverage", description = "Enable code coverage data collection (GCC only)" }
newoption { category = "Pyrogenesis", trigger = "gles", description = "Use non-working OpenGL ES 2.0 mode" }
newoption { category = "Pyrogenesis", trigger = "minimal-flags", description = "Only set compiler/linker flags that are really needed. Has no effect on Windows builds" }
newoption { category = "Pyrogenesis", trigger = "outpath", description = "Location for generated project files", default="../workspaces/default" }
newoption { category = "Pyrogenesis", trigger = "sanitize-address", description = "Enable ASAN if available" }
newoption { category = "Pyrogenesis", trigger = "sanitize-thread", description = "Enable TSAN if available" }
newoption { category = "Pyrogenesis", trigger = "sanitize-undefined-behaviour", description = "Enable UBSAN if available" }
newoption { category = "Pyrogenesis", trigger = "strip-binaries", description = "Strip created binaries" }
newoption { category = "Pyrogenesis", trigger = "with-system-cpp-httplib", description = "Search standard paths for cpp-httplib, instead of using bundled copy" }
newoption { category = "Pyrogenesis", trigger = "with-system-cxxtest", description = "Search standard paths for cxxtest, instead of using bundled copy" }
newoption { category = "Pyrogenesis", trigger = "with-lto", description = "Enable Link Time Optimization (LTO)" }
newoption { category = "Pyrogenesis", trigger = "with-system-mozjs", description = "Search standard paths for libmozjs128, instead of using bundled copy" }
newoption { category = "Pyrogenesis", trigger = "with-system-nvtt", description = "Search standard paths for nvidia-texture-tools library, instead of using bundled copy" }
newoption { category = "Pyrogenesis", trigger = "with-valgrind", description = "Enable Valgrind support (non-Windows only)" }
newoption { category = "Pyrogenesis", trigger = "without-audio", description = "Disable use of OpenAL/Ogg/Vorbis APIs" }
newoption { category = "Pyrogenesis", trigger = "without-atlas", description = "Disable Atlas scenario/map editor and ActorEditor" }
newoption { category = "Pyrogenesis", trigger = "without-dap-interface", description = "Disable Dap interface project" }
newoption { category = "Pyrogenesis", trigger = "without-lobby", description = "Disable the use of gloox and the multiplayer lobby" }
newoption { category = "Pyrogenesis", trigger = "without-miniupnpc", description = "Disable use of miniupnpc for port forwarding" }
newoption { category = "Pyrogenesis", trigger = "without-nvtt", description = "Disable use of NVTT" }
newoption { category = "Pyrogenesis", trigger = "without-pch", description = "Disable generation and usage of precompiled headers" }
newoption { category = "Pyrogenesis", trigger = "without-tests", description = "Disable generation of test projects" }

-- OS X specific options
newoption { category = "Pyrogenesis", trigger = "macosx-version-min", description = "Set minimum required version of the OS X API, the build will possibly fail if an older SDK is used, while newer API functions will be weakly linked (i.e. resolved at runtime)" }
newoption { category = "Pyrogenesis", trigger = "sysroot", description = "Set compiler system root path, used for building against a non-system SDK. For example /usr/local becomes SYSROOT/user/local" }

-- Install options
newoption { category = "Pyrogenesis", trigger = "bindir", description = "Directory for executables (typically '/usr/games'); default is to be relocatable" }
newoption { category = "Pyrogenesis", trigger = "datadir", description = "Directory for data files (typically '/usr/share/games/0ad'); default is ../data/ relative to executable" }
newoption { category = "Pyrogenesis", trigger = "libdir", description = "Directory for libraries (typically '/usr/lib/games/0ad'); default is ./ relative to executable" }

-- Root directory of project checkout relative to this .lua file
rootdir = "../.."

-- Determine C compiler
local cc = nil
if os.getenv("CC") then
	cc = os.getenv("CC")
elseif _OPTIONS["cc"] then
	cc = _OPTIONS["cc"]
else
	-- assumes cc name is toolset name
	cc = premake.action.current().toolset
end

-- detect CPU architecture (simplistic)
-- The user can target an architecture with HOSTTYPE, but the game still selects some know value.
arch = "x86"
macos_arch = "x86_64"

rtti_setting = _OPTIONS["android"] and "on" or "off"

if _OPTIONS["android"] then
	arch = "aarch64"
elseif os.istarget("windows") then
	if os.getenv("HOSTTYPE") then
		arch = os.getenv("HOSTTYPE")
	elseif os.getenv("PROCESSOR_ARCHITECTURE") == "amd64" or os.getenv("PROCESSOR_ARCHITEW6432") == "amd64" then
		arch = "amd64"
	end
else
	local machine = "x86_64"
	if os.getenv("HOSTTYPE") and os.getenv("HOSTTYPE") ~= '' then
		machine = os.getenv("HOSTTYPE")
	else
		os.execute(cc .. " -dumpmachine > .gccmachine.tmp")
		local f = io.open(".gccmachine.tmp", "r")
		machine = f:read("*line")
		f:close()
	end
	-- Special handling on mac os where xcode needs special flags.
	-- TODO: We should look into "universal" macOS compilation.
	if os.istarget("macosx") then
		if string.find(machine, "arm64") then
			arch = "aarch64"
			macos_arch = "arm64"
		else
			arch = "amd64"
			macos_arch = "x86_64"
		end
	elseif string.find(machine, "x86_64") == 1 or string.find(machine, "amd64") == 1 then
		arch = "amd64"
	elseif string.find(machine, "i.86") == 1 then
		arch = "x86"
	elseif string.find(machine, "arm") == 1 then
		arch = "arm"
	elseif string.find(machine, "aarch64") == 1 then
		arch = "aarch64"
	elseif string.find(machine, "e2k") == 1 then
		arch = "e2k"
	elseif string.find(machine, "loongarch64") == 1 then
		arch = "loong64"
	elseif string.find(machine, "ppc64") == 1 or string.find(machine, "powerpc64") == 1 then
		arch = "ppc64"
	elseif string.find(machine, "riscv64") == 1 then
		arch = "riscv64"
	else
		print("WARNING: Cannot determine architecture from GCC, assuming x86")
	end
end

-- On Windows check if wxWidgets is available, if not disable atlas and emit warning.
-- This is because there are currently not prebuilt binaries provided.
if not _OPTIONS["without-atlas"] and os.istarget("windows") then
	local win_libs_dir = rootdir .. "/libraries/" .. ( arch == "amd64" and "win64" or "win32" )
	if not os.isfile( win_libs_dir .. "/wxwidgets/include/wx/wx.h") then
		print("wxWidgets not found, disabling atlas")
		_OPTIONS["without-atlas"] = ""
	end
end

-- External libraries should know about arch.
dofile("extern_libs5.lua")

-- Test whether we need to link libexecinfo.
-- This is mostly the case on musl systems, as well as on BSD systems : only glibc provides the
-- backtrace symbols we require in the libc, for other libcs we use the libexecinfo library.
local link_execinfo = false
if os.istarget("bsd") then
	link_execinfo = true
elseif os.istarget("linux") then
	local _, link_errorCode = os.outputof(cc .. " ./tests/execinfo.c -o /dev/null")
	if link_errorCode ~= 0 then
		link_execinfo = true
	end
end

-- Test whether system mozjs is built with --enable-debug
-- The pc file doesn't specify the required -DDEBUG needed in that case
local mozjs_is_debug_build = false
if _OPTIONS["with-system-mozjs"] then
	local _, errorCode = os.outputof(cc .. " $(pkg-config mozjs-128 --cflags) ./tests/mozdebug.c -o /dev/null")
	if errorCode ~= 0 then
		mozjs_is_debug_build = true
	end
end

-- Set up the Workspace
workspace "pyrogenesis"
targetdir(rootdir.."/binaries/system")
libdirs(rootdir.."/binaries/system")
location(_OPTIONS["outpath"])
configurations { "Release", "Debug" }
-- for > 5.0.0-beta8
if defaultconfiguration ~= nil then
	defaultconfiguration("Release")
end
startproject "pyrogenesis"

source_root = rootdir.."/source/" -- default for most projects - overridden by local in others

-- Rationale: projects should not have any additional include paths except for
-- those required by external libraries. Instead, we should always write the
-- full relative path, e.g. #include "maths/Vector3d.h". This avoids confusion
-- ("which file is meant?") and avoids enormous include path lists.


-- projects: engine static libs, main exe, atlas, atlas frontends, test.

--------------------------------------------------------------------------------
-- project helper functions
--------------------------------------------------------------------------------

function project_set_target(project_name)

	-- Note: On Windows, ".exe" is added on the end, on unices the name is used directly

	local obj_dir_prefix = _OPTIONS["outpath"].."/obj/"..project_name.."_"

	filter "Debug"
		objdir(obj_dir_prefix.."Debug")
		targetsuffix("_dbg")

	filter "Release"
		objdir(obj_dir_prefix.."Release")

	filter { }

end


function project_set_build_flags()

	editandcontinue "Off"

	if _OPTIONS['strip-binaries'] then
		symbols "Off"
	else
		symbols "On"
	end

	-- ASAN, TSAN, UBSAN
	local sanitizers = {}
	if _OPTIONS['sanitize-address'] then
		table.insert(sanitizers, 'Address')
	end
	if _OPTIONS['sanitize-thread'] then
		table.insert(sanitizers, 'Thread')
	end
	if _OPTIONS['sanitize-undefined-behaviour'] then
		table.insert(sanitizers, 'UndefinedBehavior')
	end
	sanitize(sanitizers)

	-- disable Windows debug heap, since it makes malloc/free hugely slower when
	-- running inside a debugger
	if os.istarget("windows") then
		debugenvs { "_NO_DEBUG_HEAP=1" }
	end

	if os.istarget("windows") then
		-- mozilla 115 linked list destructor in debug build
		defines { "__PRETTY_FUNCTION__=__FUNCSIG__" }
	end

	filter { "Debug", "action:vs*" }
		defines { "DEBUG" }

	filter "Release"
		if os.istarget("windows") or not _OPTIONS["minimal-flags"] then
			optimize "Speed"
		end
		if _OPTIONS["with-lto"] then
			linktimeoptimization("On")
		end
		defines { "NDEBUG", "CONFIG_FINAL=1" }

	filter { }

	if mozjs_is_debug_build then
		defines "DEBUG"
	end

	if _OPTIONS["gles"] then
		defines { "CONFIG2_GLES=1" }
	end

	if _OPTIONS["without-audio"] then
		defines { "CONFIG2_AUDIO=0" }
	end

	if _OPTIONS["without-nvtt"] then
		defines { "CONFIG2_NVTT=0" }
	end

	if _OPTIONS["without-lobby"] then
		defines { "CONFIG2_LOBBY=0" }
	end

	if _OPTIONS["without-miniupnpc"] then
		defines { "CONFIG2_MINIUPNPC=0" }
	end

	if _OPTIONS["without-dap-interface"] then
		defines { "CONFIG2_DAP_INTERFACE=0" }
	end

	-- hide warnings caused by library includes
	externalwarnings "Off"

	-- various platform-specific build flags
	if os.istarget("windows") then

		if multiprocessorcompile ~= nil then
			-- since 5.0.0-beta8
			multiprocessorcompile("On")
		else
			flags { "MultiProcessorCompile" }
		end

		-- Since KB4088875 Windows 7 has a soft requirement for SSE2.
		-- Windows 8+ and Firefox ESR52 make it hard requirement.
		-- Finally since VS2012 it's enabled implicitely when not set.
		vectorextensions "SSE2"

		-- SpiderMonkey only supports building with MSVC on a best-effort basis,
		-- and the traditional MSVC preprocessor is incompatible with some headers.
		-- Use the modern, standard-compliant MSVC preprocessor instead.
		usestandardpreprocessor "On"

		-- use native wchar_t type (not typedef to unsigned short)
		nativewchar "On"

		-- enable most of the standard warnings
		warnings "Extra"

		-- FIXME: conversion warnings, should add -Wconversion to gcc and clang flags as well
		disablewarnings { "4267" }

	else	-- *nix

		-- exclude most non-essential build options for minimal-flags
		if not _OPTIONS["minimal-flags"] then
			buildoptions {
				-- enable most of the standard warnings
				"-Wall",
				"-Wextra",
				-- "-Wconversion", FIXME: should seriously consider fixing so this warning can be enabled.

				-- add some other useful warnings that need to be enabled explicitly
				"-Wunused-parameter",
				"-Wredundant-decls",	-- (useful for finding some multiply-included header files)
				-- "-Wformat=2",		-- (useful sometimes, but a bit noisy, so skip it by default)
				-- "-Wcast-qual",		-- (useful for checking const-correctness, but a bit noisy, so skip it by default)
				"-Wnon-virtual-dtor",	-- (sometimes noisy but finds real bugs)
				"-Wundef",				-- (useful for finding macro name typos)

				-- disable some warnings that currently trigger
				"-Wno-missing-field-initializers",	-- (this is common in external headers we can't fix)
				"-Wno-reorder",		-- order of initialization list in constructors (lots of noise)

				-- enable security features (stack checking etc) that shouldn't have
				-- a significant effect on performance and can catch bugs
				"-fstack-protector-strong",

				-- always enable strict aliasing (useful in debug builds because of the warnings)
				"-fstrict-aliasing",

				-- don't omit frame pointers (for now), because performance will be impacted
				-- negatively by the way this breaks profilers more than it will be impacted
				-- positively by the optimisation
				"-fno-omit-frame-pointer"
			}

			filter { "Release" }
				buildoptions {
					-- FORTIFY_SOURCE needs optimizations to be enabled
					"-U_FORTIFY_SOURCE",    -- (avoid redefinition warning if already defined)
					"-D_FORTIFY_SOURCE=2",
				}
			filter {}

			-- issues with gcc 12 to 14 with pch enabled, workaround for CI which sets CC=gcc-12
			if cc == "gcc-12" then
				buildoptions {
					-- mozilla
					"-Wno-dangling-pointer",
					-- fortify source
					"-Wno-stringop-overflow",
					"-Wno-attribute-warning",
					"-Wno-array-bounds",
					"-Wno-restrict",
				}
			end

			if not _OPTIONS["without-pch"] then
				buildoptions {
					-- do something (?) so that ccache can handle compilation with PCH enabled
					-- (ccache 3.1+ also requires CCACHE_SLOPPINESS=time_macros for this to work)
					"-fpch-preprocess"
				}
			end

			if os.istarget("linux") or os.istarget("bsd") then
				buildoptions { "-fPIC" }
				if next(sanitizers) == nil then
					linkoptions { "-Wl,--no-undefined", "-Wl,--as-needed", "-Wl,-z,relro" }
				end
			end

			if arch == "x86" then
				buildoptions {
					-- To support intrinsics like __sync_bool_compare_and_swap on x86
					-- we need to set -march to something that supports them (i686).
					-- We use pentium3 to also enable other features like mmx and sse,
					-- while tuning for generic to have good performance on every
					-- supported CPU.
					-- Note that all these features are already supported on amd64.
					"-march=pentium3 -mtune=generic",
					-- This allows x86 operating systems to handle the 2GB+ public mod.
					"-D_FILE_OFFSET_BITS=64"
				}
			end
		end

		if arch == "arm" then
			-- disable warnings about va_list ABI change and use
			-- compile-time flags for futher configuration.
			buildoptions { "-Wno-psabi" }
			if _OPTIONS["android"] then
				-- Android uses softfp, so we should too.
				buildoptions { "-mfloat-abi=softfp" }
			end
		end

		if _OPTIONS["coverage"] then
			buildoptions { "-fprofile-arcs", "-ftest-coverage" }
			links { "gcov" }
		end

		-- MacOS 10.12 only supports intel processors with SSE 4.1, so enable that.
		if os.istarget("macosx") and arch == "amd64" then
			buildoptions { "-msse4.1" }
		end

		-- Check if SDK path should be used
		if _OPTIONS["sysroot"] then
			buildoptions { "-isysroot " .. _OPTIONS["sysroot"] }
			linkoptions { "-Wl,-syslibroot," .. _OPTIONS["sysroot"] }
		end

		-- On OS X, sometimes we need to specify the minimum API version to use
		if _OPTIONS["macosx-version-min"] then
			buildoptions { "-mmacosx-version-min=" .. _OPTIONS["macosx-version-min"] }
			-- clang and llvm-gcc look at mmacosx-version-min to determine link target
			-- and CRT version, and use it to set the macosx_version_min linker flag
			linkoptions { "-mmacosx-version-min=" .. _OPTIONS["macosx-version-min"] }
		end

		-- Only libc++ is supported on MacOS
		if os.istarget("macosx") then
			buildoptions { "-stdlib=libc++" }
			linkoptions { "-stdlib=libc++" }
		end

		buildoptions {
			-- Hide symbols in dynamic shared objects by default, for efficiency and for equivalence with
			-- Windows - they should be exported explicitly with __attribute__ ((visibility ("default")))
			"-fvisibility=hidden"
		}

		if _OPTIONS["bindir"] then
			defines { "INSTALLED_BINDIR=" .. _OPTIONS["bindir"] }
		end
		if _OPTIONS["datadir"] then
			defines { "INSTALLED_DATADIR=" .. _OPTIONS["datadir"] }
		end
		if _OPTIONS["libdir"] then
			defines { "INSTALLED_LIBDIR=" .. _OPTIONS["libdir"] }
		end

		if os.istarget("linux") or os.istarget("bsd") then
			-- To use our local shared libraries, they need to be found in the
			-- runtime dynamic linker path. Add their path to -rpath.
			if _OPTIONS["libdir"] then
				linkoptions {"-Wl,-rpath," .. _OPTIONS["libdir"] }
			else
				-- On FreeBSD we need to allow use of $ORIGIN
				if os.istarget("bsd") then
					linkoptions { "-Wl,-z,origin" }
				end

				-- Adding the executable path and taking care of correct escaping
				if _ACTION == "gmake" then
					linkoptions { "-Wl,-rpath,'$$ORIGIN'" }
				elseif _ACTION == "codeblocks" then
					linkoptions { "-Wl,-R\\\\$$$ORIGIN" }
				end
			end
		end

	end
end

-- create a project and set the attributes that are common to all projects.
function project_create(project_name, target_type)

	project(project_name)
	language "C++"
	cppdialect "C++20"
	kind(target_type)

	filter "action:vs2022"
		toolset "v143"
	filter {}

	filter "action:vs*"
		buildoptions "/utf-8"
		-- disable LNK4221 warning, to avoid spending energy ordering projects in linker invocations
		linkoptions "/ignore:4221"
	filter {}

	project_set_target(project_name)
	project_set_build_flags()
end


-- OSX creates a .app bundle if the project type of the main application is set to "WindowedApp".
-- We don't want this because this bundle would be broken (it lacks all the resources and external dependencies, Info.plist etc...)
-- Windows opens a console in the background if it's set to ConsoleApp, which is not what we want.
-- I didn't check if this setting matters for linux, but WindowedApp works there.
function get_main_project_target_type()
	if _OPTIONS["android"] then
		return "SharedLib"
	elseif os.istarget("macosx") then
		return "ConsoleApp"
	else
		return "WindowedApp"
	end
end


-- source_root: rel_source_dirs and rel_include_dirs are relative to this directory
-- rel_source_dirs: A table of subdirectories. All source files in these directories are added.
-- rel_include_dirs: A table of subdirectories to be included.
-- extra_params: table including zero or more of the following:
-- * no_pch: If specified, no precompiled headers are used for this project.
-- * pch_dir: If specified, this directory will be used for precompiled headers instead of the default
--   <source_root>/pch/<projectname>/.
-- * extra_files: table of filenames (relative to source_root) to add to project
-- * extra_links: table of library names to add to link step
function project_add_contents(source_root, rel_source_dirs, rel_include_dirs, extra_params)

	for i,v in pairs(rel_source_dirs) do
		local prefix = source_root..v.."/"
		files { prefix.."*.cpp", prefix.."*.h", prefix.."*.inl", prefix.."*.js", prefix.."*.asm", prefix.."*.mm" }
	end

	-- Put the project-specific PCH directory at the start of the
	-- include path, so '#include "precompiled.h"' will look in
	-- there first
	local pch_dir
	if not extra_params["pch_dir"] then
		pch_dir = source_root .. "pch/" .. project().name .. "/"
	else
		pch_dir = extra_params["pch_dir"]
	end
	includedirs { pch_dir }

	-- Precompiled Headers
	-- rationale: we need one PCH per static lib, since one global header would
	-- increase dependencies. To that end, we can either include them as
	-- "projectdir/precompiled.h", or add "source/PCH/projectdir" to the
	-- include path and put the PCH there. The latter is better because
	-- many projects contain several dirs and it's unclear where there the
	-- PCH should be stored. This way is also a bit easier to use in that
	-- source files always include "precompiled.h".
	-- Notes:
	-- * Visual Assist manages to use the project include path and can
	--   correctly open these files from the IDE.
	-- * precompiled.cpp (needed to "Create" the PCH) also goes in
	--   the abovementioned dir.
	if (not _OPTIONS["without-pch"] and not extra_params["no_pch"]) then
		filter "action:vs*"
			pchheader("precompiled.h")
		filter "action:xcode*"
			pchheader("../"..pch_dir.."precompiled.h")
		filter { "action:not vs*", "action:not xcode*" }
			pchheader(pch_dir.."precompiled.h")
		filter {}
		pchsource(pch_dir.."precompiled.cpp")
		defines { "CONFIG_ENABLE_PCH=1" }
		files { pch_dir.."precompiled.h", pch_dir.."precompiled.cpp" }
	else
		defines { "CONFIG_ENABLE_PCH=0" }
		if enablepch ~= nil then
			-- since 5.0.0-beta8
			enablepch("Off")
		else
			flags { "NoPCH" }
		end
	end

	-- next is source root dir, for absolute (nonrelative) includes
	-- (e.g. "lib/precompiled.h")
	includedirs { source_root }

	for i,v in pairs(rel_include_dirs) do
		includedirs { source_root .. v }
	end

	if extra_params["extra_files"] then
		for i,v in pairs(extra_params["extra_files"]) do
			-- .rc files are only needed on Windows
			if path.getextension(v) ~= ".rc" or os.istarget("windows") then
				files { source_root .. v }
			end
		end
	end

	if extra_params["extra_links"] then
		links { extra_params["extra_links"] }
	end
end


-- Add command-line options to set up the manifest dependencies for Windows
function project_add_manifest()
	-- To use XP-style themed controls, we need to use the manifest to specify the
	-- desired version. (This must be set in the game's .exe in order to affect Atlas.)
	-- We can remove it once we remove wxWidgets
	if arch == "amd64" then
		linkoptions { "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df'\"" }
	else
		linkoptions { "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='X86' publicKeyToken='6595b64144ccf1df'\"" }
	end
end

--------------------------------------------------------------------------------
-- engine static libraries
--------------------------------------------------------------------------------

-- the engine is split up into several static libraries. this eases separate
-- distribution of those components, reduces dependencies a bit, and can
-- also speed up builds.
-- more to the point, it is necessary to efficiently support a separate
-- test executable that also includes much of the game code.

-- names of all static libs created. automatically added to the
-- main app project later (see explanation at end of this file)
static_lib_names = {}
static_lib_names_debug = {}
static_lib_names_release = {}

-- set up one of the static libraries into which the main engine code is split.
-- extra_params:
--	no_default_link: If specified, linking won't be done by default.
-- 	For the rest of extra_params, see project_add_contents().
-- note: rel_source_dirs and rel_include_dirs are relative to global source_root.
function setup_static_lib_project (project_name, rel_source_dirs, extern_libs, extra_params)

	local target_type = "StaticLib"
	project_create(project_name, target_type)
	project_add_contents(source_root, rel_source_dirs, {}, extra_params)
	project_add_extern_libs(extern_libs, target_type)

	if not extra_params["no_default_link"] then
		table.insert(static_lib_names, project_name)
	end

	-- Deactivate Run Time Type Information. Performance of dynamic_cast is very poor.
	-- The exception to this principle is Atlas UI, which is not a static library.
	-- On Android, -fno-rtti makes clang emit local exception typeinfos, which
	-- libc++ compares by address, so exceptions from libc++_shared would never match.
	rtti(rtti_setting)

	if os.istarget("windows") then
		if arch == "amd64" then
			architecture("x86_64")
		end
	elseif os.istarget("macosx") then
		architecture(macos_arch)
		buildoptions { "-arch " .. macos_arch }
		linkoptions { "-arch " .. macos_arch }
		xcodebuildsettings { ARCHS = macos_arch }
		if _OPTIONS["macosx-version-min"] then
			xcodebuildsettings { MACOSX_DEPLOYMENT_TARGET = _OPTIONS["macosx-version-min"] }
		end
	end
end

function setup_third_party_static_lib_project (project_name, rel_source_dirs, extern_libs, extra_params)

	setup_static_lib_project(project_name, rel_source_dirs, extern_libs, extra_params)
	includedirs { source_root .. "third_party/" .. project_name .. "/include/" }
end

function setup_shared_lib_project (project_name, rel_source_dirs, extern_libs, extra_params)

	local target_type = "SharedLib"
	project_create(project_name, target_type)
	project_add_contents(source_root, rel_source_dirs, {}, extra_params)
	project_add_extern_libs(extern_libs, target_type)

	if not extra_params["no_default_link"] then
		table.insert(static_lib_names, project_name)
	end

	if os.istarget("windows") then
		links { "delayimp" }
		if arch == "amd64" then
			architecture("x86_64")
		end
	elseif os.istarget("macosx") then
		architecture(macos_arch)
		buildoptions { "-arch " .. macos_arch }
		linkoptions { "-arch " .. macos_arch }
		xcodebuildsettings { ARCHS = macos_arch }
		if _OPTIONS["macosx-version-min"] then
			xcodebuildsettings { MACOSX_DEPLOYMENT_TARGET = _OPTIONS["macosx-version-min"] }
		end
	end
end


-- this is where the source tree is chopped up into static libs.
-- can be changed very easily; just copy+paste a new setup_static_lib_project,
-- or remove existing ones. static libs are automagically added to
-- main_exe link step.
function setup_all_libs ()

	-- relative to global source_root.
	local source_dirs = {}
	-- names of external libraries used (see libraries_dir comment)
	local extern_libs = {}


	source_dirs = {
		"network",
	}
	extern_libs = {
		"cpp_httplib",
		"spidermonkey",
		"enet",
		"sdl",
		"boost",	-- dragged in via server->simulation.h->random and NetSession.h->lockfree
		"fmt",
		"libxml2",
		"iconv",
	}
	if not _OPTIONS["without-miniupnpc"] then
		table.insert(extern_libs, "miniupnpc")
	end
	setup_static_lib_project("network", source_dirs, extern_libs, {})

	source_dirs = {
		"rlinterface",
	}
	extern_libs = {
		"boost", -- dragged in via simulation.h and scriptinterface.h
		"fmt",
		"spidermonkey",
		"cpp_httplib",
	}
	setup_static_lib_project("rlinterface", source_dirs, extern_libs, { no_pch = 1 })

	if not _OPTIONS["without-dap-interface"] then
		source_dirs = {
			"dapinterface",
		}
		extern_libs = {
			"boost", -- dragged in via simulation.h and scriptinterface.h
			"fmt",
			"spidermonkey",
			"sockets"
		}
		setup_static_lib_project("dapinterface", source_dirs, extern_libs, { no_pch = 1 })
	end

	source_dirs = {
		"third_party/tinygettext/src",
	}
	extern_libs = {
		"iconv",
		"boost",
		"fmt",
	}
	setup_third_party_static_lib_project("tinygettext", source_dirs, extern_libs, { } )

	-- it's an external library and we don't want to modify its source to fix warnings, so we just disable them to avoid noise in the compile output
	filter "action:vs*"
		buildoptions {
			"/wd4127",
			"/wd4309",
			"/wd4800",
			"/wd4100",
			"/wd4996",
			"/wd4099",
			"/wd4503"
		}
	filter {}


	if not _OPTIONS["without-lobby"] then
		source_dirs = {
			"lobby",
			"lobby/scripting",
			"i18n",
			"third_party/encryption"
		}

		extern_libs = {
			"spidermonkey",
			"boost",
			"enet",
			"gloox",
			"icu",
			"iconv",
			"libsodium",
			"tinygettext",
			"fmt",
		}
		setup_static_lib_project("lobby", source_dirs, extern_libs, {})
	else
		source_dirs = {
			"lobby/scripting",
			"third_party/encryption"
		}
		extern_libs = {
			"spidermonkey",
			"boost",
			"libsodium",
			"fmt",
		}
		setup_static_lib_project("lobby", source_dirs, extern_libs, {})
		files { source_root.."lobby/Globals.cpp" }
	end


	source_dirs = {
		"simulation2",
		"simulation2/components",
		"simulation2/helpers",
		"simulation2/scripting",
		"simulation2/serialization",
		"simulation2/system",
		"simulation2/testcomponents",
	}
	extern_libs = {
		"boost",
		"spidermonkey",
		"fmt",
		"libxml2",
		"iconv",
		"cxxtest",
	}
	setup_static_lib_project("simulation2", source_dirs, extern_libs, {})


	source_dirs = {
		"scriptinterface",
		"scriptinterface/third_party"
	}
	extern_libs = {
		"boost",
		"spidermonkey",
		"valgrind",
		"sdl",
		"fmt",
	}
	setup_static_lib_project("scriptinterface", source_dirs, extern_libs, {})


	source_dirs = {
		"ps",
		"ps/scripting",
		"network/scripting",
		"ps/GameSetup",
		"ps/XMB",
		"ps/XML",
		"soundmanager",
		"soundmanager/data",
		"soundmanager/items",
		"soundmanager/scripting",
		"maths",
		"maths/scripting",
		"i18n",
		"i18n/scripting",
	}
	extern_libs = {
		"spidermonkey",
		"sdl",	-- key definitions
		"libxml2",
		"zlib",
		"boost",
		"enet",
		"libcurl",
		"tinygettext",
		"icu",
		"iconv",
		"libsodium",
		"libpng",
		"fmt",
		"freetype",
		"cpp_httplib",
	}

	if not _OPTIONS["without-lobby"] then
		table.insert(extern_libs, "gloox")
	end

	if not _OPTIONS["without-miniupnpc"] then
		table.insert(extern_libs, "miniupnpc")
	end

	if not _OPTIONS["without-nvtt"] then
		table.insert(extern_libs, "nvtt")
	end
	if not _OPTIONS["without-audio"] then
		table.insert(extern_libs, "openal")
		table.insert(extern_libs, "vorbis")
	end

	setup_static_lib_project("engine", source_dirs, extern_libs, {})


	source_dirs = {
		"graphics",
		"graphics/scripting",
		"renderer",
		"renderer/backend",
		"renderer/backend/dummy",
		"renderer/backend/gl",
		"renderer/backend/vulkan",
		"renderer/scripting",
		"third_party/mikktspace",
		"third_party/ogre3d_preprocessor",
		"third_party/vma"
	}
	extern_libs = {
		"sdl",	-- key definitions
		"spidermonkey",	-- for graphics/scripting
		"boost",
		"fmt",
		"freetype",
		"icu",
		"libxml2",
		"iconv",
	}
	if not _OPTIONS["without-nvtt"] then
		table.insert(extern_libs, "nvtt")
	end
	setup_static_lib_project("graphics", source_dirs, extern_libs, {})


	source_dirs = {
		"tools/atlas/GameInterface",
		"tools/atlas/GameInterface/Handlers"
	}
	extern_libs = {
		"boost",
		"sdl",	-- key definitions
		"spidermonkey",
		"fmt",
		"libxml2",
		"iconv",
	}
	setup_static_lib_project("atlas", source_dirs, extern_libs, {})


	source_dirs = {
		"gui",
		"gui/ObjectTypes",
		"gui/ObjectBases",
		"gui/Scripting",
		"gui/SettingTypes",
		"i18n"
	}
	extern_libs = {
		"spidermonkey",
		"sdl",	-- key definitions
		"boost",
		"enet",
		"tinygettext",
		"icu",
		"iconv",
		"fmt",
		"libxml2",
	}
	if not _OPTIONS["without-audio"] then
		table.insert(extern_libs, "openal")
	end
	setup_static_lib_project("gui", source_dirs, extern_libs, {})


	source_dirs = {
		"lib",
		"lib/adts",
		"lib/allocators",
		"lib/external_libraries",
		"lib/file",
		"lib/file/archive",
		"lib/file/common",
		"lib/file/io",
		"lib/file/vfs",
		"lib/pch",
		"lib/posix",
		"lib/res",
		"lib/res/graphics",
		"lib/sysdep",
		"lib/tex"
	}
	extern_libs = {
		"boost",
		"sdl",
		"openal",
		"libpng",
		"zlib",
		"valgrind",
		"cxxtest",
		"fmt",
	}

	-- CPU architecture-specific
	if arch == "amd64" then
		table.insert(source_dirs, "lib/sysdep/arch/amd64");
		table.insert(source_dirs, "lib/sysdep/arch/x86_x64");
	elseif arch == "x86" then
		table.insert(source_dirs, "lib/sysdep/arch/ia32");
		table.insert(source_dirs, "lib/sysdep/arch/x86_x64");
	elseif arch == "arm" then
		table.insert(source_dirs, "lib/sysdep/arch/arm");
	elseif arch == "aarch64" then
		table.insert(source_dirs, "lib/sysdep/arch/aarch64");
	elseif arch == "e2k" then
		table.insert(source_dirs, "lib/sysdep/arch/e2k");
	elseif arch == "loong64" then
		table.insert(source_dirs, "lib/sysdep/arch/loong64");
	elseif arch == "ppc64" then
		table.insert(source_dirs, "lib/sysdep/arch/ppc64");
	elseif arch == "riscv64" then
		table.insert(source_dirs, "lib/sysdep/arch/riscv64");
	end

	-- OS-specific
	sysdep_dirs = {
		linux = { "lib/sysdep/os/linux", "lib/sysdep/os/unix" },
		-- note: RC file must be added to main_exe project.
		-- note: don't add "lib/sysdep/os/win/aken.cpp" because that must be compiled with the DDK.
		windows = { "lib/sysdep/os/win", "lib/sysdep/os/win/wposix", "lib/sysdep/os/win/whrt" },
		macosx = { "lib/sysdep/os/osx", "lib/sysdep/os/unix" },
		bsd = { "lib/sysdep/os/bsd", "lib/sysdep/os/unix", "lib/sysdep/os/unix/x" },
	}
	for i,v in pairs(sysdep_dirs[os.target()]) do
		table.insert(source_dirs, v);
	end

	if os.istarget("linux") and not _OPTIONS["android"] then
		table.insert(source_dirs, "lib/sysdep/os/unix/x")
	end

	-- On OSX, disable precompiled headers because C++ files and Objective-C++ files are
	-- mixed in this project. To fix that, we would need per-file basis configuration which
	-- is not yet supported by the gmake action in premake. We should look into using gmake2.
	extra_params = {}
	if os.istarget("macosx") then
		extra_params = { no_pch = 1 }
	end

	-- runtime-library-specific
	if _ACTION == "vs2022" then
		table.insert(source_dirs, "lib/sysdep/rtl/msc");
	else
		table.insert(source_dirs, "lib/sysdep/rtl/gcc");
	end

	setup_static_lib_project("lowlevel", source_dirs, extern_libs, extra_params)


	extern_libs = { "glad" }
	setup_static_lib_project("gladwrapper", {}, extern_libs, { no_pch = 1 })
	-- select files to build for the current platform
	glad_path = third_party_source_dir.."glad/"
	files { glad_path.."src/vulkan.cpp" }
	if _OPTIONS["gles"] then
		files { glad_path.."src/gles2.cpp" }
	else
		files { glad_path.."src/gl.cpp" }
	end
	-- on Windows, silence a build warning in vulkan.cpp
	filter "action:vs*"
		buildoptions { "/wd4551" }
	filter {}
end

--------------------------------------------------------------------------------
-- main EXE
--------------------------------------------------------------------------------

-- used for main EXE as well as test
used_extern_libs = {
	"sdl",

	"libpng",
	"zlib",

	"spidermonkey",
	"libxml2",

	"boost",
	"cpp_httplib",
	"cxxtest",
	"comsuppw",
	"enet",
	"libcurl",
	"tinygettext",
	"icu",
	"iconv",
	"libsodium",
	"fmt",
	"freetype",

	"valgrind",

	"oleaut32",
}

if not _OPTIONS["without-audio"] then
	table.insert(used_extern_libs, "openal")
	table.insert(used_extern_libs, "vorbis")
end

if not _OPTIONS["without-nvtt"] then
	table.insert(used_extern_libs, "nvtt")
end

if not _OPTIONS["without-lobby"] then
	table.insert(used_extern_libs, "gloox")
end

if not _OPTIONS["without-miniupnpc"] then
	table.insert(used_extern_libs, "miniupnpc")
end

-- Bundles static libs together with main.cpp and builds game executable.
function setup_main_exe ()

	local target_type = get_main_project_target_type()
	project_create("pyrogenesis", target_type)

	filter "system:not macosx"
		linkgroups 'On'
	filter {}

	local extra_params = {
		extra_files = { "main.cpp" },
		no_pch = 1
	}
	project_add_contents(source_root, {}, {}, extra_params)
	project_add_extern_libs(used_extern_libs, target_type)

	if not _OPTIONS["android"] then
		dependson { "Collada" }
	end

	rtti(rtti_setting)

	-- Platform Specifics
	if os.istarget("windows") then

		files { source_root.."lib/sysdep/os/win/icon.rc" }
		files { source_root.."lib/sysdep/os/win/pyrogenesis.rc" }
		-- from "lowlevel" static lib; must be added here to be linked in
		files { source_root.."lib/sysdep/os/win/error_dialog.rc" }

		linkoptions {
			-- wraps main thread in a __try block(see wseh.cpp). replace with mainCRTStartup if that's undesired.
			"/ENTRY:wseh_EntryPoint",
		}

		links { "delayimp" }

		if arch == "x86" then
			-- allow the executable to use more than 2GB of RAM.
			linkoptions { "/LARGEADDRESSAWARE" }
		end

		-- see manifest.cpp
		project_add_manifest(arch)

		if arch == "amd64" then
			architecture("x86_64")
		end

	elseif os.istarget("linux") or os.istarget("bsd") then

		if not _OPTIONS["android"] and not (os.getversion().description == "OpenBSD") then
			links { "rt" }
		end

		if _OPTIONS["android"] then
			-- openxr_loader: Meta Quest VR mode (source/ps/QuestVR.cpp)
			links { "log", "android", "openxr_loader" }
		end

		if link_execinfo then
			links {
				"execinfo"
			}
		end

		if os.istarget("linux") or os.getversion().description == "GNU/kFreeBSD" then
			links {
				-- Dynamic libraries (needed for linking for gold)
				"dl",
			}

			if arch == "riscv64" then
				-- Needed to resolve __atomic_load_16 et al.
				links { "atomic" }
			end
		end

		-- Threading support
		buildoptions { "-pthread" }
		if not _OPTIONS["android"] then
			linkoptions { "-pthread" }
		end

		-- For debug_resolve_symbol
		filter "Debug"
			linkoptions { "-rdynamic" }
		filter { }

	elseif os.istarget("macosx") then

		links { "pthread" }
		links { "ApplicationServices.framework", "Cocoa.framework", "CoreFoundation.framework" }

		architecture(macos_arch)
		buildoptions { "-arch " .. macos_arch }
		linkoptions { "-arch " .. macos_arch }
		xcodebuildsettings { ARCHS = macos_arch }
		if _OPTIONS["macosx-version-min"] then
			xcodebuildsettings { MACOSX_DEPLOYMENT_TARGET = _OPTIONS["macosx-version-min"] }
		end
	end
end


--------------------------------------------------------------------------------
-- atlas
--------------------------------------------------------------------------------

-- setup a typical Atlas component project
-- extra_params, rel_source_dirs and rel_include_dirs: as in project_add_contents;
function setup_atlas_project(project_name, target_type, rel_source_dirs, rel_include_dirs, extern_libs, extra_params)

	local source_root = rootdir.."/source/tools/atlas/" .. project_name .. "/"
	project_create(project_name, target_type)

	-- if not specified, the default for atlas pch files is in the project root.
	if not extra_params["pch_dir"] then
		extra_params["pch_dir"] = source_root
	end

	project_add_contents(source_root, rel_source_dirs, rel_include_dirs, extra_params)
	project_add_extern_libs(extern_libs, target_type)

	-- Platform Specifics
	if os.istarget("windows") then
		-- Link to required libraries
		links { "winmm", "delayimp" }
		if arch == "amd64" then
			architecture("x86_64")
		end

	elseif os.istarget("macosx") then
		architecture(macos_arch)
		buildoptions { "-arch " .. macos_arch }
		linkoptions { "-arch " .. macos_arch }
		xcodebuildsettings { ARCHS = macos_arch }
		if _OPTIONS["macosx-version-min"] then
			xcodebuildsettings { MACOSX_DEPLOYMENT_TARGET = _OPTIONS["macosx-version-min"] }
		end
	elseif os.istarget("linux") or os.istarget("bsd") then
		if os.getversion().description == "FreeBSD" then
			buildoptions { "-fPIC" }
			linkoptions { "-fPIC" }
		else
			buildoptions { "-fPIC" }
			linkoptions { "-fPIC", "-rdynamic" }
		end

		-- warnings triggered by wxWidgets
		buildoptions { "-Wno-unused-local-typedefs" }
	end

end


-- build all Atlas component projects
function setup_atlas_projects()

	setup_atlas_project("AtlasObject", "StaticLib",
	{	-- src
		".",
	},{	-- include
		"../../../",
	},{	-- extern_libs
		"boost",
		"iconv",
		"libxml2",
		"sdl",
		"spidermonkey",
		"cxxtest",
	},{	-- extra_params
		no_pch = 1
	})

	atlas_src = {
		"ActorEditor",
		"CustomControls/Buttons",
		"CustomControls/Canvas",
		"CustomControls/ColorDialog",
		"CustomControls/DraggableListCtrl",
		"CustomControls/EditableListCtrl",
		"CustomControls/FileHistory",
		"CustomControls/HighResTimer",
		"CustomControls/MapDialog",
		"CustomControls/MapResizeDialog",
		"CustomControls/SnapSplitterWindow",
		"CustomControls/VirtualDirTreeCtrl",
		"CustomControls/Windows",
		"General",
		"General/VideoRecorder",
		"Misc",
		"ScenarioEditor",
		"ScenarioEditor/Sections/Common",
		"ScenarioEditor/Sections/Cinema",
		"ScenarioEditor/Sections/Environment",
		"ScenarioEditor/Sections/Map",
		"ScenarioEditor/Sections/Object",
		"ScenarioEditor/Sections/Player",
		"ScenarioEditor/Sections/Terrain",
		"ScenarioEditor/Tools",
		"ScenarioEditor/Tools/Common",
	}
	atlas_extra_links = {
		"AtlasObject"
	}

	atlas_extern_libs = {
		"boost",
		"comsuppw",
		"iconv",
		"libxml2",
		"sdl",	-- key definitions
		"wxwidgets",
		"zlib",
	}

	setup_atlas_project("AtlasUI", "SharedLib", atlas_src,
	{	-- include
		"../../..",
	},
	atlas_extern_libs,
	{	-- extra_params
		pch_dir = rootdir.."/source/tools/atlas/AtlasUI/Misc/",
		no_pch = false,
		extra_links = atlas_extra_links,
		extra_files = { "Misc/atlas.rc" }
	})
end


-- Atlas 'frontend' tool-launching projects
function setup_atlas_frontend_project (project_name)

	local target_type = get_main_project_target_type()
	project_create(project_name, target_type)

	local source_root = rootdir.."/source/tools/atlas/AtlasFrontends/"
	files { source_root..project_name..".cpp" }

	if os.istarget("windows") then
		files { source_root..project_name..".rc" }
	end

	includedirs { source_root .. ".." }

	-- Platform Specifics
	if os.istarget("windows") then
		-- see manifest.cpp
		project_add_manifest(arch)
		if arch == "amd64" then
			architecture("x86_64")
		end

	else -- Non-Windows, = Unix
		links { "AtlasObject" }
		if os.istarget("macosx") then
			architecture(macos_arch)
			buildoptions { "-arch " .. macos_arch }
			linkoptions { "-arch " .. macos_arch }
			xcodebuildsettings { ARCHS = macos_arch }
		end
	end

	links { "AtlasUI" }

end

function setup_atlas_frontends()
	setup_atlas_frontend_project("ActorEditor")
end


--------------------------------------------------------------------------------
-- collada
--------------------------------------------------------------------------------

function setup_collada_project(project_name, target_type, rel_source_dirs, rel_include_dirs, extern_libs, extra_params)

	project_create(project_name, target_type)
	local source_root = source_root.."collada/"
	extra_params["pch_dir"] = source_root
	project_add_contents(source_root, rel_source_dirs, rel_include_dirs, extra_params)
	project_add_extern_libs(extern_libs, target_type)

	-- Platform Specifics
	if os.istarget("windows") then
		characterset "MBCS"
		if arch == "amd64" then
			architecture("x86_64")
		end
	elseif os.istarget("linux") then
		defines { "LINUX" }

		links {
			"dl",
		}

		-- FCollada is not aliasing-safe, so disallow dangerous optimisations
		-- (TODO: It'd be nice to fix FCollada, but that looks hard)
		buildoptions { "-fno-strict-aliasing" }
		if os.getversion().description ~= "FreeBSD" then
			linkoptions { "-rdynamic" }
		end

	elseif os.istarget("bsd") then
		if os.getversion().description == "OpenBSD" then
			links { "c", }
		end

		if os.getversion().description == "GNU/kFreeBSD" then
			links {
				"dl",
			}
		end

		buildoptions { "-fno-strict-aliasing" }

		linkoptions { "-rdynamic" }

	elseif os.istarget("macosx") then
		-- define MACOS-something?

		buildoptions { "-fno-strict-aliasing" }
		-- On OSX, fcollada uses a few utility functions from coreservices
		links { "CoreServices.framework" }

		architecture(macos_arch)
		buildoptions { "-arch " .. macos_arch }
		linkoptions { "-arch " .. macos_arch }
		xcodebuildsettings { ARCHS = macos_arch }
	end

end

-- build all Collada component projects
function setup_collada_projects()

	setup_collada_project("Collada", "SharedLib",
	{	-- src
		"."
	},{	-- include
	},{	-- extern_libs
		"fcollada",
		"iconv",
		"libxml2"
	},{	-- extra_params
	})

end


--------------------------------------------------------------------------------
-- tests
--------------------------------------------------------------------------------

function setup_tests()

	local cxxtest = require "cxxtest"

	if os.istarget("windows") then
		cxxtest.setpath(rootdir.."/build/bin/cxxtestgen.exe")
	else
		if _OPTIONS["with-system-cxxtest"] then
			local handle = io.popen("command -v cxxtestgen")
			local cxxtestgen = handle:read("*a"):gsub("\n", " ")
			handle:close()
			cxxtest.setpath(cxxtestgen)
		else
			cxxtest.setpath(rootdir.."/libraries/source/cxxtest-4.4/bin/cxxtestgen")
		end
	end

	local include_files = {
		-- Precompiled headers - the header is added to all generated .cpp files
		-- note that the header isn't actually precompiled here, only #included
		-- so that the build stage can use it as a precompiled header.
		"precompiled.h",
	}
	local test_root_include_files = {
		"precompiled.h",
	}
	if os.istarget("windows") then
		-- This is required to build against SDL 2.0.12 (starting from 2.0.4) on Windows.
		-- Refs #3138
		table.insert(test_root_include_files, "lib/external_libraries/libsdl.h")
	end

	cxxtest.init(true, true, nil, include_files, test_root_include_files)

	local target_type = get_main_project_target_type()
	project_create("test", target_type)

	-- Find header files in 'test' subdirectories
	local all_files = os.matchfiles(source_root .. "**/tests/*.h")
	local test_files = {}
	for i,v in pairs(all_files) do
		-- Don't include sysdep tests on the wrong sys
		-- Don't include Atlas tests unless Atlas is being built
		if not (string.find(v, "/sysdep/os/win/") and not os.istarget("windows")) and
		   not (string.find(v, "/tools/atlas/") and _OPTIONS["without-atlas"]) and
		   not (string.find(v, "/sysdep/arch/x86_x64/") and ((arch ~= "amd64") or (arch ~= "x86")))
		then
			table.insert(test_files, v)
		end
	end

	cxxtest.configure_project(test_files)

	filter "system:not macosx"
		linkgroups 'On'
	filter {}

	links { static_lib_names }
	filter "Debug"
		links { static_lib_names_debug }
	filter "Release"
		links { static_lib_names_release }
	filter { }

	if not _OPTIONS["without-atlas"] then
		links { "AtlasObject" }
	end
	extra_params = {
		extra_files = { "test_setup.cpp" },
	}

	project_add_contents(source_root, {}, {}, extra_params)
	project_add_extern_libs(used_extern_libs, target_type)

	if not _OPTIONS["android"] then
		dependson { "Collada" }
	end

	rtti(rtti_setting)

	-- TODO: should fix the duplication between this OS-specific linking
	-- code, and the similar version in setup_main_exe

	if os.istarget("windows") then
		-- from "lowlevel" static lib; must be added here to be linked in
		files { source_root.."lib/sysdep/os/win/error_dialog.rc" }

		-- Enables console for the TEST project on Windows
		linkoptions { "/SUBSYSTEM:CONSOLE" }

		links { "delayimp" }

		project_add_manifest(arch)
		if arch == "amd64" then
			architecture("x86_64")
		end

	elseif os.istarget("linux") or os.istarget("bsd") then

		if link_execinfo then
			links {
				"execinfo"
			}
		end
		if not _OPTIONS["android"] and not (os.getversion().description == "OpenBSD") then
			links { "rt" }
		end

		if _OPTIONS["android"] then
			links { "log", "android" }
		end

		if os.istarget("linux") or os.getversion().description == "GNU/kFreeBSD" then
			links {
				-- Dynamic libraries (needed for linking for gold)
				"dl",
			}

			if arch == "riscv64" then
				-- Needed to resolve __atomic_load_16 et al.
				links { "atomic" }
			end
		end

		-- Threading support
		buildoptions { "-pthread" }
		if not _OPTIONS["android"] then
			linkoptions { "-pthread" }
		end

		-- For debug_resolve_symbol
		filter "Debug"
			linkoptions { "-rdynamic" }
		filter { }

		includedirs { source_root .. "pch/test/" }

	elseif os.istarget("macosx") then
		architecture(macos_arch)
		buildoptions { "-arch " .. macos_arch }
		linkoptions { "-arch " .. macos_arch }
		xcodebuildsettings { ARCHS = macos_arch }
		if _OPTIONS["macosx-version-min"] then
			xcodebuildsettings { MACOSX_DEPLOYMENT_TARGET = _OPTIONS["macosx-version-min"] }
		end
	end
end

-- must come first, so that VC sets it as the default project and therefore
-- allows running via F5 without the "where is the EXE" dialog.
setup_main_exe()

setup_all_libs()

-- add the static libs to the main EXE project. only now (after
-- setup_all_libs has run) are the lib names known. cannot move
-- setup_main_exe to run after setup_all_libs (see comment above).
-- we also don't want to hardcode the names - that would require more
-- work when changing the static lib breakdown.
project("pyrogenesis") -- Set the main project active
	links { static_lib_names }
	filter "Debug"
		links { static_lib_names_debug }
	filter "Release"
		links { static_lib_names_release }
	filter { }

if not _OPTIONS["without-atlas"] then
	setup_atlas_projects()
	setup_atlas_frontends()
end

-- On Android, models are converted offline (archive build), so Collada is not needed.
if not _OPTIONS["android"] then
	setup_collada_projects()
end

if not _OPTIONS["without-tests"] then
	setup_tests()
end
