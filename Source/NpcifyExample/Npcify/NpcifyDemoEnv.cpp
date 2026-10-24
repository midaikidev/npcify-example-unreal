#include "NpcifyDemoEnv.h"

#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

const TCHAR* FNpcifyDemoEnv::ApiBaseUrlKey = TEXT("NPCIFY_API_BASE_URL");
const TCHAR* FNpcifyDemoEnv::ApiKeyKey = TEXT("NPCIFY_API_KEY");
const TCHAR* FNpcifyDemoEnv::OnlineCharacterIdKey = TEXT("NPCIFY_ONLINE_CHARACTER_ID");
const TCHAR* FNpcifyDemoEnv::OfflineCharacterIdKey = TEXT("NPCIFY_OFFLINE_CHARACTER_ID");

namespace
{
	const TMap<FString, FString>& FileValues()
	{
		static TMap<FString, FString> Values = []
		{
			TMap<FString, FString> Result;
			TArray<FString> Lines;
			if (!FFileHelper::LoadFileToStringArray(Lines, *FPaths::Combine(FPaths::ProjectDir(), TEXT(".env"))))
			{
				return Result;
			}
			for (FString Line : Lines)
			{
				Line.TrimStartAndEndInline();
				if (Line.IsEmpty() || Line.StartsWith(TEXT("#")))
				{
					continue;
				}
				Line.RemoveFromStart(TEXT("export "));
				FString Key;
				FString Value;
				if (!Line.Split(TEXT("="), &Key, &Value))
				{
					continue;
				}
				Key.TrimStartAndEndInline();
				Value.TrimStartAndEndInline();
				if (Value.Len() >= 2 && ((Value.StartsWith(TEXT("\"")) && Value.EndsWith(TEXT("\""))) ||
					(Value.StartsWith(TEXT("'")) && Value.EndsWith(TEXT("'")))))
				{
					Value = Value.Mid(1, Value.Len() - 2);
				}
				Result.Add(Key, Value);
			}
			return Result;
		}();
		return Values;
	}
}

FString FNpcifyDemoEnv::Get(const FString& Key, const FString& Fallback)
{
	const FString FromProcess = FPlatformMisc::GetEnvironmentVariable(*Key);
	if (!FromProcess.IsEmpty())
	{
		return FromProcess;
	}
	if (const FString* FromFile = FileValues().Find(Key); FromFile && !FromFile->IsEmpty())
	{
		return *FromFile;
	}
	return Fallback;
}

FString FNpcifyDemoEnv::ApiBaseUrl()
{
	return Get(ApiBaseUrlKey, TEXT("https://api.npcify.ai"));
}

FString FNpcifyDemoEnv::ApiKey()
{
	return Get(ApiKeyKey);
}

bool FNpcifyDemoEnv::HasApiKey()
{
	const FString Key = ApiKey();
	return !Key.IsEmpty() && !Key.StartsWith(TEXT("<"));
}
