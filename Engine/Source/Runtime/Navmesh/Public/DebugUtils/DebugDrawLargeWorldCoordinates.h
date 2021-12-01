// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#define DU_LARGE_WORLD_COORDINATES_DISABLED UE_LARGE_WORLD_COORDINATES_DISABLED

#if DU_LARGE_WORLD_COORDINATES_DISABLED

typedef float duReal;

#else // DU_LARGE_WORLD_COORDINATES_DISABLED

typedef double duReal;

#endif // DU_LARGE_WORLD_COORDINATES_DISABLED