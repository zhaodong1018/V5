// Copyright Epic Games, Inc. All Rights Reserved.

#include "WaterBodyCustomComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "WaterSubsystem.h"

#if WITH_EDITOR
#include "WaterIconHelper.h"
#endif

// ----------------------------------------------------------------------------------

UWaterBodyCustomComponent::UWaterBodyCustomComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bAffectsLandscape = false;

	// @todo_water : Remove these checks (Once AWaterBody is no more Blueprintable, these methods should become PURE_VIRTUAL and this class should overload them)
	check(!IsFlatSurface());
	check(!IsWaterSplineClosedLoop());
	check(!IsHeightOffsetSupported());
}

TArray<UPrimitiveComponent*> UWaterBodyCustomComponent::GetCollisionComponents() const
{
	TArray<UPrimitiveComponent*> Result;
	if ((MeshComp != nullptr) && (MeshComp->GetCollisionEnabled() != ECollisionEnabled::NoCollision))
	{
		Result.Add(MeshComp);
	}
	return Result;
}

TArray<UPrimitiveComponent*> UWaterBodyCustomComponent::GetStandardRenderableComponents() const 
{
	TArray<UPrimitiveComponent*> Result;
	if (MeshComp != nullptr)
	{
		Result.Add(MeshComp);
	}
	return Result;
}

void UWaterBodyCustomComponent::Reset()
{
	AActor* Owner = GetOwner();
	check(Owner);

	TArray<UStaticMeshComponent*> MeshComponents;
	Owner->GetComponents(MeshComponents);

	MeshComp = nullptr;
	for (UStaticMeshComponent* MeshComponent : MeshComponents)
	{
		MeshComponent->DestroyComponent();
	}
}

void UWaterBodyCustomComponent::OnUpdateBody(bool bWithExclusionVolumes)
{
	AActor* OwnerActor = GetOwner();
	check(OwnerActor);

	if (!MeshComp)
	{
		MeshComp = NewObject<UStaticMeshComponent>(OwnerActor, TEXT("CustomMeshComponent"));
		MeshComp->SetNetAddressable(); // it's deterministically named so it's addressable over network (needed for collision)
		MeshComp->SetupAttachment(this);
		MeshComp->SetCollisionProfileName(GetCollisionProfileName());
		// In the case of custom meshes, the static mesh component acts as both collision and visual component so we simply disable collision on it: 
		MeshComp->SetGenerateOverlapEvents(bGenerateCollisions);
		if (!bGenerateCollisions)
		{
			MeshComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		MeshComp->RegisterComponent();
	}

	TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents;
	OwnerActor->GetComponents(PrimitiveComponents);

	// Make no assumptions for custom meshes.  Add all components with collision to the list of collision components
	for (UPrimitiveComponent* Comp : PrimitiveComponents)
	{
		if (bGenerateCollisions && (Comp->GetCollisionEnabled() != ECollisionEnabled::NoCollision))
		{
			// Use value of bFillCollisionUnderWaterBodiesForNavmesh for all components with collisions.
			Comp->bFillCollisionUnderneathForNavmesh = bFillCollisionUnderWaterBodiesForNavmesh;
		}

		Comp->SetMobility(Mobility);
	}

	CreateOrUpdateWaterMID();
	MeshComp->SetStaticMesh(GetWaterMeshOverride());
	MeshComp->SetMaterial(0, WaterMID);
	MeshComp->SetCastShadow(false);
	MeshComp->MarkRenderStateDirty();
}

void UWaterBodyCustomComponent::BeginUpdateWaterBody()
{
	Super::BeginUpdateWaterBody();

	UMaterialInstanceDynamic* WaterMaterialInstance = GetWaterMaterialInstance();
	if (WaterMaterialInstance && MeshComp)
	{
		// We need to get(or create) the water MID at runtime and apply it to the static mesh component 
		// The MID is transient so it will not make it through serialization, apply it here (at runtime)
		MeshComp->SetMaterial(0, WaterMaterialInstance);
	}
}

