// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "HAL/Platform.h"

////////////////////////////////////////////////////////////////////////////////
class CORE_API FTraceAuxiliary
{
public:

	enum class EConnectionType
	{
		/**
		 * Connect to a trace server. Target is IP address or hostname.
		 */
		Network,
		/**
		 * Write to a file. Target string is filename. Absolute or relative current working directory.
		 * If target is null the current date and time is used.
		 */
		File,
		/**
		 * Don't connect, just start tracing to memory.
		 */
		None,
	};

	struct Options
	{
		/** When set trace will not start a worker thread, instead it is updated from end frame delegate. */
		bool bNoWorkerThread = false;
		/** When set the target file will be truncated if it already exists */ 
		bool bTruncateFile = false;
	};
	
	/**
	 * Start tracing to a target (network connection or file) with an active set of channels. If a connection is
	 * already active this call does nothing.
	 * @param Type Type of connection
	 * @param Target String to use for connection. See /ref EConnectionType for details.
	 * @param Channels Comma separated list of channels to enable. If the pointer is null the default channels will be active.
	 * @param Options Optional additional tracing options.
	 * @return True when successfully starting the trace, false if the data connection could not be made.
	 */
	static bool Start(EConnectionType Type, const TCHAR* Target, const TCHAR* Channels, Options* Options = nullptr);

	/**
	 * Stop tracing.
	 * @return True if the trace was stopped, false if there was no data connection.
	 */
	static bool Stop();

	/**
	 * Pause all tracing by disabling all active channels.
	 */
	static bool Pause();

	/**
	 * Resume tracing by enabling all previously active channels.
	 */
	static bool Resume();

	/**
	 * Initialize Trace systems.
	 * @param CommandLine to use for initializing
	 */
	static void Initialize(const TCHAR* CommandLine);
	
	/**
	 * Initialize channels that use the config driven presets.
	 * @param CommandLine to use for initializing
	 */
	static void InitializePresets(const TCHAR* CommandLine);
	
	/**
	 * Shut down Trace systems.
	 */
	static void Shutdown();

	/**
	 * Attempts to auto connect to an active trace server if an active session
	 * of Unreal Insights Session Browser is running.
	 */
	static void TryAutoConnect();

	/**
	 *  Enable previously selected channels. This method can be called multiple times
	 *  as channels can be announced on module loading.
	 */
	static void EnableChannels();
};

