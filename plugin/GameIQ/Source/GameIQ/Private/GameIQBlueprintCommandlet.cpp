// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQBlueprintCommandlet.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameIQJson.h"
#include "HAL/FileManager.h"
#include "K2Node_CallFunction.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQBlueprints, Log, All);

namespace
{
	const TCHAR* BlueprintProducer = TEXT("gameiq-ue-bp@0.1.0");
	constexpr int32 MaxNodesPerGraph = 400; // guard against pathological graphs

	FString OneLine(const FString& In)
	{
		return In.Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" ")).TrimStartAndEnd();
	}

	FString NodeTitle(const UEdGraphNode* Node)
	{
		return OneLine(Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
	}

	/** Render one graph as indented pseudocode: each node, then its exec successors. */
	FString RenderGraph(const UEdGraph* Graph)
	{
		FString Out;
		int32 Count = 0;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) { continue; }
			if (++Count > MaxNodesPerGraph) { Out += TEXT("  … (truncated)\n"); break; }

			Out += FString::Printf(TEXT("- %s\n"), *NodeTitle(Node));
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output) { continue; }
				if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) { continue; }
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (Linked && Linked->GetOwningNode())
					{
						Out += FString::Printf(TEXT("    -> %s\n"), *NodeTitle(Linked->GetOwningNode()));
					}
				}
			}
		}
		return Out;
	}
}

UGameIQBlueprintsCommandlet::UGameIQBlueprintsCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = true;
}

int32 UGameIQBlueprintsCommandlet::Main(const FString& Params)
{
	FString OutDir;
	if (!FParse::Value(*Params, TEXT("out="), OutDir))
	{
		OutDir = FPaths::Combine(FPaths::ProjectDir(), TEXT(".gameiq"), TEXT("extract"));
	}
	IFileManager::Get().MakeDirectory(*OutDir, /*Tree=*/true);

	FAssetRegistryModule& ARM =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AR = ARM.Get();
	AR.SearchAllAssets(/*bSynchronous=*/true);

	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true;
	Filter.PackagePaths.Add(FName(TEXT("/Game")));
	Filter.bRecursivePaths = true;

	TArray<FAssetData> BlueprintAssets;
	AR.GetAssets(Filter, BlueprintAssets);

	TArray<TSharedPtr<FJsonValue>> Entities;
	TArray<TSharedPtr<FJsonValue>> Edges;
	TArray<TSharedPtr<FJsonValue>> Chunks;

	int32 Rendered = 0;
	for (const FAssetData& Data : BlueprintAssets)
	{
		UBlueprint* BP = Cast<UBlueprint>(Data.GetAsset());
		if (!BP) { continue; }

		const FString Package = Data.PackageName.ToString();
		const FString BpId = FString::Printf(TEXT("asset:%s"), *Package);

		// Gather graphs: event graphs (ubergraph pages) + function graphs.
		TArray<UEdGraph*> Graphs;
		Graphs.Append(BP->UbergraphPages);
		Graphs.Append(BP->FunctionGraphs);

		for (const UEdGraph* Graph : Graphs)
		{
			if (!Graph) { continue; }
			const FString GraphName = Graph->GetName();
			const FString Pseudo = RenderGraph(Graph);
			if (Pseudo.IsEmpty()) { continue; }

			// a bp-function entity per graph, parented to the blueprint
			const FString FnId = FString::Printf(TEXT("%s::%s"), *BpId, *GraphName);
			Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
				FnId, TEXT("bp-function"), GraphName, Package, TEXT("asset"), BpId,
				FString::Printf(TEXT("%s graph of %s"), *GraphName, *Data.AssetName.ToString()), nullptr)));

			Chunks.Add(MakeShared<FJsonValueObject>(GameIQ::MakeChunk(
				FString::Printf(TEXT("%s#pseudo"), *FnId), FnId, TEXT("bp-pseudocode"),
				FString::Printf(TEXT("%s :: %s\n%s"), *Data.AssetName.ToString(), *GraphName, *Pseudo))));

			// calls edges into C++ (and other BPs) from CallFunction nodes
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
				if (!Call) { continue; }
				const UFunction* Fn = Call->GetTargetFunction();
				if (!Fn) { continue; }
				const UClass* Owner = Fn->GetOwnerClass();
				if (!Owner) { continue; }
				// Owner->GetName() is the prefix-less reflection name — matches the C++ extractor's cpp: ids.
				Edges.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEdge(
					BpId, FString::Printf(TEXT("cpp:%s"), *Owner->GetName()), TEXT("calls"), Fn->GetName())));
			}
		}
		++Rendered;
	}

	if (!GameIQ::WriteOutput(OutDir, TEXT("blueprints.json"), BlueprintProducer, Entities, Edges, Chunks))
	{
		UE_LOG(LogGameIQBlueprints, Error, TEXT("Failed to write blueprints.json to %s"), *OutDir);
		return 1;
	}

	UE_LOG(LogGameIQBlueprints, Display,
		TEXT("Game IQ: rendered %d Blueprints — %d graph entities, %d call edges, %d pseudocode chunks to %s"),
		Rendered, Entities.Num(), Edges.Num(), Chunks.Num(), *FPaths::Combine(OutDir, TEXT("blueprints.json")));
	return 0;
}
