// Copyright Epic Games, Inc. All Rights Reserved.
#include "MetasoundEditorModule.h"


#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistryModule.h"
#include "AssetTypeActions_Base.h"
#include "Brushes/SlateImageBrush.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphUtilities.h"
#include "EditorStyleSet.h"
#include "HAL/IConsoleManager.h"
#include "IDetailCustomization.h"
#include "ISettingsModule.h"
#include "Metasound.h"
#include "MetasoundAssetSubsystem.h"
#include "MetasoundAssetTypeActions.h"
#include "MetasoundAudioBuffer.h"
#include "MetasoundDetailCustomization.h"
#include "MetasoundEditorGraph.h"
#include "MetasoundEditorGraphBuilder.h"
#include "MetasoundEditorGraphConnectionDrawingPolicy.h"
#include "MetasoundEditorGraphInputNodes.h"
#include "MetasoundEditorGraphNodeFactory.h"
#include "MetasoundEditorGraphSchema.h"
#include "MetasoundEditorSettings.h"
#include "MetasoundFrontendDataTypeRegistry.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendRegistries.h"
#include "MetasoundFrontendTransform.h"
#include "MetasoundNodeDetailCustomization.h"
#include "MetasoundSource.h"
#include "MetasoundTime.h"
#include "MetasoundTrigger.h"
#include "MetasoundUObjectRegistry.h"
#include "MetasoundVariableDetailCustomization.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorDelegates.h"
#include "PropertyEditorModule.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleMacros.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"
#include "Templates/SharedPointer.h"
#include "UObject/UObjectGlobals.h"


DEFINE_LOG_CATEGORY(LogMetasoundEditor);


static int32 MetaSoundEditorAsyncRegistrationEnabledCVar = 1;
FAutoConsoleVariableRef CVarMetaSoundEditorAsyncRegistrationEnabled(
	TEXT("au.MetaSounds.Editor.AsyncRegistrationEnabled"),
	MetaSoundEditorAsyncRegistrationEnabledCVar,
	TEXT("Enable registering all MetaSound asset classes asyncronously on editor load.\n")
	TEXT("0: Disabled, !0: Enabled (default)"),
	ECVF_Default);


namespace Metasound
{
	namespace Editor
	{
		static const FName AssetToolName { "AssetTools" };

		template <typename T>
		void AddAssetAction(IAssetTools& AssetTools, TArray<TSharedPtr<FAssetTypeActions_Base>>& AssetArray)
		{
			TSharedPtr<T> AssetAction = MakeShared<T>();
			TSharedPtr<FAssetTypeActions_Base> AssetActionBase = StaticCastSharedPtr<FAssetTypeActions_Base>(AssetAction);
			AssetTools.RegisterAssetTypeActions(AssetAction.ToSharedRef());
			AssetArray.Add(AssetActionBase);
		}

		class FSlateStyle final : public FSlateStyleSet
		{
		public:
			FSlateStyle()
				: FSlateStyleSet("MetaSoundStyle")
			{
				SetParentStyleName(FEditorStyle::GetStyleSetName());

				SetContentRoot(FPaths::EnginePluginsDir() / TEXT("Runtime/Metasound/Content/Editor/Slate"));
				SetCoreContentRoot(FPaths::EngineContentDir() / TEXT("Slate"));

				static const FVector2D Icon20x20(20.0f, 20.0f);
				static const FVector2D Icon40x40(40.0f, 40.0f);

				static const FVector2D Icon16 = FVector2D(16.0f, 16.0f);
				static const FVector2D Icon64 = FVector2D(64.0f, 64.0f);

				const FVector2D Icon15x11(15.0f, 11.0f);

				// Metasound Editor
				{
					// Actions
					Set("MetasoundEditor.Play", new FSlateImageBrush(RootToContentDir(TEXT("Icons/play_40x.png")), Icon40x40));
					Set("MetasoundEditor.Play.Small", new FSlateImageBrush(RootToContentDir(TEXT("Icons/play_40x.png")), Icon20x20));
					Set("MetasoundEditor.Stop", new FSlateImageBrush(RootToContentDir(TEXT("Icons/stop_40x.png")), Icon40x40));
					Set("MetasoundEditor.Stop.Small", new FSlateImageBrush(RootToContentDir(TEXT("Icons/stop_40x.png")), Icon20x20));
					Set("MetasoundEditor.Import", new FSlateImageBrush(RootToContentDir(TEXT("/Icons/build_40x.png")), Icon40x40));
					Set("MetasoundEditor.Import.Small", new FSlateImageBrush(RootToContentDir(TEXT("/Icons/build_40x.png")), Icon20x20));
					Set("MetasoundEditor.Export", new FSlateImageBrush(RootToContentDir(TEXT("/Icons/build_40x.png")), Icon40x40));
					Set("MetasoundEditor.Export.Small", new FSlateImageBrush(RootToContentDir(TEXT("/Icons/build_40x.png")), Icon20x20));
					Set("MetasoundEditor.ExportError", new FSlateImageBrush(RootToContentDir(TEXT("/Icons/build_error_40x.png")), Icon40x40));
					Set("MetasoundEditor.ExportError.Small", new FSlateImageBrush(RootToContentDir(TEXT("/Icons/build_error_40x.png")), Icon20x20));
					Set("MetasoundEditor.Settings", new FSlateImageBrush(RootToContentDir(TEXT("/Icons/settings_40x.png")), Icon20x20));

					// Graph Editor
					Set("MetasoundEditor.Graph.Node.Body.Input", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_input_body_64x.png")), FVector2D(114.0f, 64.0f)));
					Set("MetasoundEditor.Graph.Node.Body.Default", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_default_body_64x.png")), FVector2D(64.0f, 64.0f)));

					Set("MetasoundEditor.Graph.TriggerPin.Connected", new IMAGE_BRUSH(TEXT("Graph/pin_trigger_connected"), Icon15x11));
					Set("MetasoundEditor.Graph.TriggerPin.Disconnected", new IMAGE_BRUSH(TEXT("Graph/pin_trigger_disconnected"), Icon15x11));

					Set("MetasoundEditor.Graph.Node.Class.Native", new IMAGE_BRUSH_SVG(TEXT("Icons/native_node"), FVector2D(8.0f, 16.0f)));
					Set("MetasoundEditor.Graph.Node.Class.Graph", new IMAGE_BRUSH_SVG(TEXT("Icons/graph_node"), Icon16));
					Set("MetasoundEditor.Graph.Node.Class.Input", new IMAGE_BRUSH_SVG(TEXT("Icons/input_node"), FVector2D(16.0f, 13.0f)));
					Set("MetasoundEditor.Graph.Node.Class.Output", new IMAGE_BRUSH_SVG(TEXT("Icons/output_node"), FVector2D(16.0f, 13.0f)));
					Set("MetasoundEditor.Graph.Node.Class.Variable", new IMAGE_BRUSH_SVG(TEXT("Icons/variable_node"), FVector2D(8.0f, 16.0f)));

					Set("MetasoundEditor.Graph.Node.Math.Add", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_math_add_40x.png")), Icon40x40));
					Set("MetasoundEditor.Graph.Node.Math.Divide", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_math_divide_40x.png")), Icon40x40));
					Set("MetasoundEditor.Graph.Node.Math.Modulo", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_math_modulo_40x.png")), Icon40x40));
					Set("MetasoundEditor.Graph.Node.Math.Multiply", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_math_multiply_40x.png")), Icon40x40));
					Set("MetasoundEditor.Graph.Node.Math.Subtract", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_math_subtract_40x.png")), Icon40x40));
					Set("MetasoundEditor.Graph.Node.Math.Modulo", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_math_modulo_40x.png")), Icon40x40));
					Set("MetasoundEditor.Graph.Node.Math.Power", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_math_power_40x.png")), Icon40x40));
					Set("MetasoundEditor.Graph.Node.Math.Logarithm", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_math_logarithm_40x.png")), Icon40x40));

					// Analyzers
					Set("MetasoundEditor.Analyzers.BackgroundColor", FLinearColor(0.0075f, 0.0075f, 0.0075, 1.0f));

					// Misc
					Set("MetasoundEditor.Speaker", new FSlateImageBrush(RootToContentDir(TEXT("/Icons/speaker_144x.png")), FVector2D(144.0f, 144.0f)));
					Set("MetasoundEditor.Metasound.Icon", new IMAGE_BRUSH_SVG(TEXT("Icons/metasound_icon"), Icon16));

					// Class Icons
					auto SetClassIcon = [this, InIcon16 = Icon16, InIcon64 = Icon64](const FString& ClassName)
					{
						const FString IconFileName = FString::Printf(TEXT("Icons/%s"), *ClassName.ToLower());
						const FSlateColor DefaultForeground(FStyleColors::Foreground);

						Set(*FString::Printf(TEXT("ClassIcon.%s"), *ClassName), new IMAGE_BRUSH_SVG(IconFileName, InIcon16));
						Set(*FString::Printf(TEXT("ClassThumbnail.%s"), *ClassName), new IMAGE_BRUSH_SVG(IconFileName, InIcon64));
					};

					SetClassIcon(TEXT("Metasound"));
					SetClassIcon(TEXT("MetasoundSource"));
				}

				FSlateStyleRegistry::RegisterSlateStyle(*this);
			}
		};

		class FMetasoundGraphPanelPinFactory : public FGraphPanelPinFactory
		{
		};

		class FModule : public IMetasoundEditorModule
		{
			void AddOrUpdateClassRegistryAsset(const FAssetData& InAssetData)
			{
				using namespace Metasound::Frontend;

				if (IsMetaSoundAssetClass(InAssetData.AssetClass))
				{
					// in case they need to be 
					check(GEngine);
					UMetaSoundAssetSubsystem* AssetSubsystem = GEngine->GetEngineSubsystem<UMetaSoundAssetSubsystem>();
					check(AssetSubsystem);

					// Use the editor version of `RegisterGraphWithFrontend` so it re-registers any open MetaSound editors
					AssetSubsystem->AddOrUpdateAsset(InAssetData, false /* bRegisterWithFrontend */);
					
					// Loading all assets necessary only in editor to register &
					// populate potential graphs to reference in MetaSound editor.
					if (InAssetData.IsAssetLoaded())
					{
						if (UObject* AssetObject = InAssetData.GetAsset())
						{
							FGraphBuilder::RegisterGraphWithFrontend(*AssetObject);
						}
					}
					else if (MetaSoundEditorAsyncRegistrationEnabledCVar)
					{
						FSoftObjectPath Path = InAssetData.ToSoftObjectPath();
						LoadPackageAsync(Path.GetLongPackageName(), FLoadPackageAsyncDelegate::CreateLambda(
							[this, ObjectPath = MoveTemp(Path)](const FName& PackageName, UPackage* Package, EAsyncLoadingResult::Type Result)
							{
								if (Result == EAsyncLoadingResult::Succeeded)
								{
									FGraphBuilder::RegisterGraphWithFrontend(*ObjectPath.ResolveObject());
								}
							}
						));
					}
				}
			}

			void OnPackageReloaded(const EPackageReloadPhase InPackageReloadPhase, FPackageReloadedEvent* InPackageReloadedEvent)
			{
				using namespace Metasound;
				using namespace Metasound::Editor;
				using namespace Metasound::Frontend;

				if (!InPackageReloadedEvent)
				{
					return;
				}

				if (InPackageReloadPhase != EPackageReloadPhase::OnPackageFixup)
				{
					return;
				}

				for (const TPair<UObject*, UObject*>& Pair : InPackageReloadedEvent->GetRepointedObjects())
				{
					if (UObject* Obj = Pair.Key)
					{
						if (IsMetaSoundAssetClass(Obj->GetClass()->GetFName()))
						{
							check(GEngine);
							UMetaSoundAssetSubsystem* AssetSubsystem = GEngine->GetEngineSubsystem<UMetaSoundAssetSubsystem>();
							check(AssetSubsystem);

							// Use the editor version of UnregisterWithFrontend so it refreshes any open MetaSound editors
							AssetSubsystem->RemoveAsset(*Pair.Key, false /* bUnregisterWithFrontend */);
							FGraphBuilder::UnregisterGraphWithFrontend(*Pair.Key);
						}
					}

					if (UObject* Obj = Pair.Value)
					{
						if (IsMetaSoundAssetClass(Obj->GetClass()->GetFName()))
						{
							check(GEngine);
							UMetaSoundAssetSubsystem* AssetSubsystem = GEngine->GetEngineSubsystem<UMetaSoundAssetSubsystem>();
							check(AssetSubsystem);
							// Use the editor version of RegisterWithFrontend so it refreshes any open MetaSound editors
							AssetSubsystem->AddOrUpdateAsset(*Pair.Value, false /* bRegisterWithFrontend */);
							FGraphBuilder::RegisterGraphWithFrontend(*Pair.Value);
						}
					}
				}
			}

			void OnAssetScanFinished()
			{
				FARFilter Filter;
				Filter.ClassNames = MetaSoundClassNames;

				FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
				AssetRegistryModule.Get().EnumerateAssets(Filter, [this](const FAssetData& AssetData) { AddOrUpdateClassRegistryAsset(AssetData); return true; });
				AssetRegistryModule.Get().OnAssetAdded().AddRaw(this, &FModule::AddOrUpdateClassRegistryAsset);
				AssetRegistryModule.Get().OnAssetUpdated().AddRaw(this, &FModule::AddOrUpdateClassRegistryAsset);
				AssetRegistryModule.Get().OnAssetRemoved().AddRaw(this, &FModule::RemoveAssetFromClassRegistry);
				AssetRegistryModule.Get().OnAssetRenamed().AddRaw(this, &FModule::RenameAssetInClassRegistry);

				AssetRegistryModule.Get().OnFilesLoaded().RemoveAll(this);

				FCoreUObjectDelegates::OnPackageReloaded.AddRaw(this, &FModule::OnPackageReloaded);
			}

			void RemoveAssetFromClassRegistry(const FAssetData& InAssetData)
			{
				if (IsMetaSoundAssetClass(InAssetData.AssetClass))
				{
					check(GEngine);
					UMetaSoundAssetSubsystem* AssetSubsystem = GEngine->GetEngineSubsystem<UMetaSoundAssetSubsystem>();
					check(AssetSubsystem);

					// Use the editor version of UnregisterWithFrontend so it refreshes any open MetaSound editors
					AssetSubsystem->RemoveAsset(InAssetData, false /* bUnregisterWithFrontend */);
					if (UObject* AssetObject = InAssetData.GetAsset())
					{
						FGraphBuilder::UnregisterGraphWithFrontend(*AssetObject);
					}
				}
			}

			void RenameAssetInClassRegistry(const FAssetData& InAssetData, const FString& InOldObjectPath)
			{
				if (IsMetaSoundAssetClass(InAssetData.AssetClass))
				{
					check(GEngine);
					UMetaSoundAssetSubsystem* AssetSubsystem = GEngine->GetEngineSubsystem<UMetaSoundAssetSubsystem>();
					check(AssetSubsystem);

					AssetSubsystem->RenameAsset(InAssetData, false /* bReregisterWithFrontend */);
					if (UObject* AssetObject = InAssetData.GetAsset())
					{
						FGraphBuilder::RegisterGraphWithFrontend(*AssetObject);
					}
				}
			}

			void RegisterNodeInputClasses()
			{
				TSubclassOf<UMetasoundEditorGraphInputLiteral> NodeClass;
				for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
				{
					UClass* Class = *ClassIt;
					if (!Class->IsNative())
					{
						continue;
					}

					if (Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
					{
						continue;
					}

					if (!ClassIt->IsChildOf(UMetasoundEditorGraphInputLiteral::StaticClass()))
					{
						continue;
					}

					if (const UMetasoundEditorGraphInputLiteral* InputCDO = Class->GetDefaultObject<UMetasoundEditorGraphInputLiteral>())
					{
						NodeInputClassRegistry.Add(InputCDO->GetLiteralType(), InputCDO->GetClass());
					}
				}
			}

			void RegisterCoreDataTypes()
			{
				using namespace Metasound::Frontend;

				const IDataTypeRegistry& DataTypeRegistry = IDataTypeRegistry::Get();

				TArray<FName> DataTypeNames;
				DataTypeRegistry.GetRegisteredDataTypeNames(DataTypeNames);

				for (FName DataTypeName : DataTypeNames)
				{
					FDataTypeRegistryInfo RegistryInfo;
					if (ensure(DataTypeRegistry.GetDataTypeInfo(DataTypeName, RegistryInfo)))
					{
						FName PinCategory = DataTypeName;
						FName PinSubCategory;

						// Execution path triggers are specialized
						if (DataTypeName == GetMetasoundDataTypeName<FTrigger>())
						{
							PinCategory = FGraphBuilder::PinCategoryTrigger;
						}

						// GraphEditor by default designates specialized connection
						// specification for Int64, so use it even though literal is
						// boiled down to int32
						//else if (DataTypeName == Frontend::GetDataTypeName<int64>())
						//{
						//	PinCategory = FGraphBuilder::PinCategoryInt64;
						//}

						// Primitives
						else
						{
							switch (RegistryInfo.PreferredLiteralType)
							{
								case ELiteralType::Boolean:
								case ELiteralType::BooleanArray:
								{
									PinCategory = FGraphBuilder::PinCategoryBoolean;
								}
								break;

								case ELiteralType::Float:
								case ELiteralType::FloatArray:
								{
									PinCategory = FGraphBuilder::PinCategoryFloat;

									// Doubles use the same preferred literal
									// but different colorization
									//if (DataTypeName == Frontend::GetDataTypeName<double>())
									//{
									//	PinCategory = FGraphBuilder::PinCategoryDouble;
									//}

									// Differentiate stronger numeric types associated with audio
									if (DataTypeName == GetMetasoundDataTypeName<FTime>())
									{
										PinSubCategory = FGraphBuilder::PinSubCategoryTime;
									}
								}
								break;

								case ELiteralType::Integer:
								case ELiteralType::IntegerArray:
								{
									PinCategory = FGraphBuilder::PinCategoryInt32;
								}
								break;

								case ELiteralType::String:
								case ELiteralType::StringArray:
								{
									PinCategory = FGraphBuilder::PinCategoryString;
								}
								break;

								case ELiteralType::UObjectProxy:
								case ELiteralType::UObjectProxyArray:
								{
									PinCategory = FGraphBuilder::PinCategoryObject;
								}
								break;

								case ELiteralType::None:
								case ELiteralType::Invalid:
								default:
								{
									// Audio types are ubiquitous, so added as subcategory
									// to be able to stylize connections (i.e. wire color & wire animation)
									if (DataTypeName == GetMetasoundDataTypeName<FAudioBuffer>())
									{
										PinCategory = FGraphBuilder::PinCategoryAudio;
									}
									static_assert(static_cast<int32>(ELiteralType::Invalid) == 12, "Possible missing binding of pin category to primitive type");
								}
								break;
							}
						}

						const bool bIsArray = RegistryInfo.IsArrayType();
						const EPinContainerType ContainerType = bIsArray ? EPinContainerType::Array : EPinContainerType::None;
						FEdGraphPinType PinType(PinCategory, PinSubCategory, nullptr, ContainerType, false, FEdGraphTerminalType());
						UClass* ClassToUse = DataTypeRegistry.GetUClassForDataType(DataTypeName);
						PinType.PinSubCategoryObject = Cast<UObject>(ClassToUse);

						DataTypeInfo.Emplace(DataTypeName, FEditorDataType(MoveTemp(PinType), MoveTemp(RegistryInfo)));
					}
				}
			}

			void ShutdownAssetClassRegistry()
			{
				if (FAssetRegistryModule* AssetRegistryModule = static_cast<FAssetRegistryModule*>(FModuleManager::Get().GetModule("AssetRegistry")))
				{
					AssetRegistryModule->Get().OnAssetAdded().RemoveAll(this);
					AssetRegistryModule->Get().OnAssetUpdated().RemoveAll(this);
					AssetRegistryModule->Get().OnAssetRemoved().RemoveAll(this);
					AssetRegistryModule->Get().OnAssetRenamed().RemoveAll(this);
					AssetRegistryModule->Get().OnFilesLoaded().RemoveAll(this);

					FCoreUObjectDelegates::OnPackageReloaded.RemoveAll(this);
				}
			}

			virtual void RegisterExplicitProxyClass(const UClass& InClass) override
			{
				using namespace Metasound::Frontend;

				const IDataTypeRegistry& DataTypeRegistry = IDataTypeRegistry::Get();
				FDataTypeRegistryInfo RegistryInfo;
				ensureAlways(DataTypeRegistry.IsUObjectProxyFactory(InClass.GetDefaultObject()));

				ExplicitProxyClasses.Add(&InClass);
			}

			virtual bool IsExplicitProxyClass(const UClass& InClass) const override
			{
				return ExplicitProxyClasses.Contains(&InClass);
			}

			virtual TUniquePtr<IMetaSoundInputLiteralCustomization> CreateInputLiteralCustomization(UClass& InClass, IDetailCategoryBuilder& InDefaultCategoryBuilder) const override
			{
				const TUniquePtr<IMetaSoundInputLiteralCustomizationFactory>* CustomizationFactory = LiteralCustomizationFactories.Find(&InClass);
				if (CustomizationFactory && CustomizationFactory->IsValid())
				{
					return (*CustomizationFactory)->CreateLiteralCustomization(InDefaultCategoryBuilder);
				}

				return nullptr;
			}

			virtual const TSubclassOf<UMetasoundEditorGraphInputLiteral> FindInputLiteralClass(EMetasoundFrontendLiteralType InLiteralType) const override
			{
				return NodeInputClassRegistry.FindRef(InLiteralType);
			}

			virtual const FEditorDataType* FindDataType(FName InDataTypeName) const override
			{
				return DataTypeInfo.Find(InDataTypeName);
			}

			virtual const FEditorDataType& FindDataTypeChecked(FName InDataTypeName) const override
			{
				return DataTypeInfo.FindChecked(InDataTypeName);
			}

			virtual bool IsRegisteredDataType(FName InDataTypeName) const override
			{
				return DataTypeInfo.Contains(InDataTypeName);
			}

			virtual void IterateDataTypes(TUniqueFunction<void(const FEditorDataType&)> InDataTypeFunction) const override
			{
				for (const TPair<FName, FEditorDataType>& Pair : DataTypeInfo)
				{
					InDataTypeFunction(Pair.Value);
				}
			}

			virtual bool IsMetaSoundAssetClass(const FName InClassName) const override
			{
				// TODO: Move to IMetasoundUObjectRegistry (overload IsRegisteredClass to take in class name?)
				return MetaSoundClassNames.Contains(InClassName);
			}

			virtual void StartupModule() override
			{
				// Register Metasound asset type actions
				IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(AssetToolName).Get();

				AddAssetAction<FAssetTypeActions_MetaSound>(AssetTools, AssetActions);
				AddAssetAction<FAssetTypeActions_MetaSoundSource>(AssetTools, AssetActions);

				FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
				
				PropertyModule.RegisterCustomClassLayout(
					UMetaSound::StaticClass()->GetFName(),
					FOnGetDetailCustomizationInstance::CreateLambda([]() { return MakeShared<FMetasoundDetailCustomization>(UMetaSound::GetDocumentPropertyName()); }));

				PropertyModule.RegisterCustomClassLayout(
					UMetaSoundSource::StaticClass()->GetFName(),
					FOnGetDetailCustomizationInstance::CreateLambda([]() { return MakeShared<FMetasoundDetailCustomization>(UMetaSoundSource::GetDocumentPropertyName()); }));

				PropertyModule.RegisterCustomClassLayout(
					UMetasoundEditorGraphInput::StaticClass()->GetFName(),
					FOnGetDetailCustomizationInstance::CreateLambda([]() { return MakeShared<FMetasoundInputDetailCustomization>(); }));

				PropertyModule.RegisterCustomClassLayout(
					UMetasoundEditorGraphOutput::StaticClass()->GetFName(),
					FOnGetDetailCustomizationInstance::CreateLambda([]() { return MakeShared<FMetasoundOutputDetailCustomization>(); }));

				PropertyModule.RegisterCustomClassLayout(
					UMetasoundEditorGraphVariable::StaticClass()->GetFName(),
					FOnGetDetailCustomizationInstance::CreateLambda([]() { return MakeShared<FMetasoundVariableDetailCustomization>(); }));

				PropertyModule.RegisterCustomPropertyTypeLayout(
					"MetasoundEditorGraphInputBoolRef",
					FOnGetPropertyTypeCustomizationInstance::CreateLambda([]() { return MakeShared<FMetasoundInputBoolDetailCustomization>(); }));

				PropertyModule.RegisterCustomPropertyTypeLayout(
					"MetasoundEditorGraphInputIntRef",
					FOnGetPropertyTypeCustomizationInstance::CreateLambda([]() { return MakeShared<FMetasoundInputIntDetailCustomization>(); }));

				PropertyModule.RegisterCustomPropertyTypeLayout(
					"MetasoundEditorGraphInputObjectRef",
					FOnGetPropertyTypeCustomizationInstance::CreateLambda([]() { return MakeShared<FMetasoundInputObjectDetailCustomization>(); }));

				LiteralCustomizationFactories.Add(UMetasoundEditorGraphInputFloat::StaticClass(), MakeUnique<FMetasoundFloatLiteralCustomizationFactory>());

				StyleSet = MakeShared<FSlateStyle>();

				RegisterCoreDataTypes();
				RegisterNodeInputClasses();

				GraphConnectionFactory = MakeShared<FGraphConnectionDrawingPolicyFactory>();
				FEdGraphUtilities::RegisterVisualPinConnectionFactory(GraphConnectionFactory);

				GraphNodeFactory = MakeShared<FMetasoundGraphNodeFactory>();
				FEdGraphUtilities::RegisterVisualNodeFactory(GraphNodeFactory);

				GraphPanelPinFactory = MakeShared<FMetasoundGraphPanelPinFactory>();
				FEdGraphUtilities::RegisterVisualPinFactory(GraphPanelPinFactory);

				ISettingsModule& SettingsModule = FModuleManager::LoadModuleChecked<ISettingsModule>("Settings");

				SettingsModule.RegisterSettings("Editor", "Audio", "MetaSound Editor",
					NSLOCTEXT("MetaSoundsEditor", "MetaSoundEditorSettingsName", "MetaSound Editor"),
					NSLOCTEXT("MetaSoundsEditor", "MetaSoundEditorSettingsDescription", "Customize MetaSound Editor."),
					GetMutableDefault<UMetasoundEditorSettings>()
				);

				MetaSoundClassNames.Add(UMetaSound::StaticClass()->GetFName());
				MetaSoundClassNames.Add(UMetaSoundSource::StaticClass()->GetFName());

				FAssetTypeActions_MetaSound::RegisterMenuActions();
				FAssetTypeActions_MetaSoundSource::RegisterMenuActions();

				FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
				AssetRegistryModule.Get().OnFilesLoaded().AddRaw(this, &FModule::OnAssetScanFinished);

				RegisterExplicitProxyClass(*USoundWave::StaticClass());
			}

			virtual void ShutdownModule() override
			{
				if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
				{
					SettingsModule->UnregisterSettings("Editor", "Audio", "MetaSound Editor");
				}

				if (FModuleManager::Get().IsModuleLoaded(AssetToolName))
				{
					IAssetTools& AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>(AssetToolName).Get();
					for (TSharedPtr<FAssetTypeActions_Base>& AssetAction : AssetActions)
					{
						AssetTools.UnregisterAssetTypeActions(AssetAction.ToSharedRef());
					}
				}

				if (GraphConnectionFactory.IsValid())
				{
					FEdGraphUtilities::UnregisterVisualPinConnectionFactory(GraphConnectionFactory);
				}

				if (GraphNodeFactory.IsValid())
				{
					FEdGraphUtilities::UnregisterVisualNodeFactory(GraphNodeFactory);
					GraphNodeFactory.Reset();
				}

				if (GraphPanelPinFactory.IsValid())
				{
					FEdGraphUtilities::UnregisterVisualPinFactory(GraphPanelPinFactory);
					GraphPanelPinFactory.Reset();
				}

				ShutdownAssetClassRegistry();

				AssetActions.Reset();
				DataTypeInfo.Reset();
				MetaSoundClassNames.Reset();
			}

			TArray<FName> MetaSoundClassNames;

			TArray<TSharedPtr<FAssetTypeActions_Base>> AssetActions;
			TMap<FName, FEditorDataType> DataTypeInfo;
			TMap<EMetasoundFrontendLiteralType, const TSubclassOf<UMetasoundEditorGraphInputLiteral>> NodeInputClassRegistry;

			TMap<UClass*, TUniquePtr<IMetaSoundInputLiteralCustomizationFactory>> LiteralCustomizationFactories;

			TSharedPtr<FMetasoundGraphNodeFactory> GraphNodeFactory;
			TSharedPtr<FGraphPanelPinConnectionFactory> GraphConnectionFactory;
			TSharedPtr<FMetasoundGraphPanelPinFactory> GraphPanelPinFactory;
			TSharedPtr<FSlateStyleSet> StyleSet;

			TSet<const UClass*> ExplicitProxyClasses;
		};
	} // namespace Editor
} // namespace Metasound

IMPLEMENT_MODULE(Metasound::Editor::FModule, MetasoundEditor);
