/* Copyright (C) 2026 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "precompiled.h"

#include "Paths.h"

#include "lib/debug.h"
#include "lib/file/file_system.h"
#include "lib/path.h"
#include "lib/status.h"
#include "lib/sysdep/os.h"
#include "lib/sysdep/sysdep.h"	// sys_get_executable_name
#include "ps/CLogger.h"
#include "ps/CStr.h"
#include "ps/GameSetup/CmdLineArgs.h"

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#if OS_ANDROID
namespace
{
// The Java activity sets this to the app's external files directory.
OsPath AndroidRoot()
{
	const char* root = std::getenv("ZEROAD_ANDROID_ROOT");
	return OsPath(root && root[0] ? root : "/sdcard/0ad");
}
} // anonymous namespace
#endif

#if OS_WIN
# include "lib/sysdep/os/win/wutil.h"	// wutil_*Path
#elif OS_MACOSX
# include "lib/sysdep/os/osx/osx_paths.h"
# include "lib/sysdep/os/osx/osx_bundle.h"
#endif


Paths::Paths(const CmdLineArgs& args)
{
	m_root = Root(args.GetArg0());

	m_rdata = RootData(args.GetArg0());

	const char* subdirectoryName = args.Has("writableRoot")? 0 : "0ad";

	if(!subdirectoryName)
	{
		// Note: if writableRoot option is passed to the game, then
		//	all the data is a subdirectory of the root
		m_gameData = m_rdata;
		m_userData = m_gameData;
		m_config = m_gameData / "config"/"";
		m_cache = m_gameData / "cache"/"";
		m_logs = m_root / "logs"/"";
	}
	else // OS-specific path handling
	{

#if OS_ANDROID

		const OsPath appdata = AndroidRoot() / "appdata";

		// We don't make the game vs. user data distinction on Android
		m_gameData = appdata/"data"/"";
		m_userData = m_gameData;
		m_config = appdata/"config"/"";
		m_cache = appdata/"cache"/"";
		m_logs = appdata/"logs"/"";

#elif OS_WIN

		/* For reasoning behind our Windows paths, see the discussion here:
		 * http://www.wildfiregames.com/forum/index.php?showtopic=14759
		 *
		 * Summary:
		 *	1. Local appdata: for bulky unfriendly data like the cache,
		 *      which can be recreated if deleted; doesn't need backing up.
		 *  2. Roaming appdata: for slightly less unfriendly data like config
		 *      files that might theoretically be shared between different
		 *      machines on a domain.
		 *  3. Personal / My Documents: for data explicitly created by the user,
		 *      and which should be visible and easily accessed. We use a non-
		 *      localized My Games subfolder for improved organization.
		 */

		// %localappdata%/0ad/
		const OsPath localAppdata = wutil_LocalAppdataPath() / subdirectoryName/"";
		// %appdata%/0ad/
		const OsPath roamingAppData = wutil_RoamingAppdataPath() / subdirectoryName/"";
		// My Documents/My Games/0ad/
		const OsPath personalData = wutil_PersonalPath() / "My Games" / subdirectoryName/"";

		m_cache = localAppdata / "cache"/"";
		m_gameData = roamingAppData / "data"/"";
		m_userData = personalData/"";
		m_config = roamingAppData / "config"/"";
		m_logs = localAppdata / "logs"/"";

#elif OS_MACOSX

		/* For reasoning behind our OS X paths, see the discussion here:
		 * http://www.wildfiregames.com/forum/index.php?showtopic=15511
		 *
		 * Summary:
		 *  1. Application Support: most data associated with the app
		 *		should be stored here, with few exceptions (e.g. temporary
		 *		data, cached data, and managed media files).
		 *  2. Caches: used for non-critial app data that can be easily
		 *		regenerated if this directory is deleted. It is not
		 *		included in backups by default.
		 *
		 * Note: the paths returned by osx_Get*Path are not guaranteed to exist,
		 *     but that's OK since we always create them on demand.
		 */

		// We probably want to use the same subdirectoryName regardless
		//	of whether running a bundle or from SVN. Apple recommends using
		//	company name, bundle name or bundle identifier.
		OsPath appSupportPath;  // ~/Library/Application Support/0ad
		OsPath cachePath;       // ~/Library/Caches/0ad

		{
			std::string path = osx_GetAppSupportPath();
			ENSURE(!path.empty());
			appSupportPath = OsPath(path) / subdirectoryName;
		}
		{
			std::string path = osx_GetCachesPath();
			ENSURE(!path.empty());
			cachePath = OsPath(path) / subdirectoryName;
		}

		// We don't make the game vs. user data distinction on OS X
		m_gameData = appSupportPath /"";
		m_userData = m_gameData;
		m_cache = cachePath/"";
		m_config = appSupportPath / "config"/"";
		m_logs = appSupportPath / "logs"/"";

#else // OS_UNIX

		const char* envHome = getenv("HOME");
		ENSURE(envHome);
		const OsPath home(envHome);
		const OsPath xdgData   = XDG_Path("XDG_DATA_HOME",   home, home/".local/share/") / subdirectoryName;
		const OsPath xdgConfig = XDG_Path("XDG_CONFIG_HOME", home, home/".config/"     ) / subdirectoryName;
		const OsPath xdgCache  = XDG_Path("XDG_CACHE_HOME",  home, home/".cache/"      ) / subdirectoryName;
		const OsPath xdgState  = XDG_Path("XDG_STATE_HOME",  home, home/".local/state/") / subdirectoryName;

		// We don't make the game vs. user data distinction on Unix
		m_gameData = xdgData/"";
		m_userData = m_gameData;
		m_cache  = xdgCache/"";
		m_config = xdgConfig / "config"/"";
		m_logs   = xdgState  / "log"/"";

#endif
	}
}


/*static*/ OsPath Paths::Root(const OsPath& argv0)
{
#if OS_ANDROID
	return AndroidRoot();
#else

	// get full path to executable
	OsPath pathname = sys_ExecutablePathname();	// safe, but requires OS-specific implementation
	if(pathname.empty())	// failed, use argv[0] instead
	{
		const std::filesystem::path rpath{argv0.string()};
		std::error_code ec{};
		const std::filesystem::path cpath{std::filesystem::canonical(rpath, ec)};
		if (ec)
			LOGERROR("Failed to get absolute path of argv0, reason: %s", ec.message());
		else
			pathname = OsPath(cpath.wstring());
	}

	// make sure it's valid
	if(!std::filesystem::is_regular_file(pathname.string()))
	{
		LOGERROR("Cannot find executable (expected at '%s')", pathname.string8());
		WARN_IF_ERR(StatusFromErrno());
	}

	for(size_t i = 0; i < 2; i++)	// remove "system/name.exe"
		pathname = pathname.Parent();
	return pathname;

#endif
}

/*static*/ OsPath Paths::RootData([[maybe_unused]] const OsPath& argv0)
{

#ifdef INSTALLED_DATADIR
	return OsPath(STRINGIZE(INSTALLED_DATADIR))/"";
#else

# if OS_MACOSX
	if (osx_IsAppBundleValid())
	{
		debug_printf("Valid app bundle detected\n");

		std::string resourcesPath = osx_GetBundleResourcesPath();
		// Ensure we have a valid resources path
		ENSURE(!resourcesPath.empty());

		return OsPath(resourcesPath)/"data"/"";
	}
# endif // OS_MACOSX

	return Root(argv0)/"data"/"";

#endif // INSTALLED_DATADIR
}

/*static*/ OsPath Paths::XDG_Path(const char* envname, const OsPath& home, const OsPath& defaultPath)
{
	const char* path = getenv(envname);
	// Use if set and non-empty
	if(path && path[0] != '\0')
	{
		if(path[0] != '/')	// relative to $HOME
			return home / path/"";
		return OsPath(path)/"";
	}
	return defaultPath/"";
}
