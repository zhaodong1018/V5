// Copyright Epic Games, Inc. All Rights Reserved.
#include "MetasoundDetailCustomization.h"

#include "Containers/Set.h"
#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Framework/Notifications/NotificationManager.h"
#include "IAudioGeneratorInterfaceRegistry.h"
#include "IAudioParameterTransmitter.h"
#include "IDetailGroup.h"
#include "Input/Events.h"
#include "MetasoundAssetBase.h"
#include "MetasoundEditor.h"
#include "MetasoundEditorGraphBuilder.h"
#include "MetasoundEditorSettings.h"
#include "MetasoundFrontend.h"
#include "MetasoundFrontendArchetypeRegistry.h"
#include "MetasoundFrontendController.h"
#include "MetasoundFrontendSearchEngine.h"
#include "MetasoundSource.h"
#include "MetasoundUObjectRegistry.h"
#include "PropertyCustomizationHelpers.h"
#include "PropertyEditorDelegates.h"
#include "PropertyHandle.h"
#include "PropertyRestriction.h"
#include "SGraphPalette.h"
#include "SlateCore/Public/Styling/SlateColor.h"
#include "Sound/SoundWave.h"
#include "Templates/Casts.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/SlateTypes.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SCheckBox.h"
#include "ScopedTransaction.h"


#define LOCTEXT_NAMESPACE "MetaSoundEditor"


namespace Metasound
{
	namespace Editor
	{
		FName BuildChildPath(const FString& InBasePath, FName InPropertyName)
		{
			return FName(InBasePath + TEXT(".") + InPropertyName.ToString());
		}

		FName BuildChildPath(const FName& InBasePath, FName InPropertyName)
		{
			return FName(InBasePath.ToString() + TEXT(".") + InPropertyName.ToString());
		}

		FMetasoundDetailCustomization::FMetasoundDetailCustomization(FName InDocumentPropertyName)
			: IDetailCustomization()
			, DocumentPropertyName(InDocumentPropertyName)
		{
			IsGraphEditableAttribute = TAttribute<bool>::Create([this]()
			{
				using namespace Metasound;
				using namespace Metasound::Frontend;
				if (const FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(MetaSound.Get()))
				{
					FConstGraphHandle GraphHandle = MetaSoundAsset->GetRootGraphHandle();
					return GraphHandle->GetGraphStyle().bIsGraphEditable;
				}

				return false;
			});
		}

		void FMetasoundDetailCustomization::CustomizeInterfaces(IDetailLayoutBuilder& DetailLayout)
		{
			UpdateInterfaceNames();

			SAssignNew(InterfaceComboBox, SSearchableComboBox)
				.OptionsSource(&AddableInterfaceNames)
				.OnGenerateWidget_Lambda([](TSharedPtr<FString> InItem)
				{
					return SNew(STextBlock)
						.Text(FText::FromString(*InItem));
				})
				.OnSelectionChanged_Lambda([this](TSharedPtr<FString> NameToAdd, ESelectInfo::Type InSelectInfo)
				{
					using namespace Metasound;
					using namespace Metasound::Frontend;

					FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(MetaSound.Get());
					if (!ensure(MetaSoundAsset))
					{
						return;
					}

					if (InSelectInfo != ESelectInfo::OnNavigation)
					{
						FMetasoundFrontendInterface InterfaceToAdd;
						if (ensure(ISearchEngine::Get().FindInterfaceWithHighestVersion(FName(*NameToAdd.Get()), InterfaceToAdd)))
						{
							FScopedTransaction(LOCTEXT("AddInterfaceTransaction", "Add MetaSound Interface"));
							MetaSound.Get()->Modify();
							MetaSoundAsset->GetGraphChecked().Modify();

							FDocumentHandle DocumentHandle = MetaSoundAsset->GetDocumentHandle();
							FModifyRootGraphInterfaces ModifyTransform({ }, { InterfaceToAdd });
							ModifyTransform.SetDefaultNodeLocations(false); // Don't automatically add nodes to ed graph
							ModifyTransform.Transform(DocumentHandle);
							MetaSoundAsset->SetInterfacesUpdatedOnSynchronization();
						}

						UpdateInterfaceNames();
						InterfaceComboBox->RefreshOptions();
						MetaSoundAsset->SetSynchronizationRequired();
					}
				})
				.Content()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("UpdateInterfaceAction", "Add Interface..."))
					.IsEnabled(IsGraphEditableAttribute)
				];

			TSharedRef<SWidget> InterfaceUtilities = SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.Padding(2.0f)
			.HAlign(HAlign_Left)
			.VAlign(VAlign_Center)
			.AutoWidth()
			[
				InterfaceComboBox->AsShared()
			]
			+ SHorizontalBox::Slot()
			.Padding(2.0f)
			.HAlign(HAlign_Left)
			.VAlign(VAlign_Center)
			.AutoWidth()
			[
				PropertyCustomizationHelpers::MakeDeleteButton(FSimpleDelegate::CreateLambda([this]()
				{
					using namespace Frontend;

					FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(MetaSound.Get());
					if (!ensure(MetaSoundAsset))
					{
						return;
					}

					TArray<FMetasoundFrontendInterface> ImplementedInterfaces;
					Algo::Transform(ImplementedInterfaceNames, ImplementedInterfaces, [](const FName& Name)
					{
						FMetasoundFrontendInterface Interface;
						ISearchEngine::Get().FindInterfaceWithHighestVersion(Name, Interface);
						return Interface;
					});

					{
						FScopedTransaction(LOCTEXT("RemoveAllInterfacesTransaction", "Remove All MetaSound Interfaces"));
						MetaSound.Get()->Modify();
						MetaSoundAsset->GetGraphChecked().Modify();

						FDocumentHandle DocumentHandle = MetaSoundAsset->GetDocumentHandle();
						FModifyRootGraphInterfaces({ ImplementedInterfaces }, { }).Transform(DocumentHandle);
					}

					UpdateInterfaceNames();
					InterfaceComboBox->RefreshOptions();
					MetaSoundAsset->SetInterfacesUpdatedOnSynchronization();
					MetaSoundAsset->SetSynchronizationRequired();
				}), LOCTEXT("RemoveInterfaceTooltip", "Removes all interfaces from the given MetaSound."))
			];

			const FText HeaderName = LOCTEXT("InterfacesGroupDisplayName", "Interfaces");
			IDetailCategoryBuilder& InterfaceCategory = DetailLayout.EditCategory("Interfaces", HeaderName);

			InterfaceCategory.AddCustomRow(HeaderName)
			.ValueContent()
			[
				InterfaceUtilities
			];

			auto CreateInterfaceEntryWidget = [&](FName InInterfaceName) -> TSharedPtr<SWidget>
			{
				using namespace Frontend;

				FMetasoundFrontendInterface InterfaceEntry;
				if (!ensure(ISearchEngine::Get().FindInterfaceWithHighestVersion(InInterfaceName, InterfaceEntry)))
				{
					return SNullWidget::NullWidget;
				}

				TSharedRef<SWidget> RemoveButtonWidget = PropertyCustomizationHelpers::MakeDeleteButton(FSimpleDelegate::CreateLambda([this, InterfaceEntry]()
				{
					using namespace Frontend;

					FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(MetaSound.Get());
					if (!ensure(MetaSoundAsset))
					{
						return;
					}

					{
						FScopedTransaction(LOCTEXT("RemoveInterfaceTransaction", "Remove MetaSound Interface"));
						MetaSound.Get()->Modify();
						MetaSoundAsset->GetGraphChecked().Modify();

						FDocumentHandle DocumentHandle = MetaSoundAsset->GetDocumentHandle();
						FModifyRootGraphInterfaces({ InterfaceEntry }, { }).Transform(DocumentHandle);
					}

					UpdateInterfaceNames();
					InterfaceComboBox->RefreshOptions();
					MetaSoundAsset->SetInterfacesUpdatedOnSynchronization();
					MetaSoundAsset->SetSynchronizationRequired();
				}), LOCTEXT("RemoveInterfaceTooltip", "Removes the associated interface from the MetaSound."));

				return SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.Padding(2.0f)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.AutoWidth()
					[
						SNew(STextBlock)
						.Text(FText::FromName(InterfaceEntry.Version.Name))
					]
					+ SHorizontalBox::Slot()
					.Padding(2.0f)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.AutoWidth()
					[
						RemoveButtonWidget
					];
			};

			TArray<FName> InterfaceNames = ImplementedInterfaceNames.Array();
			InterfaceNames.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
			for (const FName& InterfaceName : InterfaceNames)
			{
				InterfaceCategory.AddCustomRow(FText::FromName(InterfaceName))
				.ValueContent()
				[
					CreateInterfaceEntryWidget(InterfaceName)->AsShared()
				];
			}
		}

		FName FMetasoundDetailCustomization::GetInterfaceVersionsPath() const
		{
			return Metasound::Editor::BuildChildPath(DocumentPropertyName, GET_MEMBER_NAME_CHECKED(FMetasoundFrontendDocument, InterfaceVersions));
		}

		FName FMetasoundDetailCustomization::GetMetadataRootClassPath() const
		{
			return Metasound::Editor::BuildChildPath(DocumentPropertyName, GET_MEMBER_NAME_CHECKED(FMetasoundFrontendDocument, RootGraph));
		}

		FName FMetasoundDetailCustomization::GetMetadataPropertyPath() const
		{
			const FName RootClass = FName(GetMetadataRootClassPath());
			return Metasound::Editor::BuildChildPath(RootClass, GET_MEMBER_NAME_CHECKED(FMetasoundFrontendClass, Metadata));
		}

		void FMetasoundDetailCustomization::UpdateInterfaceNames()
		{
			using namespace Frontend;

			AddableInterfaceNames.Reset();
			ImplementedInterfaceNames.Reset();

			if (const FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(MetaSound.Get()))
			{
				const TArray<FMetasoundFrontendVersion>& ImplementedInterfaces = MetaSoundAsset->GetDocumentChecked().InterfaceVersions;

				auto GetVersionName = [](const FMetasoundFrontendVersion& Version) { return Version.Name; };
				auto IsTransmittableVersion = [](const FMetasoundFrontendVersion& Version)
				{
					using namespace Metasound::Frontend;

					const FInterfaceRegistryKey Key = GetInterfaceRegistryKey(Version);
					if (const IInterfaceRegistryEntry* Entry = IInterfaceRegistry::Get().FindInterfaceRegistryEntry(Key))
					{
						return Entry->GetRouterName() == Audio::IParameterTransmitter::RouterName;
					}

					return false;
				};

				Algo::TransformIf(ImplementedInterfaces, ImplementedInterfaceNames, IsTransmittableVersion, GetVersionName);

				Audio::IGeneratorInterfaceRegistry::Get().IterateInterfaces([this](Audio::FGeneratorInterfacePtr Interface)
				{
					if (!ImplementedInterfaceNames.Contains(Interface->Name))
					{
						FString Name = Interface->Name.ToString();
						AddableInterfaceNames.Add(MakeShared<FString>(MoveTemp(Name)));
					}
				});
			}
		}

		void FMetasoundDetailCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailLayout)
		{
			using namespace Frontend;

			EMetasoundActiveDetailView DetailsView = EMetasoundActiveDetailView::Metasound;
			if (const UMetasoundEditorSettings* EditorSettings = GetDefault<UMetasoundEditorSettings>())
			{
				DetailsView = EditorSettings->DetailView;
			}

			TArray<TWeakObjectPtr<UObject>> Objects;
			DetailLayout.GetObjectsBeingCustomized(Objects);

			// Only support modifying a single MetaSound at a time (Multiple
			// MetaSound editing will be covered most likely by separate tool).
			if (Objects.Num() > 1)
			{
				return;
			}
			MetaSound = Objects.Last();
			if (!MetaSound.IsValid())
			{
				return;
			}

			TWeakObjectPtr<UMetaSoundSource> MetaSoundSource = Cast<UMetaSoundSource>(MetaSound.Get());

			switch (DetailsView)
			{
				case EMetasoundActiveDetailView::Metasound:
				{
					IDetailCategoryBuilder& GeneralCategoryBuilder = DetailLayout.EditCategory("MetaSound");
					const FName AuthorPropertyPath = BuildChildPath(GetMetadataPropertyPath(), FMetasoundFrontendClassMetadata::GetAuthorPropertyName());
					const FName CategoryHierarchyPropertyPath = BuildChildPath(GetMetadataPropertyPath(), FMetasoundFrontendClassMetadata::GetCategoryHierarchyPropertyName());
					const FName DescPropertyPath = BuildChildPath(GetMetadataPropertyPath(), FMetasoundFrontendClassMetadata::GetDescriptionPropertyName());

					const FName ClassNamePropertyPath = BuildChildPath(GetMetadataPropertyPath(), FMetasoundFrontendClassMetadata::GetClassNamePropertyName());
					const FName ClassNameNamePropertyPath = BuildChildPath(ClassNamePropertyPath, GET_MEMBER_NAME_CHECKED(FMetasoundFrontendClassName, Name));

					const FName VersionPropertyPath = BuildChildPath(GetMetadataPropertyPath(), FMetasoundFrontendClassMetadata::GetVersionPropertyName());
					const FName MajorVersionPropertyPath = BuildChildPath(VersionPropertyPath, GET_MEMBER_NAME_CHECKED(FMetasoundFrontendVersionNumber, Major));
					const FName MinorVersionPropertyPath = BuildChildPath(VersionPropertyPath, GET_MEMBER_NAME_CHECKED(FMetasoundFrontendVersionNumber, Minor));

					const FName InterfaceVersionsPropertyPath = GetInterfaceVersionsPath();

					TSharedPtr<IPropertyHandle> AuthorHandle = DetailLayout.GetProperty(AuthorPropertyPath);
					TSharedPtr<IPropertyHandle> CategoryHierarchyHandle = DetailLayout.GetProperty(CategoryHierarchyPropertyPath);
					TSharedPtr<IPropertyHandle> ClassNameHandle = DetailLayout.GetProperty(ClassNameNamePropertyPath);
					TSharedPtr<IPropertyHandle> DescHandle = DetailLayout.GetProperty(DescPropertyPath);
					TSharedPtr<IPropertyHandle> MajorVersionHandle = DetailLayout.GetProperty(MajorVersionPropertyPath);
					TSharedPtr<IPropertyHandle> MinorVersionHandle = DetailLayout.GetProperty(MinorVersionPropertyPath);
					TSharedPtr<IPropertyHandle> InterfaceVersionsHandle = DetailLayout.GetProperty(InterfaceVersionsPropertyPath);

					// Invalid for UMetaSounds
					TSharedPtr<IPropertyHandle> OutputFormat = DetailLayout.GetProperty(GET_MEMBER_NAME_CHECKED(UMetaSoundSource, OutputFormat));
					if (OutputFormat.IsValid())
					{
						if (MetaSoundSource.IsValid())
						{
							OutputFormat->SetOnPropertyValuePreChange(FSimpleDelegate::CreateLambda([Source = MetaSoundSource]()
							{
								if (Source.IsValid())
								{
									TSharedPtr<FEditor> ParentEditor = FGraphBuilder::GetEditorForMetasound(*Source.Get());
									if (ParentEditor.IsValid())
									{
										ParentEditor->DestroyAnalyzers();
									};
								}
							}));

							OutputFormat->SetOnPropertyValueChanged(FSimpleDelegate::CreateLambda([Source = MetaSoundSource]()
							{
								if (Source.IsValid())
								{
									TSharedPtr<FEditor> ParentEditor = FGraphBuilder::GetEditorForMetasound(*Source.Get());
									if (ParentEditor.IsValid())
									{
										ParentEditor->CreateAnalyzers();
									};
								}
							}));
						}

						TSharedRef<SWidget> OutputFormatValueWidget = OutputFormat->CreatePropertyValueWidget();
						OutputFormatValueWidget->SetEnabled(IsGraphEditableAttribute);

						static const FText OutputFormatName = LOCTEXT("MetasoundOutputFormatPropertyName", "Output Format");
						GeneralCategoryBuilder.AddCustomRow(OutputFormatName)
						.NameContent()
						[
							OutputFormat->CreatePropertyNameWidget()
						]
						.ValueContent()
						[
							OutputFormatValueWidget
						];

						OutputFormat->MarkHiddenByCustomization();
					}

					GeneralCategoryBuilder.AddProperty(AuthorHandle);
					GeneralCategoryBuilder.AddProperty(DescHandle);
					GeneralCategoryBuilder.AddProperty(MajorVersionHandle);
					GeneralCategoryBuilder.AddProperty(MinorVersionHandle);

					static const FText ClassGuidName = LOCTEXT("MetasoundClassGuidPropertyName", "Class Guid");
					GeneralCategoryBuilder.AddCustomRow(ClassGuidName).NameContent()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(ClassGuidName)
							.Font(FEditorStyle::GetFontStyle(TEXT("PropertyWindow.NormalFont")))
						]
					]
					.ValueContent()
					[
						ClassNameHandle->CreatePropertyValueWidget()
					];
					GeneralCategoryBuilder.AddProperty(CategoryHierarchyHandle);

					CustomizeInterfaces(DetailLayout);


					DetailLayout.HideCategory("Attenuation");
					DetailLayout.HideCategory("Effects");
					DetailLayout.HideCategory("Loading");
					DetailLayout.HideCategory("Modulation");
					DetailLayout.HideCategory("Sound");
					DetailLayout.HideCategory("Voice Management");
				}
				break;

				case EMetasoundActiveDetailView::General:
				default:
					DetailLayout.HideCategory("MetaSound");

					const bool bShouldBeInitiallyCollapsed = true;
					IDetailCategoryBuilder& SoundCategory = DetailLayout.EditCategory("Sound");
					SoundCategory.InitiallyCollapsed(bShouldBeInitiallyCollapsed);

					static const TSet<FName> SoundPropsToHide =
					{
						GET_MEMBER_NAME_CHECKED(USoundWave, bLooping),
						GET_MEMBER_NAME_CHECKED(USoundWave, SoundGroup)
					};

					TArray<TSharedRef<IPropertyHandle>>SoundProperties;
					SoundCategory.GetDefaultProperties(SoundProperties);
					for (TSharedRef<IPropertyHandle> Property : SoundProperties)
					{
						if (SoundPropsToHide.Contains(Property->GetProperty()->GetFName()))
						{
							Property->MarkHiddenByCustomization();
						}
					}

					DetailLayout.EditCategory("Attenuation").InitiallyCollapsed(bShouldBeInitiallyCollapsed);
					DetailLayout.EditCategory("Effects").InitiallyCollapsed(bShouldBeInitiallyCollapsed);
					DetailLayout.EditCategory("Modulation").InitiallyCollapsed(bShouldBeInitiallyCollapsed);
					DetailLayout.EditCategory("Voice Management").InitiallyCollapsed(bShouldBeInitiallyCollapsed);

					break;
			}

			// Hack to hide parent structs for nested metadata properties
			DetailLayout.HideCategory("CustomView");

			DetailLayout.HideCategory("Curves");
			DetailLayout.HideCategory("Developer");
			DetailLayout.HideCategory("File Path");
			DetailLayout.HideCategory("Format");
			DetailLayout.HideCategory("Info");
			DetailLayout.HideCategory("Loading");
			DetailLayout.HideCategory("Playback");
			DetailLayout.HideCategory("Subtitles");
			DetailLayout.HideCategory("Analysis");
			DetailLayout.HideCategory("Advanced");
		}
	} // namespace Editor
} // namespace Metasound
#undef LOCTEXT_NAMESPACE
