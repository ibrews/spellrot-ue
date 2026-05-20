// Copyright 2025-2026 Betide Studio. All Rights Reserved.

#include "Lua/LuaBindingRegistry.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimCompositeBase.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "ScopedTransaction.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ---- Post-mutation helper for AnimComposite ----
// Engine flow: SortAnimSegments → CollapseAnimSegments → InvalidateRecursiveAsset →
//              AnimationTrack.GetLength → SetCompositeLength → PostEditChange → MarkPackageDirty
static void CompositePostMutationUpdate(UAnimComposite* Composite)
{
	if (!Composite) return;

	// 1. Sort + collapse segments
	Composite->AnimationTrack.SortAnimSegments();
	Composite->AnimationTrack.CollapseAnimSegments();

	// 2. Invalidate recursive asset references
	Composite->InvalidateRecursiveAsset();

	// 3. Recalculate and set composite length
	const float NewLength = Composite->AnimationTrack.GetLength();
	if (!FMath::IsNearlyEqual(NewLength, Composite->GetPlayLength(), UE_KINDA_SMALL_NUMBER))
	{
		Composite->SetCompositeLength(NewLength);
	}

	// 4. Final callbacks
	Composite->PostEditChange();
	Composite->MarkPackageDirty();
}

static TArray<FLuaFunctionDoc> AnimCompositeDocs = {};

static void BindAnimComposite(sol::state& Lua, FLuaSessionData& Session)
{
	Lua.set_function("_enrich_anim_composite", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view Lua(S);
		UAnimComposite* Composite = Cast<UAnimComposite>(static_cast<UObject*>(AssetObj["_ptr"].get<void*>()));
		if (!Composite) { Session.Log(TEXT("[FAIL] _enrich_anim_composite -> invalid asset")); return; }

		// ---- _help_text (read by OpenAsset's help() alongside generic methods) ----
		AssetObj["_help_text"] =
			"AnimComposite — chain animation sequences together as a single unit.\n"
			"\n"
			"info()  — length, segment_count, skeleton, frame_rate, additive info, root motion\n"
			"\n"
			"list(type):\n"
			"  list(\"segments\")          — all animation segments with timing details\n"
			"  list(\"referenced_assets\") — all recursively referenced animation assets\n"
			"\n"
			"add(type, params):\n"
			"  add(\"segment\", {animation=\"/Game/Anim\", start_pos=0, play_rate=1, loops=1})\n"
			"    animation (required): asset path to AnimSequence/AnimComposite (not Montage)\n"
			"    start_pos: position in composite timeline (default: end of last segment)\n"
			"    play_rate: playback speed, non-zero (default: 1.0)\n"
			"    loops: loop count, >= 1 (default: 1)\n"
			"    anim_start: start time within source animation (default: 0)\n"
			"    anim_end: end time within source animation (default: full length)\n"
			"\n"
			"remove(type, id):\n"
			"  remove(\"segment\", 1)  — 1-based index\n"
			"\n"
			"configure(type, id, params):\n"
			"  configure(\"segment\", 1, {start_pos=0.5, play_rate=2, loops=3})\n"
			"    Modifiable: start_pos, play_rate, loops, animation, anim_start, anim_end\n"
			"    Changing animation auto-clamps anim_end to new animation length\n";

		// ---- list(type) ----
		AssetObj.set_function("list", [Composite, &Session](sol::table /*self*/,
			const std::string& Type, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("segments"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				const TArray<FAnimSegment>& Segments = Composite->AnimationTrack.AnimSegments;
				for (int32 i = 0; i < Segments.Num(); i++)
				{
					const FAnimSegment& Seg = Segments[i];
					sol::table Entry = Lua.create_table();
					Entry["index"] = i + 1;
					Entry["animation"] = Seg.GetAnimReference()
						? TCHAR_TO_UTF8(*Seg.GetAnimReference()->GetPathName())
						: "";
					Entry["animation_name"] = Seg.GetAnimReference()
						? TCHAR_TO_UTF8(*Seg.GetAnimReference()->GetName())
						: "";
					Entry["start_pos"] = Seg.StartPos;
					Entry["end_pos"] = Seg.GetEndPos();
					Entry["length"] = Seg.GetLength();
					Entry["play_rate"] = Seg.AnimPlayRate;
					Entry["loops"] = Seg.LoopingCount;
					Entry["anim_start"] = Seg.AnimStartTime;
					Entry["anim_end"] = Seg.AnimEndTime;
					Entry["is_valid"] = Seg.IsValid();
					if (Seg.GetAnimReference())
					{
						Entry["source_length"] = Seg.GetAnimReference()->GetPlayLength();
					}
					Result[i + 1] = Entry;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"segments\") -> %d segments"), Segments.Num()));
				return Result;
			}

			if (FType.Equals(TEXT("referenced_assets"), ESearchCase::IgnoreCase))
			{
				TArray<UAnimationAsset*> Assets;
				Composite->GetAllAnimationSequencesReferred(Assets, true);
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Assets.Num(); i++)
				{
					if (!Assets[i]) continue;
					sol::table Entry = Lua.create_table();
					Entry["name"] = TCHAR_TO_UTF8(*Assets[i]->GetName());
					Entry["path"] = TCHAR_TO_UTF8(*Assets[i]->GetPathName());
					Entry["class"] = TCHAR_TO_UTF8(*Assets[i]->GetClass()->GetName());
					Result.add(Entry);
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"referenced_assets\") -> %d assets"), Assets.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: segments, referenced_assets"), *FType));
			return sol::lua_nil;
		});

		// ---- add(type, params) ----
		AssetObj.set_function("add", [Composite, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("segment"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value()) { Session.Log(TEXT("[FAIL] add(\"segment\") -> {animation=..} required")); return sol::lua_nil; }
				sol::table P = Params.value();
				std::string AnimPath = P.get_or<std::string>("animation", "");
				if (AnimPath.empty()) { Session.Log(TEXT("[FAIL] add(\"segment\") -> animation required")); return sol::lua_nil; }

				FString FAnim = UTF8_TO_TCHAR(AnimPath.c_str());
				if (!FAnim.StartsWith(TEXT("/"))) FAnim = TEXT("/Game/") + FAnim;
				UAnimSequenceBase* Anim = LoadObject<UAnimSequenceBase>(nullptr, *FAnim);
				if (!Anim) { Session.Log(FString::Printf(TEXT("[FAIL] add(\"segment\") -> animation not found: %s"), *FAnim)); return sol::lua_nil; }

				// Validate: checks play length > 0, CanBeUsedInComposition (rejects Montages), and additive type match
				FText Reason;
				if (!Composite->AnimationTrack.IsValidToAdd(Anim, &Reason))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"segment\") -> invalid animation: %s"), *Reason.ToString()));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "AddCompositeSegment", "Add Composite Segment"));
				Composite->Modify();

				FAnimSegment Seg;
				Seg.SetAnimReference(Anim, true); // Sets AnimStartTime=0, AnimEndTime=PlayLength, AnimPlayRate=1, LoopingCount=1, StartPos=0

				// Default start_pos: end of last segment
				float LastEnd = 0.0f;
				for (const FAnimSegment& Existing : Composite->AnimationTrack.AnimSegments)
				{
					float End = Existing.StartPos + Existing.GetLength();
					if (End > LastEnd) LastEnd = End;
				}
				Seg.StartPos = LastEnd;

				// Apply overrides from params with validation
				Seg.StartPos = static_cast<float>(P.get_or("start_pos", static_cast<double>(Seg.StartPos)));

				double PlayRateVal = P.get_or("play_rate", 1.0);
				Seg.AnimPlayRate = FMath::IsNearlyZero(PlayRateVal) ? 1.0f : static_cast<float>(PlayRateVal);

				Seg.LoopingCount = FMath::Max(1, P.get_or("loops", 1));

				// Clamp anim_start/anim_end to source animation range
				const float PlayLen = Anim->GetPlayLength();
				Seg.AnimStartTime = FMath::Clamp(static_cast<float>(P.get_or("anim_start", 0.0)), 0.0f, PlayLen);
				Seg.AnimEndTime = FMath::Clamp(static_cast<float>(P.get_or("anim_end", static_cast<double>(PlayLen))), Seg.AnimStartTime, PlayLen);

				Composite->AnimationTrack.AnimSegments.Add(Seg);
				CompositePostMutationUpdate(Composite);
				Session.Log(FString::Printf(TEXT("[OK] add(\"segment\", anim=\"%s\")"), *Anim->GetName()));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: segment"), *FType));
			return sol::lua_nil;
		});

		// ---- remove(type, id) ----
		AssetObj.set_function("remove", [Composite, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("segment"), ESearchCase::IgnoreCase))
			{
				int32 SegIdx = -1;
				if (Id.is<int>()) SegIdx = Id.as<int>() - 1;
				else if (Id.is<double>()) SegIdx = static_cast<int32>(Id.as<double>()) - 1;
				else { Session.Log(TEXT("[FAIL] remove(\"segment\") -> 1-based index required")); return sol::lua_nil; }

				if (SegIdx < 0 || SegIdx >= Composite->AnimationTrack.AnimSegments.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"segment\") -> index %d out of range (1-%d)"),
						SegIdx + 1, Composite->AnimationTrack.AnimSegments.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "RemCompositeSegment", "Remove Composite Segment"));
				Composite->Modify();
				Composite->AnimationTrack.AnimSegments.RemoveAt(SegIdx);
				CompositePostMutationUpdate(Composite);
				Session.Log(FString::Printf(TEXT("[OK] remove(\"segment\", %d)"), SegIdx + 1));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: segment"), *FType));
			return sol::lua_nil;
		});

		// ---- configure(type, id, params) ----
		AssetObj.set_function("configure", [Composite, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::table Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("segment"), ESearchCase::IgnoreCase))
			{
				int32 SegIdx = -1;
				if (Id.is<int>()) SegIdx = Id.as<int>() - 1;
				else if (Id.is<double>()) SegIdx = static_cast<int32>(Id.as<double>()) - 1;
				else { Session.Log(TEXT("[FAIL] configure(\"segment\") -> 1-based index required")); return sol::lua_nil; }

				if (SegIdx < 0 || SegIdx >= Composite->AnimationTrack.AnimSegments.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"segment\") -> index %d out of range (1-%d)"),
						SegIdx + 1, Composite->AnimationTrack.AnimSegments.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Tx(NSLOCTEXT("AIK", "ConfigCompositeSegment", "Configure Composite Segment"));
				Composite->Modify();
				FAnimSegment& Seg = Composite->AnimationTrack.AnimSegments[SegIdx];

				// Change animation reference first (affects valid ranges for other params)
				std::string AnimPath = Params.get_or<std::string>("animation", "");
				if (!AnimPath.empty())
				{
					FString FAnim = UTF8_TO_TCHAR(AnimPath.c_str());
					if (!FAnim.StartsWith(TEXT("/"))) FAnim = TEXT("/Game/") + FAnim;
					UAnimSequenceBase* Anim = LoadObject<UAnimSequenceBase>(nullptr, *FAnim);
					if (Anim)
					{
						// Validate before applying
						FText Reason;
						if (!Composite->AnimationTrack.IsValidToAdd(Anim, &Reason))
						{
							Session.Log(FString::Printf(TEXT("[FAIL] configure(\"segment\") -> invalid animation: %s"), *Reason.ToString()));
							return sol::lua_nil;
						}
						Seg.SetAnimReference(Anim, false);
						// Clamp existing AnimEndTime to new animation's length
						float NewPlayLen = Anim->GetPlayLength();
						if (Seg.AnimEndTime > NewPlayLen) Seg.AnimEndTime = NewPlayLen;
						if (Seg.AnimStartTime > Seg.AnimEndTime) Seg.AnimStartTime = 0.0f;
					}
					else
					{
						Session.Log(FString::Printf(TEXT("[WARN] configure(\"segment\") -> animation not found: %s"), *FAnim));
					}
				}

				sol::optional<double> StartPos = Params.get<sol::optional<double>>("start_pos");
				if (StartPos.has_value()) Seg.StartPos = static_cast<float>(StartPos.value());

				sol::optional<double> PlayRate = Params.get<sol::optional<double>>("play_rate");
				if (PlayRate.has_value())
				{
					Seg.AnimPlayRate = FMath::IsNearlyZero(PlayRate.value()) ? 1.0f : static_cast<float>(PlayRate.value());
				}

				sol::optional<double> Loops = Params.get<sol::optional<double>>("loops");
				if (Loops.has_value()) Seg.LoopingCount = FMath::Max(1, static_cast<int32>(Loops.value()));

				// Clamp anim_start/anim_end to source animation range
				const float PlayLen = Seg.GetAnimReference() ? Seg.GetAnimReference()->GetPlayLength() : 0.0f;

				sol::optional<double> AnimStart = Params.get<sol::optional<double>>("anim_start");
				if (AnimStart.has_value()) Seg.AnimStartTime = FMath::Clamp(static_cast<float>(AnimStart.value()), 0.0f, PlayLen);

				sol::optional<double> AnimEnd = Params.get<sol::optional<double>>("anim_end");
				if (AnimEnd.has_value()) Seg.AnimEndTime = FMath::Clamp(static_cast<float>(AnimEnd.value()), Seg.AnimStartTime, PlayLen);

				CompositePostMutationUpdate(Composite);
				Session.Log(FString::Printf(TEXT("[OK] configure(\"segment\", %d)"), SegIdx + 1));
				return sol::make_object(Lua, true);
			}

			Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: segment"), *FType));
			return sol::lua_nil;
		});

		// ---- info() ----
		AssetObj.set_function("info", [Composite, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			Result["type"] = "AnimComposite";
			Result["name"] = TCHAR_TO_UTF8(*Composite->GetName());
			Result["path"] = TCHAR_TO_UTF8(*Composite->GetPathName());
			Result["length"] = Composite->GetPlayLength();
			Result["segment_count"] = Composite->AnimationTrack.AnimSegments.Num();

			USkeleton* Skeleton = Composite->GetSkeleton();
			if (Skeleton)
			{
				Result["skeleton"] = TCHAR_TO_UTF8(*Skeleton->GetPathName());
				Result["skeleton_name"] = TCHAR_TO_UTF8(*Skeleton->GetName());
			}

#if WITH_EDITORONLY_DATA
			if (Composite->PreviewBasePose)
			{
				Result["preview_base_pose"] = TCHAR_TO_UTF8(*Composite->PreviewBasePose->GetPathName());
			}
#endif

			EAdditiveAnimationType AdditiveType = Composite->GetAdditiveAnimType();
			Result["additive"] = AdditiveType != AAT_None;
			switch (AdditiveType)
			{
			case AAT_LocalSpaceBase:       Result["additive_type"] = "local_space"; break;
			case AAT_RotationOffsetMeshSpace: Result["additive_type"] = "mesh_space"; break;
			default:                        Result["additive_type"] = "none"; break;
			}
			Result["is_valid_additive"] = Composite->IsValidAdditive();
			Result["has_root_motion"] = Composite->HasRootMotion();

			// Frame rate
			FFrameRate SamplingRate = Composite->GetSamplingFrameRate();
			if (SamplingRate.IsValid())
			{
				Result["frame_rate"] = TCHAR_TO_UTF8(*SamplingRate.ToPrettyText().ToString());
				Result["frame_rate_numerator"] = SamplingRate.Numerator;
				Result["frame_rate_denominator"] = SamplingRate.Denominator;
			}

			Session.Log(FString::Printf(TEXT("[OK] info() -> %.2fs, %d segments"),
				Composite->GetPlayLength(), Composite->AnimationTrack.AnimSegments.Num()));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(AnimComposite, AnimCompositeDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindAnimComposite(Lua, Session);
});
