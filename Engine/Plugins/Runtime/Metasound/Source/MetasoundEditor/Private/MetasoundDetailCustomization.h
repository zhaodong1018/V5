// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "IDetailCustomization.h"
#include "SGraphActionMenu.h"
#include "SSearchableComboBox.h"
#include "Templates/SharedPointer.h"
#include "Types/SlateEnums.h"
#include "UObject/NameTypes.h"


// Forward Declarations
class FPropertyRestriction;
class IDetailLayoutBuilder;
struct FPointerEvent;

namespace Metasound
{
	namespace Editor
	{
		class FMetasoundDetailCustomization : public IDetailCustomization
		{
		public:
			FMetasoundDetailCustomization(FName InDocumentPropertyName);

			// IDetailCustomization interface
			virtual void CustomizeDetails(IDetailLayoutBuilder& DetailLayout) override;
			// End of IDetailCustomization interface

		private:
			void CustomizeInterfaces(IDetailLayoutBuilder& DetailLayout);

			FName GetInterfaceVersionsPath() const;
			FName GetMetadataRootClassPath() const;
			FName GetMetadataPropertyPath() const;

			void UpdateInterfaceNames();

			TArray<TSharedPtr<FString>> AddableInterfaceNames;

			TSet<FName> ImplementedInterfaceNames;
			TSharedPtr<SSearchableComboBox> InterfaceComboBox;
			TAttribute<bool> IsGraphEditableAttribute;

			TWeakObjectPtr<UObject> MetaSound;

			FName DocumentPropertyName;
		};
	} // namespace Editor
} // namespace Metasound