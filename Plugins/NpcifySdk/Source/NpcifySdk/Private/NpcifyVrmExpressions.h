#pragma once

#include "CoreMinimal.h"
#include "Core/NpcifyFacialMixer.h"

#include <vector>

class USkeletalMesh;

/**
 * The expressions of the VRM file of a mesh that VRM4U imported, for FNpcifyMorphTargetPlan:
 * the morph targets that the file binds to each expression, "is binary" and the overrides of the
 * blinks and of the gaze. VRM4U stores them with the mesh: asset user data ->
 * VrmAssetListObject -> VrmMetaObject -> BlendShapeGroup. They are read by reflection, so the
 * SDK needs no VRM4U module.
 *
 * MorphNames are the morph targets of the mesh, in their order. Returns an empty list for a
 * mesh without that data: the face then plays by the alias names of the morph targets.
 */
std::vector<Npcify::FNpcifyVrmExpression> ReadNpcifyVrmExpressions(const USkeletalMesh* Asset, const TArray<FName>& MorphNames);
