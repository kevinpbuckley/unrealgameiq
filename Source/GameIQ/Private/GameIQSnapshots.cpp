// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQSnapshots.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameIQQuery.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SQLiteDatabase.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQSnapshots, Log, All);

namespace
{
	struct FEntityRow
	{
		FString Id;
		FString Kind;
		FString Name;
		FString Path;
		FString Parent;
		FString Source;
		FString Summary;
		FString Detail;
		FString Authority;

		FString Fingerprint() const
		{
			// Exclude identity fields. A unique before/after match lets us report a rename/move
			// instead of a misleading delete+create pair.
			return Kind + TEXT("\x1f") + Source + TEXT("\x1f") + Summary + TEXT("\x1f") +
				Detail + TEXT("\x1f") + Authority;
		}
	};

	struct FEdgeRow
	{
		FString Src;
		FString Dst;
		FString Type;
		FString Attrs;

		FString Key() const { return Src + TEXT("\x1f") + Dst + TEXT("\x1f") + Type; }
	};

	struct FChunkRow
	{
		FString Id;
		FString EntityId;
		FString Kind;
		FString Text;
	};

	FString SerializeSnapshotJson(const TSharedRef<FJsonObject>& Root)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		return Out;
	}

	FString SnapshotError(const FString& Message)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("error"), Message);
		return SerializeSnapshotJson(Root);
	}

	FString Col(const FSQLitePreparedStatement& Stmt, const TCHAR* Name)
	{
		FString Value;
		Stmt.GetColumnValueByName(Name, Value);
		return Value;
	}

	bool IsSafeId(const FString& Id)
	{
		if (Id.IsEmpty()) { return false; }
		for (const TCHAR C : Id)
		{
			if (!(FChar::IsAlnum(C) || C == TEXT('-') || C == TEXT('_'))) { return false; }
		}
		return true;
	}

	FString SnapshotDir()
	{
		return FPaths::Combine(FPaths::GetPath(GameIQQuery::GetDbPathForServices()), TEXT("snapshots"));
	}

	FString ManifestPath(const FString& Id)
	{
		return FPaths::Combine(SnapshotDir(), Id + TEXT(".json"));
	}

	FString SnapshotDbPath(const FString& Id)
	{
		return FPaths::Combine(SnapshotDir(), Id + TEXT(".db"));
	}

	TSharedPtr<FJsonObject> ReadJsonObject(const FString& Path)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path)) { return nullptr; }
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Object) ? Object : nullptr;
	}

	bool WriteJsonObject(const FString& Path, const TSharedRef<FJsonObject>& Object)
	{
		const FString TempPath = Path + TEXT(".tmp");
		if (!FFileHelper::SaveStringToFile(SerializeSnapshotJson(Object), *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return false;
		}
		return IFileManager::Get().Move(*Path, *TempPath, true, true, false, true);
	}

	bool OpenReadOnly(const FString& Path, FSQLiteDatabase& Db, FString& OutError)
	{
		if (!FPaths::FileExists(Path))
		{
			OutError = FString::Printf(TEXT("database not found: %s"), *Path);
			return false;
		}
		if (!Db.Open(*Path, ESQLiteDatabaseOpenMode::ReadOnly))
		{
			OutError = FString::Printf(TEXT("failed to open %s: %s"), *Path, *Db.GetLastError());
			return false;
		}
		Db.Execute(TEXT("PRAGMA busy_timeout=2000;"));
		return true;
	}

	FString Meta(FSQLiteDatabase& Db, const FString& Key)
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT("SELECT value FROM meta WHERE key = ?"));
		if (!Stmt.IsValid()) { return FString(); }
		Stmt.SetBindingValueByIndex(1, Key);
		return Stmt.Step() == ESQLitePreparedStatementStepResult::Row ? Col(Stmt, TEXT("value")) : FString();
	}

	bool ResolveSnapshot(const FString& IdOrCurrent, FString& OutDbPath, TSharedPtr<FJsonObject>& OutManifest,
		FString& OutError)
	{
		if (IdOrCurrent.IsEmpty() || IdOrCurrent.Equals(TEXT("current"), ESearchCase::IgnoreCase))
		{
			OutDbPath = GameIQQuery::GetDbPathForServices();
			OutManifest = MakeShared<FJsonObject>();
			OutManifest->SetStringField(TEXT("id"), TEXT("current"));
			return true;
		}
		if (!IsSafeId(IdOrCurrent))
		{
			OutError = TEXT("snapshot id contains invalid characters");
			return false;
		}
		OutManifest = ReadJsonObject(ManifestPath(IdOrCurrent));
		if (!OutManifest.IsValid())
		{
			OutError = FString::Printf(TEXT("snapshot not found: %s"), *IdOrCurrent);
			return false;
		}
		OutDbPath = SnapshotDbPath(IdOrCurrent);
		if (!FPaths::FileExists(OutDbPath))
		{
			OutError = FString::Printf(TEXT("snapshot database is missing: %s"), *IdOrCurrent);
			return false;
		}
		return true;
	}

	TMap<FString, FEntityRow> LoadEntities(FSQLiteDatabase& Db)
	{
		TMap<FString, FEntityRow> Rows;
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT(
			"SELECT id,kind,name,path,COALESCE(parent,'') AS parent,source,COALESCE(summary,'') AS summary,"
			"COALESCE(detail,'') AS detail,COALESCE(authority,'extracted-fact') AS authority FROM entities ORDER BY id"));
		if (!Stmt.IsValid()) { return Rows; }
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FEntityRow Row;
			Row.Id = Col(Stmt, TEXT("id"));
			Row.Kind = Col(Stmt, TEXT("kind"));
			Row.Name = Col(Stmt, TEXT("name"));
			Row.Path = Col(Stmt, TEXT("path"));
			Row.Parent = Col(Stmt, TEXT("parent"));
			Row.Source = Col(Stmt, TEXT("source"));
			Row.Summary = Col(Stmt, TEXT("summary"));
			Row.Detail = Col(Stmt, TEXT("detail"));
			Row.Authority = Col(Stmt, TEXT("authority"));
			Rows.Add(Row.Id, MoveTemp(Row));
		}
		return Rows;
	}

	TMap<FString, FEdgeRow> LoadEdges(FSQLiteDatabase& Db)
	{
		TMap<FString, FEdgeRow> Rows;
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT(
			"SELECT src,dst,type,COALESCE(attrs,'') AS attrs FROM edges ORDER BY src,dst,type"));
		if (!Stmt.IsValid()) { return Rows; }
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FEdgeRow Row{ Col(Stmt, TEXT("src")), Col(Stmt, TEXT("dst")), Col(Stmt, TEXT("type")), Col(Stmt, TEXT("attrs")) };
			Rows.Add(Row.Key(), MoveTemp(Row));
		}
		return Rows;
	}

	TMap<FString, FChunkRow> LoadChunks(FSQLiteDatabase& Db)
	{
		TMap<FString, FChunkRow> Rows;
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT(
			"SELECT id,entity_id,kind,text FROM chunks ORDER BY id"));
		if (!Stmt.IsValid()) { return Rows; }
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FChunkRow Row{ Col(Stmt, TEXT("id")), Col(Stmt, TEXT("entity_id")), Col(Stmt, TEXT("kind")), Col(Stmt, TEXT("text")) };
			Rows.Add(Row.Id, MoveTemp(Row));
		}
		return Rows;
	}

	bool Matches(const FEntityRow* Row, const FString& Kind, const FString& PathPrefix)
	{
		if (!Row) { return Kind.IsEmpty() && PathPrefix.IsEmpty(); }
		return (Kind.IsEmpty() || Row->Kind.Equals(Kind, ESearchCase::IgnoreCase)) &&
			(PathPrefix.IsEmpty() || Row->Path.StartsWith(PathPrefix, ESearchCase::IgnoreCase) ||
			 Row->Id.StartsWith(TEXT("asset:") + PathPrefix, ESearchCase::IgnoreCase));
	}

	TSharedRef<FJsonObject> EntitySummary(const FEntityRow& Row)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Row.Id);
		Obj->SetStringField(TEXT("kind"), Row.Kind);
		Obj->SetStringField(TEXT("name"), Row.Name);
		Obj->SetStringField(TEXT("path"), Row.Path);
		return Obj;
	}

	FString Snip(const FString& Text, int32 Max = 2000)
	{
		return Text.Len() <= Max ? Text : Text.Left(Max) + TEXT("\n...[truncated]");
	}

	void AddChangedFields(const FEntityRow& Before, const FEntityRow& After, TSharedRef<FJsonObject>& Change)
	{
		TArray<TSharedPtr<FJsonValue>> Fields;
		auto Add = [&Fields](const TCHAR* Name) { Fields.Add(MakeShared<FJsonValueString>(Name)); };
		if (Before.Kind != After.Kind) { Add(TEXT("kind")); }
		if (Before.Name != After.Name) { Add(TEXT("name")); }
		if (Before.Path != After.Path) { Add(TEXT("path")); }
		if (Before.Parent != After.Parent) { Add(TEXT("parent")); }
		if (Before.Source != After.Source) { Add(TEXT("source")); }
		if (Before.Summary != After.Summary) { Add(TEXT("summary")); }
		if (Before.Detail != After.Detail) { Add(TEXT("detail")); }
		if (Before.Authority != After.Authority) { Add(TEXT("authority")); }
		Change->SetArrayField(TEXT("fields"), Fields);
	}

	FString SemanticEntityChangeType(const FString& Kind, const TCHAR* Operation)
	{
		const FString Lower = Kind.ToLower();
		if (Lower == TEXT("bp-variable")) { return FString::Printf(TEXT("blueprint-variable-%s"), Operation); }
		if (Lower == TEXT("bp-component")) { return FString::Printf(TEXT("blueprint-component-%s"), Operation); }
		if (Lower == TEXT("bp-function")) { return FString::Printf(TEXT("blueprint-function-%s"), Operation); }
		if (FCString::Strcmp(Operation, TEXT("changed")) == 0 && Lower.Contains(TEXT("blueprint")))
		{
			return TEXT("blueprint-structure-changed");
		}
		return FString(Operation) == TEXT("changed") ? TEXT("entity-modified") : FString(Operation);
	}
}

FString GameIQSnapshots::Create(const FString& Name, const FString& MetadataJson)
{
	const FString SourcePath = GameIQQuery::GetDbPathForServices();
	FSQLiteDatabase Db;
	FString OpenError;
	if (!OpenReadOnly(SourcePath, Db, OpenError)) { return SnapshotError(OpenError); }
	ON_SCOPE_EXIT { Db.Close(); };

	IFileManager::Get().MakeDirectory(*SnapshotDir(), true);
	const FString Id = FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ")) + TEXT("-") +
		FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString DbPath = SnapshotDbPath(Id);
	FString EscapedPath = DbPath;
	EscapedPath.ReplaceInline(TEXT("'"), TEXT("''"));
	const FString Sql = FString::Printf(TEXT("VACUUM INTO '%s';"), *EscapedPath);
	if (!Db.Execute(*Sql))
	{
		return SnapshotError(FString::Printf(TEXT("failed to create consistent snapshot: %s"), *Db.GetLastError()));
	}

	TSharedPtr<FJsonObject> Metadata;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MetadataJson.IsEmpty() ? TEXT("{}") : MetadataJson);
	if (!FJsonSerializer::Deserialize(Reader, Metadata) || !Metadata.IsValid())
	{
		IFileManager::Get().Delete(*DbPath, false, true, true);
		return SnapshotError(TEXT("metadata_json must be a JSON object"));
	}

	TSharedRef<FJsonObject> Manifest = MakeShared<FJsonObject>();
	Manifest->SetStringField(TEXT("id"), Id);
	Manifest->SetStringField(TEXT("name"), Name.IsEmpty() ? Id : Name);
	Manifest->SetStringField(TEXT("createdAtIso"), FDateTime::UtcNow().ToIso8601());
	Manifest->SetStringField(TEXT("sourceLastIngestAtIso"), Meta(Db, TEXT("lastIngestAtIso")));
	Manifest->SetStringField(TEXT("database"), FPaths::GetCleanFilename(DbPath));
	Manifest->SetObjectField(TEXT("metadata"), Metadata.ToSharedRef());
	if (!WriteJsonObject(ManifestPath(Id), Manifest))
	{
		IFileManager::Get().Delete(*DbPath, false, true, true);
		return SnapshotError(TEXT("snapshot database was created but its manifest could not be written"));
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("result"), Manifest);
	return SerializeSnapshotJson(Root);
}

FString GameIQSnapshots::List(int32 Limit)
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(SnapshotDir(), TEXT("*.json")), true, false);
	Files.Sort([](const FString& A, const FString& B) { return A > B; });
	const int32 Cap = FMath::Clamp(Limit, 1, 500);
	TArray<TSharedPtr<FJsonValue>> Items;
	for (int32 Index = 0; Index < Files.Num() && Items.Num() < Cap; ++Index)
	{
		const TSharedPtr<FJsonObject> Manifest = ReadJsonObject(FPaths::Combine(SnapshotDir(), Files[Index]));
		if (Manifest.IsValid()) { Items.Add(MakeShared<FJsonValueObject>(Manifest.ToSharedRef())); }
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("snapshots"), Items);
	Result->SetNumberField(TEXT("total"), Files.Num());
	Result->SetBoolField(TEXT("truncated"), Files.Num() > Items.Num());
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("result"), Result);
	return SerializeSnapshotJson(Root);
}

FString GameIQSnapshots::Remove(const FString& Id)
{
	if (!IsSafeId(Id)) { return SnapshotError(TEXT("snapshot id contains invalid characters")); }
	const bool bHadManifest = FPaths::FileExists(ManifestPath(Id));
	const bool bHadDb = FPaths::FileExists(SnapshotDbPath(Id));
	if (!bHadManifest && !bHadDb) { return SnapshotError(FString::Printf(TEXT("snapshot not found: %s"), *Id)); }
	const bool bManifestDeleted = !bHadManifest || IFileManager::Get().Delete(*ManifestPath(Id), false, true, true);
	const bool bDbDeleted = !bHadDb || IFileManager::Get().Delete(*SnapshotDbPath(Id), false, true, true);
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("id"), Id);
	Result->SetBoolField(TEXT("removed"), bManifestDeleted && bDbDeleted);
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("result"), Result);
	return SerializeSnapshotJson(Root);
}

FString GameIQSnapshots::Changes(const FString& SinceSnapshot, const FString& UntilSnapshot,
	const FString& Kind, const FString& PathPrefix, int32 Limit, int32 Offset)
{
	FString BeforePath, AfterPath, ResolveError;
	TSharedPtr<FJsonObject> BeforeManifest, AfterManifest;
	if (!ResolveSnapshot(SinceSnapshot, BeforePath, BeforeManifest, ResolveError)) { return SnapshotError(ResolveError); }
	if (!ResolveSnapshot(UntilSnapshot, AfterPath, AfterManifest, ResolveError)) { return SnapshotError(ResolveError); }

	FSQLiteDatabase BeforeDb, AfterDb;
	FString OpenError;
	if (!OpenReadOnly(BeforePath, BeforeDb, OpenError)) { return SnapshotError(OpenError); }
	ON_SCOPE_EXIT { BeforeDb.Close(); };
	if (!OpenReadOnly(AfterPath, AfterDb, OpenError)) { return SnapshotError(OpenError); }
	ON_SCOPE_EXIT { AfterDb.Close(); };

	const TMap<FString, FEntityRow> BeforeEntities = LoadEntities(BeforeDb);
	const TMap<FString, FEntityRow> AfterEntities = LoadEntities(AfterDb);
	const TMap<FString, FEdgeRow> BeforeEdges = LoadEdges(BeforeDb);
	const TMap<FString, FEdgeRow> AfterEdges = LoadEdges(AfterDb);
	const TMap<FString, FChunkRow> BeforeChunks = LoadChunks(BeforeDb);
	const TMap<FString, FChunkRow> AfterChunks = LoadChunks(AfterDb);

	TMap<FString, TArray<FString>> DeletedByFingerprint, CreatedByFingerprint;
	for (const TPair<FString, FEntityRow>& Pair : BeforeEntities)
	{
		if (!AfterEntities.Contains(Pair.Key)) { DeletedByFingerprint.FindOrAdd(Pair.Value.Fingerprint()).Add(Pair.Key); }
	}
	for (const TPair<FString, FEntityRow>& Pair : AfterEntities)
	{
		if (!BeforeEntities.Contains(Pair.Key)) { CreatedByFingerprint.FindOrAdd(Pair.Value.Fingerprint()).Add(Pair.Key); }
	}

	TSet<FString> RenameDeleted, RenameCreated;
	TArray<TSharedPtr<FJsonValue>> AllChanges;
	for (const TPair<FString, TArray<FString>>& Pair : DeletedByFingerprint)
	{
		const TArray<FString>* Created = CreatedByFingerprint.Find(Pair.Key);
		if (!Created || Pair.Value.Num() != 1 || Created->Num() != 1) { continue; }
		const FEntityRow& Before = BeforeEntities.FindChecked(Pair.Value[0]);
		const FEntityRow& After = AfterEntities.FindChecked((*Created)[0]);
		if (!Matches(&After, Kind, PathPrefix) && !Matches(&Before, Kind, PathPrefix)) { continue; }
		TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
		Change->SetStringField(TEXT("type"), TEXT("renamed"));
		Change->SetObjectField(TEXT("before"), EntitySummary(Before));
		Change->SetObjectField(TEXT("after"), EntitySummary(After));
		AllChanges.Add(MakeShared<FJsonValueObject>(Change));
		RenameDeleted.Add(Before.Id);
		RenameCreated.Add(After.Id);
	}

	for (const TPair<FString, FEntityRow>& Pair : BeforeEntities)
	{
		const FEntityRow* After = AfterEntities.Find(Pair.Key);
		if (!After)
		{
			if (!RenameDeleted.Contains(Pair.Key) && Matches(&Pair.Value, Kind, PathPrefix))
			{
				TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
				Change->SetStringField(TEXT("type"), SemanticEntityChangeType(Pair.Value.Kind, TEXT("deleted")));
				Change->SetObjectField(TEXT("entity"), EntitySummary(Pair.Value));
				AllChanges.Add(MakeShared<FJsonValueObject>(Change));
			}
			continue;
		}
		if (Pair.Value.Fingerprint() != After->Fingerprint() || Pair.Value.Name != After->Name ||
			Pair.Value.Path != After->Path || Pair.Value.Parent != After->Parent)
		{
			if (!Matches(After, Kind, PathPrefix) && !Matches(&Pair.Value, Kind, PathPrefix)) { continue; }
			TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("type"), SemanticEntityChangeType(Pair.Value.Kind, TEXT("changed")));
			Change->SetObjectField(TEXT("before"), EntitySummary(Pair.Value));
			Change->SetObjectField(TEXT("after"), EntitySummary(*After));
			AddChangedFields(Pair.Value, *After, Change);
			AllChanges.Add(MakeShared<FJsonValueObject>(Change));
		}
	}
	for (const TPair<FString, FEntityRow>& Pair : AfterEntities)
	{
		if (!BeforeEntities.Contains(Pair.Key) && !RenameCreated.Contains(Pair.Key) && Matches(&Pair.Value, Kind, PathPrefix))
		{
			TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("type"), SemanticEntityChangeType(Pair.Value.Kind, TEXT("created")));
			Change->SetObjectField(TEXT("entity"), EntitySummary(Pair.Value));
			AllChanges.Add(MakeShared<FJsonValueObject>(Change));
		}
	}

	auto AddEdgeChange = [&](const FEdgeRow& Edge, const TCHAR* Type)
	{
		const FEntityRow* Context = AfterEntities.Find(Edge.Src);
		if (!Context) { Context = BeforeEntities.Find(Edge.Src); }
		if (!Matches(Context, Kind, PathPrefix))
		{
			const FEntityRow* Other = AfterEntities.Find(Edge.Dst);
			if (!Other) { Other = BeforeEntities.Find(Edge.Dst); }
			if (!Matches(Other, Kind, PathPrefix)) { return; }
		}
		TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
		Change->SetStringField(TEXT("type"), Type);
		Change->SetStringField(TEXT("src"), Edge.Src);
		Change->SetStringField(TEXT("dst"), Edge.Dst);
		Change->SetStringField(TEXT("edgeType"), Edge.Type);
		Change->SetStringField(TEXT("attrs"), Edge.Attrs);
		AllChanges.Add(MakeShared<FJsonValueObject>(Change));
	};
	for (const TPair<FString, FEdgeRow>& Pair : BeforeEdges)
	{
		const FEdgeRow* After = AfterEdges.Find(Pair.Key);
		if (!After) { AddEdgeChange(Pair.Value, TEXT("relationship-removed")); }
		else if (After->Attrs != Pair.Value.Attrs)
		{
			AddEdgeChange(Pair.Value, TEXT("relationship-metadata-changed"));
		}
	}
	for (const TPair<FString, FEdgeRow>& Pair : AfterEdges)
	{
		if (!BeforeEdges.Contains(Pair.Key)) { AddEdgeChange(Pair.Value, TEXT("relationship-added")); }
	}

	for (const TPair<FString, FChunkRow>& Pair : BeforeChunks)
	{
		const FChunkRow* After = AfterChunks.Find(Pair.Key);
		if (!After)
		{
			const FEntityRow* Entity = BeforeEntities.Find(Pair.Value.EntityId);
			if (!Matches(Entity, Kind, PathPrefix)) { continue; }
			TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("type"), Entity && Entity->Kind.Contains(TEXT("blueprint"))
				? TEXT("blueprint-logic-removed") : TEXT("content-removed"));
			Change->SetStringField(TEXT("entityId"), Pair.Value.EntityId);
			Change->SetStringField(TEXT("chunkId"), Pair.Value.Id);
			Change->SetStringField(TEXT("chunkKind"), Pair.Value.Kind);
			Change->SetStringField(TEXT("before"), Snip(Pair.Value.Text));
			AllChanges.Add(MakeShared<FJsonValueObject>(Change));
			continue;
		}
		if (After->Text == Pair.Value.Text) { continue; }
		const FEntityRow* Entity = AfterEntities.Find(After->EntityId);
		if (!Entity) { Entity = BeforeEntities.Find(Pair.Value.EntityId); }
		if (!Matches(Entity, Kind, PathPrefix)) { continue; }
		TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
		Change->SetStringField(TEXT("type"), Entity && Entity->Kind.Contains(TEXT("blueprint"))
			? TEXT("blueprint-logic-changed") : TEXT("content-changed"));
		Change->SetStringField(TEXT("entityId"), After->EntityId);
		Change->SetStringField(TEXT("chunkId"), After->Id);
		Change->SetStringField(TEXT("chunkKind"), After->Kind);
		Change->SetStringField(TEXT("before"), Snip(Pair.Value.Text));
		Change->SetStringField(TEXT("after"), Snip(After->Text));
		AllChanges.Add(MakeShared<FJsonValueObject>(Change));
	}
	for (const TPair<FString, FChunkRow>& Pair : AfterChunks)
	{
		if (BeforeChunks.Contains(Pair.Key)) { continue; }
		const FEntityRow* Entity = AfterEntities.Find(Pair.Value.EntityId);
		if (!Matches(Entity, Kind, PathPrefix)) { continue; }
		TSharedRef<FJsonObject> Change = MakeShared<FJsonObject>();
		Change->SetStringField(TEXT("type"), Entity && Entity->Kind.Contains(TEXT("blueprint"))
			? TEXT("blueprint-logic-added") : TEXT("content-added"));
		Change->SetStringField(TEXT("entityId"), Pair.Value.EntityId);
		Change->SetStringField(TEXT("chunkId"), Pair.Value.Id);
		Change->SetStringField(TEXT("chunkKind"), Pair.Value.Kind);
		Change->SetStringField(TEXT("after"), Snip(Pair.Value.Text));
		AllChanges.Add(MakeShared<FJsonValueObject>(Change));
	}

	AllChanges.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		return SerializeSnapshotJson(A->AsObject().ToSharedRef()) < SerializeSnapshotJson(B->AsObject().ToSharedRef());
	});
	const int32 Start = FMath::Clamp(Offset, 0, AllChanges.Num());
	const int32 Cap = FMath::Clamp(Limit, 1, 1000);
	TArray<TSharedPtr<FJsonValue>> Page;
	for (int32 Index = Start; Index < AllChanges.Num() && Page.Num() < Cap; ++Index) { Page.Add(AllChanges[Index]); }

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetObjectField(TEXT("since"), BeforeManifest.ToSharedRef());
	Result->SetObjectField(TEXT("until"), AfterManifest.ToSharedRef());
	Result->SetStringField(TEXT("sinceLastIngestAtIso"), Meta(BeforeDb, TEXT("lastIngestAtIso")));
	Result->SetStringField(TEXT("untilLastIngestAtIso"), Meta(AfterDb, TEXT("lastIngestAtIso")));
	Result->SetArrayField(TEXT("changes"), Page);
	Result->SetNumberField(TEXT("total"), AllChanges.Num());
	Result->SetNumberField(TEXT("offset"), Start);
	Result->SetBoolField(TEXT("truncated"), Start + Page.Num() < AllChanges.Num());
	Result->SetStringField(TEXT("provenance"), TEXT("semantic comparison of GameIQ index entities, edges, and chunks"));
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("result"), Result);
	return SerializeSnapshotJson(Root);
}
