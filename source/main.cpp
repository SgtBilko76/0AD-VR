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

/*

This module drives the game when running without Atlas (our integrated
map editor). It receives input and OS messages via SDL and feeds them
into the input dispatcher, where they are passed on to the game GUI and
simulation.
It also contains main(), which either runs the above controller or
that of Atlas depending on commandline parameters.

*/

// not for any PCH effort, but instead for the (common) definitions
// included there.
#define MINIMAL_PCH 2
#include "lib/precompiled.h"

#include "dapinterface/DapInterface.h"
#include "graphics/GameView.h"
#include "graphics/TextureManager.h"
#include "gui/GUIManager.h"
#include "lib/config2.h"
#include "lib/debug.h"
#include "lib/external_libraries/libsdl.h"
#include "lib/file/file_system.h"
#include "lib/file/vfs/vfs.h"
#include "lib/frequency_filter.h"
#include "lib/path.h"
#include "lib/posix/posix_types.h"
#include "lib/secure_crt.h"
#include "lib/status.h"
#include "lib/sysdep/compiler.h"
#include "lib/sysdep/os.h"
#include "lib/timer.h"
#include "lobby/XmppClient.h"
#include "network/NetClient.h"
#include "ps/ArchiveBuilder.h"
#include "ps/CConsole.h"
#include "ps/CLogger.h"
#include "ps/CStr.h"
#include "ps/ConfigDB.h"
#include "ps/Filesystem.h"
#include "ps/Game.h"
#include "ps/GameSetup/Atlas.h"
#include "ps/GameSetup/CmdLineArgs.h"
#include "ps/GameSetup/Config.h"
#include "ps/GameSetup/GameSetup.h"
#include "ps/GameSetup/Paths.h"
#include "ps/Globals.h"
#include "ps/Hotkey.h"
#include "ps/Input.h"
#include "ps/Loader.h"
#include "ps/Mod.h"
#include "ps/ModInstaller.h"
#include "ps/Profile.h"
#include "ps/Profiler2.h"
#include "ps/Pyrogenesis.h"
#include "ps/QuestVR.h"
#include "ps/Replay.h"
#include "ps/TaskManager.h"
#include "ps/TouchInput.h"
#include "ps/UserReport.h"
#include "ps/VideoMode.h"
#include "ps/XML/Xeromyces.h"
#include "renderer/Renderer.h"
#include "rlinterface/RLInterface.h"
#include "scriptinterface/JSON.h"
#include "scriptinterface/Context.h"
#include "scriptinterface/Conversions.h"
#include "scriptinterface/Engine.h"
#include "scriptinterface/Interface.h"
#include "scriptinterface/Request.h"
#include "simulation2/system/TurnManager.h"
#include "soundmanager/ISoundManager.h"

#include <SDL_events.h>
#include <SDL_stdinc.h>
#include <SDL_timer.h>
#include <SDL_video.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <js/RootingAPI.h>
#include <js/TypeDecls.h>
#include <js/Value.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#if OS_UNIX
#include <iostream>
#include <unistd.h> // geteuid
#endif // OS_UNIX

#if OS_MACOSX
#include "lib/sysdep/os/osx/osx_atlas.h"
#endif

#if MSC_VERSION
#include <process.h>
#define getpid _getpid // Use the non-deprecated function name
#endif

#if OS_WIN
// Forward declarations to avoid including Windows dependent headers.
Status waio_Shutdown();
Status wdir_watch_Init();
Status wdir_watch_Shutdown();
Status wutil_Init();
Status wutil_Shutdown();

// We don't want to include Windows.h as it might mess up the rest
// of the file so we just define DWORD as done in Windef.h.
#ifndef DWORD
typedef unsigned long DWORD;
#endif // !DWORD
// Request the high performance GPU on Windows by default if no system override is specified.
// See:
// - https://github.com/supertuxkart/stk-code/pull/4693/commits/0a99c667ef513b2ce0f5755729a6e05df8aac48a
// - https://docs.nvidia.com/gameworks/content/technologies/desktop/optimus.htm
// - https://gpuopen.com/learn/amdpowerxpressrequesthighperformance/
extern "C"
{
	__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
	__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 0x00000001;
}
#endif

extern CStrW g_UniqueLogPostfix;

// Determines the lifetime of the mainloop
enum ShutdownType
{
	// The application shall continue the main loop.
	None,

	// The process shall terminate as soon as possible.
	Quit,

	// The engine should be restarted in the same process, for instance to activate different mods.
	Restart,

	// Atlas should be started in the same process.
	RestartAsAtlas
};

static ShutdownType g_Shutdown = ShutdownType::None;
static int g_ExitStatus{EXIT_SUCCESS};

static std::chrono::high_resolution_clock::time_point lastFrameTime;

bool IsQuitRequested()
{
	return g_Shutdown == ShutdownType::Quit;
}

void QuitEngine(int exitStatus)
{
	g_Shutdown = ShutdownType::Quit;
	g_ExitStatus = exitStatus;
}

void RestartEngine()
{
	g_Shutdown = ShutdownType::Restart;
}

// main app message handler
static Input::Reaction MainInputHandler(const SDL_Event& ev)
{
	switch(ev.type)
	{
	case SDL_QUIT:
		QuitEngine(EXIT_SUCCESS);
		break;

	case SDL_DROPFILE:
	{
		char* dropped_filedir = ev.drop.file;
		const Paths paths(g_CmdLineArgs);
		CModInstaller installer(paths.UserData() / "mods", paths.Cache());
		installer.Install(std::string(dropped_filedir), g_ScriptContext, true);
		SDL_free(dropped_filedir);
		if (installer.GetInstalledMods().empty())
			LOGERROR("Failed to install mod %s", dropped_filedir);
		else
		{
			LOGMESSAGE("Installed mod %s", installer.GetInstalledMods().front());
			Script::Interface modInterface("Engine", "Mod", g_ScriptContext);
			g_Mods.UpdateAvailableMods(modInterface);
			RestartEngine();
		}
		break;
	}

	case SDL_HOTKEYPRESS:
		std::string hotkey = static_cast<const char*>(ev.user.data1);
		if (hotkey == "exit")
		{
			QuitEngine(EXIT_SUCCESS);
			return Input::Reaction::HANDLED;
		}
		else if (hotkey == "togglefullscreen")
		{
			g_VideoMode.ToggleFullscreen();
			return Input::Reaction::HANDLED;
		}
		else if (hotkey == "profile2.toggle")
		{
			g_Profiler2.Toggle();
			return Input::Reaction::HANDLED;
		}
		break;
	}

	return Input::Reaction::PASS;
}


// dispatch all pending events to the various receivers.
static void PumpEvents()
{
	Script::Request rq(g_GUI->GetScriptInterface());

	PROFILE3("dispatch events");

	for (const SDL_Event& ev : g_VideoMode.m_InputManager.PollEvents())
	{
		PROFILE2("event");
		if (g_GUI)
		{
			JS::RootedValue tmpVal(rq.cx);
			Script::ToJSVal(rq, &tmpVal, ev);
			std::string data = Script::StringifyJSON(rq, &tmpVal);
			PROFILE2_ATTR("%s", data.c_str());
		}
		g_VideoMode.m_InputManager.DispatchEvent(ev);
	}

	g_TouchInput.Frame();
}

/**
 * Optionally throttle the render frequency in order to
 * prevent 100% workload of the currently used CPU core.
 */
inline static void LimitFPS()
{
	// The OpenXR runtime paces frames in VR.
	if (g_VideoMode.IsVSyncEnabled() || QuestVR::IsActive())
		return;

	const double fpsLimit{
		g_ConfigDB.Get(g_Game && g_Game->IsGameStarted() ? "adaptivefps.session" : "adaptivefps.menu", 0.0)};

	// Keep in sync with options.json
	if (fpsLimit < 20.0 || fpsLimit >= 360.0)
		return;

	double wait = 1000.0 / fpsLimit -
		std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::high_resolution_clock::now() - lastFrameTime).count() / 1000.0;

	if (wait > 0.0)
		SDL_Delay(wait);

	lastFrameTime = std::chrono::high_resolution_clock::now();
}

static int ProgressiveLoad()
{
	PROFILE3("progressive load");

	const double budget = 1.0 / std::clamp(
		g_VideoMode.IsVSyncEnabled() ?
			static_cast<double>(g_VideoMode.GetDesktopFreq()) :
			g_ConfigDB.Get("adaptivefps.menu", 60.0),
		10.0, 360.0);

	std::wstring description;
	int progressPercent{0};
	try
	{
		const PS::Loader::ProgressiveLoadResult result{PS::Loader::ProgressiveLoad(budget)};
		description = result.nextDescription;
		progressPercent = result.progressPercent;
		switch(result.status)
		{
			// no load active => no-op (skip code below)
		case INFO::OK:
			return 0;
			// current task didn't complete. we only care about this insofar as the
			// load process is therefore not yet finished.
		case ERR::TIMED_OUT:
			break;
			// just finished loading
		case INFO::ALL_COMPLETE:
			g_Game->ReallyStartGame();
			description = L"Game is starting..";
			// PS::Loader::ProgressiveLoad returns L""; set to valid text to
			// avoid problems in converting to JSString
			break;
			// error!
		default:
			WARN_RETURN_STATUS_IF_ERR(result.status);
			// can't do this above due to legit ERR::TIMED_OUT
			break;
		}
	}
	catch (std::exception& e)
	{
		// Map loading failed

		// Call script function to do the actual work
		//	(delete game data, switch GUI page, show error, etc.)
		CancelLoad(CStr(e.what()).FromUTF8());
	}

	g_GUI->DisplayLoadProgress(progressPercent, description.c_str());
	return 0;
}


static void RendererIncrementalLoad()
{
	PROFILE3("renderer incremental load");

	const double maxTime = 0.1f;

	double startTime = timer_Time();
	bool more;
	do {
		more = g_Renderer.GetTextureManager().MakeProgress();
	}
	while (more && timer_Time() - startTime < maxTime);
}

#if CONFIG2_DAP_INTERFACE
static void Frame(RL::Interface* rlInterface, const int fixedFrameFrequency, DAP::Interface* dapInterface)
#else
static void Frame(RL::Interface* rlInterface, const int fixedFrameFrequency)
#endif // CONFIG2_DAP_INTERFACE
{
	g_Profiler2.RecordFrameStart();
	PROFILE2("frame");
	g_Profiler2.IncrementFrameNumber();
	PROFILE2_ATTR("%d", g_Profiler2.GetFrameNumber());

	// get elapsed time
	const double time = timer_Time();
	g_frequencyFilter->Update(time);
	// .. old method - "exact" but contains jumps
#if 0
	static double last_time;
	const double time = timer_Time();
	const float TimeSinceLastFrame = (float)(time-last_time);
	last_time = time;
	ONCE(return);	// first call: set last_time and return

	// .. new method - filtered and more smooth, but errors may accumulate
#else
	const float realTimeSinceLastFrame{static_cast<float>(
		1.0 / (fixedFrameFrequency > 0 ? fixedFrameFrequency : g_frequencyFilter->SmoothedFrequency()))};
#endif
	ENSURE(realTimeSinceLastFrame > 0.0f);

	// Decide if update is necessary
	const bool needUpdate{g_app_has_focus || g_NetClient || !g_PauseOnFocusLoss};

	// If we are not running a multiplayer game, disable updates when the game is
	// minimized or out of focus and relinquish the CPU a bit, in order to make
	// debugging easier.
	if (!needUpdate)
	{
		PROFILE3("non-focus delay");
		// don't use SDL_WaitEvent: don't want the main loop to freeze until app focus is restored
		SDL_Delay(10);
	}

	// this scans for changed files/directories and reloads them, thus
	// allowing hotloading (changes are immediately assimilated in-game).
	ReloadChangedFiles();

	ProgressiveLoad();

	RendererIncrementalLoad();

	// Waits for the headset's next frame and queues controller input as SDL
	// events (no-op outside Quest VR).
	QuestVR::BeginFrame();

	PumpEvents();

	// if the user quit by closing the window, the GL context will be broken and
	// may crash when we call Render() on some drivers, so leave this loop
	// before rendering
	if (g_Shutdown != ShutdownType::None)
		return;

	g_VideoMode.OnceAFrameWork();

	if (g_NetClient)
		g_NetClient->Poll();

#if CONFIG2_DAP_INTERFACE
	if (dapInterface)
		dapInterface->TryHandleMessage();
#endif // CONFIG2_DAP_INTERFACE

	std::optional<bool> completionCommand{g_GUI->TickObjects()};
	if (completionCommand.has_value())
		g_Shutdown = completionCommand.value() ? ShutdownType::RestartAsAtlas : ShutdownType::Quit;

	if (rlInterface)
		rlInterface->TryApplyMessage();

	if (g_Game && g_Game->IsGameStarted() && needUpdate)
	{
		if (!rlInterface)
			g_Game->Update(realTimeSinceLastFrame);

		g_Game->GetView()->Update(float(realTimeSinceLastFrame));
	}

	// Keep us connected to any XMPP servers
	if (g_XmppClient)
		g_XmppClient->recv();

	g_UserReporter.Update();

	g_Console->Update(realTimeSinceLastFrame);

	if (g_SoundManager)
		g_SoundManager->IdleTask();

	g_Renderer.RenderFrame(true);

	g_Profiler.Frame();

	LimitFPS();
}

static void NonVisualFrame()
{
	g_Profiler2.RecordFrameStart();
	PROFILE2("frame");
	g_Profiler2.IncrementFrameNumber();
	PROFILE2_ATTR("%d", g_Profiler2.GetFrameNumber());

	if (g_NetClient)
		g_NetClient->Poll();

	static std::uint32_t turn = 0;
	if (g_Game && g_Game->IsGameStarted() && g_Game->GetTurnManager())
	{
		if (g_Game->GetTurnManager()->Update(DEFAULT_TURN_LENGTH, 1,
			std::bind_front(&CGUIManager::SendEventToAll, g_GUI)))
		{
			debug_printf("Turn %u (%u)...\n", turn++, DEFAULT_TURN_LENGTH);
		}
	}

	g_Profiler.Frame();

	if (g_Game->IsGameFinished())
		QuitEngine(EXIT_SUCCESS);
}

static std::optional<RL::Interface> CreateRLInterface(const CmdLineArgs& args)
{
	if (!args.Has("rl-interface"))
		return std::nullopt;

	const std::string server_address{args.Get("rl-interface").empty() ?
		g_ConfigDB.Get("rlinterface.address", std::string{}) : args.Get("rl-interface")};

	debug_printf("RL interface listening on %s\n", server_address.c_str());
	return std::make_optional<RL::Interface>(server_address);
}

#if CONFIG2_DAP_INTERFACE
static std::optional<DAP::Interface> CreateDAPInterface(const CmdLineArgs& args)
{
	if (!args.Has("dap-interface"))
		return std::nullopt;

	const std::string server_address{args.Get("dapinterface.address").empty() ?
		g_ConfigDB.Get("dapinterface.address", std::string{}) : args.Get("dapinterface.address")};

	const int port{args.Get("dapinterface.port").empty() ?
		g_ConfigDB.Get("dapinterface.port", int{}) : args.Get("dapinterface.port").ToInt()};

	debug_printf("DAP interface listening on %s:%d\n", server_address.c_str(), port);
	return std::make_optional<DAP::Interface>(server_address, port, *g_ScriptContext);
}
#endif // CONFIG2_DAP_INTERFACE

// moved into a helper function to ensure args is destroyed before
// exit(), which may result in a memory leak.
static void RunGameOrAtlas(const std::span<const char* const> argv)
{
	const CmdLineArgs args(argv);

	g_CmdLineArgs = args;

	if (args.Has("version"))
	{
		debug_printf("Pyrogenesis %s\n", PS_VERSION);
		if (std::strcmp(PS_VERSION, PS_SERIALIZATION_VERSION) != 0)
			debug_printf("Compatible down to patch %s\n", PS_SERIALIZATION_VERSION);
		return;
	}

	if (args.Has("autostart-nonvisual") && args.Get("autostart").empty() && !args.Has("rl-interface") && !args.Has("autostart-client"))
	{
		LOGERROR("-autostart-nonvisual can't be used alone. A map with -autostart=\"TYPEDIR/MAPNAME\" is needed.");
		return;
	}

	if (args.Has("unique-logs"))
		g_UniqueLogPostfix = L"_" + std::to_wstring(std::time(nullptr)) + L"_" + std::to_wstring(getpid());

	const bool isVisualReplay = args.Has("replay-visual");
	const bool isNonVisualReplay = args.Has("replay");
	const bool isVisual = !args.Has("autostart-nonvisual");

	const int fixedFrameFrequency{args.Has("fixed-frame-frequency")
		? args.Get("fixed-frame-frequency").ToInt() : 0};

	const OsPath replayFile(
		isVisualReplay ? args.Get("replay-visual") :
		isNonVisualReplay ? args.Get("replay") : "");

	if (isVisualReplay || isNonVisualReplay)
	{
		if (!std::filesystem::is_regular_file(replayFile.string()))
		{
			debug_printf("ERROR: The requested replay file '%s' does not exist!\n", replayFile.string8().c_str());
			return;
		}
		if (DirectoryExists(replayFile))
		{
			debug_printf("ERROR: The requested replay file '%s' is a directory!\n", replayFile.string8().c_str());
			return;
		}
	}

	std::vector<OsPath> modsToInstall;
	for (const CStr& arg : args.GetArgsWithoutName())
	{
		const OsPath modPath(arg);
		if (!CModInstaller::IsDefaultModExtension(modPath.Extension()))
		{
			debug_printf("Skipping file '%s' which does not have a mod file extension.\n", modPath.string8().c_str());
			continue;
		}
		if (!std::filesystem::is_regular_file(modPath.string()))
		{
			debug_printf("ERROR: The mod file '%s' does not exist!\n", modPath.string8().c_str());
			continue;
		}
		if (DirectoryExists(modPath))
		{
			debug_printf("ERROR: The mod file '%s' is a directory!\n", modPath.string8().c_str());
			continue;
		}
		modsToInstall.emplace_back(std::move(modPath));
	}

	// We need to initialize SpiderMonkey and libxml2 in the main thread before
	// any thread uses them. So initialize them here before we might run Atlas.
	Script::Engine scriptEngine;
	CXeromycesEngine xeromycesEngine;

	// Initialise the global task manager at this point (JS & Profiler2 are set up).
	Threading::TaskManager taskManager;

	if (ATLAS_RunIfOnCmdLine(args, false))
		return;

	if (isNonVisualReplay)
	{
		Paths paths(args);
		g_VFS = CreateVfs();
		// Mount with highest priority, we don't want mods overwriting this.
		g_VFS->Mount(L"cache/", paths.Cache(), VFS_MOUNT_ARCHIVABLE, VFS_MAX_PRIORITY);

		{
			CReplayPlayer replay;
			replay.Load(replayFile);
			const int serializationTestTurn{[&] {
				if (!args.Has("serializationtest"))
					return -1;

				const CStr str{args.Get("serializationtest")};
				return str.empty() ? 0 : str.ToInt();
			}()};
			replay.Replay(
				serializationTestTurn,
				args.Has("rejointest") ? args.Get("rejointest").ToInt() : -1,
				args.Has("ooslog"),
				!args.Has("hashtest-full") || args.Get("hashtest-full") == "true",
				args.Has("hashtest-quick") && args.Get("hashtest-quick") == "true");
		}

		g_VFS.reset();
		return;
	}

	// run in archive-building mode if requested
	if (args.Has("archivebuild"))
	{
		Paths paths(args);

		OsPath mod(args.Get("archivebuild"));
		OsPath zip;
		if (args.Has("archivebuild-output"))
			zip = args.Get("archivebuild-output");
		else
			zip = mod.Filename().ChangeExtension(L".zip");

		CArchiveBuilder builder(mod, paths.Cache());

		// Add mods provided on the command line
		// NOTE: We do not handle mods in the user mod path here
		std::vector<CStr> mods = args.GetMultiple("mod");
		for (size_t i = 0; i < mods.size(); ++i)
			builder.AddBaseMod(paths.RData()/"mods"/mods[i]);

		builder.Build(zip, args.Has("archivebuild-compress"));
		return;
	}

	const double res = timer_Resolution();
	g_frequencyFilter = CreateFrequencyFilter(res, 30.0);

	// run the game
	bool rlInterfaceError{false};
	bool firstIteration{true};
	do
	{
		g_Shutdown = ShutdownType::None;

		// Do this as soon as possible, because it chdirs and will mess up the error reporting if
		// anything crashes before the working directory is set.
		InitVfs(args);

		// This must come after VFS init, which sets the current directory (required for finding our
		// output log files).
		FileLogger logger;

		if (!Init(args, std::exchange(firstIteration, false) ? INIT_MODS : 0))
		{
			ShutdownConfigAndSubsequent();
			continue;
		}

		std::vector<CStr> installedMods;
		if (!modsToInstall.empty())
		{
			Paths paths(args);
			CModInstaller installer(paths.UserData() / "mods", paths.Cache());

			// Install the mods without deleting the pyromod files
			// `modsToInstall` is cleared so we don't intstall the mods again on restart.
			for (const OsPath& modPath : std::exchange(modsToInstall, {}))
			{
				CModInstaller::ModInstallationResult result = installer.Install(modPath, g_ScriptContext, true);
				if (result != CModInstaller::ModInstallationResult::SUCCESS)
					LOGERROR("Failed to install '%s'", modPath.string8().c_str());
			}

			installedMods = installer.GetInstalledMods();

			Script::Interface modInterface("Engine", "Mod", g_ScriptContext);
			g_Mods.UpdateAvailableMods(modInterface);
		}

#if CONFIG2_DAP_INTERFACE
		std::optional<DAP::Interface> dapInterface{CreateDAPInterface(args)};
#endif // CONFIG2_DAP_INTERFACE

		{
			class VisualData
			{
			public:
				VisualData(const CmdLineArgs& args, std::vector<CStr>& installedMods)
				{
					inputHandlers = InitGraphics(args, 0, installedMods, *g_ScriptContext,
						scriptInterface);
				}

				VisualData(const VisualData&) = delete;
				VisualData& operator=(const VisualData&) = delete;
				VisualData(VisualData&&) = delete;
				VisualData& operator=(VisualData&) = delete;
				~VisualData() = default;

			private:
				Script::Interface scriptInterface{"Engine", "gui", *g_ScriptContext};
				std::unique_ptr<InputHandlers> inputHandlers;
				Input::Handler<Input::Reaction(&)(const SDL_Event&)> mainInputHandler{
					g_VideoMode.m_InputManager, Input::Slot::PRIMARY, MainInputHandler};
			};

			// MSVC doesn't support copy elision in ternary expressions. So we use a lambda instead.
			const std::optional<VisualData> visualData{[&]() -> std::optional<VisualData>
				{
					if (isVisual)
						return std::make_optional<VisualData>(args, installedMods);
					else
						return std::nullopt;
				}()};
			if (!isVisual && !InitNonVisual(args))
				g_Shutdown = ShutdownType::Quit;

			try
			{
				// MSVC doesn't support copy elision in ternary expressions. So we use a lambda instead.
				std::optional<RL::Interface> rlInterface{[&]() -> std::optional<RL::Interface>
					{
						if (g_Shutdown == ShutdownType::None)
							return CreateRLInterface(args);
						else
							return std::nullopt;
					}()};

				while (g_Shutdown == ShutdownType::None)
				{
					if (isVisual)
					{
#if CONFIG2_DAP_INTERFACE
						Frame(rlInterface ? &*rlInterface : nullptr, fixedFrameFrequency,
							dapInterface ? &*dapInterface : nullptr);
#else
						Frame(rlInterface ? &*rlInterface : nullptr, fixedFrameFrequency);
#endif
					}
					else if(rlInterface)
						rlInterface->TryApplyMessage();
					else
						NonVisualFrame();
				}

			}
			catch (const RL::SetupError&)
			{
				rlInterfaceError = true;
			}

			ShutdownNetworkAndUI();
		}

#if CONFIG2_DAP_INTERFACE
		dapInterface.reset();
#endif // CONFIG2_DAP_INTERFACE

		ShutdownConfigAndSubsequent();
	} while (g_Shutdown == ShutdownType::Restart);

	if (rlInterfaceError)
	      throw RL::SetupError{};

#if OS_MACOSX
	if (g_Shutdown == ShutdownType::RestartAsAtlas)
		startNewAtlasProcess(g_Mods.GetEnabledMods());
#else
	if (g_Shutdown == ShutdownType::RestartAsAtlas)
		ATLAS_RunIfOnCmdLine(args, true);
#endif
}

#if OS_ANDROID
// In Android we compile the engine as a shared library, not an executable,
// so rename main() to a different symbol that the wrapper library can load
extern "C" __attribute__((visibility ("default"))) int pyrogenesis_main(int argc, char* argv[])
#else
int main(int argc, char* argv[])
#endif
{
#if OS_UNIX
	// Don't allow people to run the game with root permissions,
	//	because bad things can happen, check before we do anything
	if (geteuid() == 0)
	{
		std::cerr << "********************************************************\n"
				  << "WARNING: Attempted to run the game with root permission!\n"
				  << "This is not allowed because it can alter home directory \n"
				  << "permissions and opens your system to vulnerabilities.   \n"
				  << "(You received this message because you were either      \n"
				  <<"  logged in as root or used e.g. the 'sudo' command.)    \n"
				  << "********************************************************\n\n";
		return EXIT_FAILURE;
	}
#endif // OS_UNIX

#if OS_WIN
	wutil_Init();
	wdir_watch_Init();
#endif

	EarlyInit();	// must come at beginning of main

	try
	{
		// static_cast is ok, argc is never negative.
		RunGameOrAtlas({argv, static_cast<std::size_t>(argc)});
	}
	catch (const RL::SetupError&)
	{
		g_ExitStatus = EXIT_FAILURE;
	}

	// Shut down profiler initialised by EarlyInit
	g_Profiler2.Shutdown();

#if OS_WIN
	// All calls to Windows specific functions have to happen before the following
	// shutdowns.
	wdir_watch_Shutdown();
	waio_Shutdown();
	wutil_Shutdown();
#endif

	return g_ExitStatus;
}
