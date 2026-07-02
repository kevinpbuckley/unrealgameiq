// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQLinkCommandlet.h"

#include "Dom/JsonObject.h"
#include "GameIQJson.h"
#include "GameIQStore.h"
#include "Internationalization/Regex.h"
#include "Misc/Paths.h"
#include "SQLiteDatabase.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQLink, Log, All);

namespace
{
	const TCHAR* LinkProducer = TEXT("gameiq-doc-links@0.1.0");

	FString LinkIndexDbPath()
	{
		const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		return FPaths::Combine(Root, TEXT(".gameiq"), TEXT("index.db"));
	}

	struct FCandidate { FString Id; FString Kind; };
	struct FIntent { FString Id; FString Kind; FString Text; }; // doc-section or image (stated intent)

	/**
	 * A "distinctive" name is safe to match inside prose without flooding false positives: it carries an
	 * asset-style underscore prefix (BP_/IA_/M_/SM_/T_…) or is multi-word CamelCase (DirectionalLight).
	 * Plain English words (Health, Ammo, Overview) are rejected so they don't match every doc.
	 */
	bool IsDistinctive(const FString& Name)
	{
		if (Name.Len() < 5) { return false; }
		if (Name.Contains(TEXT("_"))) { return true; }
		int32 Upper = 0, Lower = 0;
		for (const TCHAR C : Name)
		{
			if (FChar::IsUpper(C)) { ++Upper; }
			else if (FChar::IsLower(C)) { ++Lower; }
		}
		return Upper >= 2 && Lower >= 1; // CamelCase compound
	}

	/** Split text into alnum/underscore word tokens. */
	TSet<FString> WordTokens(const FString& Text)
	{
		TSet<FString> Out;
		FString Cur;
		for (const TCHAR C : Text)
		{
			if (FChar::IsAlnum(C) || C == TEXT('_')) { Cur.AppendChar(C); }
			else if (Cur.Len() > 0) { Out.Add(Cur); Cur.Reset(); }
		}
		if (Cur.Len() > 0) { Out.Add(Cur); }
		return Out;
	}
}

UGameIQLinkCommandlet::UGameIQLinkCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 UGameIQLinkCommandlet::Main(const FString& /*Params*/)
{
	const FString Path = LinkIndexDbPath();
	if (!FPaths::FileExists(Path))
	{
		UE_LOG(LogGameIQLink, Warning, TEXT("Game IQ link: no index at %s — run the index first."), *Path);
		return 1;
	}

	// --- read phase: intent sources (doc sections + images) + candidate implementation entities ---
	TArray<FIntent> Intents;
	TArray<FCandidate> Candidates;
	TArray<FString> BrandDocIds;                // brand-guideline documents (for brand-image links)
	TMap<FString, TArray<FString>> NameToIds;   // distinctive name -> entity ids (for inferred matches)
	{
		FSQLiteDatabase Db;
		if (!Db.Open(*Path, ESQLiteDatabaseOpenMode::ReadOnly) && !Db.Open(*Path, ESQLiteDatabaseOpenMode::ReadWrite))
		{
			UE_LOG(LogGameIQLink, Warning, TEXT("Game IQ link: cannot open index: %s"), *Db.GetLastError());
			return 1;
		}

		{
			FSQLitePreparedStatement S = Db.PrepareStatement(
				TEXT("SELECT e.id AS id, e.kind AS kind, IFNULL(c.text,'') AS text FROM entities e "
				     "LEFT JOIN chunks c ON c.entity_id = e.id "
				     "WHERE e.kind IN ('doc-section','image')"));
			if (S.IsValid())
			{
				while (S.Step() == ESQLitePreparedStatementStepResult::Row)
				{
					FIntent It;
					S.GetColumnValueByName(TEXT("id"), It.Id);
					S.GetColumnValueByName(TEXT("kind"), It.Kind);
					S.GetColumnValueByName(TEXT("text"), It.Text);
					Intents.Add(MoveTemp(It));
				}
			}
		}
		{
			// Brand-guideline documents, so brand/logo images can link to them.
			FSQLitePreparedStatement S = Db.PrepareStatement(
				TEXT("SELECT id FROM entities WHERE kind='document' AND json_extract(detail,'$.docType')='brand-guidelines'"));
			if (S.IsValid())
			{
				while (S.Step() == ESQLitePreparedStatementStepResult::Row)
				{
					FString Id; S.GetColumnValueByName(TEXT("id"), Id); BrandDocIds.Add(Id);
				}
			}
		}
		{
			// Ground-truth entities only; never link intent→intent. Skip blueprint sub-parts' noisiest
			// kinds later via the ambiguity cap.
			FSQLitePreparedStatement S = Db.PrepareStatement(
				TEXT("SELECT id, name, kind FROM entities WHERE authority = 'extracted-fact'"));
			if (S.IsValid())
			{
				while (S.Step() == ESQLitePreparedStatementStepResult::Row)
				{
					FString Id, Name, Kind;
					S.GetColumnValueByName(TEXT("id"), Id);
					S.GetColumnValueByName(TEXT("name"), Name);
					S.GetColumnValueByName(TEXT("kind"), Kind);
					Candidates.Add(FCandidate{ Id, Kind });
					if (IsDistinctive(Name)) { NameToIds.FindOrAdd(Name).Add(Id); }
				}
			}
		}
		Db.Close();
	}

	// Fast existence set for explicit id matches.
	TSet<FString> AllIds;
	for (const FCandidate& C : Candidates) { AllIds.Add(C.Id); }

	// --- compute intent→implementation edges (doc-section 'describes', image 'illustrates') ---
	TArray<TSharedPtr<FJsonValue>> Edges;
	const FRegexPattern IdPattern(TEXT("(asset:[^\\s\\)\\]]+|cpp:[A-Za-z0-9_:]+|config:[^\\s\\)\\]]+)"));
	int32 Explicit = 0, Inferred = 0, Illustrates = 0;

	auto AddEdge = [&Edges](const FString& Src, const FString& Dst, const FString& Type,
		const FString& Confidence, const FString& Match)
	{
		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("src"), Src);
		E->SetStringField(TEXT("dst"), Dst);
		E->SetStringField(TEXT("type"), Type);
		TSharedRef<FJsonObject> Attrs = MakeShared<FJsonObject>();
		Attrs->SetStringField(TEXT("confidence"), Confidence);
		Attrs->SetStringField(TEXT("via"), Match);
		E->SetObjectField(TEXT("attrs"), Attrs);
		Edges.Add(MakeShared<FJsonValueObject>(E));
	};

	for (const FIntent& It : Intents)
	{
		const bool bImage = It.Kind == TEXT("image");
		const FString EdgeType = bImage ? TEXT("illustrates") : TEXT("describes");
		TSet<FString> Linked; // dedupe per source

		// Explicit: verbatim entity ids mentioned (prose only; images rarely carry ids).
		if (!bImage)
		{
			FRegexMatcher M(IdPattern, It.Text);
			while (M.FindNext())
			{
				FString Id = It.Text.Mid(M.GetMatchBeginning(), M.GetMatchEnding() - M.GetMatchBeginning());
				Id.TrimEndInline();
				if (AllIds.Contains(Id) && !Linked.Contains(Id))
				{
					Linked.Add(Id); AddEdge(It.Id, Id, EdgeType, TEXT("explicit"), Id); ++Explicit;
				}
			}
		}

		// Inferred: distinctive names appearing as whole words, only when unambiguous (≤3 targets).
		const TSet<FString> Words = WordTokens(It.Text);
		for (const FString& W : Words)
		{
			const TArray<FString>* Ids = NameToIds.Find(W);
			if (!Ids || Ids->Num() == 0 || Ids->Num() > 3) { continue; }
			for (const FString& Id : *Ids)
			{
				if (!Linked.Contains(Id))
				{
					Linked.Add(Id);
					AddEdge(It.Id, Id, EdgeType, TEXT("inferred"), W);
					bImage ? ++Illustrates : ++Inferred;
				}
			}
		}

		// Brand/logo imagery → the brand-guidelines document(s).
		if (bImage)
		{
			const FString Lower = It.Text.ToLower();
			if (Lower.Contains(TEXT("brand")) || Lower.Contains(TEXT("logo")))
			{
				for (const FString& DocId : BrandDocIds)
				{
					if (!Linked.Contains(DocId)) { Linked.Add(DocId); AddEdge(It.Id, DocId, TEXT("illustrates"), TEXT("inferred"), TEXT("brand")); ++Illustrates; }
				}
			}
		}
	}

	// --- ingest as its own producer (replaces previous link edges) ---
	FGameIQStore Store;
	if (!Store.Open())
	{
		UE_LOG(LogGameIQLink, Error, TEXT("Game IQ link: could not open index for write."));
		return 1;
	}
	static const TArray<TSharedPtr<FJsonValue>> None;
	Store.IngestProducer(LinkProducer, None, Edges, None);
	Store.Close();

	UE_LOG(LogGameIQLink, Display,
		TEXT("Game IQ link: %d intent sources × %d candidates → %d edges "
		     "(%d describes-explicit, %d describes-inferred, %d illustrates)."),
		Intents.Num(), Candidates.Num(), Edges.Num(), Explicit, Inferred, Illustrates);
	return 0;
}
