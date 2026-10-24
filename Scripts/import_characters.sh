#!/bin/sh
# Import RawCharacters/* into Content/Npcify/Characters with VRM4U (no editor window).
# Usage: Scripts/import_characters.sh [path to the Unreal Engine 5.8 folder]
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENGINE="${1:-${UE_ROOT:-/Users/Shared/Epic Games/UE_5.8}}"
case "$(uname -s)" in
  Darwin) EDITOR_CMD="$ENGINE/Engine/Binaries/Mac/UnrealEditor-Cmd" ;;
  Linux) EDITOR_CMD="$ENGINE/Engine/Binaries/Linux/UnrealEditor-Cmd" ;;
  *) EDITOR_CMD="$ENGINE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" ;;
esac
rm -rf "$ROOT/Content/NpcifyImportTemp"
"$EDITOR_CMD" "$ROOT/NpcifyExample.uproject" -run=NpcifyImportCharacters -unattended -nopause -nosplash -stdout
# The temporary VRMA models, IK rigs and retargeters are not needed after the bake.
rm -rf "$ROOT/Content/NpcifyImportTemp"
