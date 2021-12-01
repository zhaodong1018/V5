// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once
#include "TraceServices/Model/AnalysisSession.h"
#include "Templates/UnrealTemplate.h"
#include <atomic>

namespace TraceServices {

////////////////////////////////////////////////////////////////////////////////
/**
  * Result of a query. Since symbol resolving can be deferred this signals if a
  * symbol has been resolved, waiting to be resolved or wasn't found at all.
  */
enum class ESymbolQueryResult : uint8
{
	Pending,		// Symbol is pending resolution
	OK,			// Symbol has been correctly resolved
	NotLoaded,	// Module debug data could not be loaded or found
	Mismatch,	// Module debug data could not be loaded because debug data did not match traced binary
	NotFound,	// Symbol was not found in module debug data
	StatusNum
};

////////////////////////////////////////////////////////////////////////////////
/**
 * Helper method to get a string representation of the query result.
 */
inline const TCHAR* QueryResultToString(ESymbolQueryResult Result)
{
	static const TCHAR* DisplayStrings[] = {
		TEXT("Pending..."),
		TEXT("Ok"),
		TEXT("Not loaded"),
		TEXT("Version mismatch"),
		TEXT("Not found")
	};
	static_assert(UE_ARRAY_COUNT(DisplayStrings) == (uint8) ESymbolQueryResult::StatusNum, "Missing QueryResult");
	return DisplayStrings[(uint8)Result];
}

////////////////////////////////////////////////////////////////////////////////
/**
  * Represent a resolved symbol. The resolve status and string values may change
  * over time, but string pointers returned from the methods are guaranteed to live 
  * during the entire analysis session.
  */
struct FResolvedSymbol
{
	std::atomic<ESymbolQueryResult> Result;
	const TCHAR* Module;
	const TCHAR* Name;
	const TCHAR* File;
	uint16 Line;
	
	inline ESymbolQueryResult GetResult() const
	{
		return Result.load(std::memory_order_acquire);
	}

	FResolvedSymbol(ESymbolQueryResult InResult, const TCHAR* InModule, const TCHAR* InName, const TCHAR* InFile, uint16 InLine) 
		: Result(InResult)
		, Module(InModule)
		, Name(InName)
		, File(InFile)
		, Line(InLine)
	{}
};

////////////////////////////////////////////////////////////////////////////////
class IModuleProvider : public IProvider
{
public:
	struct FStats
	{
		uint32 ModulesDiscovered;
		uint32 ModulesLoaded;
		uint32 ModulesFailed;
		uint32 SymbolsDiscovered;
		uint32 SymbolsResolved;
		uint32 SymbolsFailed;
	};

	virtual ~IModuleProvider() = default;

	/** Queries the name of the symbol at address. This function returns immedately, 
	 * but the lookup is async. See \ref FResolvedSymbol for details. It assumed that 
	 * all calls to this function happens before analysis has ended.
	 */
	virtual const FResolvedSymbol* GetSymbol(uint64 Address) = 0;

	/** Gets statistics from provider */
	virtual void GetStats(FStats* OutStats) const = 0;
};

////////////////////////////////////////////////////////////////////////////////

TRACESERVICES_API FName GetModuleProviderName();
TRACESERVICES_API const IModuleProvider* ReadModuleProvider(const IAnalysisSession& Session);

} // namespace TraceServices
