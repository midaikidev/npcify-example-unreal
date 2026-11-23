#include "NpcifyVrmExpressions.h"

#include "Engine/AssetUserData.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/UnrealType.h"

namespace
{
    UObject* ReadObject(UObject* Object, const TCHAR* Name)
    {
        const FObjectPropertyBase* Property = Object ? FindFProperty<FObjectPropertyBase>(Object->GetClass(), Name) : nullptr;
        return Property ? Property->GetObjectPropertyValue_InContainer(Object) : nullptr;
    }

    FString ReadString(const UStruct* Struct, const void* Container, const TCHAR* Name)
    {
        const FStrProperty* Property = FindFProperty<FStrProperty>(Struct, Name);
        return Property ? Property->GetPropertyValue_InContainer(Container) : FString();
    }

    bool ReadBool(const UStruct* Struct, const void* Container, const TCHAR* Name)
    {
        const FBoolProperty* Property = FindFProperty<FBoolProperty>(Struct, Name);
        return Property && Property->GetPropertyValue_InContainer(Container);
    }

    Npcify::ENpcifyExpressionOverride ToOverride(const FString& Value)
    {
        if (Value.Equals(TEXT("block"), ESearchCase::IgnoreCase)) return Npcify::ENpcifyExpressionOverride::Block;
        if (Value.Equals(TEXT("blend"), ESearchCase::IgnoreCase)) return Npcify::ENpcifyExpressionOverride::Blend;
        return Npcify::ENpcifyExpressionOverride::None;
    }

    void ReadGroups(UObject* Meta, const TArray<FName>& MorphNames, std::vector<Npcify::FNpcifyVrmExpression>& Out)
    {
        const FArrayProperty* Groups = FindFProperty<FArrayProperty>(Meta->GetClass(), TEXT("BlendShapeGroup"));
        const FStructProperty* GroupProperty = Groups ? CastField<FStructProperty>(Groups->Inner) : nullptr;
        if (!GroupProperty)
        {
            return;
        }
        const UScriptStruct* GroupStruct = GroupProperty->Struct;
        const FArrayProperty* Shapes = FindFProperty<FArrayProperty>(GroupStruct, TEXT("BlendShape"));
        const FStructProperty* ShapeProperty = Shapes ? CastField<FStructProperty>(Shapes->Inner) : nullptr;
        const FIntProperty* WeightProperty = ShapeProperty ? FindFProperty<FIntProperty>(ShapeProperty->Struct, TEXT("weight")) : nullptr;

        FScriptArrayHelper GroupArray(Groups, Groups->ContainerPtrToValuePtr<void>(Meta));
        for (int32 GroupIndex = 0; GroupIndex < GroupArray.Num(); ++GroupIndex)
        {
            const void* Group = GroupArray.GetRawPtr(GroupIndex);
            Npcify::FNpcifyVrmExpression Expression;
            Expression.Name = TCHAR_TO_UTF8(*ReadString(GroupStruct, Group, TEXT("name")));
            Expression.bIsBinary = ReadBool(GroupStruct, Group, TEXT("isBinary"));
            Expression.OverrideBlink = ToOverride(ReadString(GroupStruct, Group, TEXT("overrideBlink")));
            Expression.OverrideLookAt = ToOverride(ReadString(GroupStruct, Group, TEXT("overrideLookAt")));
            if (ShapeProperty)
            {
                FScriptArrayHelper ShapeArray(Shapes, Shapes->ContainerPtrToValuePtr<void>(Group));
                for (int32 ShapeIndex = 0; ShapeIndex < ShapeArray.Num(); ++ShapeIndex)
                {
                    const void* Shape = ShapeArray.GetRawPtr(ShapeIndex);
                    // VRM4U gives one mesh the morph targets of all meshes of the file, by
                    // name: a bind plays the morph target of its name.
                    const FString MorphName = ReadString(ShapeProperty->Struct, Shape, TEXT("morphTargetName"));
                    const int32 MorphIndex = MorphName.IsEmpty() ? INDEX_NONE : MorphNames.IndexOfByKey(FName(*MorphName));
                    if (MorphIndex == INDEX_NONE)
                    {
                        Expression.bBoundOnOtherMesh = true;
                        continue;
                    }
                    // VRM4U keeps a weight of 0 to 100 (VRM 0); for VRM 1 it keeps 100.
                    const int32 Weight = WeightProperty ? WeightProperty->GetPropertyValue_InContainer(Shape) : 100;
                    Expression.Binds.push_back({static_cast<size_t>(MorphIndex), static_cast<float>(Weight) * 0.01f});
                }
            }
            Out.push_back(std::move(Expression));
        }
    }
}

std::vector<Npcify::FNpcifyVrmExpression> ReadNpcifyVrmExpressions(const USkeletalMesh* Asset, const TArray<FName>& MorphNames)
{
    std::vector<Npcify::FNpcifyVrmExpression> Out;
    const TArray<UAssetUserData*>* UserData = Asset ? Asset->GetAssetUserDataArray() : nullptr;
    if (!UserData)
    {
        return Out;
    }
    for (UAssetUserData* Data : *UserData)
    {
        UObject* Meta = ReadObject(ReadObject(Data, TEXT("VrmAssetListObject")), TEXT("VrmMetaObject"));
        if (Meta)
        {
            ReadGroups(Meta, MorphNames, Out);
            if (!Out.empty())
            {
                break;
            }
        }
    }
    return Out;
}
