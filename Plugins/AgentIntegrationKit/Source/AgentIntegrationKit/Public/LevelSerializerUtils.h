// Copyright 2026 Betide Studio. All Rights Reserved.
// Level serializer utilities for training data export

#pragma once

#include "CoreMinimal.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Components/StaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "FoliageType.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "LandscapeProxy.h"

namespace LevelSerializerUtils
{

inline FString VecLua(const FVector& V)
{
	return FString::Printf(TEXT("{x=%.1f, y=%.1f, z=%.1f}"), V.X, V.Y, V.Z);
}

inline FString RotLua(const FRotator& R)
{
	TArray<FString> Parts;
	if (!FMath::IsNearlyZero(R.Pitch)) Parts.Add(FString::Printf(TEXT("pitch=%.1f"), R.Pitch));
	if (!FMath::IsNearlyZero(R.Yaw))   Parts.Add(FString::Printf(TEXT("yaw=%.1f"), R.Yaw));
	if (!FMath::IsNearlyZero(R.Roll))  Parts.Add(FString::Printf(TEXT("roll=%.1f"), R.Roll));
	if (Parts.Num() == 0) return TEXT("{pitch=0, yaw=0, roll=0}");
	return FString::Printf(TEXT("{%s}"), *FString::Join(Parts, TEXT(", ")));
}

inline FString Esc(const FString& S)
{
	FString R = S;
	R.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	R.ReplaceInline(TEXT("\""), TEXT("\\\""));
	R.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	return R;
}

inline void AppendActorParams(FString& Params, AActor* A)
{
	Params += FString::Printf(TEXT(", location=%s"), *VecLua(A->GetActorLocation()));

	FRotator Rot = A->GetActorRotation();
	if (!Rot.IsNearlyZero()) Params += FString::Printf(TEXT(", rotation=%s"), *RotLua(Rot));

	FVector Scale = A->GetActorScale3D();
	if (!Scale.Equals(FVector::OneVector, 0.01)) Params += FString::Printf(TEXT(", scale=%s"), *VecLua(Scale));

	FString Label = A->GetActorLabel();
	if (!Label.IsEmpty()) Params += FString::Printf(TEXT(", label=\"%s\""), *Esc(Label));

	FString Folder = A->GetFolderPath().ToString();
	if (!Folder.IsEmpty()) Params += FString::Printf(TEXT(", folder=\"%s\""), *Esc(Folder));
}

inline FString GenerateScript(UWorld* World)
{
	FString Script;
	Script += TEXT("local level = open_level()\n\n");

	// Folders
	TSet<FName> Folders;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (*It && !(*It)->IsA<AWorldSettings>())
		{
			FName Folder = (*It)->GetFolderPath();
			if (!Folder.IsNone()) Folders.Add(Folder);
		}
	}
	if (Folders.Num() > 0)
	{
		Script += TEXT("-- Folders\n");
		for (const FName& F : Folders)
			Script += FString::Printf(TEXT("level:add(\"folder\", {path=\"%s\"})\n"), *Esc(F.ToString()));
		Script += TEXT("\n");
	}

	// Lights
	bool bSection = false;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (!A || A->IsA<AWorldSettings>()) continue;
		ULightComponent* LC = A->FindComponentByClass<ULightComponent>();
		if (!LC) continue;

		FString LightType;
		if (A->IsA<APointLight>())           LightType = TEXT("point");
		else if (A->IsA<ASpotLight>())        LightType = TEXT("spot");
		else if (A->IsA<ADirectionalLight>()) LightType = TEXT("directional");
		else if (A->IsA<ASkyLight>())         LightType = TEXT("sky");
		else continue;

		if (!bSection) { Script += TEXT("-- Lights\n"); bSection = true; }

		FString Params = FString::Printf(TEXT("type=\"%s\""), *LightType);
		AppendActorParams(Params, A);
		Params += FString::Printf(TEXT(", intensity=%.1f"), LC->Intensity);

		FLinearColor Color = LC->GetLightColor();
		Params += FString::Printf(TEXT(", color=\"%d,%d,%d\""),
			FMath::RoundToInt(Color.R * 255), FMath::RoundToInt(Color.G * 255), FMath::RoundToInt(Color.B * 255));

		if (USpotLightComponent* SLC = Cast<USpotLightComponent>(LC))
		{
			Params += FString::Printf(TEXT(", inner_cone=%.1f, outer_cone=%.1f"),
				SLC->InnerConeAngle, SLC->OuterConeAngle);
		}

		Script += FString::Printf(TEXT("level:add(\"light\", {%s})\n"), *Params);
	}
	if (bSection) Script += TEXT("\n");

	// Actors (meshes, BPs, other)
	bSection = false;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (!A || A->IsA<AWorldSettings>()) continue;
		if (A->FindComponentByClass<ULightComponent>()) continue;
		if (A->IsA<ALandscapeProxy>()) continue;
		if (A->IsA<AInstancedFoliageActor>()) continue;

		UStaticMeshComponent* SMC = A->FindComponentByClass<UStaticMeshComponent>();
		UClass* ActorClass = A->GetClass();

		// BP actors
		if (ActorClass->ClassGeneratedBy)
		{
			if (!bSection) { Script += TEXT("-- Actors\n"); bSection = true; }
			FString Params = FString::Printf(TEXT("class=\"%s\""), *Esc(ActorClass->ClassGeneratedBy->GetPathName()));
			AppendActorParams(Params, A);
			Script += FString::Printf(TEXT("level:add(\"actor\", {%s})\n"), *Params);
			continue;
		}

		// Static mesh actors
		if (SMC)
		{
			UStaticMesh* SM = SMC->GetStaticMesh();
			if (!SM) continue;

			if (!bSection) { Script += TEXT("-- Actors\n"); bSection = true; }
			FString Params = FString::Printf(TEXT("mesh=\"%s\""), *Esc(SM->GetPathName()));
			AppendActorParams(Params, A);

			if (UMaterialInterface* Mat = SMC->GetMaterial(0))
			{
				UMaterialInterface* DefaultMat = SM->GetMaterial(0);
				if (Mat != DefaultMat)
					Params += FString::Printf(TEXT(", material=\"%s\""), *Esc(Mat->GetPathName()));
			}

			Script += FString::Printf(TEXT("level:add(\"actor\", {%s})\n"), *Params);
			continue;
		}

		// Other placeable actors
		if (!ActorClass->HasAnyClassFlags(CLASS_Abstract | CLASS_NotPlaceable))
		{
			if (!bSection) { Script += TEXT("-- Actors\n"); bSection = true; }
			FString Params = FString::Printf(TEXT("class=\"%s\""), *Esc(ActorClass->GetPathName()));
			AppendActorParams(Params, A);
			Script += FString::Printf(TEXT("level:add(\"actor\", {%s})\n"), *Params);
		}
	}
	if (bSection) Script += TEXT("\n");

	// Foliage
	bSection = false;
	for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
	{
		AInstancedFoliageActor* IFA = *It;
		IFA->ForEachFoliageInfo([&](UFoliageType* FType, FFoliageInfo& Info)
		{
			UFoliageType_InstancedStaticMesh* FISM = Cast<UFoliageType_InstancedStaticMesh>(FType);
			if (!FISM || !FISM->Mesh) return true;

			UHierarchicalInstancedStaticMeshComponent* Comp = Info.GetComponent();
			if (!Comp || Comp->GetInstanceCount() == 0) return true;

			if (!bSection) { Script += TEXT("-- Foliage\n"); bSection = true; }

			Script += FString::Printf(TEXT("level:add(\"foliage\", {mesh=\"%s\", transforms={\n"), *Esc(FISM->Mesh->GetPathName()));

			int32 Count = Comp->GetInstanceCount();
			int32 Max = FMath::Min(Count, 500);
			for (int32 i = 0; i < Max; i++)
			{
				FTransform T;
				Comp->GetInstanceTransform(i, T, true);
				FString Entry = FString::Printf(TEXT("  {location=%s"), *VecLua(T.GetLocation()));
				FRotator Rot = T.Rotator();
				if (!Rot.IsNearlyZero()) Entry += FString::Printf(TEXT(", rotation=%s"), *RotLua(Rot));
				FVector Sc = T.GetScale3D();
				if (!Sc.Equals(FVector::OneVector, 0.01)) Entry += FString::Printf(TEXT(", scale=%s"), *VecLua(Sc));
				Entry += (i < Max - 1) ? TEXT("},") : TEXT("}");
				Script += Entry + TEXT("\n");
			}
			if (Count > Max)
				Script += FString::Printf(TEXT("  -- ... %d more instances omitted\n"), Count - Max);

			Script += TEXT("}})\n");
			return true;
		});
	}
	if (bSection) Script += TEXT("\n");

	Script += TEXT("level:save()\n");
	return Script;
}

inline FString GenerateMetadata(UWorld* World)
{
	int32 ActorCount = 0, LightCount = 0, MeshCount = 0, FoliageTypes = 0, BPCount = 0;
	TSet<FString> MeshPaths, MaterialPaths;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (!A || A->IsA<AWorldSettings>()) continue;
		ActorCount++;

		if (A->FindComponentByClass<ULightComponent>()) LightCount++;

		if (UStaticMeshComponent* SMC = A->FindComponentByClass<UStaticMeshComponent>())
		{
			MeshCount++;
			if (UStaticMesh* SM = SMC->GetStaticMesh())
				MeshPaths.Add(SM->GetPathName());
			for (int32 i = 0; i < SMC->GetNumMaterials(); i++)
			{
				if (UMaterialInterface* Mat = SMC->GetMaterial(i))
					MaterialPaths.Add(Mat->GetPathName());
			}
		}

		if (A->GetClass()->ClassGeneratedBy) BPCount++;
	}

	for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
	{
		(*It)->ForEachFoliageInfo([&](UFoliageType* FType, FFoliageInfo& Info)
		{
			if (Info.GetComponent() && Info.GetComponent()->GetInstanceCount() > 0)
				FoliageTypes++;
			return true;
		});
	}

	FString JSON;
	JSON += TEXT("{\n");
	JSON += FString::Printf(TEXT("  \"level_name\": \"%s\",\n"), *Esc(World->GetName()));
	JSON += FString::Printf(TEXT("  \"actor_count\": %d,\n"), ActorCount);
	JSON += FString::Printf(TEXT("  \"light_count\": %d,\n"), LightCount);
	JSON += FString::Printf(TEXT("  \"mesh_actor_count\": %d,\n"), MeshCount);
	JSON += FString::Printf(TEXT("  \"blueprint_actor_count\": %d,\n"), BPCount);
	JSON += FString::Printf(TEXT("  \"foliage_types\": %d,\n"), FoliageTypes);
	JSON += FString::Printf(TEXT("  \"unique_meshes\": %d,\n"), MeshPaths.Num());
	JSON += FString::Printf(TEXT("  \"unique_materials\": %d,\n"), MaterialPaths.Num());
	JSON += TEXT("  \"description\": \"TODO: Add natural language description of this level\"\n");
	JSON += TEXT("}\n");
	return JSON;
}

} // namespace LevelSerializerUtils
