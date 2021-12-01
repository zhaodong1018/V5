// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rigs/RigHierarchyElements.h"
#include "Rigs/RigHierarchy.h"
#include "Units/RigUnitContext.h"
#include "ControlRigObjectVersion.h"
#include "ControlRigGizmoLibrary.h"

////////////////////////////////////////////////////////////////////////////////
// FRigBaseElement
////////////////////////////////////////////////////////////////////////////////

UScriptStruct* FRigBaseElement::GetElementStruct() const
{
	switch(GetType())
	{
		case ERigElementType::Bone:
		{
			return FRigBoneElement::StaticStruct();
		}
		case ERigElementType::Null:
		{
			return FRigNullElement::StaticStruct();
		}
		case ERigElementType::Control:
		{
			return FRigControlElement::StaticStruct();
		}
		case ERigElementType::Curve:
		{
			return FRigCurveElement::StaticStruct();
		}
		case ERigElementType::Reference:
		{
			return FRigReferenceElement::StaticStruct();
		}
		case ERigElementType::RigidBody:
		{
			return FRigRigidBodyElement::StaticStruct();
		}
		default:
		{
				break;
		}
	}
	return FRigBaseElement::StaticStruct();
}

void FRigBaseElement::Serialize(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	Ar.UsingCustomVersion(FControlRigObjectVersion::GUID);

	if (Ar.IsSaving() || Ar.IsObjectReferenceCollector() || Ar.IsCountingMemory())
	{
		Save(Ar, Hierarchy, SerializationPhase);
	}
	else if (Ar.IsLoading())
	{
		Load(Ar, Hierarchy, SerializationPhase);
	}
	else
	{
		// remove due to FPIEFixupSerializer hitting this checkNoEntry();
	}
}

void FRigBaseElement::Save(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	if(SerializationPhase == ESerializationPhase::StaticData)
	{
		Ar << Key;
	}
}

void FRigBaseElement::Load(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	if(SerializationPhase == ESerializationPhase::StaticData)
	{
		FRigElementKey LoadedKey;
	
		Ar << LoadedKey;

		ensure(LoadedKey.Type == Key.Type);
		Key = LoadedKey;

		NameString = Key.Name.ToString();
	}
}

////////////////////////////////////////////////////////////////////////////////
// FRigComputedTransform
////////////////////////////////////////////////////////////////////////////////

void FRigComputedTransform::Save(FArchive& Ar)
{
	Ar << Transform;
	Ar << bDirty;
}

void FRigComputedTransform::Load(FArchive& Ar)
{
	// load and save are identical
	Save(Ar);
}

////////////////////////////////////////////////////////////////////////////////
// FRigLocalAndGlobalTransform
////////////////////////////////////////////////////////////////////////////////

void FRigLocalAndGlobalTransform::Save(FArchive& Ar)
{
	Local.Save(Ar);
	Global.Save(Ar);
}

void FRigLocalAndGlobalTransform::Load(FArchive& Ar)
{
	Local.Load(Ar);
	Global.Load(Ar);
}

////////////////////////////////////////////////////////////////////////////////
// FRigCurrentAndInitialTransform
////////////////////////////////////////////////////////////////////////////////

void FRigCurrentAndInitialTransform::Save(FArchive& Ar)
{
	Current.Save(Ar);
	Initial.Save(Ar);
}

void FRigCurrentAndInitialTransform::Load(FArchive& Ar)
{
	Current.Load(Ar);
	Initial.Load(Ar);
}

////////////////////////////////////////////////////////////////////////////////
// FRigTransformElement
////////////////////////////////////////////////////////////////////////////////

void FRigTransformElement::Save(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	Super::Save(Ar, Hierarchy, SerializationPhase);

	if(SerializationPhase == ESerializationPhase::StaticData)
	{
		Pose.Save(Ar);
	}
}

void FRigTransformElement::Load(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	Super::Load(Ar, Hierarchy, SerializationPhase);

	if(SerializationPhase == ESerializationPhase::StaticData)
	{
		Pose.Load(Ar);
	}
}

void FRigTransformElement::CopyPose(FRigBaseElement* InOther, bool bCurrent, bool bInitial)
{
	Super::CopyPose(InOther, bCurrent, bInitial);

	if(FRigTransformElement* Other = Cast<FRigTransformElement>(InOther))
	{
		if(bCurrent)
		{
			Pose.Current = Other->Pose.Current;
		}
		if(bInitial)
		{
			Pose.Initial = Other->Pose.Initial;
		}
	}
}

void FRigTransformElement::CopyFrom(URigHierarchy* InHierarchy, FRigBaseElement* InOther,
	URigHierarchy* InOtherHierarchy)
{
	Super::CopyFrom(InHierarchy, InOther, InOtherHierarchy);
	
	const FRigTransformElement* SourceTransform = CastChecked<FRigTransformElement>(InOther);
	Pose = SourceTransform->Pose;

	ElementsToDirty.Reset();
	ElementsToDirty.Reserve(SourceTransform->ElementsToDirty.Num());
	
	for(int32 ElementToDirtyIndex = 0; ElementToDirtyIndex < SourceTransform->ElementsToDirty.Num(); ElementToDirtyIndex++)
	{
		const FElementToDirty& Source = SourceTransform->ElementsToDirty[ElementToDirtyIndex];
		FRigTransformElement* TargetTransform = CastChecked<FRigTransformElement>(InHierarchy->Get(Source.Element->Index));
		const FElementToDirty Target(TargetTransform, Source.HierarchyDistance);
		ElementsToDirty.Add(Target);
		check(ElementsToDirty[ElementToDirtyIndex].Element->GetKey() == Source.Element->GetKey());
	}
}

////////////////////////////////////////////////////////////////////////////////
// FRigSingleParentElement
////////////////////////////////////////////////////////////////////////////////

void FRigSingleParentElement::Save(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	Super::Save(Ar, Hierarchy, SerializationPhase);

	if(SerializationPhase == ESerializationPhase::InterElementData)
	{
		FRigElementKey ParentKey;
		if(ParentElement)
		{
			ParentKey = ParentElement->GetKey();
		}
		Ar << ParentKey;
	}
}

void FRigSingleParentElement::Load(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	Super::Load(Ar, Hierarchy, SerializationPhase);

	if(SerializationPhase == ESerializationPhase::InterElementData)
	{
		FRigElementKey ParentKey;
		Ar << ParentKey;

		if(ParentKey.IsValid())
		{
			ParentElement = Hierarchy->FindChecked<FRigTransformElement>(ParentKey);
		}
	}
}

void FRigSingleParentElement::CopyFrom(URigHierarchy* InHierarchy, FRigBaseElement* InOther,
	URigHierarchy* InOtherHierarchy)
{
	Super::CopyFrom(InHierarchy, InOther, InOtherHierarchy);

	const FRigSingleParentElement* Source = CastChecked<FRigSingleParentElement>(InOther); 
	if(Source->ParentElement)
	{
		ParentElement = CastChecked<FRigTransformElement>(InHierarchy->Get(Source->ParentElement->Index));
		check(ParentElement->GetKey() == Source->ParentElement->GetKey());
	}
	else
	{
		ParentElement = nullptr;
	}
}

////////////////////////////////////////////////////////////////////////////////
// FRigMultiParentElement
////////////////////////////////////////////////////////////////////////////////

void FRigMultiParentElement::Save(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	Super::Save(Ar, Hierarchy, SerializationPhase);

	if(SerializationPhase == ESerializationPhase::StaticData)
	{
		Parent.Save(Ar);

		int32 NumParents = ParentConstraints.Num();
		Ar << NumParents;
	}
	else if(SerializationPhase == ESerializationPhase::InterElementData)
	{
		for(int32 ParentIndex = 0; ParentIndex < ParentConstraints.Num(); ParentIndex++)
		{
			FRigElementKey ParentKey;
			if(ParentConstraints[ParentIndex].ParentElement)
			{
				ParentKey = ParentConstraints[ParentIndex].ParentElement->GetKey();
			}

			Ar << ParentKey;
			Ar << ParentConstraints[ParentIndex].InitialWeight;
			Ar << ParentConstraints[ParentIndex].Weight;
		}
	}
}

void FRigMultiParentElement::Load(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	Super::Load(Ar, Hierarchy, SerializationPhase);

	if(SerializationPhase == ESerializationPhase::StaticData)
	{
		Parent.Load(Ar);

		int32 NumParents = 0;
		Ar << NumParents;

		ParentConstraints.SetNum(NumParents);
	}
	else if(SerializationPhase == ESerializationPhase::InterElementData)
	{
		for(int32 ParentIndex = 0; ParentIndex < ParentConstraints.Num(); ParentIndex++)
		{
			FRigElementKey ParentKey;
			Ar << ParentKey;
			ensure(ParentKey.IsValid());

			ParentConstraints[ParentIndex].ParentElement = Hierarchy->FindChecked<FRigTransformElement>(ParentKey);
			ParentConstraints[ParentIndex].Cache.bDirty = true;

			if (Ar.CustomVer(FControlRigObjectVersion::GUID) >= FControlRigObjectVersion::RigHierarchyMultiParentConstraints)
			{
				Ar << ParentConstraints[ParentIndex].InitialWeight;
				Ar << ParentConstraints[ParentIndex].Weight;
			}
			else
			{
				float InitialWeight = 0.f;
				Ar << InitialWeight;
				ParentConstraints[ParentIndex].InitialWeight = FRigElementWeight(InitialWeight);

				float Weight = 0.f;
				Ar << Weight;
				ParentConstraints[ParentIndex].Weight = FRigElementWeight(Weight);
			}

			IndexLookup.Add(ParentKey, ParentIndex);
		}
	}
}

void FRigMultiParentElement::CopyFrom(URigHierarchy* InHierarchy, FRigBaseElement* InOther,
                                      URigHierarchy* InOtherHierarchy)
{
	Super::CopyFrom(InHierarchy, InOther, InOtherHierarchy);
	
	const FRigMultiParentElement* Source = CastChecked<FRigMultiParentElement>(InOther);
	Parent = Source->Parent;
	ParentConstraints.Reset();
	ParentConstraints.Reserve(Source->ParentConstraints.Num());
	IndexLookup.Reset();
	IndexLookup.Reserve(Source->IndexLookup.Num());

	for(int32 ParentIndex = 0; ParentIndex < Source->ParentConstraints.Num(); ParentIndex++)
	{
		FRigElementParentConstraint ParentConstraint = Source->ParentConstraints[ParentIndex];
		const FRigTransformElement* SourceParentElement = ParentConstraint.ParentElement;
		ParentConstraint.ParentElement = CastChecked<FRigTransformElement>(InHierarchy->Get(SourceParentElement->Index));
		ParentConstraints.Add(ParentConstraint);
		check(ParentConstraints[ParentIndex].ParentElement->GetKey() == SourceParentElement->GetKey());
		IndexLookup.Add(ParentConstraint.ParentElement->GetKey(), ParentIndex);
	}
}

void FRigMultiParentElement::CopyPose(FRigBaseElement* InOther, bool bCurrent, bool bInitial)
{
	Super::CopyPose(InOther, bCurrent, bInitial);

	if(FRigMultiParentElement* Other = Cast<FRigMultiParentElement>(InOther))
	{
		if(bCurrent)
		{
			Parent.Current = Other->Parent.Current;
		}
		if(bInitial)
		{
			Parent.Initial = Other->Parent.Initial;
		}
	}
}

////////////////////////////////////////////////////////////////////////////////
// FRigBoneElement
////////////////////////////////////////////////////////////////////////////////

void FRigBoneElement::Save(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	Super::Save(Ar, Hierarchy, SerializationPhase);

	if(SerializationPhase == ESerializationPhase::StaticData)
	{
		static const UEnum* BoneTypeEnum = StaticEnum<ERigBoneType>();
		FName TypeName = BoneTypeEnum->GetNameByValue((int64)BoneType);
		Ar << TypeName;
	}
}

void FRigBoneElement::Load(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	Super::Load(Ar, Hierarchy, SerializationPhase);

	if(SerializationPhase == ESerializationPhase::StaticData)
	{
		static const UEnum* BoneTypeEnum = StaticEnum<ERigBoneType>();
		FName TypeName;
		Ar << TypeName;
		BoneType = (ERigBoneType)BoneTypeEnum->GetValueByName(TypeName);
	}
}

void FRigBoneElement::CopyFrom(URigHierarchy* InHierarchy, FRigBaseElement* InOther, URigHierarchy* InOtherHierarchy)
{
	Super::CopyFrom(InHierarchy, InOther, InOtherHierarchy);
	
	const FRigBoneElement* Source = CastChecked<FRigBoneElement>(InOther);
	BoneType = Source->BoneType;
}

////////////////////////////////////////////////////////////////////////////////
// FRigControlSettings
////////////////////////////////////////////////////////////////////////////////

FRigControlSettings::FRigControlSettings()
: ControlType(ERigControlType::Transform)
, DisplayName(NAME_None)
, PrimaryAxis(ERigControlAxis::X)
, bIsCurve(false)
, bAnimatable(true)
, bLimitTranslation(false)
, bLimitRotation(false)
, bLimitScale(false)
, bDrawLimits(true)
, MinimumValue()
, MaximumValue()
, bShapeEnabled(true)
, bShapeVisible(true)
, ShapeName(NAME_None)
, ShapeColor(FLinearColor::Red)
, bIsTransientControl(false)
, ControlEnum(nullptr)
, Customization()
{
	// rely on the default provided by the shape definition
	ShapeName = FControlRigShapeDefinition().ShapeName; 
}

void FRigControlSettings::Save(FArchive& Ar)
{
	Ar.UsingCustomVersion(FControlRigObjectVersion::GUID);

	static const UEnum* ControlTypeEnum = StaticEnum<ERigControlType>();
	static const UEnum* ControlAxisEnum = StaticEnum<ERigControlAxis>();

	FName ControlTypeName = ControlTypeEnum->GetNameByValue((int64)ControlType);
	FName PrimaryAxisName = ControlAxisEnum->GetNameByValue((int64)PrimaryAxis);

	FString ControlEnumPathName;
	if(ControlEnum)
	{
		ControlEnumPathName = ControlEnum->GetPathName();
	}

	Ar << ControlTypeName;
	Ar << DisplayName;
	Ar << PrimaryAxisName;
	Ar << bIsCurve;
	Ar << bAnimatable;
	Ar << bLimitTranslation;
	Ar << bLimitRotation;
	Ar << bLimitScale;
	Ar << bDrawLimits;
	Ar << MinimumValue;
	Ar << MaximumValue;
	Ar << bShapeEnabled;
	Ar << bShapeVisible;
	Ar << ShapeName;
	Ar << ShapeColor;
	Ar << bIsTransientControl;
	Ar << ControlEnumPathName;
	Ar << Customization.AvailableSpaces;
}

void FRigControlSettings::Load(FArchive& Ar)
{
	Ar.UsingCustomVersion(FControlRigObjectVersion::GUID);

	static const UEnum* ControlTypeEnum = StaticEnum<ERigControlType>();
	static const UEnum* ControlAxisEnum = StaticEnum<ERigControlAxis>();

	FName ControlTypeName, PrimaryAxisName;
	FString ControlEnumPathName;

	Ar << ControlTypeName;
	Ar << DisplayName;
	Ar << PrimaryAxisName;
	Ar << bIsCurve;
	Ar << bAnimatable;
	Ar << bLimitTranslation;
	Ar << bLimitRotation;
	Ar << bLimitScale;
	Ar << bDrawLimits;

	FTransform MinimumTransform, MaximumTransform;
	if (Ar.CustomVer(FControlRigObjectVersion::GUID) >= FControlRigObjectVersion::StorageMinMaxValuesAsFloatStorage)
	{
		Ar << MinimumValue;
		Ar << MaximumValue;
	}
	else
	{
		Ar << MinimumTransform;
		Ar << MaximumTransform;
	}

	Ar << bShapeEnabled;
	Ar << bShapeVisible;
	Ar << ShapeName;
	Ar << ShapeColor;
	Ar << bIsTransientControl;
	Ar << ControlEnumPathName;

	ControlType = (ERigControlType)ControlTypeEnum->GetValueByName(ControlTypeName);
	PrimaryAxis = (ERigControlAxis)ControlAxisEnum->GetValueByName(PrimaryAxisName);

	if (Ar.CustomVer(FControlRigObjectVersion::GUID) < FControlRigObjectVersion::StorageMinMaxValuesAsFloatStorage)
	{
		MinimumValue.SetFromTransform(MinimumTransform, ControlType, PrimaryAxis);
		MaximumValue.SetFromTransform(MaximumTransform, ControlType, PrimaryAxis);
	}

	ControlEnum = nullptr;
	if(!ControlEnumPathName.IsEmpty())
	{
		ControlEnum = FindObject<UEnum>(ANY_PACKAGE, *ControlEnumPathName);
	}

	if (Ar.CustomVer(FControlRigObjectVersion::GUID) >= FControlRigObjectVersion::RigHierarchyControlSpaceFavorites)
	{
		Ar << Customization.AvailableSpaces;
	}
	else
	{
		Customization.AvailableSpaces.Reset();
	}
}

////////////////////////////////////////////////////////////////////////////////
// FRigControlElement
////////////////////////////////////////////////////////////////////////////////

bool FRigControlSettings::operator==(const FRigControlSettings& InOther) const
{
	if(ControlType != InOther.ControlType)
	{
		return false;
	}
	if(DisplayName != InOther.DisplayName)
	{
		return false;
	}
	if(PrimaryAxis != InOther.PrimaryAxis)
	{
		return false;
	}
	if(bIsCurve != InOther.bIsCurve)
	{
		return false;
	}
	if(bAnimatable != InOther.bAnimatable)
	{
		return false;
	}
	if(bLimitTranslation != InOther.bLimitTranslation)
	{
		return false;
	}
	if(bLimitRotation != InOther.bLimitRotation)
	{
		return false;
	}
	if(bLimitScale != InOther.bLimitScale)
	{
		return false;
	}
	if(bDrawLimits != InOther.bDrawLimits)
	{
		return false;
	}
	if(bShapeEnabled != InOther.bShapeEnabled)
	{
		return false;
	}
	if(bShapeVisible != InOther.bShapeVisible)
	{
		return false;
	}
	if(ShapeName != InOther.ShapeName)
	{
		return false;
	}
	if(bIsTransientControl != InOther.bIsTransientControl)
	{
		return false;
	}
	if( ControlEnum != InOther. ControlEnum)
	{
		return false;
	}
	if(!ShapeColor.Equals(InOther.ShapeColor, 0.001))
	{
		return false;
	}
	if(Customization.AvailableSpaces != InOther.Customization.AvailableSpaces)
	{
		return false;
	}

	const FTransform MinimumTransform = MinimumValue.GetAsTransform(ControlType, PrimaryAxis);
	const FTransform OtherMinimumTransform = InOther.MinimumValue.GetAsTransform(ControlType, PrimaryAxis);
	if(!MinimumTransform.Equals(OtherMinimumTransform, 0.001))
	{
		return false;
	}

	const FTransform MaximumTransform = MaximumValue.GetAsTransform(ControlType, PrimaryAxis);
	const FTransform OtherMaximumTransform = InOther.MaximumValue.GetAsTransform(ControlType, PrimaryAxis);
	if(!MaximumTransform.Equals(OtherMaximumTransform, 0.001))
	{
		return false;
	}

	return true;
}

void FRigControlElement::Save(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	Super::Save(Ar, Hierarchy, SerializationPhase);

	if(SerializationPhase == ESerializationPhase::StaticData)
	{
		Settings.Save(Ar);
		Offset.Save(Ar);
		Shape.Save(Ar);
	}
}

void FRigControlElement::Load(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	Super::Load(Ar, Hierarchy, SerializationPhase);

	if(SerializationPhase == ESerializationPhase::StaticData)
	{
		Settings.Load(Ar);
		Offset.Load(Ar);
		Shape.Load(Ar);
	}
}

void FRigControlElement::CopyFrom(URigHierarchy* InHierarchy, FRigBaseElement* InOther, URigHierarchy* InOtherHierarchy)
{
	Super::CopyFrom(InHierarchy, InOther, InOtherHierarchy);
	
	const FRigControlElement* Source = CastChecked<FRigControlElement>(InOther);
	Settings = Source->Settings;
	Offset = Source->Offset;
	Shape = Source->Shape;
}

void FRigControlElement::CopyPose(FRigBaseElement* InOther, bool bCurrent, bool bInitial)
{
	Super::CopyPose(InOther, bCurrent, bInitial);
	
	if(FRigControlElement* Other = Cast<FRigControlElement>(InOther))
	{
		if(bCurrent)
		{
			Offset.Current = Other->Offset.Current;
			Shape.Current = Other->Shape.Current;
		}
		if(bInitial)
		{
			Offset.Initial = Other->Offset.Initial;
			Shape.Initial = Other->Shape.Initial;
		}
	}
}

////////////////////////////////////////////////////////////////////////////////
// FRigCurveElement
////////////////////////////////////////////////////////////////////////////////

void FRigCurveElement::Save(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	Super::Save(Ar, Hierarchy, SerializationPhase);

	if(SerializationPhase == ESerializationPhase::StaticData)
	{
		Ar << Value;
	}
}

void FRigCurveElement::Load(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	Super::Load(Ar, Hierarchy, SerializationPhase);

	if(SerializationPhase == ESerializationPhase::StaticData)
	{
		Ar << Value;
	}
}

void FRigCurveElement::CopyPose(FRigBaseElement* InOther, bool bCurrent, bool bInitial)
{
	Super::CopyPose(InOther, bCurrent, bInitial);
	
	if(FRigCurveElement* Other = Cast<FRigCurveElement>(InOther))
	{
		Value = Other->Value;
	}
}

void FRigCurveElement::CopyFrom(URigHierarchy* InHierarchy, FRigBaseElement* InOther, URigHierarchy* InOtherHierarchy)
{
	Super::CopyFrom(InHierarchy, InOther, InOtherHierarchy);
	Value = CastChecked<FRigCurveElement>(InOther)->Value;
}

////////////////////////////////////////////////////////////////////////////////
// FRigRigidBodySettings
////////////////////////////////////////////////////////////////////////////////

FRigRigidBodySettings::FRigRigidBodySettings()
	: Mass(1.f)
{
}

void FRigRigidBodySettings::Save(FArchive& Ar)
{
	Ar << Mass;
}

void FRigRigidBodySettings::Load(FArchive& Ar)
{
	Ar << Mass;
}

////////////////////////////////////////////////////////////////////////////////
// FRigRigidBodyElement
////////////////////////////////////////////////////////////////////////////////

void FRigRigidBodyElement::Save(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	Super::Save(Ar, Hierarchy, SerializationPhase);

	if(SerializationPhase == ESerializationPhase::StaticData)
	{
		Settings.Save(Ar);
	}
}

void FRigRigidBodyElement::Load(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	Super::Load(Ar, Hierarchy, SerializationPhase);

	if(SerializationPhase == ESerializationPhase::StaticData)
	{
		Settings.Load(Ar);
	}
}

void FRigRigidBodyElement::CopyFrom(URigHierarchy* InHierarchy, FRigBaseElement* InOther,
	URigHierarchy* InOtherHierarchy)
{
	Super::CopyFrom(InHierarchy, InOther, InOtherHierarchy);
	
	const FRigRigidBodyElement* Source = CastChecked<FRigRigidBodyElement>(InOther);
	Settings = Source->Settings;
}

////////////////////////////////////////////////////////////////////////////////
// FRigReferenceElement
////////////////////////////////////////////////////////////////////////////////

void FRigReferenceElement::Save(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	Super::Save(Ar, Hierarchy, SerializationPhase);
}

void FRigReferenceElement::Load(FArchive& Ar, URigHierarchy* Hierarchy, ESerializationPhase SerializationPhase)
{
	Super::Load(Ar, Hierarchy, SerializationPhase);
}

void FRigReferenceElement::CopyFrom(URigHierarchy* InHierarchy, FRigBaseElement* InOther, URigHierarchy* InOtherHierarchy)
{
	Super::CopyFrom(InHierarchy, InOther, InOtherHierarchy);
	
	const FRigReferenceElement* Source = CastChecked<FRigReferenceElement>(InOther);
	GetWorldTransformDelegate = Source->GetWorldTransformDelegate;
}

FTransform FRigReferenceElement::GetReferenceWorldTransform(const FRigUnitContext* InContext, bool bInitial) const
{
	if(GetWorldTransformDelegate.IsBound())
	{
		return GetWorldTransformDelegate.Execute(InContext, GetKey(), bInitial);
	}
	return FTransform::Identity;
}

void FRigReferenceElement::CopyPose(FRigBaseElement* InOther, bool bCurrent, bool bInitial)
{
	Super::CopyPose(InOther, bCurrent, bInitial);
	
	if(FRigReferenceElement* Other = Cast<FRigReferenceElement>(InOther))
	{
		if(Other->GetWorldTransformDelegate.IsBound())
		{
			GetWorldTransformDelegate = Other->GetWorldTransformDelegate;
		}
	}
}
