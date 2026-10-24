#pragma once

#include "CoreMinimal.h"

/**
 * Demo settings from environment variables and the project's .env file.
 * Environment variables have priority. A packaged game reads the .env file in its
 * project folder (<Game>/NpcifyExample/.env).
 */
struct FNpcifyDemoEnv
{
	static const TCHAR* ApiBaseUrlKey;
	static const TCHAR* ApiKeyKey;
	static const TCHAR* OnlineCharacterIdKey;
	static const TCHAR* OfflineCharacterIdKey;

	static FString Get(const FString& Key, const FString& Fallback = FString());
	static FString ApiBaseUrl();
	static FString ApiKey();
	static bool HasApiKey();
};
