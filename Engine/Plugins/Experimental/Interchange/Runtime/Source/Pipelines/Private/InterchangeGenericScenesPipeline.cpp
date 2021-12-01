// Copyright Epic Games, Inc. All Rights Reserved. 

#include "InterchangeGenericScenesPipeline.h"

#include "InterchangeActorFactoryNode.h"
#include "InterchangeCameraNode.h"
#include "InterchangeCineCameraFactoryNode.h"
#include "InterchangeLightNode.h"
#include "InterchangeMeshNode.h"
#include "InterchangePipelineLog.h"
#include "InterchangeSceneNode.h"

#include "Animation/SkeletalMeshActor.h"
#include "CineCameraActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/RectLight.h"
#include "Engine/SpotLight.h"
#include "Engine/StaticMeshActor.h"

bool UInterchangeGenericLevelPipeline::ExecutePreImportPipeline(UInterchangeBaseNodeContainer* InBaseNodeContainer, const TArray<UInterchangeSourceData*>& InSourceDatas)
{
	if (!InBaseNodeContainer)
	{
		UE_LOG(LogInterchangePipeline, Warning, TEXT("UInterchangeGenericAssetsPipeline: Cannot execute pre-import pipeline because InBaseNodeContrainer is null"));
		return false;
	}

	TArray<UInterchangeSceneNode*> SceneNodes;

	//Find all translated node we need for this pipeline
	InBaseNodeContainer->IterateNodes([&SceneNodes](const FString& NodeUid, UInterchangeBaseNode* Node)
	{
		switch(Node->GetNodeContainerType())
		{
		case EInterchangeNodeContainerType::NodeContainerType_TranslatedScene:
		{
			if (UInterchangeSceneNode* SceneNode = Cast<UInterchangeSceneNode>(Node))
			{
				SceneNodes.Add(SceneNode);
			}
		}
		break;
		}
	});

	for (const UInterchangeSceneNode* SceneNode : SceneNodes)
	{
		//Ignore specialized types for now as they are used for bones hierarchies and other asset internal data
		if (SceneNode && SceneNode->GetSpecializedTypeCount() == 0)
		{
			CreateActorFactoryNode(SceneNode, InBaseNodeContainer);
		}
	}

	return true;
}

void UInterchangeGenericLevelPipeline::CreateActorFactoryNode(const UInterchangeSceneNode* SceneNode, UInterchangeBaseNodeContainer* FactoryNodeContainer)
{
	if (!SceneNode)
	{
		return;
	}

	UInterchangeActorFactoryNode* ActorFactoryNode = nullptr;
	const UInterchangeBaseNode* TranslatedAssetNode = nullptr;

	FString AssetInstanceUid;
	if (SceneNode->GetCustomAssetInstanceUid(AssetInstanceUid))
	{
		TranslatedAssetNode = FactoryNodeContainer->GetNode(AssetInstanceUid);
	}

	if (TranslatedAssetNode && TranslatedAssetNode->IsA<UInterchangeCameraNode>())
	{
		ActorFactoryNode = NewObject<UInterchangeCineCameraFactoryNode>(FactoryNodeContainer, NAME_None);
	}
	else
	{
		ActorFactoryNode = NewObject<UInterchangeActorFactoryNode>(FactoryNodeContainer, NAME_None);
	}

	if (!ensure(ActorFactoryNode))
	{
		return;
	}

	ActorFactoryNode->InitializeNode(TEXT("Factory_") + SceneNode->GetUniqueID(), SceneNode->GetDisplayLabel(), EInterchangeNodeContainerType::NodeContainerType_FactoryData);

	if (!SceneNode->GetParentUid().IsEmpty())
	{
		ActorFactoryNode->SetParentUid(TEXT("Factory_") + SceneNode->GetParentUid());
	}

	ActorFactoryNode->AddTargetNodeUid(SceneNode->GetUniqueID());

	FTransform GlobalTransform;
	if (SceneNode->GetCustomGlobalTransform(GlobalTransform))
	{
		ActorFactoryNode->SetCustomGlobalTransform(GlobalTransform);
	}
	
	ActorFactoryNode->SetCustomMobility(EComponentMobility::Static);

	if (TranslatedAssetNode)
	{
		if (const UInterchangeMeshNode* MeshNode = Cast<UInterchangeMeshNode>(TranslatedAssetNode))
		{
			if (MeshNode->IsSkinnedMesh())
			{
				ActorFactoryNode->SetCustomActorClassName(ASkeletalMeshActor::StaticClass()->GetPathName());
				ActorFactoryNode->SetCustomMobility(EComponentMobility::Movable);
			}
			else
			{
				ActorFactoryNode->SetCustomActorClassName(AStaticMeshActor::StaticClass()->GetPathName());
			}
		}
		else if (const UInterchangeLightNode* LightNode = Cast<UInterchangeLightNode>(TranslatedAssetNode))
		{
			//Test for spot before point since a spot light is a point light
			if (LightNode->IsA<UInterchangeSpotLightNode>())
			{
				ActorFactoryNode->SetCustomActorClassName(ASpotLight::StaticClass()->GetPathName());
			}
			else if (LightNode->IsA<UInterchangePointLightNode>())
			{
				ActorFactoryNode->SetCustomActorClassName(APointLight::StaticClass()->GetPathName());
			} 
			else if (LightNode->IsA<UInterchangeRectLightNode>())
			{
				ActorFactoryNode->SetCustomActorClassName(ARectLight::StaticClass()->GetPathName());
			}
			else if (LightNode->IsA<UInterchangeDirectionalLightNode>())
			{
				ActorFactoryNode->SetCustomActorClassName(ADirectionalLight::StaticClass()->GetPathName());
			}
			else
			{
				ActorFactoryNode->SetCustomActorClassName(APointLight::StaticClass()->GetPathName());
			}
		}
		else if (const UInterchangeCameraNode* CameraNode = Cast<UInterchangeCameraNode>(TranslatedAssetNode))
		{
			ActorFactoryNode->SetCustomActorClassName(ACineCameraActor::StaticClass()->GetPathName());
			ActorFactoryNode->SetCustomMobility(EComponentMobility::Movable);

			if (UInterchangeCineCameraFactoryNode* CineCameraFactoryNode = Cast<UInterchangeCineCameraFactoryNode>(ActorFactoryNode))
			{
				float FocalLength;
				if (CameraNode->GetCustomFocalLength(FocalLength))
				{
					CineCameraFactoryNode->SetCustomFocalLength(FocalLength);
				}
				
				float SensorHeight;
				if (CameraNode->GetCustomSensorHeight(SensorHeight))
				{
					CineCameraFactoryNode->SetCustomSensorHeight(SensorHeight);
				}

				float SensorWidth;
				if (CameraNode->GetCustomSensorWidth(SensorWidth))
				{
					CineCameraFactoryNode->SetCustomSensorWidth(SensorWidth);
				}
			}
		}
	}

	FactoryNodeContainer->AddNode(ActorFactoryNode);
}

