// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "MetasoundEditorGraphInputNodes.h"
#include "MetasoundFrontend.h"
#include "MetasoundFrontendController.h"
#include "MetasoundFrontendDocument.h"


// Forward Declarations
class SGraphEditor;
class UEdGraphNode;
class UEdGraphPin;
class UMetaSound;
class UMetasoundEditorGraphNode;

struct FEdGraphPinType;
struct FMetasoundFrontendNodeStyle;


namespace Metasound
{
	// Forward Declarations
	struct FLiteral;

	namespace Editor
	{
		// Forward Declarations
		class FEditor;
		class IMetasoundEditorModule;

		class FGraphBuilder
		{
			static void InitGraphNode(Frontend::FNodeHandle& InNodeHandle, UMetasoundEditorGraphNode* NewGraphNode, UObject& InMetaSound);

			// Validates MetaSound graph.
			static bool ValidateGraph(UObject& InMetaSound);

		public:
			static const FName PinCategoryAudio;
			static const FName PinCategoryBoolean;
			//static const FName PinCategoryDouble;
			static const FName PinCategoryFloat;
			static const FName PinCategoryInt32;
			//static const FName PinCategoryInt64;
			static const FName PinCategoryObject;
			static const FName PinCategoryString;
			static const FName PinCategoryTrigger;

			// Custom pin-related styles for non-literal types (ex. wire color, pin heads, etc.)
			static const FName PinSubCategoryTime; // Time type

			static const FText FunctionMenuName;
			static const FText GraphMenuName;

			// Adds an EdGraph node to mirror the provided FNodeHandle.
			static UMetasoundEditorGraphNode* AddNode(UObject& InMetaSound, Frontend::FNodeHandle InNodeHandle, FVector2D InLocation, bool bInSelectNewNode = true);

			// Convenience functions for retrieving the editor for the given MetaSound/EdGraph
			static TSharedPtr<FEditor> GetEditorForMetasound(const UObject& MetaSound);
			static TSharedPtr<FEditor> GetEditorForGraph(const UEdGraph& EdGraph);

			// Initializes editor graph for given MetaSound
			static bool InitGraph(UObject& InMetaSound);

			// Wraps RegisterGraphWithFrontend logic in Frontend with any additional logic required to refresh editor & respective editor object state.
			static void RegisterGraphWithFrontend(UObject& InMetaSound);

			// Wraps RegisterGraphWithFrontend logic in Frontend with any additional logic required to refresh editor & respective editor object state.
			static void UnregisterGraphWithFrontend(UObject& InMetaSound);

			// Returns a display name for a node. If the node has an empty or whitespace
			// only DisplayName, then the NodeName is used. 
			static FText GetDisplayName(const Frontend::INodeController& InFrontendNode);

			// Returns a display name for an input. If the input has an empty or whitespace
			// only DisplayName, then the VertexName is used. 
			static FText GetDisplayName(const Frontend::IInputController& InFrontendInput);

			// Returns a display name for a output. If the output has an empty or whitespace
			// only DisplayName, then the VertexName is used. 
			static FText GetDisplayName(const Frontend::IOutputController& InFrontendOutput);

			// Returns a display name for a variable. If the variable has an empty or whitespace
			// only DisplayName, then the VariableName is used. 
			static FText GetDisplayName(const Frontend::IVariableController& InFrontendVariable);

			// Returns the PinName for an IOutputController.
			static FName GetPinName(const Frontend::IOutputController& InFrontendOutput);

			// Returns the PinName for an IInputController.
			static FName GetPinName(const Frontend::IInputController& InFrontendInput);

			// Adds a node handle to mirror the provided graph node and binds to it.  Does *NOT* mirror existing EdGraph connections
			// nor does it remove existing bound Frontend Node (if set) from associated Frontend Graph.
			static Frontend::FNodeHandle AddNodeHandle(UObject& InMetaSound, UMetasoundEditorGraphNode& InGraphNode);

			// Adds a corresponding UMetasoundEditorGraphInputNode for the provided node handle.
			static UMetasoundEditorGraphInputNode* AddInputNode(UObject& InMetaSound, Frontend::FNodeHandle InNodeHandle, FVector2D InLocation, bool bInSelectNewNode = true);

			// Generates FNodeHandle for the given external node data. Does not bind or create EdGraph representation of given node.
			static Frontend::FNodeHandle AddInputNodeHandle(
				UObject& InMetaSound,
				const FName InTypeName,
				const FText& InToolTip,
				const FMetasoundFrontendLiteral* InDefaultValue = nullptr,
				const FName* InNameBase = nullptr);

			// Adds a corresponding UMetasoundEditorGraphExternalNode for the provided node handle.
			static UMetasoundEditorGraphExternalNode* AddExternalNode(UObject& InMetaSound, Frontend::FNodeHandle& InNodeHandle, FVector2D InLocation, bool bInSelectNewNode = true);

			// Adds an externally-defined node with the given class info to both the editor and document graphs.
			// Generates analogous FNodeHandle.
			static UMetasoundEditorGraphExternalNode* AddExternalNode(UObject& InMetaSound, const FMetasoundFrontendClassMetadata& InMetadata, FVector2D InLocation, bool bInSelectNewNode = true);

			// Adds an variable node with the given node handle to the editor graph.
			static UMetasoundEditorGraphVariableNode* AddVariableNode(UObject& InMetaSound, Frontend::FNodeHandle& InNodeHandle, FVector2D InLocation, bool bInSelectNewNode = true);

			// Synchronizes node location data
			static void SynchronizeNodeLocation(FVector2D InLocation, Frontend::FNodeHandle InNodeHandle, UMetasoundEditorGraphNode& InNode);

			// Adds an output node to the editor graph that corresponds to the provided node handle.
			static UMetasoundEditorGraphOutputNode* AddOutputNode(UObject& InMetaSound, Frontend::FNodeHandle& InNodeHandle, FVector2D InLocation, bool bInSelectNewNode = true);

			// Generates analogous FNodeHandle for the given internal node data. Does not bind nor create EdGraph representation of given node.
			static Frontend::FNodeHandle AddOutputNodeHandle(UObject& InMetaSound, const FName InTypeName, const FText& InToolTip, const FName* InNameBase = nullptr);

			// Create a unique name for the variable.
			static FName GenerateUniqueVariableName(const Frontend::FConstGraphHandle& InFrontendGraph, const FString& InBaseName);

			// Adds a frontend variable to the root graph of the MetaSound
			//
			// @param InMetaSound - FMetasoundAssetBase derived UObject.
			// @param InTypeName - Data type of variable.
			//
			// @return The added frontend variable handle. On error, the returned handle is invalid.
			static Frontend::FVariableHandle AddVariableHandle(UObject& InMetaSound, const FName& InTypeName);

			// Adds a frontend variable node to root graph using the supplied node class name.
			//
			// @param InMetaSound - FMetasoundAssetBase derived UObject.
			// @param InVariableID - ID of variable existing on the root graph.
			// @param InVariableNodeClassName - FNodeClassName of the variable node to add.
			//
			// @return The added frontend node handle. On error, the returned handle is invalid.
			static Frontend::FNodeHandle AddVariableNodeHandle(UObject& InMetaSound, const FGuid& InVariableID, const Metasound::FNodeClassName& InVariableNodeClassName);

			// Attempts to connect Frontend node counterparts together for provided pins.  Returns true if succeeded,
			// and breaks pin link and returns false if failed.  If bConnectEdPins is set, will attempt to connect
			// the Editor Graph representation of the pins.
			static bool ConnectNodes(UEdGraphPin& InInputPin, UEdGraphPin& InOutputPin, bool bInConnectEdPins);

			// Disconnects pin's associated frontend vertex from any linked input
			// or output nodes, and reflects change in the Frontend graph. Does *not*
			// disconnect the EdGraph pins.
			static void DisconnectPinVertex(UEdGraphPin& InPin, bool bAddLiteralInputs = true);

			// Generates a unique output name for the given MetaSound object
			static FName GenerateUniqueNameByClassType(const UObject& InMetaSound, EMetasoundFrontendClassType InClassType, const FString& InBaseName);

			// Whether or not associated editor graph is in an error state or not.
			static bool GraphContainsErrors(const UObject& InMetaSound);

			static TArray<FString> GetDataTypeNameCategories(const FName& InDataTypeName);

			// Get the input handle from an input pin.  Ensures pin is an input pin.
			// TODO: use IDs to connect rather than names. Likely need an UMetasoundEditorGraphPin
			static Frontend::FInputHandle GetInputHandleFromPin(const UEdGraphPin* InPin);
			static Frontend::FConstInputHandle GetConstInputHandleFromPin(const UEdGraphPin* InPin);

			// Get the output handle from an output pin.  Ensures pin is an output pin.
			// TODO: use IDs to connect rather than names. Likely need an UMetasoundEditorGraphPin
			static Frontend::FOutputHandle GetOutputHandleFromPin(const UEdGraphPin* InPin);
			static Frontend::FConstOutputHandle GetConstOutputHandleFromPin(const UEdGraphPin* InPin);

			// Returns the default literal stored on the respective Frontend Node's Input.
			static bool GetPinLiteral(UEdGraphPin& InInputPin, FMetasoundFrontendLiteral& OutLiteralDefault);

			// Retrieves the proper pin color for the given PinType
			static FLinearColor GetPinCategoryColor(const FEdGraphPinType& PinType);

			// Initializes MetaSound with default inputs & outputs.
			static void InitMetaSound(UObject& InMetaSound, const FString& InAuthor);

			// Initializes a MetaSound Preset using the provided ReferencedMetaSound asset's
			// root graph as the sole, encapsulated topology.
			static void InitMetaSoundPreset(UObject& InMetaSoundReferenced, UObject& InMetaSoundPreset);

			// Rebuilds all editor node pins based on the provided node handle's class definition.
			static void RebuildNodePins(UMetasoundEditorGraphNode& InGraphNode);

			// Deletes both the editor graph & frontend nodes from respective graphs
			static bool DeleteNode(UEdGraphNode& InNode);

			// Adds an Input UEdGraphPin to a UMetasoundEditorGraphNode
			static UEdGraphPin* AddPinToNode(UMetasoundEditorGraphNode& InEditorNode, Frontend::FConstInputHandle InInputHandle);

			// Adds an Output UEdGraphPin to a UMetasoundEditorGraphNode
			static UEdGraphPin* AddPinToNode(UMetasoundEditorGraphNode& InEditorNode, Frontend::FConstOutputHandle InOutputHandle);

			// Refreshes pin state from class FrontendClassVertexMetadata
			static void RefreshPinMetadata(UEdGraphPin& InPin, const FMetasoundFrontendVertexMetadata& InMetadata);

			// Adds and removes nodes, pins and connections so that the UEdGraph of the MetaSound matches the FMetasoundFrontendDocumentModel
			//
			// @return True if the UEdGraph is synchronized and is in valid state, false otherwise.
			static bool SynchronizeGraph(UObject& InMetaSound);

			// Synchronizes editor nodes with frontend nodes, removing editor nodes that are not represented in the frontend, and adding editor nodes to represent missing frontend nodes.
			//
			// @return True if the UMetasoundEditorGraphNode was altered. False otherwise.
			static bool SynchronizeNodes(UObject& InMetaSound);

			// Synchronizes and reports to log whether or not an editor member node's associated FrontendNode ID has changed and therefore been updated through node versioning.
			//
			// @return True if the UMetasoundEditorGraphNode was altered. False otherwise.
			static bool SynchronizeNodeMembers(UObject& InMetaSound);

			// Adds and removes pins so that the UMetasoundEditorGraphNode matches the InNode.
			//
			// @return True if the UMetasoundEditorGraphNode was altered. False otherwise.
			static bool SynchronizeNodePins(UMetasoundEditorGraphNode& InEditorNode, Frontend::FConstNodeHandle InNode, bool bRemoveUnusedPins = true, bool bLogChanges = true);

			// Adds and removes connections so that the UEdGraph of the MetaSound has the same
			// connections as the FMetasoundFrontendDocument graph.
			//
			// @return True if the UEdGraph was altered. False otherwise.
			static bool SynchronizeConnections(UObject& InMetaSound);

			// Synchronizes literal for a given input with the EdGraph's pin value.
			static bool SynchronizePinLiteral(UEdGraphPin& InPin);

			// Synchronizes pin type for a given pin with that registered with the MetaSound editor module provided.
			static bool SynchronizePinType(const IMetasoundEditorModule& InEditorModule, UEdGraphPin& InPin, const FName InDataType);

			// Synchronizes inputs and outputs for the given MetaSound.
			//
			// @return True if the UEdGraph was altered. False otherwise.
			static bool SynchronizeGraphVertices(UObject& InMetaSound);

			// Returns true if the FInputHandle and UEdGraphPin match each other.
			static bool IsMatchingInputHandleAndPin(const Frontend::FConstInputHandle& InInputHandle, const UEdGraphPin& InEditorPin);

			// Returns true if the FOutputHandle and UEdGraphPin match each other.
			static bool IsMatchingOutputHandleAndPin(const Frontend::FConstOutputHandle& InOutputHandle, const UEdGraphPin& InEditorPin);

			// Function signature for visiting a node doing depth first traversal.
			//
			// Functions accept a UEdGraphNode* and return a TSet<UEdGraphNode*> which
			// represent all the children of the node. 
			using FDepthFirstVisitFunction = TFunctionRef<TSet<UEdGraphNode*> (UEdGraphNode*)>;

			// Traverse depth first starting at the InInitialNode and calling the InVisitFunction
			// for each node. 
			//
			// This implementation avoids recursive function calls to support deep
			// graphs.
			static void DepthFirstTraversal(UEdGraphNode* InInitialNode, FDepthFirstVisitFunction InVisitFunction);
		};
	} // namespace Editor
} // namespace Metasound
