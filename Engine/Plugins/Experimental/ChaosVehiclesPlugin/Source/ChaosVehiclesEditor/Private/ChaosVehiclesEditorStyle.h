// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"

class FChaosVehiclesEditorStyle final : public FSlateStyleSet
{
public:
	FChaosVehiclesEditorStyle() : FSlateStyleSet("ChaosVehiclesEditorStyle")
	{
		const FVector2D Icon16x16(16.f, 16.f);
		const FVector2D Icon64x64(64.f, 64.f);

#if !IS_MONOLITHIC
		SetContentRoot(FPaths::EnginePluginsDir() / TEXT("Experimental/ChaosVehiclesPlugin/Resources"));
#endif

		Set("ClassIcon.ChaosVehicles", new FSlateVectorImageBrush(RootToContentDir(TEXT("ChaosVehicles_16.svg")), Icon16x16));
		Set("ClassThumbnail.ChaosVehicles", new FSlateVectorImageBrush(RootToContentDir(TEXT("ChaosVehicles_64.svg")), Icon64x64));

		FSlateStyleRegistry::RegisterSlateStyle(*this);
	}

	~FChaosVehiclesEditorStyle()
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*this);
	}

public:

	static FChaosVehiclesEditorStyle& Get()
	{
		if (!Singleton.IsSet())
		{
			Singleton.Emplace();
		}
		return Singleton.GetValue();
	}

	static void Destroy()
	{
		Singleton.Reset();
	}

private:
	static TOptional<FChaosVehiclesEditorStyle> Singleton;
};

TOptional<FChaosVehiclesEditorStyle> FChaosVehiclesEditorStyle::Singleton;