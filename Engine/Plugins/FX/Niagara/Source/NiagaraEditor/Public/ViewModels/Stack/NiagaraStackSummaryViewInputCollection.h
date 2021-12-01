// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ViewModels/Stack/NiagaraStackItem.h"
#include "Misc/NotifyHook.h"
#include "PropertyEditorDelegates.h"
#include "NiagaraStackFunctionInputCollection.h"
#include "NiagaraStackSummaryViewInputCollection.generated.h"

class IPropertyRowGenerator;
class UNiagaraNode;
class IDetailTreeNode;
class UNiagaraStackFunctionInputCollection;
class UNiagaraNodeFunctionCall;

UCLASS()
class NIAGARAEDITOR_API UNiagaraStackSummaryViewObject : public UNiagaraStackFunctionInputCollectionBase
{
	GENERATED_BODY()
		
public:
	DECLARE_DELEGATE_TwoParams(FOnSelectRootNodes, TArray<TSharedRef<IDetailTreeNode>>, TArray<TSharedRef<IDetailTreeNode>>*);

public:
	UNiagaraStackSummaryViewObject();

	void Initialize(FRequiredEntryData InRequiredEntryData, UNiagaraEmitter* InEmitter, FString InOwningStackItemEditorDataKey);


	virtual FText GetDisplayName() const override;
	virtual bool GetShouldShowInStack() const override;
	virtual bool GetIsEnabled() const;
protected:

	virtual void RefreshChildrenInternal(const TArray<UNiagaraStackEntry*>& CurrentChildren, TArray<UNiagaraStackEntry*>& NewChildren, TArray<FStackIssue>& NewIssues) override;

	void AppendEmitterCategory(TSharedPtr<FNiagaraScriptViewModel> ScriptViewModelPinned, ENiagaraScriptUsage ScriptUsage, FGuid ScriptUsageId, TArray<UNiagaraStackEntry*>& NewChildren, const TArray<UNiagaraStackEntry*>& CurrentChildren, TArray<FStackIssue>& NewIssues);

	virtual void PostRefreshChildrenInternal() override;

private:

	void OnViewStateChanged();
private:

	UNiagaraEmitter* Emitter;
	TMap<FGuid, UNiagaraStackFunctionInputCollection*> KnownInputCollections;
};
