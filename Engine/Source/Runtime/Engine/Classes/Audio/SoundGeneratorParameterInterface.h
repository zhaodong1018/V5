// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "AudioParameterInterface.h"
#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "SoundGeneratorParameterInterface.generated.h"


// Forward Declarations
class FAudioDevice;
class USoundBase;
struct FActiveSound;

UINTERFACE(BlueprintType, meta = (CannotImplementInterfaceInBlueprint))
class ENGINE_API USoundGeneratorParameterInterface : public UAudioParameterInterface
{
	GENERATED_UINTERFACE_BODY()
};

class ENGINE_API ISoundGeneratorParameterInterface : public IAudioParameterInterface
{
	GENERATED_IINTERFACE_BODY()

public:
	// IAudioParameterInterface
	void ResetParameters() override;

	void SetTriggerParameter(FName InName) override;
	void SetBoolParameter(FName InName, bool InBool) override;
	void SetBoolArrayParameter(FName InName, const TArray<bool>& InValue) override;
	void SetIntParameter(FName InName, int32 InInt) override;
	void SetIntArrayParameter(FName InName, const TArray<int32>& InValue) override;
	void SetFloatParameter(FName InName, float InFloat) override;
	void SetFloatArrayParameter(FName InName, const TArray<float>& InValue) override;
	void SetStringParameter(FName InName, const FString& InValue) override;
	void SetStringArrayParameter(FName InName, const TArray<FString>& InValue) override;
	void SetObjectParameter(FName InName, UObject* InValue) override;
	void SetObjectArrayParameter(FName InName, const TArray<UObject*>& InValue) override;

	void SetParameter(FAudioParameter&& InValue) override;
	void SetParameters(TArray<FAudioParameter>&& InValues) override;

	/** Returns the active audio device to use for this component based on whether or not the component is playing in a world. */
	virtual FAudioDevice* GetAudioDevice() const = 0;

	/** Returns the id of the sound owner's instance associated with the parameter interface. */
	virtual uint64 GetInstanceOwnerID() const = 0;

	/** Returns the Game Thread copy of parameters to modify in place. */
	virtual TArray<FAudioParameter>& GetInstanceParameters() = 0;

	/** Returns the USoundBase used to initialize instance parameters to update. */
	virtual USoundBase* GetSound() = 0;

	virtual bool IsPlaying() const = 0;

	virtual bool GetDisableParameterUpdatesWhilePlaying() const = 0;

private:
	void SetParameterInternal(FAudioParameter&& InValue);
};
