// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SQLitePreparedStatement.h"

/**
 * Binding helpers that behave the same on UE 5.8 and 5.9.
 *
 * UE 5.9's FSQLitePreparedStatement binds an FString through FStringView. An empty, unallocated
 * FString has a null data pointer, and sqlite3_bind_text(nullptr) binds SQL NULL (5.8 bound '').
 * Every `?N = ''` "no filter" test then evaluates NULL, so Search, References and Children
 * returned nothing, and NOT NULL text columns could reject empty values on insert.
 * Bind empty strings explicitly as '' instead. Explicit NULL binds keep using the one-argument
 * Stmt.SetBindingValueByIndex(Index).
 */
namespace GameIQSql
{
	template <typename T>
	inline bool Bind(FSQLitePreparedStatement& Stmt, const int32 Index, const T& Value)
	{
		return Stmt.SetBindingValueByIndex(Index, Value);
	}

	inline bool Bind(FSQLitePreparedStatement& Stmt, const int32 Index, const FString& Value)
	{
		return Value.IsEmpty()
			? Stmt.SetBindingValueByIndex(Index, TEXT(""))
			: Stmt.SetBindingValueByIndex(Index, Value);
	}

	inline bool Bind(FSQLitePreparedStatement& Stmt, const int32 Index, const FStringView Value)
	{
		return Value.IsEmpty()
			? Stmt.SetBindingValueByIndex(Index, TEXT(""))
			: Stmt.SetBindingValueByIndex(Index, FString(Value)); // UE 5.8 has no FStringView overload
	}
}
