#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "NpcifyImportCharactersCommandlet.generated.h"

/**
 * Imports the downloaded Npcify characters (RawCharacters/<folder>/character.vrm and
 * animations/<id>.vrma) into /Game/Npcify/Characters/<folder> with VRM4U, without dialogs.
 *
 * UnrealEditor-Cmd NpcifyExample.uproject -run=NpcifyImportCharacters [-Folder=hinata]
 */
UCLASS()
class UNpcifyImportCharactersCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UNpcifyImportCharactersCommandlet();

	virtual int32 Main(const FString& Params) override;

private:
	bool ImportCharacter(const FString& Folder);
};
