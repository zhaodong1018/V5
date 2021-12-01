// Copyright Epic Games, Inc. All Rights Reserved.

#include "ProfilingDebugging/TraceAuxiliary.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ProfilingDebugging/CallstackTrace.h"
#include "Trace/Trace.h"

#if PLATFORM_WINDOWS
#	include "Windows/AllowWindowsPlatformTypes.h"
#	include <Windows.h>
#	include "Windows/HideWindowsPlatformTypes.h"
#endif

#if !defined(WITH_UNREAL_TRACE_LAUNCH)
#	define WITH_UNREAL_TRACE_LAUNCH (PLATFORM_DESKTOP && !UE_BUILD_SHIPPING && !IS_PROGRAM)
#endif

#include "CoreGlobals.h"
#include "Misc/Paths.h"
#include "Misc/Fork.h"

#if WITH_UNREAL_TRACE_LAUNCH
#	include "Misc/Parse.h"
#endif

#include <atomic>

#if UE_TRACE_ENABLED

#include "BuildSettings.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/StringView.h"
#include "Containers/UnrealString.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/CString.h"
#include "Misc/DateTime.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "ProfilingDebugging/PlatformFileTrace.h"
#include "ProfilingDebugging/PlatformEvents.h"
#include "ProfilingDebugging/MemoryTrace.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "String/ParseTokens.h"
#include "Templates/UnrealTemplate.h"
#include "Trace/Trace.inl"

////////////////////////////////////////////////////////////////////////////////
const TCHAR* GDefaultChannels = TEXT("cpu,gpu,frame,log,bookmark");
const TCHAR* GMemoryChannels = TEXT("memtag,memalloc,callstack,module");

////////////////////////////////////////////////////////////////////////////////
CSV_DEFINE_CATEGORY(Trace, true);

////////////////////////////////////////////////////////////////////////////////
DECLARE_STATS_GROUP(TEXT("TraceLog"), STATGROUP_Trace, STATCAT_Advanced);
DECLARE_MEMORY_STAT(TEXT("Memory used"), STAT_TraceMemoryUsed, STATGROUP_Trace);
DECLARE_MEMORY_STAT(TEXT("Important event cache used"), STAT_TraceCacheUsed, STATGROUP_Trace);
DECLARE_MEMORY_STAT(TEXT("Important event cache waste"), STAT_TraceCacheWaste, STATGROUP_Trace);
DECLARE_MEMORY_STAT(TEXT("Sent"), STAT_TraceSent, STATGROUP_Trace);

////////////////////////////////////////////////////////////////////////////////
enum class ETraceConnectType
{
	Network,
	File
};

////////////////////////////////////////////////////////////////////////////////
class FTraceAuxiliaryImpl
{
public:
	const TCHAR*			GetDest() const;
	template <class T> void	ReadChannels(T&& Callback) const;
	void					AddChannels(const TCHAR* ChannelList);
	bool					Connect(ETraceConnectType Type, const TCHAR* Parameter);
	bool					Stop();
	void					EnableChannels();
	void					DisableChannels();
	void					SetTruncateFile(bool bTruncateFile);
	void					UpdateCsvStats() const;
	void					StartWorkerThread();
	void					StartEndFramePump();

private:
	enum class EState : uint8
	{
		Stopped,
		Tracing,
	};

	struct FChannel
	{
		FString				Name;
		bool				bActive = false;
	};

	void					AddChannel(const TCHAR* Name);
	void					AddChannels(const TCHAR* Name, bool bResolvePresets);
	void					EnableChannel(FChannel& Channel);
	bool					SendToHost(const TCHAR* Host);
	bool					WriteToFile(const TCHAR* Path=nullptr);

	TMap<uint32, FChannel>	Channels;
	FString					TraceDest;
	EState					State = EState::Stopped;
	bool					bTruncateFile = false;
	bool					bWorkerThreadStarted = false;
};

static FTraceAuxiliaryImpl GTraceAuxiliary;
static FDelegateHandle GEndFrameDelegateHandle;
static FDelegateHandle GEndFrameStatDelegateHandle;

////////////////////////////////////////////////////////////////////////////////
void FTraceAuxiliaryImpl::AddChannels(const TCHAR* ChannelList)
{
	AddChannels(ChannelList, true);
}

////////////////////////////////////////////////////////////////////////////////
void FTraceAuxiliaryImpl::AddChannels(const TCHAR* ChannelList, bool bResolvePresets)
{
	UE::String::ParseTokens(ChannelList, TEXT(","), [this, bResolvePresets] (const FStringView& Token)
	{
		TCHAR Name[80];
		const size_t ChannelNameSize = Token.CopyString(Name, UE_ARRAY_COUNT(Name) - 1);
		Name[ChannelNameSize] = '\0';

		if (bResolvePresets)
		{
			FString Value;
			// Check against hard coded presets
			if(FCString::Stricmp(Name, TEXT("default")) == 0)
			{
				AddChannels(GDefaultChannels, false);
			}
			else if(FCString::Stricmp(Name, TEXT("memory"))== 0)
			{
				AddChannels(GMemoryChannels, false);
			}
			// Check against data driven presets (if available)
			else if (GConfig && GConfig->GetString(TEXT("Trace.ChannelPresets"), Name, Value, GEngineIni))
			{
				AddChannels(*Value, false);
				return;
			}
		}

		AddChannel(Name);
	});
}

////////////////////////////////////////////////////////////////////////////////
void FTraceAuxiliaryImpl::AddChannel(const TCHAR* Name)
{
	uint32 Hash = 5381;
	for (const TCHAR* c = Name; *c; ++c)
	{
		uint32 LowerC = *c | 0x20;
        Hash = ((Hash << 5) + Hash) + LowerC;
	}

	if (Channels.Find(Hash) != nullptr)
	{
		return;
	}

	FChannel& Value = Channels.Add(Hash, {});
	Value.Name = Name;

	if (State >= EState::Tracing)
	{
		EnableChannel(Value);
	}
}

////////////////////////////////////////////////////////////////////////////////
bool FTraceAuxiliaryImpl::Connect(ETraceConnectType Type, const TCHAR* Parameter)
{
	// Connect/write to file. But only if we're not already sending/writing
	bool bConnected = UE::Trace::IsTracing();
	if (!bConnected)
	{
		if (Type == ETraceConnectType::Network)
		{
			bConnected = SendToHost(Parameter);
			if (bConnected)
			{
				UE_LOG(LogCore, Display, TEXT("Trace started (connected to trace server %s)."), GetDest());
			}
			else
			{
				UE_LOG(LogCore, Error, TEXT("Trace failed to connect (trace server: %s)!"), Parameter ? Parameter : TEXT(""));
			}	
		}

		else if (Type == ETraceConnectType::File)
		{
			bConnected = WriteToFile(Parameter);
			if (bConnected)
			{
				UE_LOG(LogCore, Display, TEXT("Trace started (writing to file \"%s\")."), GetDest());
			}
			else
			{
				UE_LOG(LogCore, Error, TEXT("Trace failed to connect (file: \"%s\")!"), Parameter ? Parameter : TEXT(""));
			}	
		}
	}

	if (!bConnected)
	{
		return false;
	}

	// We're now connected. If we don't appear to have any channels we'll set
	// some defaults for the user. Less futzing.
	if (!Channels.Num())
	{
		AddChannels(GDefaultChannels);
	}

	EnableChannels();

	State = EState::Tracing;
	return true;
}

////////////////////////////////////////////////////////////////////////////////
bool FTraceAuxiliaryImpl::Stop()
{
	if (!UE::Trace::Stop())
	{
		return false;
	}

	DisableChannels();
	State = EState::Stopped;
	TraceDest.Reset();
	return true;
}

////////////////////////////////////////////////////////////////////////////////
void FTraceAuxiliaryImpl::EnableChannel(FChannel& Channel)
{
	if (Channel.bActive)
	{
		return;
	}

	// Channel names have been provided by the user and may not exist yet. As
	// we want to maintain bActive accurately (channels toggles are reference
	// counted), we will first check Trace knows of the channel.
	if (!UE::Trace::IsChannel(*Channel.Name))
	{
		return;
	}

	EPlatformEvent Event = PlatformEvents_GetEvent(Channel.Name);
	if (Event != EPlatformEvent::None)
	{
		PlatformEvents_Enable(Event);
	}

	UE::Trace::ToggleChannel(*Channel.Name, true);
	Channel.bActive = true;
}

////////////////////////////////////////////////////////////////////////////////
void FTraceAuxiliaryImpl::EnableChannels()
{
	for (auto& ChannelPair : Channels)
	{
		EnableChannel(ChannelPair.Value);
	}
}

////////////////////////////////////////////////////////////////////////////////
void FTraceAuxiliaryImpl::DisableChannels()
{
	for (auto& ChannelPair : Channels)
	{
		FChannel& Channel = ChannelPair.Value;
		if (Channel.bActive)
		{
			UE::Trace::ToggleChannel(*Channel.Name, false);
			Channel.bActive = false;

			EPlatformEvent Event = PlatformEvents_GetEvent(Channel.Name);
			if (Event != EPlatformEvent::None)
			{
				PlatformEvents_Disable(Event);
			}
		}
	}
}

////////////////////////////////////////////////////////////////////////////////
void FTraceAuxiliaryImpl::SetTruncateFile(bool bNewTruncateFileState)
{
	bTruncateFile = bNewTruncateFileState;
}

////////////////////////////////////////////////////////////////////////////////
bool FTraceAuxiliaryImpl::SendToHost(const TCHAR* Host)
{
	if (!UE::Trace::SendTo(Host))
	{
		UE_LOG(LogCore, Warning, TEXT("Unable to trace to host '%s'"), Host);
		return false;
	}

	TraceDest = Host;
	return true;
}

////////////////////////////////////////////////////////////////////////////////
bool FTraceAuxiliaryImpl::WriteToFile(const TCHAR* Path)
{
	if (Path == nullptr || *Path == '\0')
	{
		FString Name = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S.utrace"));
		return WriteToFile(*Name);
	}

	FString WritePath;

	// If there's no slash in the path, we'll put it in the profiling directory
	if (FCString::Strchr(Path, '\\') == nullptr && FCString::Strchr(Path, '/') == nullptr)
	{
		WritePath = FPaths::ProfilingDir();
		WritePath += Path;
	}
	else
	{
		WritePath = Path;
	}

	// The user may not have provided a suitable extension
	if (!WritePath.EndsWith(".utrace"))
	{
		WritePath += ".utrace";
	}

	IFileManager& FileManager = IFileManager::Get();

	// Ensure we can write the trace file appropriately
	FString WriteDir = FPaths::GetPath(WritePath);
	if (!FileManager.MakeDirectory(*WriteDir, true))
	{
		UE_LOG(LogCore, Warning, TEXT("Failed to create directory '%s'"), *WriteDir);
		return false;
	}

	if (!bTruncateFile && FileManager.FileExists(*WritePath))
	{
		UE_LOG(LogCore, Warning, TEXT("Trace file '%s' already exists"), *WritePath);
		return false;
	}

	// Finally, tell trace to write the trace to a file.
	FString NativePath = FileManager.ConvertToAbsolutePathForExternalAppForWrite(*WritePath);
	if (!UE::Trace::WriteTo(*NativePath))
	{
		UE_LOG(LogCore, Warning, TEXT("Unable to trace to file '%s'"), *WritePath);
		return false;
	}

	TraceDest = MoveTemp(NativePath);
	return true;
}

////////////////////////////////////////////////////////////////////////////////
const TCHAR* FTraceAuxiliaryImpl::GetDest() const
{
	return *TraceDest;
}

////////////////////////////////////////////////////////////////////////////////
template <class T>
void FTraceAuxiliaryImpl::ReadChannels(T&& Callback) const
{
	for (const auto& ChannelPair : Channels)
	{
		Callback(*(ChannelPair.Value.Name));
	}
}

////////////////////////////////////////////////////////////////////////////////
void FTraceAuxiliaryImpl::UpdateCsvStats() const
{
#if TRACE_PRIVATE_STATISTICS
	// Only publish CSV stats if we have ever run tracing in order to reduce overhead in most runs.
	static bool bDoStats = false;
	if (UE::Trace::IsTracing() || bDoStats)
	{
		bDoStats = true;

		UE::Trace::FStatistics Stats;
		UE::Trace::GetStatistics(Stats);

		CSV_CUSTOM_STAT(Trace, MemoryUsedMb,	double(Stats.MemoryUsed) / 1024.0 / 1024.0, ECsvCustomStatOp::Set);
		CSV_CUSTOM_STAT(Trace, CacheUsedMb,		double(Stats.CacheUsed) / 1024.0 / 1024.0, ECsvCustomStatOp::Set);
		CSV_CUSTOM_STAT(Trace, CacheWasteMb,	double(Stats.CacheWaste) / 1024.0 / 1024.0, ECsvCustomStatOp::Set);
	}
#endif
}

////////////////////////////////////////////////////////////////////////////////
void FTraceAuxiliaryImpl::StartWorkerThread()
{
	if (!bWorkerThreadStarted)
	{
		UE::Trace::StartWorkerThread();
		bWorkerThreadStarted = true;
	}
}

////////////////////////////////////////////////////////////////////////////////
void FTraceAuxiliaryImpl::StartEndFramePump()
{
	if (!GEndFrameDelegateHandle.IsValid())
	{
		// If the worker thread is disabled, pump the update from end frame
		GEndFrameDelegateHandle = FCoreDelegates::OnEndFrame.AddStatic(UE::Trace::Update);
	}
	if (!GEndFrameStatDelegateHandle.IsValid())
	{
		// Update stats every frame
		GEndFrameStatDelegateHandle = FCoreDelegates::OnEndFrame.AddLambda([]()
		{
			UE::Trace::FStatistics Stats;
			UE::Trace::GetStatistics(Stats);
			SET_MEMORY_STAT(STAT_TraceMemoryUsed, Stats.MemoryUsed);
			SET_MEMORY_STAT(STAT_TraceCacheUsed, Stats.CacheUsed);
			SET_MEMORY_STAT(STAT_TraceCacheWaste, Stats.CacheWaste);
			SET_MEMORY_STAT(STAT_TraceSent, Stats.BytesSent);
		});
	}
}


////////////////////////////////////////////////////////////////////////////////
static void TraceAuxiliaryConnectEpilogue()
{
	// It is possible that something outside of TraceAux's world view has called
	// UE::Trace::SendTo/WriteTo(). A plugin that has created its own store for
	// example. There's not really much that can be done about that here (tracing
	// is singular within a process. We can at least detect the obvious case and
	// inform the user.
	const TCHAR* TraceDest = GTraceAuxiliary.GetDest();
	if (TraceDest[0] == '\0')
	{
		UE_LOG(LogConsoleResponse, Warning, TEXT("Trace system already in use by a plugin or -trace*=... argument. Use 'Trace.Stop' first."));
		return;
	}

	// Give the user some feedback that everything's underway.
	FString Channels;
	GTraceAuxiliary.ReadChannels([&Channels] (const TCHAR* Channel)
	{
		if (Channels.Len())
		{
			Channels += TEXT(",");
		}

		Channels += Channel;
	});
	UE_LOG(LogConsoleResponse, Log, TEXT("Tracing to: %s"), TraceDest);
	UE_LOG(LogConsoleResponse, Log, TEXT("Trace channels: %s"), *Channels);
}

////////////////////////////////////////////////////////////////////////////////
static void TraceAuxiliarySend(const TArray<FString>& Args)
{
	if (Args.Num() < 1)
	{
		UE_LOG(LogConsoleResponse, Warning, TEXT("No host name given; Trace.Send <Host> [ChannelSet]"));
		return;
	}

	const TCHAR* Target = *Args[0];
	const TCHAR* Channels = Args.Num() > 1 ? *Args[1] : nullptr;
	if (!FTraceAuxiliary::Start(FTraceAuxiliary::EConnectionType::Network, Target, Channels))
	{
		UE_LOG(LogConsoleResponse, Warning, TEXT("Failed to start tracing to '%s'"), *Args[0]);
		return;
	}

	TraceAuxiliaryConnectEpilogue();
}

////////////////////////////////////////////////////////////////////////////////
static void TraceAuxiliaryStart(const TArray<FString>& Args)
{
	const TCHAR* Channels = Args.Num() > 0 ? *Args[0] : nullptr;
	FTraceAuxiliary::Options Opts;
	Opts.bNoWorkerThread = true;
	if (!FTraceAuxiliary::Start(FTraceAuxiliary::EConnectionType::File, nullptr, Channels, &Opts))
	{
		UE_LOG(LogConsoleResponse, Warning, TEXT("Failed to start tracing to a file"));
		return;
	}

	TraceAuxiliaryConnectEpilogue();
}

////////////////////////////////////////////////////////////////////////////////
static void TraceAuxiliaryStop()
{
	UE_LOG(LogConsoleResponse, Log, TEXT("Tracing stopped."));
	GTraceAuxiliary.Stop();
}

////////////////////////////////////////////////////////////////////////////////
static void TraceAuxiliaryPause()
{
	UE_LOG(LogConsoleResponse, Log, TEXT("Tracing paused"));
	GTraceAuxiliary.DisableChannels();
}

////////////////////////////////////////////////////////////////////////////////
static void TraceAuxiliaryResume()
{
	UE_LOG(LogConsoleResponse, Log, TEXT("Tracing resumed"));
	GTraceAuxiliary.EnableChannels();
}

////////////////////////////////////////////////////////////////////////////////
static FAutoConsoleCommand TraceAuxiliarySendCmd(
	TEXT("Trace.Send"),
	TEXT("Send trace data to the trace store; Trace.Send <Host> [ChannelSet]"),
	FConsoleCommandWithArgsDelegate::CreateStatic(TraceAuxiliarySend)
);

////////////////////////////////////////////////////////////////////////////////
static FAutoConsoleCommand TraceAuxiliaryStartCmd(
	TEXT("Trace.Start"),
	TEXT(
		"Begin tracing profiling events to a file; Trace.Start [ChannelSet]"
		" where ChannelSet is either comma-separated list of trace channels, a Config/Trace.ChannelPresets key, or optional."
	),
	FConsoleCommandWithArgsDelegate::CreateStatic(TraceAuxiliaryStart)
);

////////////////////////////////////////////////////////////////////////////////
static FAutoConsoleCommand TraceAuxiliaryStopCmd(
	TEXT("Trace.Stop"),
	TEXT("Stops tracing profiling events"),
	FConsoleCommandDelegate::CreateStatic(TraceAuxiliaryStop)
);

////////////////////////////////////////////////////////////////////////////////
static FAutoConsoleCommand TraceAuxiliaryPauseCmd(
	TEXT("Trace.Pause"),
	TEXT("Pauses all trace channels currently sending events"),
	FConsoleCommandDelegate::CreateStatic(TraceAuxiliaryPause)
);

////////////////////////////////////////////////////////////////////////////////
static FAutoConsoleCommand TraceAuxiliaryResumeCmd(
	TEXT("Trace.Resume"),
	TEXT("Resume tracing that was previously paused"),
	FConsoleCommandDelegate::CreateStatic(TraceAuxiliaryResume)
);

#endif // UE_TRACE_ENABLED



#if WITH_UNREAL_TRACE_LAUNCH
////////////////////////////////////////////////////////////////////////////////
static std::atomic<int32> GUnrealTraceLaunched; // = 0;

////////////////////////////////////////////////////////////////////////////////
#if PLATFORM_WINDOWS
static void LaunchUnrealTraceInternal(const TCHAR* CommandLine)
{
	if (GUnrealTraceLaunched.load(std::memory_order_relaxed))
	{
		UE_LOG(LogCore, Log, TEXT("UnrealTraceServer: Trace store already started"));
		return;
	}

	TWideStringBuilder<MAX_PATH + 32> CreateProcArgs;
	CreateProcArgs << "\"";
	CreateProcArgs << FPaths::EngineDir();
	CreateProcArgs << TEXT("/Binaries/Win64/UnrealTraceServer.exe\"");
	CreateProcArgs << TEXT(" fork");

	uint32 CreateProcFlags = CREATE_BREAKAWAY_FROM_JOB;
	if (FParse::Param(CommandLine, TEXT("traceshowstore")))
	{
		CreateProcFlags |= CREATE_NEW_CONSOLE;
	}
	else
	{
		CreateProcFlags |= CREATE_NO_WINDOW;
	}
	STARTUPINFOW StartupInfo = { sizeof(STARTUPINFOW) };
	PROCESS_INFORMATION ProcessInfo = {};
	BOOL bOk = CreateProcessW(nullptr, LPWSTR(*CreateProcArgs), nullptr, nullptr,
		false, CreateProcFlags, nullptr, nullptr, &StartupInfo, &ProcessInfo);

	if (!bOk)
	{
		UE_LOG(LogCore, Display, TEXT("UnrealTraceServer: Unable to launch the trace store with '%s' (%08x)"), *CreateProcArgs, GetLastError());
		return;
	}

	if (WaitForSingleObject(ProcessInfo.hProcess, 5000) == WAIT_TIMEOUT)
	{
		UE_LOG(LogCore, Warning, TEXT("UnrealTraceServer: Timed out waiting for the trace store to start"));
	}
	else
	{
		DWORD ExitCode = 0x0000'a9e0;
		GetExitCodeProcess(ProcessInfo.hProcess, &ExitCode);
		if (ExitCode)
		{
			UE_LOG(LogCore, Warning, TEXT("UnrealTraceServer: Trace store returned an error (0x%08x)"), ExitCode);
		}
		else
		{
			UE_LOG(LogCore, Log, TEXT("UnrealTraceServer: Trace store launch successful"));
			GUnrealTraceLaunched.fetch_add(1, std::memory_order_relaxed);
		}
	}

	CloseHandle(ProcessInfo.hProcess);
	CloseHandle(ProcessInfo.hThread);
}
#endif // PLATFORM_WINDOWS

////////////////////////////////////////////////////////////////////////////////
#if PLATFORM_UNIX || PLATFORM_MAC
static void LaunchUnrealTraceInternal(const TCHAR* CommandLine)
{
	/* nop */

#if 0
	if (GUnrealTraceLaunched.load(std::memory_order_relaxed))
	{
		UE_LOG(LogCore, Log, TEXT("UnrealTraceServer: Trace store already started"));
		return;
	}

	TAnsiStringBuilder<320> BinPath;
	BinPath << TCHAR_TO_UTF8(*FPaths::EngineDir());
#if PLATFORM_UNIX
	BinPath << "Binaries/Linux/UnrealTraceServer";
#elif PLATFORM_MAC
	BinPath << "Binaries/Mac/UnrealTraceServer";
#endif

	if (access(*BinPath, F_OK) < 0)
	{
		UE_LOG(LogCore, Display, TEXT("UnrealTraceServer: Binary not found (%s)"), ANSI_TO_TCHAR(*BinPath));
		return;
	}

	TAnsiStringBuilder<64> ForkArg;
	ForkArg << "fork";

	pid_t UtsPid = vfork();
	if (UtsPid < 0)
	{
		UE_LOG(LogCore, Display, TEXT("UnrealTraceServer: Unable to fork (errno: %d)"), errno);
		return;
	}
	else if (UtsPid == 0)
	{
		char* Args[] = { BinPath.GetData(), ForkArg.GetData(), nullptr };
		extern char** environ;
		execve(*BinPath, Args, environ);
		_exit(0x80 | (errno & 0x7f));
	}
	
	int32 WaitStatus = 0;
	do
	{
		int32 WaitRet = waitpid(UtsPid, &WaitStatus, 0);
		if (WaitRet < 0)
		{
			UE_LOG(LogCore, Display, TEXT("UnrealTraceServer: waitpid() error; (errno: %d)"), errno);
			return;
		}
	}
	while (!WIFEXITED(WaitStatus));

	int32 UtsRet = WEXITSTATUS(WaitStatus);
	if (UtsRet)
	{
		UE_LOG(LogCore, Display, TEXT("UnrealTraceServer: Trace store returned an error (0x%08x)"), UtsRet);
	}
	else
	{
		UE_LOG(LogCore, Log, TEXT("UnrealTraceServer: Trace store launch successful"));
		GUnrealTraceLaunched.fetch_add(1, std::memory_order_relaxed);
	}
#endif // 0
}
#endif // PLATFORM_UNIX/MAC
#endif // WITH_UNREAL_TRACE_LAUNCH



////////////////////////////////////////////////////////////////////////////////
UE_TRACE_EVENT_BEGIN(Diagnostics, Session2, NoSync|Important)
	UE_TRACE_EVENT_FIELD(UE::Trace::AnsiString, Platform)
	UE_TRACE_EVENT_FIELD(UE::Trace::AnsiString, AppName)
	UE_TRACE_EVENT_FIELD(UE::Trace::WideString, CommandLine)
	UE_TRACE_EVENT_FIELD(UE::Trace::WideString, Branch)
	UE_TRACE_EVENT_FIELD(UE::Trace::WideString, BuildVersion)
	UE_TRACE_EVENT_FIELD(uint32, Changelist)
	UE_TRACE_EVENT_FIELD(uint8, ConfigurationType)
	UE_TRACE_EVENT_FIELD(uint8, TargetType)
UE_TRACE_EVENT_END()

////////////////////////////////////////////////////////////////////////////////
static bool StartFromCommandlineArguments(const TCHAR* CommandLine)
{
#if UE_TRACE_ENABLED

	// Get active channels
	FString Channels;
	if (FParse::Value(CommandLine, TEXT("-trace="), Channels, false))
	{
	}
	else if (FParse::Param(CommandLine, TEXT("trace")))
	{
		Channels = GDefaultChannels;
	}
	
	// By default, if any channels are enabled we trace to memory.
	FTraceAuxiliary::EConnectionType Type = FTraceAuxiliary::EConnectionType::None;

	// Setup options
	FTraceAuxiliary::Options Opts;
	Opts.bTruncateFile = FParse::Param(CommandLine, TEXT("tracefiletrunc"));
	Opts.bNoWorkerThread = !FPlatformProcess::SupportsMultithreading();

	// Find if a connection type is specified
	FString Parameter;
	const TCHAR* Target = nullptr;
	if (FParse::Value(CommandLine, TEXT("-tracehost="), Parameter))
	{
		Type = FTraceAuxiliary::EConnectionType::Network;
		Target = *Parameter;
	}
	else if (FParse::Value(CommandLine, TEXT("-tracefile="), Parameter))
	{
		Type = FTraceAuxiliary::EConnectionType::File;
		if (Parameter.IsEmpty())
		{
			UE_LOG(LogCore, Warning, TEXT("Empty parameter to 'tracefile' argument. Using default filename."));
			Target = nullptr;
		}
		else
		{
			Target = *Parameter;
		}
	}
	else if (FParse::Param(CommandLine, TEXT("tracefile")))
	{
		Type = FTraceAuxiliary::EConnectionType::File;
		Target = nullptr;
	}

	// If user has defined a connection type but not specified channels, use the default channel set.
	if (Type != FTraceAuxiliary::EConnectionType::None && Channels.IsEmpty())
	{
		Channels = GDefaultChannels;
	}

	if (Channels.IsEmpty())
	{
		return false;
	}
	
	// Finally start tracing to the requested connection
	return FTraceAuxiliary::Start(Type, Target, nullptr, &Opts);
#endif
	return false;
}

////////////////////////////////////////////////////////////////////////////////
static void SetupChannelsFromCommandline(const TCHAR* CommandLine)
{
#if UE_TRACE_ENABLED
	// Get active channels
	FString Channels;
	if (FParse::Value(CommandLine, TEXT("-trace="), Channels, false))
	{
	}
	else if (FParse::Param(CommandLine, TEXT("trace")))
	{
		Channels = TEXT("default");
	}
	else
	{
		return;
	}
	
	GTraceAuxiliary.AddChannels(*Channels);
	GTraceAuxiliary.EnableChannels();
	
	UE_LOG(LogCore, Display, TEXT("Trace channels: %s"), *Channels);
#endif
}

////////////////////////////////////////////////////////////////////////////////
bool FTraceAuxiliary::Start(EConnectionType Type, const TCHAR* Target, const TCHAR* Channels, Options* Options)
{
#if UE_TRACE_ENABLED
	// Make sure the worker thread is started unless explicitly opt out.
	if (!Options || !Options->bNoWorkerThread)
	{
		GTraceAuxiliary.StartWorkerThread();
	}

	if (Channels)
	{
		UE_LOG(LogCore, Display, TEXT("Trace channels: '%s'"), Channels);
		GTraceAuxiliary.AddChannels(Channels);
		GTraceAuxiliary.EnableChannels();
	}
	
	if (Options)
	{
		// Truncation is only valid when tracing to file and filename is set
		if (Options->bTruncateFile && Type == EConnectionType::File && Target != nullptr)
		{
			GTraceAuxiliary.SetTruncateFile(Options->bTruncateFile);
		}
	}

	if (Type == EConnectionType::File)
	{
		return GTraceAuxiliary.Connect(ETraceConnectType::File, Target);
	}
	else if(Type == EConnectionType::Network)
	{
		return GTraceAuxiliary.Connect(ETraceConnectType::Network, Target);	
	}
#endif
	return false;
}

////////////////////////////////////////////////////////////////////////////////
bool FTraceAuxiliary::Stop()
{
#if UE_TRACE_ENABLED
	return GTraceAuxiliary.Stop();
#else
	return false;
#endif
}

////////////////////////////////////////////////////////////////////////////////
bool FTraceAuxiliary::Pause()
{
#if UE_TRACE_ENABLED
	GTraceAuxiliary.DisableChannels();
#endif
	return true;
}

////////////////////////////////////////////////////////////////////////////////
bool FTraceAuxiliary::Resume()
{
#if UE_TRACE_ENABLED
	GTraceAuxiliary.EnableChannels();
#endif
	return true;
}

////////////////////////////////////////////////////////////////////////////////
void FTraceAuxiliary::Initialize(const TCHAR* CommandLine)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FTraceAux_Init);

#if WITH_UNREAL_TRACE_LAUNCH
	if (!(FParse::Param(CommandLine, TEXT("notraceserver")) || FParse::Param(CommandLine, TEXT("buildmachine"))))
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(FTraceAux_LaunchUnrealTrace);
		LaunchUnrealTraceInternal(CommandLine);
	}
#endif

#if UE_TRACE_ENABLED
	const TCHAR* AppName = TEXT(UE_APP_NAME);
#if IS_MONOLITHIC && !IS_PROGRAM
	extern TCHAR GInternalProjectName[];
	if (GInternalProjectName[0] != '\0')
	{
		AppName = GInternalProjectName;
	}
#endif

#if UE_MEMORY_TRACE_ENABLED
	MemoryTrace_InitializeLate();
#endif

	// Trace out information about this session. This is done before initialisation
	// so that it is always sent (all channels are enabled prior to initialisation)
	const TCHAR* BranchName = BuildSettings::GetBranchName();
	const TCHAR* BuildVersion = BuildSettings::GetBuildVersion();
	constexpr uint32 PlatformLen = UE_ARRAY_COUNT(PREPROCESSOR_TO_STRING(UBT_COMPILED_PLATFORM)) - 1;
	const uint32 AppNameLen = FCString::Strlen(AppName);
	const uint32 CommandLineLen = FCString::Strlen(CommandLine);
	const uint32 BranchNameLen = FCString::Strlen(BranchName);
	const uint32 BuildVersionLen = FCString::Strlen(BuildVersion);
	uint32 DataSize =
		(PlatformLen * sizeof(ANSICHAR)) +
		(AppNameLen * sizeof(ANSICHAR)) +
		(CommandLineLen * sizeof(TCHAR)) +
		(BranchNameLen * sizeof(TCHAR)) +
		(BuildVersionLen * sizeof(TCHAR));
	UE_TRACE_LOG(Diagnostics, Session2, UE::Trace::TraceLogChannel, DataSize)
		<< Session2.Platform(PREPROCESSOR_TO_STRING(UBT_COMPILED_PLATFORM), PlatformLen)
		<< Session2.AppName(AppName, AppNameLen)
		<< Session2.CommandLine(CommandLine, CommandLineLen)
		<< Session2.Branch(BranchName, BranchNameLen)
		<< Session2.BuildVersion(BuildVersion, BuildVersionLen)
		<< Session2.Changelist(BuildSettings::GetCurrentChangelist())
		<< Session2.ConfigurationType(uint8(FApp::GetBuildConfiguration()))
		<< Session2.TargetType(uint8(FApp::GetBuildTargetType()));

	// Attempt to send trace data somewhere from the command line. It prehaps
	// seems odd to do this before initialising Trace, but it is done this way
	// to support disabling the "important" cache without losing any events.
	// When Forking only the forked child should start the actual tracing
	const bool bShouldStartTracingNow = !FForkProcessHelper::IsForkRequested();
	if (bShouldStartTracingNow)
	{
		StartFromCommandlineArguments(CommandLine);
	}
	
	// Initialize Trace
	UE::Trace::FInitializeDesc Desc;
	Desc.bUseWorkerThread = false;
	Desc.bUseImportantCache = (FParse::Param(CommandLine, TEXT("tracenocache")) == false);
	if (FParse::Value(CommandLine, TEXT("-tracetailmb="), Desc.TailSizeBytes))
	{
		Desc.TailSizeBytes <<= 20;
	}
	UE::Trace::Initialize(Desc);

	// Always register end frame updates. This path is short circuited if a worker thread
	// exists.
	GTraceAuxiliary.StartEndFramePump();
	if (FPlatformProcess::SupportsMultithreading() && !FForkProcessHelper::IsForkRequested())
	{
		GTraceAuxiliary.StartWorkerThread();
	}

	// Initialize callstack tracing with the regular malloc (it might have already been initialized by memory tracing).
	CallstackTrace_Create(GMalloc);
	CallstackTrace_Initialize();

	// By default use 1msec for stack sampling interval
	uint32 Microseconds = 1000;
	FParse::Value(CommandLine, TEXT("-samplinginterval="), Microseconds);
	PlatformEvents_Init(Microseconds);

#if CSV_PROFILER
	FCoreDelegates::OnEndFrame.AddRaw(&GTraceAuxiliary, &FTraceAuxiliaryImpl::UpdateCsvStats);
#endif
	
	FModuleManager::Get().OnModulesChanged().AddLambda([](FName Name, EModuleChangeReason Reason)
	{
		if (Reason == EModuleChangeReason::ModuleLoaded)
		{
			GTraceAuxiliary.EnableChannels();
		}
	});
	
	UE::Trace::ThreadRegister(TEXT("GameThread"), FPlatformTLS::GetCurrentThreadId(), -1);

	SetupChannelsFromCommandline(CommandLine);

#endif
}

////////////////////////////////////////////////////////////////////////////////
void FTraceAuxiliary::InitializePresets(const TCHAR* CommandLine)
{
#if UE_TRACE_ENABLED
	// Second pass over trace arguments, this time to allow config defined presets
	// to be applied.
	FString Parameter;
	if (FParse::Value(CommandLine, TEXT("-trace="), Parameter, false))
	{
		GTraceAuxiliary.AddChannels(*Parameter);
		GTraceAuxiliary.EnableChannels();
	}
#endif 
}

////////////////////////////////////////////////////////////////////////////////
void FTraceAuxiliary::Shutdown()
{
#if UE_TRACE_ENABLED
	// make sure all platform event functionality has shut down as on some
	// platforms it impacts whole system, even if application has terminated
	PlatformEvents_Stop();
#endif
}

////////////////////////////////////////////////////////////////////////////////
void FTraceAuxiliary::EnableChannels()
{
#if UE_TRACE_ENABLED
	GTraceAuxiliary.EnableChannels();
#endif
}

////////////////////////////////////////////////////////////////////////////////
void FTraceAuxiliary::TryAutoConnect()
{
#if UE_TRACE_ENABLED
	// Do not attempt to autoconnect when forking is requested.
	const bool bShouldAutoConnect = !FForkProcessHelper::IsForkRequested();
	if (bShouldAutoConnect)
	{
	#if PLATFORM_WINDOWS
	// If we can detect a named event it means UnrealInsights (Browser Mode) is running.
	// In this case, we try to auto-connect with the Trace Server.
		HANDLE KnownEvent = ::OpenEvent(EVENT_ALL_ACCESS, false, TEXT("Local\\UnrealInsightsBrowser"));
		if (KnownEvent != nullptr)
		{
			Start(EConnectionType::Network, TEXT("127.0.0.1"), nullptr, nullptr);
			::CloseHandle(KnownEvent);
		}
	#endif
	}
#endif
}
