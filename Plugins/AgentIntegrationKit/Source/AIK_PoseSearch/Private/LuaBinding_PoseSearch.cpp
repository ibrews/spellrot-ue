// PoseSearch Lua binding — lives in AIK_PoseSearch extension module.
// This module is only loaded when PoseSearch plugin is available,
// so no #if WITH_POSE_SEARCH guard is needed.

#include "Lua/LuaBindingRegistry.h"
#include "Tools/NeoStackToolUtils.h"
#include "ScopedTransaction.h"

#include "PoseSearch/PoseSearchSchema.h"
#include "PoseSearch/PoseSearchDatabase.h"
#include "PoseSearch/PoseSearchNormalizationSet.h"
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
#include "PoseSearch/PoseSearchFeatureChannel_Pose.h"
#include "PoseSearch/PoseSearchFeatureChannel_Trajectory.h"
#include "PoseSearch/PoseSearchFeatureChannel_Velocity.h"
#include "PoseSearch/PoseSearchFeatureChannel_Position.h"
#include "PoseSearch/PoseSearchFeatureChannel_Heading.h"
#include "PoseSearch/PoseSearchFeatureChannel_Phase.h"
#include "PoseSearch/PoseSearchFeatureChannel_Curve.h"
#include "PoseSearch/PoseSearchFeatureChannel_Distance.h"
#include "PoseSearch/PoseSearchFeatureChannel_Group.h"
#include "PoseSearch/PoseSearchFeatureChannel_TimeToEvent.h"
#endif
#include "PoseSearch/PoseSearchDerivedData.h"
#include "Lua/LuaDynamicTypeHelper.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimMontage.h"
#include "Animation/Skeleton.h"
#include "Animation/MirrorDataTable.h"

#define SOL_ALL_SAFETIES_ON 1
#include <sol/sol.hpp>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
static int32 ParseBoneFlags(sol::table FlagsTable)
{
	int32 Flags = 0;
	for (auto& Kv : FlagsTable)
	{
		sol::optional<std::string> FlagOpt = Kv.second.as<sol::optional<std::string>>();
		if (!FlagOpt.has_value()) continue;
		FString Flag = UTF8_TO_TCHAR(FlagOpt.value().c_str());
		if (Flag.Equals(TEXT("Position"), ESearchCase::IgnoreCase))          Flags |= EPoseSearchBoneFlags::Position;
		else if (Flag.Equals(TEXT("Velocity"), ESearchCase::IgnoreCase))     Flags |= EPoseSearchBoneFlags::Velocity;
		else if (Flag.Equals(TEXT("Rotation"), ESearchCase::IgnoreCase))     Flags |= EPoseSearchBoneFlags::Rotation;
		else if (Flag.Equals(TEXT("Phase"), ESearchCase::IgnoreCase))        Flags |= EPoseSearchBoneFlags::Phase;
	}
	return Flags != 0 ? Flags : int32(EPoseSearchBoneFlags::Position);
}

static int32 ParseTrajectoryFlags(sol::table FlagsTable)
{
	int32 Flags = 0;
	for (auto& Kv : FlagsTable)
	{
		sol::optional<std::string> FlagOpt = Kv.second.as<sol::optional<std::string>>();
		if (!FlagOpt.has_value()) continue;
		FString Flag = UTF8_TO_TCHAR(FlagOpt.value().c_str());
		if (Flag.Equals(TEXT("Position"), ESearchCase::IgnoreCase))              Flags |= EPoseSearchTrajectoryFlags::Position;
		else if (Flag.Equals(TEXT("Velocity"), ESearchCase::IgnoreCase))         Flags |= EPoseSearchTrajectoryFlags::Velocity;
		else if (Flag.Equals(TEXT("VelocityDirection"), ESearchCase::IgnoreCase))Flags |= EPoseSearchTrajectoryFlags::VelocityDirection;
		else if (Flag.Equals(TEXT("FacingDirection"), ESearchCase::IgnoreCase))  Flags |= EPoseSearchTrajectoryFlags::FacingDirection;
		else if (Flag.Equals(TEXT("VelocityXY"), ESearchCase::IgnoreCase))       Flags |= EPoseSearchTrajectoryFlags::VelocityXY;
		else if (Flag.Equals(TEXT("PositionXY"), ESearchCase::IgnoreCase))       Flags |= EPoseSearchTrajectoryFlags::PositionXY;
		else if (Flag.Equals(TEXT("VelocityDirectionXY"), ESearchCase::IgnoreCase)) Flags |= EPoseSearchTrajectoryFlags::VelocityDirectionXY;
		else if (Flag.Equals(TEXT("FacingDirectionXY"), ESearchCase::IgnoreCase))   Flags |= EPoseSearchTrajectoryFlags::FacingDirectionXY;
	}
	return Flags != 0 ? Flags : int32(EPoseSearchTrajectoryFlags::Position);
}
#endif // ENGINE_MINOR_VERSION >= 6

static EPoseSearchMirrorOption ParseMirrorOption(const std::string& Str)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("MirroredOnly"), ESearchCase::IgnoreCase))
		return EPoseSearchMirrorOption::MirroredOnly;
	if (S.Equals(TEXT("UnmirroredAndMirrored"), ESearchCase::IgnoreCase))
		return EPoseSearchMirrorOption::UnmirroredAndMirrored;
	return EPoseSearchMirrorOption::UnmirroredOnly;
}

static const char* MirrorOptionToString(EPoseSearchMirrorOption Opt)
{
	switch (Opt)
	{
	case EPoseSearchMirrorOption::MirroredOnly:            return "MirroredOnly";
	case EPoseSearchMirrorOption::UnmirroredAndMirrored:   return "UnmirroredAndMirrored";
	default:                                               return "UnmirroredOnly";
	}
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
static const TArray<FString> ChannelPrefixes = { TEXT("PoseSearchFeatureChannel_") };

static const char* ChannelTypeString(const UPoseSearchFeatureChannel* Ch)
{
	if (!Ch) return "Unknown";
	// Dynamic: strip prefix from class name
	static TMap<UClass*, FString> CachedNames;
	FString* Cached = CachedNames.Find(Ch->GetClass());
	if (!Cached)
	{
		FString Name = LuaDynamicType::GetFriendlyTypeName(Ch, ChannelPrefixes);
		Cached = &CachedNames.Add(Ch->GetClass(), Name);
	}
	// Return a stable pointer — cached string persists
	static thread_local std::string Buffer;
	Buffer = TCHAR_TO_UTF8(**Cached);
	return Buffer.c_str();
}
#endif // ENGINE_MINOR_VERSION >= 6

static EPoseSearchMode ParseSearchMode(const std::string& Str)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("BruteForce"), ESearchCase::IgnoreCase))   return EPoseSearchMode::BruteForce;
	if (S.Equals(TEXT("VPTree"), ESearchCase::IgnoreCase))       return EPoseSearchMode::VPTree;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	if (S.Equals(TEXT("EventOnly"), ESearchCase::IgnoreCase))    return EPoseSearchMode::EventOnly;
#endif
	return EPoseSearchMode::PCAKDTree; // default
}

static const char* SearchModeToString(EPoseSearchMode Mode)
{
	switch (Mode)
	{
	case EPoseSearchMode::BruteForce: return "BruteForce";
	case EPoseSearchMode::PCAKDTree:  return "PCAKDTree";
	case EPoseSearchMode::VPTree:     return "VPTree";
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
	case EPoseSearchMode::EventOnly:  return "EventOnly";
#endif
	default:                          return "Unknown";
	}
}

#if WITH_EDITORONLY_DATA
static EPoseSearchDataPreprocessor ParseDataPreprocessor(const std::string& Str)
{
	FString S = UTF8_TO_TCHAR(Str.c_str());
	if (S.Equals(TEXT("None"), ESearchCase::IgnoreCase))                      return EPoseSearchDataPreprocessor::None;
	if (S.Equals(TEXT("NormalizeOnlyByDeviation"), ESearchCase::IgnoreCase))   return EPoseSearchDataPreprocessor::NormalizeOnlyByDeviation;
	if (S.Equals(TEXT("NormalizeWithCommonSchema"), ESearchCase::IgnoreCase))  return EPoseSearchDataPreprocessor::NormalizeWithCommonSchema;
	return EPoseSearchDataPreprocessor::Normalize; // default
}
#endif

static void RequestDatabaseRebuild(const UPoseSearchDatabase* Database)
{
#if WITH_EDITOR
	UE::PoseSearch::FAsyncPoseSearchDatabasesManagement::RequestAsyncBuildIndex(
		Database, UE::PoseSearch::ERequestAsyncBuildFlag::NewRequest);
#endif
}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
// Access the private Channels array via reflection for editing
static TArray<TObjectPtr<UPoseSearchFeatureChannel>>* GetChannelsArrayMutable(UPoseSearchSchema* Schema)
{
	FProperty* Prop = UPoseSearchSchema::StaticClass()->FindPropertyByName(TEXT("Channels"));
	if (!Prop) return nullptr;
	return Prop->ContainerPtrToValuePtr<TArray<TObjectPtr<UPoseSearchFeatureChannel>>>(Schema);
}
#endif // ENGINE_MINOR_VERSION >= 6

static void AddSchemaEditorOnlyInfo(UPoseSearchSchema* Schema, sol::table& Result)
{
#if WITH_EDITORONLY_DATA
	const char* PreprocessorStr = "Unknown";
	switch (Schema->DataPreprocessor)
	{
	case EPoseSearchDataPreprocessor::None:                     PreprocessorStr = "None"; break;
	case EPoseSearchDataPreprocessor::Normalize:                PreprocessorStr = "Normalize"; break;
	case EPoseSearchDataPreprocessor::NormalizeOnlyByDeviation: PreprocessorStr = "NormalizeOnlyByDeviation"; break;
	case EPoseSearchDataPreprocessor::NormalizeWithCommonSchema:PreprocessorStr = "NormalizeWithCommonSchema"; break;
	}
	Result["data_preprocessor"] = PreprocessorStr;
#else
	(void)Schema;
	(void)Result;
#endif
}

static void AddDatabaseEditorOnlyInfo(UPoseSearchDatabase* Database, sol::table& Result)
{
#if WITH_EDITORONLY_DATA
	Result["normalization_set"] = Database->NormalizationSet ? TCHAR_TO_UTF8(*Database->NormalizationSet->GetName()) : "None";
#else
	(void)Database;
	(void)Result;
#endif
}

// ---------------------------------------------------------------------------
// Docs
// ---------------------------------------------------------------------------

static TArray<FLuaFunctionDoc> PoseSearchDocs = {};

// ---------------------------------------------------------------------------
// Binding
// ---------------------------------------------------------------------------

static void BindPoseSearch(sol::state& Lua, FLuaSessionData& Session)
{
	// ========================================================================
	// SCHEMA enrichment
	// ========================================================================
	Lua.set_function("_enrich_pose_search_schema", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UPoseSearchSchema* Schema = LoadObject<UPoseSearchSchema>(nullptr, *FPath);
		if (!Schema) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure:\n"
			"  channel  — feature channel (Pose, Trajectory, Velocity, Position, Heading, Phase, Curve, Distance, Group, TimeToEvent)\n"
			"  skeleton — skeleton with role and optional mirror data table\n"
			"\n"
			"add(type, params):\n"
			"  add(\"channel\", {type=\"Pose\", weight=1.0, bones={{bone=\"foot_l\", flags={\"Position\",\"Velocity\"}, weight=1}}})\n"
			"  add(\"channel\", {type=\"Trajectory\", weight=1.0, samples={{offset=-0.5, flags={\"Position\"}, weight=1}}})\n"
			"  add(\"channel\", {type=\"Velocity\", weight=1.0, bone=\"hand_r\"})\n"
			"  add(\"channel\", {type=\"Position\", weight=1.0, bone=\"head\"})\n"
			"  add(\"channel\", {type=\"Heading\", weight=1.0, bone=\"root\", heading_axis=\"X\"})\n"
			"  add(\"channel\", {type=\"Phase\", weight=1.0, bone=\"foot_l\"})\n"
			"  add(\"channel\", {type=\"Curve\", weight=1.0, curve_name=\"Speed\"})\n"
			"  add(\"channel\", {type=\"Distance\", weight=1.0, bone=\"hand_r\", origin_bone=\"root\", sample_time_offset=0.5})\n"
			"  add(\"channel\", {type=\"Group\", sub_channels={{type=\"Position\", bone=\"hand_l\"}, {type=\"Velocity\", bone=\"hand_l\"}}})\n"
			"  add(\"channel\", {type=\"TimeToEvent\", weight=1.0, sampling_attribute_id=0})\n"
			"  add(\"default_channels\") — adds Pose + Trajectory defaults\n"
			"  add(\"skeleton\", {skeleton=\"/Game/SK_Mannequin\", role=\"Default\", mirror_data_table=\"/Game/MirrorTable\"})\n"
			"\n"
			"remove(type, index):\n"
			"  remove(\"channel\", 1) — 1-based index\n"
			"  remove(\"skeleton\", 1) — 1-based index\n"
			"\n"
			"list(type):\n"
			"  list(\"channels\"), list(\"skeletons\")\n"
			"\n"
			"configure({sample_rate=30, data_preprocessor=\"Normalize\", add_data_padding=true, inject_debug_channels=false}):\n"
			"  Set sample rate, data_preprocessor (None/Normalize/NormalizeOnlyByDeviation/NormalizeWithCommonSchema)\n"
			"  Also: num_permutations, permutations_sample_rate, permutations_time_offset\n"
			"\n"
			"configure(\"channel\", {index=1, weight=1.5, bone=\"foot_r\"}):\n"
			"  Modify existing channel properties by 1-based index\n"
			"\n"
			"info() — summary of schema configuration\n";

		// ---- add(type, params) ----
		AssetObj.set_function("add", [Schema, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("default_channels"), ESearchCase::IgnoreCase))
			{
				const FScopedTransaction Txn(NSLOCTEXT("AIK", "AddDefaultChannels", "AI Add Default Channels"));
				Schema->Modify();
				Schema->AddDefaultChannels();
				Schema->PostEditChange();
				Schema->GetPackage()->MarkPackageDirty();
				Session.Log(TEXT("[OK] add(\"default_channels\") -> added Pose + Trajectory defaults"));
				return sol::make_object(Lua, true);
			}

			if (FType.Equals(TEXT("skeleton"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"skeleton\") -> params required: {skeleton=\"/Game/...\", role=\"Default\"}"));
					return sol::lua_nil;
				}
				sol::table P = Params.value();
				std::string SkPath = P.get_or<std::string>("skeleton", "");
				if (SkPath.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"skeleton\") -> 'skeleton' path required"));
					return sol::lua_nil;
				}

				FString FullSkPath = UTF8_TO_TCHAR(SkPath.c_str());
				if (!FullSkPath.StartsWith(TEXT("/"))) FullSkPath = TEXT("/Game/") + FullSkPath;

				USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *FullSkPath);
				if (!Skeleton)
				{
					USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *FullSkPath);
					if (Mesh) Skeleton = Mesh->GetSkeleton();
				}
				if (!Skeleton)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"skeleton\") -> not found: %s"), *FullSkPath));
					return sol::lua_nil;
				}

				UMirrorDataTable* MirrorTable = nullptr;
				std::string MirrorStr = P.get_or<std::string>("mirror_data_table", "");
				if (!MirrorStr.empty())
				{
					FString MPath = UTF8_TO_TCHAR(MirrorStr.c_str());
					if (!MPath.StartsWith(TEXT("/"))) MPath = TEXT("/Game/") + MPath;
					MirrorTable = LoadObject<UMirrorDataTable>(nullptr, *MPath);
					if (!MirrorTable)
						Session.Log(FString::Printf(TEXT("[WARN] MirrorDataTable not found: %s"), *MPath));
				}

				std::string RoleStr = P.get_or<std::string>("role", "");
				FName Role = RoleStr.empty() ? UE::PoseSearch::DefaultRole : FName(UTF8_TO_TCHAR(RoleStr.c_str()));

				const FScopedTransaction Txn(NSLOCTEXT("AIK", "AddSchemaSkeleton", "AI Add Schema Skeleton"));
				Schema->Modify();
				Schema->AddSkeleton(Skeleton, MirrorTable, Role);
				Schema->PostEditChange();
				Schema->GetPackage()->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"skeleton\", \"%s\", role=\"%s\")"),
					*Skeleton->GetName(), *Role.ToString()));
				return sol::make_object(Lua, true);
			}

			if (!FType.Equals(TEXT("channel"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: channel, skeleton, default_channels"), *FType));
				return sol::lua_nil;
			}

			if (!Params.has_value())
			{
				Session.Log(TEXT("[FAIL] add(\"channel\") -> params required: {type=\"Pose\", ...}"));
				return sol::lua_nil;
			}
			sol::table P = Params.value();
			std::string ChannelType = P.get_or<std::string>("type", "");
			if (ChannelType.empty())
			{
				Session.Log(TEXT("[FAIL] add(\"channel\") -> 'type' required in params"));
				return sol::lua_nil;
			}

			FString CT = UTF8_TO_TCHAR(ChannelType.c_str());
			double Weight = P.get<sol::optional<double>>("weight").value_or(1.0);

			const FScopedTransaction Txn(NSLOCTEXT("AIK", "AddPSChannel", "AI Add PoseSearch Channel"));
			Schema->Modify();

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			UPoseSearchFeatureChannel* Channel = nullptr;

			if (CT.Equals(TEXT("Pose"), ESearchCase::IgnoreCase))
			{
				UPoseSearchFeatureChannel_Pose* Ch = NewObject<UPoseSearchFeatureChannel_Pose>(Schema, NAME_None, RF_Transactional);
#if WITH_EDITORONLY_DATA
				Ch->Weight = static_cast<float>(Weight);
#endif

				sol::optional<sol::table> BonesOpt = P.get<sol::optional<sol::table>>("bones");
				if (BonesOpt.has_value())
				{
					for (auto& Kv : BonesOpt.value())
					{
						sol::optional<sol::table> BoneTable = Kv.second.as<sol::optional<sol::table>>();
						if (!BoneTable.has_value()) continue;
						sol::table BT = BoneTable.value();

						FPoseSearchBone Bone;
						Bone.Reference.BoneName = FName(UTF8_TO_TCHAR(BT.get_or<std::string>("bone", "").c_str()));

						sol::optional<sol::table> FlagsOpt = BT.get<sol::optional<sol::table>>("flags");
						if (FlagsOpt.has_value())
						{
							Bone.Flags = ParseBoneFlags(FlagsOpt.value());
						}

#if WITH_EDITORONLY_DATA
						Bone.Weight = static_cast<float>(BT.get<sol::optional<double>>("weight").value_or(1.0));
#endif
						Ch->SampledBones.Add(Bone);
					}
				}

				Channel = Ch;
				Session.Log(FString::Printf(TEXT("[OK] add(\"channel\", type=\"Pose\", bones=%d)"), Ch->SampledBones.Num()));
			}
			else if (CT.Equals(TEXT("Trajectory"), ESearchCase::IgnoreCase))
			{
				UPoseSearchFeatureChannel_Trajectory* Ch = NewObject<UPoseSearchFeatureChannel_Trajectory>(Schema, NAME_None, RF_Transactional);
#if WITH_EDITORONLY_DATA
				Ch->Weight = static_cast<float>(Weight);
#endif

				sol::optional<sol::table> SamplesOpt = P.get<sol::optional<sol::table>>("samples");
				if (SamplesOpt.has_value())
				{
					for (auto& Kv : SamplesOpt.value())
					{
						sol::optional<sol::table> SampleTable = Kv.second.as<sol::optional<sol::table>>();
						if (!SampleTable.has_value()) continue;
						sol::table ST = SampleTable.value();

						FPoseSearchTrajectorySample Sample;
						Sample.Offset = static_cast<float>(ST.get<sol::optional<double>>("offset").value_or(0.0));

						sol::optional<sol::table> FlagsOpt = ST.get<sol::optional<sol::table>>("flags");
						if (FlagsOpt.has_value())
						{
							Sample.Flags = ParseTrajectoryFlags(FlagsOpt.value());
						}

#if WITH_EDITORONLY_DATA
						Sample.Weight = static_cast<float>(ST.get<sol::optional<double>>("weight").value_or(1.0));
#endif
						Ch->Samples.Add(Sample);
					}
				}

				Channel = Ch;
				Session.Log(FString::Printf(TEXT("[OK] add(\"channel\", type=\"Trajectory\", samples=%d)"), Ch->Samples.Num()));
			}
			else if (CT.Equals(TEXT("Velocity"), ESearchCase::IgnoreCase))
			{
				UPoseSearchFeatureChannel_Velocity* Ch = NewObject<UPoseSearchFeatureChannel_Velocity>(Schema, NAME_None, RF_Transactional);
#if WITH_EDITORONLY_DATA
				Ch->Weight = static_cast<float>(Weight);
#endif
				Ch->Bone.BoneName = FName(UTF8_TO_TCHAR(P.get_or<std::string>("bone", "").c_str()));
				Channel = Ch;
				Session.Log(FString::Printf(TEXT("[OK] add(\"channel\", type=\"Velocity\", bone=\"%s\")"), *Ch->Bone.BoneName.ToString()));
			}
			else if (CT.Equals(TEXT("Position"), ESearchCase::IgnoreCase))
			{
				UPoseSearchFeatureChannel_Position* Ch = NewObject<UPoseSearchFeatureChannel_Position>(Schema, NAME_None, RF_Transactional);
#if WITH_EDITORONLY_DATA
				Ch->Weight = static_cast<float>(Weight);
#endif
				Ch->Bone.BoneName = FName(UTF8_TO_TCHAR(P.get_or<std::string>("bone", "").c_str()));
				Channel = Ch;
				Session.Log(FString::Printf(TEXT("[OK] add(\"channel\", type=\"Position\", bone=\"%s\")"), *Ch->Bone.BoneName.ToString()));
			}
			else if (CT.Equals(TEXT("Heading"), ESearchCase::IgnoreCase))
			{
				UPoseSearchFeatureChannel_Heading* Ch = NewObject<UPoseSearchFeatureChannel_Heading>(Schema, NAME_None, RF_Transactional);
#if WITH_EDITORONLY_DATA
				Ch->Weight = static_cast<float>(Weight);
#endif
				Ch->Bone.BoneName = FName(UTF8_TO_TCHAR(P.get_or<std::string>("bone", "").c_str()));

				std::string AxisStr = P.get_or<std::string>("heading_axis", "X");
				FString Axis = UTF8_TO_TCHAR(AxisStr.c_str());
				if (Axis.Equals(TEXT("Y"), ESearchCase::IgnoreCase))      Ch->HeadingAxis = EHeadingAxis::Y;
				else if (Axis.Equals(TEXT("Z"), ESearchCase::IgnoreCase)) Ch->HeadingAxis = EHeadingAxis::Z;
				else                                                      Ch->HeadingAxis = EHeadingAxis::X;

				Channel = Ch;
				Session.Log(FString::Printf(TEXT("[OK] add(\"channel\", type=\"Heading\", bone=\"%s\", axis=%s)"),
					*Ch->Bone.BoneName.ToString(), *Axis));
			}
			else if (CT.Equals(TEXT("Phase"), ESearchCase::IgnoreCase))
			{
				UPoseSearchFeatureChannel_Phase* Ch = NewObject<UPoseSearchFeatureChannel_Phase>(Schema, NAME_None, RF_Transactional);
#if WITH_EDITORONLY_DATA
				Ch->Weight = static_cast<float>(Weight);
#endif
				Ch->Bone.BoneName = FName(UTF8_TO_TCHAR(P.get_or<std::string>("bone", "").c_str()));
				Channel = Ch;
				Session.Log(FString::Printf(TEXT("[OK] add(\"channel\", type=\"Phase\", bone=\"%s\")"), *Ch->Bone.BoneName.ToString()));
			}
			else if (CT.Equals(TEXT("Curve"), ESearchCase::IgnoreCase))
			{
				UPoseSearchFeatureChannel_Curve* Ch = NewObject<UPoseSearchFeatureChannel_Curve>(Schema, NAME_None, RF_Transactional);
#if WITH_EDITORONLY_DATA
				Ch->Weight = static_cast<float>(Weight);
#endif
				Ch->CurveName = FName(UTF8_TO_TCHAR(P.get_or<std::string>("curve_name", "").c_str()));
				Channel = Ch;
				Session.Log(FString::Printf(TEXT("[OK] add(\"channel\", type=\"Curve\", curve=\"%s\")"), *Ch->CurveName.ToString()));
			}
			else if (CT.Equals(TEXT("Distance"), ESearchCase::IgnoreCase))
			{
				UPoseSearchFeatureChannel_Distance* Ch = NewObject<UPoseSearchFeatureChannel_Distance>(Schema, NAME_None, RF_Transactional);
#if WITH_EDITORONLY_DATA
				Ch->Weight = static_cast<float>(Weight);
#endif
				Ch->Bone.BoneName = FName(UTF8_TO_TCHAR(P.get_or<std::string>("bone", "").c_str()));
				Ch->OriginBone.BoneName = FName(UTF8_TO_TCHAR(P.get_or<std::string>("origin_bone", "").c_str()));
				Ch->SampleTimeOffset = static_cast<float>(P.get<sol::optional<double>>("sample_time_offset").value_or(0.0));
				Ch->OriginTimeOffset = static_cast<float>(P.get<sol::optional<double>>("origin_time_offset").value_or(0.0));
				Ch->MaxDistance = static_cast<float>(P.get<sol::optional<double>>("max_distance").value_or(0.0));
				Ch->SamplingAttributeId = P.get<sol::optional<int>>("sampling_attribute_id").value_or(INDEX_NONE);

				sol::optional<bool> DefaultRootOpt = P.get<sol::optional<bool>>("default_with_root_bone");
				if (DefaultRootOpt.has_value()) Ch->bDefaultWithRootBone = DefaultRootOpt.value();

				Channel = Ch;
				Session.Log(FString::Printf(TEXT("[OK] add(\"channel\", type=\"Distance\", bone=\"%s\", origin=\"%s\")"),
					*Ch->Bone.BoneName.ToString(), *Ch->OriginBone.BoneName.ToString()));
			}
			else if (CT.Equals(TEXT("Group"), ESearchCase::IgnoreCase))
			{
				UPoseSearchFeatureChannel_Group* Ch = NewObject<UPoseSearchFeatureChannel_Group>(Schema, NAME_None, RF_Transactional);

				std::string RoleStr = P.get_or<std::string>("role", "");
				if (!RoleStr.empty()) Ch->SampleRole = FName(UTF8_TO_TCHAR(RoleStr.c_str()));

				sol::optional<int> WeightGroupOpt = P.get<sol::optional<int>>("debug_weight_group_id");
				if (WeightGroupOpt.has_value()) Ch->DebugWeightGroupID = WeightGroupOpt.value();

				// Add sub-channels from the sub_channels array
				sol::optional<sol::table> SubsOpt = P.get<sol::optional<sol::table>>("sub_channels");
				if (SubsOpt.has_value())
				{
					for (auto& Kv : SubsOpt.value())
					{
						sol::optional<sol::table> SubTable = Kv.second.as<sol::optional<sol::table>>();
						if (!SubTable.has_value()) continue;
						sol::table ST = SubTable.value();
						std::string SubType = ST.get_or<std::string>("type", "");
						float SubWeight = static_cast<float>(ST.get<sol::optional<double>>("weight").value_or(1.0));

						FString SCT = UTF8_TO_TCHAR(SubType.c_str());
						UPoseSearchFeatureChannel* SubCh = nullptr;

						if (SCT.Equals(TEXT("Position"), ESearchCase::IgnoreCase))
						{
							auto* NewCh = NewObject<UPoseSearchFeatureChannel_Position>(Ch, NAME_None, RF_Transactional);
#if WITH_EDITORONLY_DATA
							NewCh->Weight = SubWeight;
#endif
							NewCh->Bone.BoneName = FName(UTF8_TO_TCHAR(ST.get_or<std::string>("bone", "").c_str()));
							SubCh = NewCh;
						}
						else if (SCT.Equals(TEXT("Velocity"), ESearchCase::IgnoreCase))
						{
							auto* NewCh = NewObject<UPoseSearchFeatureChannel_Velocity>(Ch, NAME_None, RF_Transactional);
#if WITH_EDITORONLY_DATA
							NewCh->Weight = SubWeight;
#endif
							NewCh->Bone.BoneName = FName(UTF8_TO_TCHAR(ST.get_or<std::string>("bone", "").c_str()));
							SubCh = NewCh;
						}
						else if (SCT.Equals(TEXT("Heading"), ESearchCase::IgnoreCase))
						{
							auto* NewCh = NewObject<UPoseSearchFeatureChannel_Heading>(Ch, NAME_None, RF_Transactional);
#if WITH_EDITORONLY_DATA
							NewCh->Weight = SubWeight;
#endif
							NewCh->Bone.BoneName = FName(UTF8_TO_TCHAR(ST.get_or<std::string>("bone", "").c_str()));
							SubCh = NewCh;
						}
						else if (SCT.Equals(TEXT("Phase"), ESearchCase::IgnoreCase))
						{
							auto* NewCh = NewObject<UPoseSearchFeatureChannel_Phase>(Ch, NAME_None, RF_Transactional);
#if WITH_EDITORONLY_DATA
							NewCh->Weight = SubWeight;
#endif
							NewCh->Bone.BoneName = FName(UTF8_TO_TCHAR(ST.get_or<std::string>("bone", "").c_str()));
							SubCh = NewCh;
						}
						else if (SCT.Equals(TEXT("Distance"), ESearchCase::IgnoreCase))
						{
							auto* NewCh = NewObject<UPoseSearchFeatureChannel_Distance>(Ch, NAME_None, RF_Transactional);
#if WITH_EDITORONLY_DATA
							NewCh->Weight = SubWeight;
#endif
							NewCh->Bone.BoneName = FName(UTF8_TO_TCHAR(ST.get_or<std::string>("bone", "").c_str()));
							NewCh->OriginBone.BoneName = FName(UTF8_TO_TCHAR(ST.get_or<std::string>("origin_bone", "").c_str()));
							SubCh = NewCh;
						}

						if (SubCh) Ch->SubChannels.Add(SubCh);
					}
				}

				Channel = Ch;
				Session.Log(FString::Printf(TEXT("[OK] add(\"channel\", type=\"Group\", sub_channels=%d)"), Ch->SubChannels.Num()));
			}
			else if (CT.Equals(TEXT("TimeToEvent"), ESearchCase::IgnoreCase))
			{
				UPoseSearchFeatureChannel_TimeToEvent* Ch = NewObject<UPoseSearchFeatureChannel_TimeToEvent>(Schema, NAME_None, RF_Transactional);
				Ch->Weight = static_cast<float>(Weight);
				Ch->SamplingAttributeId = P.get<sol::optional<int>>("sampling_attribute_id").value_or(0);
				Channel = Ch;
				Session.Log(FString::Printf(TEXT("[OK] add(\"channel\", type=\"TimeToEvent\", sampling_attribute_id=%d)"), Ch->SamplingAttributeId));
			}
			else
			{
				// Dynamic fallback: try to discover any channel subclass by name
				UClass* ChClass = LuaDynamicType::FindDerivedClass(UPoseSearchFeatureChannel::StaticClass(), CT, ChannelPrefixes);
				if (!ChClass)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] add(\"channel\") -> unknown channel type '%s'. Valid: %s"),
						*CT, *LuaDynamicType::FormatAvailableTypes(UPoseSearchFeatureChannel::StaticClass(), ChannelPrefixes)));
					return sol::lua_nil;
				}
				UPoseSearchFeatureChannel* Ch = NewObject<UPoseSearchFeatureChannel>(Schema, ChClass, NAME_None, RF_Transactional);
				// Set Weight via reflection if available
				FProperty* WeightProp = ChClass->FindPropertyByName(TEXT("Weight"));
				if (WeightProp)
				{
					if (FFloatProperty* FP = CastField<FFloatProperty>(WeightProp))
						FP->SetPropertyValue_InContainer(Ch, static_cast<float>(Weight));
				}
				Channel = Ch;
				Session.Log(FString::Printf(TEXT("[OK] add(\"channel\", type=\"%s\") -> created via dynamic discovery"), *CT));
			}

			if (Channel)
			{
				Schema->AddChannel(Channel);
				Schema->PostEditChange();
				Schema->GetPackage()->MarkPackageDirty();
			}

			return Channel ? sol::make_object(Lua, true) : sol::lua_nil;
#else
			Session.Log(TEXT("[FAIL] add(\"channel\") -> feature channel editing requires UE 5.6+"));
			return sol::lua_nil;
#endif // ENGINE_MINOR_VERSION >= 6
		});

		// ---- remove(type, index) ----
		AssetObj.set_function("remove", [Schema, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (!Id.is<int>())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> 1-based index required"), *FType));
				return sol::lua_nil;
			}

			int32 LuaIdx = Id.as<int>();
			int32 Idx = LuaIdx - 1;

			if (FType.Equals(TEXT("skeleton"), ESearchCase::IgnoreCase))
			{
				// Access private Skeletons via reflection
				FProperty* SkeletonsProp = UPoseSearchSchema::StaticClass()->FindPropertyByName(TEXT("Skeletons"));
				if (!SkeletonsProp)
				{
					Session.Log(TEXT("[FAIL] remove(\"skeleton\") -> could not access Skeletons array"));
					return sol::lua_nil;
				}
				auto* Skeletons = SkeletonsProp->ContainerPtrToValuePtr<TArray<FPoseSearchRoledSkeleton>>(Schema);
				if (!Skeletons || Idx < 0 || Idx >= Skeletons->Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"skeleton\", %d) -> index out of range (1..%d)"),
						LuaIdx, Skeletons ? Skeletons->Num() : 0));
					return sol::lua_nil;
				}

				const FScopedTransaction Txn(NSLOCTEXT("AIK", "RemoveSchemaSkeleton", "AI Remove Schema Skeleton"));
				Schema->Modify();
				Skeletons->RemoveAt(Idx);
				Schema->PostEditChange();
				Schema->GetPackage()->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"skeleton\", %d)"), LuaIdx));
				return sol::make_object(Lua, true);
			}

			if (!FType.Equals(TEXT("channel"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: channel, skeleton"), *FType));
				return sol::lua_nil;
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			TArray<TObjectPtr<UPoseSearchFeatureChannel>>* Channels = GetChannelsArrayMutable(Schema);
			if (!Channels)
			{
				Session.Log(TEXT("[FAIL] remove(\"channel\") -> could not access Channels array"));
				return sol::lua_nil;
			}

			if (Idx < 0 || Idx >= Channels->Num())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"channel\", %d) -> index out of range (1..%d)"), LuaIdx, Channels->Num()));
				return sol::lua_nil;
			}

			const FScopedTransaction Txn(NSLOCTEXT("AIK", "RemovePSChannel", "AI Remove PoseSearch Channel"));
			Schema->Modify();
			Channels->RemoveAt(Idx);
			Schema->PostEditChange();
			Schema->GetPackage()->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] remove(\"channel\", %d)"), LuaIdx));
			return sol::make_object(Lua, true);
#else
			Session.Log(TEXT("[FAIL] remove(\"channel\") -> feature channel editing requires UE 5.6+"));
			return sol::lua_nil;
#endif // ENGINE_MINOR_VERSION >= 6
		});

		// ---- list(type?) ----
		AssetObj.set_function("list", [Schema, &Session](sol::table self,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = self["info"];
				if (InfoFn.valid()) return InfoFn(self);
				return sol::lua_nil;
			}

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			if (FType.Equals(TEXT("channels"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("channel"), ESearchCase::IgnoreCase))
			{
				// Use the raw editable Channels array (not GetChannels() which returns FinalizedChannels
				// that may be empty if Finalize() failed, e.g. due to missing skeleton)
				const TArray<TObjectPtr<UPoseSearchFeatureChannel>>* ChannelsPtr = GetChannelsArrayMutable(Schema);
				if (!ChannelsPtr)
				{
					Session.Log(TEXT("[FAIL] list(\"channels\") -> could not access Channels array"));
					return sol::lua_nil;
				}
				TConstArrayView<TObjectPtr<UPoseSearchFeatureChannel>> Channels = *ChannelsPtr;
				sol::table Result = Lua.create_table();
				int32 i = 0;
				for (const TObjectPtr<UPoseSearchFeatureChannel>& Ch : Channels)
				{
					if (!Ch) continue;
					sol::table E = Lua.create_table();
					E["index"] = i + 1; // 1-based Lua index
					E["type"] = ChannelTypeString(Ch);

					// Channel-specific properties
					if (const auto* Pose = Cast<UPoseSearchFeatureChannel_Pose>(Ch.Get()))
					{
#if WITH_EDITORONLY_DATA
						E["weight"] = Pose->Weight;
#endif
						E["bones"] = static_cast<int>(Pose->SampledBones.Num());
					}
					else if (const auto* Traj = Cast<UPoseSearchFeatureChannel_Trajectory>(Ch.Get()))
					{
#if WITH_EDITORONLY_DATA
						E["weight"] = Traj->Weight;
#endif
						E["samples"] = static_cast<int>(Traj->Samples.Num());
					}
					else if (const auto* Vel = Cast<UPoseSearchFeatureChannel_Velocity>(Ch.Get()))
					{
#if WITH_EDITORONLY_DATA
						E["weight"] = Vel->Weight;
#endif
						E["bone"] = TCHAR_TO_UTF8(*Vel->Bone.BoneName.ToString());
					}
					else if (const auto* Pos = Cast<UPoseSearchFeatureChannel_Position>(Ch.Get()))
					{
#if WITH_EDITORONLY_DATA
						E["weight"] = Pos->Weight;
#endif
						E["bone"] = TCHAR_TO_UTF8(*Pos->Bone.BoneName.ToString());
					}
					else if (const auto* Head = Cast<UPoseSearchFeatureChannel_Heading>(Ch.Get()))
					{
#if WITH_EDITORONLY_DATA
						E["weight"] = Head->Weight;
#endif
						E["bone"] = TCHAR_TO_UTF8(*Head->Bone.BoneName.ToString());
						E["heading_axis"] = Head->HeadingAxis == EHeadingAxis::Y ? "Y" : (Head->HeadingAxis == EHeadingAxis::Z ? "Z" : "X");
					}
					else if (const auto* Phase = Cast<UPoseSearchFeatureChannel_Phase>(Ch.Get()))
					{
#if WITH_EDITORONLY_DATA
						E["weight"] = Phase->Weight;
#endif
						E["bone"] = TCHAR_TO_UTF8(*Phase->Bone.BoneName.ToString());
					}
					else if (const auto* Curve = Cast<UPoseSearchFeatureChannel_Curve>(Ch.Get()))
					{
#if WITH_EDITORONLY_DATA
						E["weight"] = Curve->Weight;
#endif
						E["curve_name"] = TCHAR_TO_UTF8(*Curve->CurveName.ToString());
					}
					else if (const auto* Dist = Cast<UPoseSearchFeatureChannel_Distance>(Ch.Get()))
					{
#if WITH_EDITORONLY_DATA
						E["weight"] = Dist->Weight;
#endif
						E["bone"] = TCHAR_TO_UTF8(*Dist->Bone.BoneName.ToString());
						E["origin_bone"] = TCHAR_TO_UTF8(*Dist->OriginBone.BoneName.ToString());
						E["sample_time_offset"] = Dist->SampleTimeOffset;
						E["origin_time_offset"] = Dist->OriginTimeOffset;
						E["max_distance"] = Dist->MaxDistance;
					}
					else if (const auto* Group = Cast<UPoseSearchFeatureChannel_Group>(Ch.Get()))
					{
						E["sub_channels"] = static_cast<int>(Group->SubChannels.Num());
						E["role"] = TCHAR_TO_UTF8(*Group->SampleRole.ToString());
					}
					else if (const auto* TTE = Cast<UPoseSearchFeatureChannel_TimeToEvent>(Ch.Get()))
					{
						E["weight"] = TTE->Weight; // TimeToEvent Weight is NOT editor-only
						E["sampling_attribute_id"] = TTE->SamplingAttributeId;
					}

					Result[i + 1] = E;
					i++;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"channels\") -> %d"), i));
				return Result;
			}
#endif // ENGINE_MINOR_VERSION >= 6

	#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			if (FType.Equals(TEXT("skeletons"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("skeleton"), ESearchCase::IgnoreCase))
			{
				const TArray<FPoseSearchRoledSkeleton>& Skeletons = Schema->GetRoledSkeletons();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Skeletons.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = i + 1;
					E["skeleton"] = Skeletons[i].Skeleton ? TCHAR_TO_UTF8(*Skeletons[i].Skeleton->GetName()) : "None";
					E["role"] = TCHAR_TO_UTF8(*Skeletons[i].Role.ToString());
					E["mirror_data_table"] = Skeletons[i].MirrorDataTable ? TCHAR_TO_UTF8(*Skeletons[i].MirrorDataTable->GetName()) : "None";
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"skeletons\") -> %d"), Skeletons.Num()));
				return Result;
			}
#endif

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: channels, skeletons"), *FType));
			return sol::lua_nil;
		});

		// ---- configure({skeleton=..}) or configure("channel", {index=1, weight=..}) ----
		AssetObj.set_function("configure", [Schema, &Session](sol::table /*self*/,
			sol::object FirstArg, sol::optional<sol::table> SecondArg, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			// ---- configure("channel", {index=N, ...}) form ----
			if (FirstArg.is<std::string>())
			{
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
				FString FType = UTF8_TO_TCHAR(FirstArg.as<std::string>().c_str());
				if (!FType.Equals(TEXT("channel"), ESearchCase::IgnoreCase))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: channel"), *FType));
					return sol::lua_nil;
				}
				if (!SecondArg.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"channel\") -> params table required: {index=1, weight=..}"));
					return sol::lua_nil;
				}
				sol::table P = SecondArg.value();
				sol::optional<int> IndexOpt = P.get<sol::optional<int>>("index");
				if (!IndexOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"channel\") -> 'index' (1-based) required"));
					return sol::lua_nil;
				}

				int32 Idx = IndexOpt.value() - 1;
				TArray<TObjectPtr<UPoseSearchFeatureChannel>>* Channels = GetChannelsArrayMutable(Schema);
				if (!Channels || Idx < 0 || Idx >= Channels->Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"channel\", index=%d) -> out of range"), IndexOpt.value()));
					return sol::lua_nil;
				}

				UPoseSearchFeatureChannel* Ch = (*Channels)[Idx];
				if (!Ch)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"channel\", index=%d) -> null channel"), IndexOpt.value()));
					return sol::lua_nil;
				}

				const FScopedTransaction Txn(NSLOCTEXT("AIK", "ConfigureChannel", "AI Configure PoseSearch Channel"));
				Schema->Modify();
				int32 Changes = 0;

				// Weight (common to most channel types, but editor-only on some)
				sol::optional<double> WeightOpt = P.get<sol::optional<double>>("weight");

				if (auto* Pose = Cast<UPoseSearchFeatureChannel_Pose>(Ch))
				{
#if WITH_EDITORONLY_DATA
					if (WeightOpt.has_value()) { Pose->Weight = static_cast<float>(WeightOpt.value()); Changes++; }
#endif
				}
				else if (auto* Traj = Cast<UPoseSearchFeatureChannel_Trajectory>(Ch))
				{
#if WITH_EDITORONLY_DATA
					if (WeightOpt.has_value()) { Traj->Weight = static_cast<float>(WeightOpt.value()); Changes++; }
#endif
				}
				else if (auto* Vel = Cast<UPoseSearchFeatureChannel_Velocity>(Ch))
				{
#if WITH_EDITORONLY_DATA
					if (WeightOpt.has_value()) { Vel->Weight = static_cast<float>(WeightOpt.value()); Changes++; }
#endif
					std::string BoneStr = P.get_or<std::string>("bone", "");
					if (!BoneStr.empty()) { Vel->Bone.BoneName = FName(UTF8_TO_TCHAR(BoneStr.c_str())); Changes++; }
				}
				else if (auto* Pos = Cast<UPoseSearchFeatureChannel_Position>(Ch))
				{
#if WITH_EDITORONLY_DATA
					if (WeightOpt.has_value()) { Pos->Weight = static_cast<float>(WeightOpt.value()); Changes++; }
#endif
					std::string BoneStr = P.get_or<std::string>("bone", "");
					if (!BoneStr.empty()) { Pos->Bone.BoneName = FName(UTF8_TO_TCHAR(BoneStr.c_str())); Changes++; }
				}
				else if (auto* Head = Cast<UPoseSearchFeatureChannel_Heading>(Ch))
				{
#if WITH_EDITORONLY_DATA
					if (WeightOpt.has_value()) { Head->Weight = static_cast<float>(WeightOpt.value()); Changes++; }
#endif
					std::string BoneStr = P.get_or<std::string>("bone", "");
					if (!BoneStr.empty()) { Head->Bone.BoneName = FName(UTF8_TO_TCHAR(BoneStr.c_str())); Changes++; }
					std::string AxisStr = P.get_or<std::string>("heading_axis", "");
					if (!AxisStr.empty())
					{
						FString Axis = UTF8_TO_TCHAR(AxisStr.c_str());
						if (Axis.Equals(TEXT("Y"), ESearchCase::IgnoreCase))      Head->HeadingAxis = EHeadingAxis::Y;
						else if (Axis.Equals(TEXT("Z"), ESearchCase::IgnoreCase)) Head->HeadingAxis = EHeadingAxis::Z;
						else                                                      Head->HeadingAxis = EHeadingAxis::X;
						Changes++;
					}
				}
				else if (auto* Phase = Cast<UPoseSearchFeatureChannel_Phase>(Ch))
				{
#if WITH_EDITORONLY_DATA
					if (WeightOpt.has_value()) { Phase->Weight = static_cast<float>(WeightOpt.value()); Changes++; }
#endif
					std::string BoneStr = P.get_or<std::string>("bone", "");
					if (!BoneStr.empty()) { Phase->Bone.BoneName = FName(UTF8_TO_TCHAR(BoneStr.c_str())); Changes++; }
				}
				else if (auto* Curve = Cast<UPoseSearchFeatureChannel_Curve>(Ch))
				{
#if WITH_EDITORONLY_DATA
					if (WeightOpt.has_value()) { Curve->Weight = static_cast<float>(WeightOpt.value()); Changes++; }
#endif
					std::string CurveStr = P.get_or<std::string>("curve_name", "");
					if (!CurveStr.empty()) { Curve->CurveName = FName(UTF8_TO_TCHAR(CurveStr.c_str())); Changes++; }
				}
				else if (auto* Dist = Cast<UPoseSearchFeatureChannel_Distance>(Ch))
				{
#if WITH_EDITORONLY_DATA
					if (WeightOpt.has_value()) { Dist->Weight = static_cast<float>(WeightOpt.value()); Changes++; }
#endif
					std::string BoneStr = P.get_or<std::string>("bone", "");
					if (!BoneStr.empty()) { Dist->Bone.BoneName = FName(UTF8_TO_TCHAR(BoneStr.c_str())); Changes++; }
					std::string OriginStr = P.get_or<std::string>("origin_bone", "");
					if (!OriginStr.empty()) { Dist->OriginBone.BoneName = FName(UTF8_TO_TCHAR(OriginStr.c_str())); Changes++; }
					sol::optional<double> STO = P.get<sol::optional<double>>("sample_time_offset");
					if (STO.has_value()) { Dist->SampleTimeOffset = static_cast<float>(STO.value()); Changes++; }
					sol::optional<double> OTO = P.get<sol::optional<double>>("origin_time_offset");
					if (OTO.has_value()) { Dist->OriginTimeOffset = static_cast<float>(OTO.value()); Changes++; }
					sol::optional<double> MaxDist = P.get<sol::optional<double>>("max_distance");
					if (MaxDist.has_value()) { Dist->MaxDistance = static_cast<float>(MaxDist.value()); Changes++; }
				}
				else if (auto* TTE = Cast<UPoseSearchFeatureChannel_TimeToEvent>(Ch))
				{
					if (WeightOpt.has_value()) { TTE->Weight = static_cast<float>(WeightOpt.value()); Changes++; }
					sol::optional<int> SampAttr = P.get<sol::optional<int>>("sampling_attribute_id");
					if (SampAttr.has_value()) { TTE->SamplingAttributeId = SampAttr.value(); Changes++; }
				}

				if (Changes > 0)
				{
					Schema->PostEditChange();
					Schema->GetPackage()->MarkPackageDirty();
					Session.Log(FString::Printf(TEXT("[OK] configure(\"channel\", index=%d) -> %d changes"), IndexOpt.value(), Changes));
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"channel\", index=%d) -> no applicable properties changed"), IndexOpt.value()));
					return sol::lua_nil;
				}
				return sol::make_object(Lua, Changes);
#else
				Session.Log(TEXT("[FAIL] configure(\"channel\") -> feature channel editing requires UE 5.6+"));
				return sol::lua_nil;
#endif // ENGINE_MINOR_VERSION >= 6
			}

			// ---- configure({skeleton=.., sample_rate=.., data_preprocessor=..}) form ----
			if (!FirstArg.is<sol::table>())
			{
				Session.Log(TEXT("[FAIL] configure() -> pass a table {skeleton=.., sample_rate=..} or (\"channel\", {index=..})"));
				return sol::lua_nil;
			}
			sol::table Params = FirstArg.as<sol::table>();

			const FScopedTransaction Txn(NSLOCTEXT("AIK", "ConfigureSchema", "AI Configure PoseSearch Schema"));
			Schema->Modify();
			int32 Changes = 0;

			// Skeleton
			sol::optional<std::string> SkeletonOpt = Params.get<sol::optional<std::string>>("skeleton");
			if (SkeletonOpt.has_value())
			{
				FString SkPath = UTF8_TO_TCHAR(SkeletonOpt.value().c_str());
				if (!SkPath.StartsWith(TEXT("/"))) SkPath = TEXT("/Game/") + SkPath;

				USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkPath);
				if (!Skeleton)
				{
					USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *SkPath);
					if (Mesh) Skeleton = Mesh->GetSkeleton();
				}

				if (!Skeleton)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(skeleton) -> not found: %s"), *SkPath));
					return sol::lua_nil;
				}

				// Clear existing skeletons via reflection (private member)
				FProperty* SkeletonsProp = UPoseSearchSchema::StaticClass()->FindPropertyByName(TEXT("Skeletons"));
				if (SkeletonsProp)
				{
					SkeletonsProp->ClearValue(SkeletonsProp->ContainerPtrToValuePtr<void>(Schema));
				}

				UMirrorDataTable* MirrorTable = nullptr;
				sol::optional<std::string> MirrorOpt = Params.get<sol::optional<std::string>>("mirror_data_table");
				if (MirrorOpt.has_value())
				{
					FString MPath = UTF8_TO_TCHAR(MirrorOpt.value().c_str());
					if (!MPath.StartsWith(TEXT("/"))) MPath = TEXT("/Game/") + MPath;
					MirrorTable = LoadObject<UMirrorDataTable>(nullptr, *MPath);
					if (!MirrorTable)
					{
						Session.Log(FString::Printf(TEXT("[WARN] MirrorDataTable not found: %s"), *MPath));
					}
				}

				Schema->AddSkeleton(Skeleton, MirrorTable);
				Session.Log(FString::Printf(TEXT("[OK] configure(skeleton=\"%s\")"), *Skeleton->GetName()));
				Changes++;
			}

			// Sample rate
			sol::optional<int> SampleRateOpt = Params.get<sol::optional<int>>("sample_rate");
			if (SampleRateOpt.has_value())
			{
				Schema->SampleRate = FMath::Clamp(SampleRateOpt.value(), 1, 240);
				Session.Log(FString::Printf(TEXT("[OK] configure(sample_rate=%d)"), Schema->SampleRate));
				Changes++;
			}

			// Data preprocessor (editor-only)
#if WITH_EDITORONLY_DATA
			sol::optional<std::string> PreprocessorOpt = Params.get<sol::optional<std::string>>("data_preprocessor");
			if (PreprocessorOpt.has_value())
			{
				Schema->DataPreprocessor = ParseDataPreprocessor(PreprocessorOpt.value());
				Session.Log(FString::Printf(TEXT("[OK] configure(data_preprocessor=\"%s\")"), UTF8_TO_TCHAR(PreprocessorOpt.value().c_str())));
				Changes++;
			}

			// Permutation settings (editor-only)
			sol::optional<int> NumPermOpt = Params.get<sol::optional<int>>("num_permutations");
			if (NumPermOpt.has_value())
			{
				Schema->NumberOfPermutations = FMath::Max(1, NumPermOpt.value());
				Session.Log(FString::Printf(TEXT("[OK] configure(num_permutations=%d)"), Schema->NumberOfPermutations));
				Changes++;
			}

			sol::optional<int> PermSROpt = Params.get<sol::optional<int>>("permutations_sample_rate");
			if (PermSROpt.has_value())
			{
				Schema->PermutationsSampleRate = FMath::Clamp(PermSROpt.value(), 1, 240);
				Session.Log(FString::Printf(TEXT("[OK] configure(permutations_sample_rate=%d)"), Schema->PermutationsSampleRate));
				Changes++;
			}

			sol::optional<double> PermTOOpt = Params.get<sol::optional<double>>("permutations_time_offset");
			if (PermTOOpt.has_value())
			{
				Schema->PermutationsTimeOffset = static_cast<float>(PermTOOpt.value());
				Session.Log(FString::Printf(TEXT("[OK] configure(permutations_time_offset=%f)"), Schema->PermutationsTimeOffset));
				Changes++;
			}
#endif

			// Performance / debug settings (not editor-only)
			sol::optional<bool> PadOpt = Params.get<sol::optional<bool>>("add_data_padding");
			if (PadOpt.has_value())
			{
				Schema->bAddDataPadding = PadOpt.value();
				Session.Log(FString::Printf(TEXT("[OK] configure(add_data_padding=%s)"), Schema->bAddDataPadding ? TEXT("true") : TEXT("false")));
				Changes++;
			}

			sol::optional<bool> InjectDebugOpt = Params.get<sol::optional<bool>>("inject_debug_channels");
			if (InjectDebugOpt.has_value())
			{
				Schema->bInjectAdditionalDebugChannels = InjectDebugOpt.value();
				Session.Log(FString::Printf(TEXT("[OK] configure(inject_debug_channels=%s)"), Schema->bInjectAdditionalDebugChannels ? TEXT("true") : TEXT("false")));
				Changes++;
			}

			if (Changes > 0)
			{
				Schema->PostEditChange();
				Schema->GetPackage()->MarkPackageDirty();
			}
			else
			{
				Session.Log(TEXT("[FAIL] configure() -> no valid parameters. Use: skeleton, mirror_data_table, sample_rate, data_preprocessor, num_permutations, permutations_sample_rate, permutations_time_offset, add_data_padding, inject_debug_channels"));
				return sol::lua_nil;
			}

			return sol::make_object(Lua, Changes);
		});

		// ---- info() — override default ----
		AssetObj.set_function("info", [Schema, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			Result["sample_rate"] = Schema->SampleRate;
			Result["add_data_padding"] = Schema->bAddDataPadding;
			Result["inject_debug_channels"] = Schema->bInjectAdditionalDebugChannels;

			AddSchemaEditorOnlyInfo(Schema, Result);

#if WITH_EDITORONLY_DATA
			Result["num_permutations"] = Schema->NumberOfPermutations;
			Result["permutations_sample_rate"] = Schema->PermutationsSampleRate;
			Result["permutations_time_offset"] = Schema->PermutationsTimeOffset;
#endif

			// Skeleton info
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			const TArray<FPoseSearchRoledSkeleton>& Skeletons = Schema->GetRoledSkeletons();
			if (Skeletons.Num() > 0 && Skeletons[0].Skeleton)
			{
				Result["skeleton"] = TCHAR_TO_UTF8(*Skeletons[0].Skeleton->GetName());
			}
			else
			{
				Result["skeleton"] = "None";
			}
			Result["num_skeletons"] = static_cast<int>(Skeletons.Num());
#endif

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 6
			// Use raw Channels array for accurate count (GetChannels() returns FinalizedChannels)
			const TArray<TObjectPtr<UPoseSearchFeatureChannel>>* ChannelsPtr = GetChannelsArrayMutable(Schema);
			int32 NumChannels = ChannelsPtr ? ChannelsPtr->Num() : 0;
			Result["num_channels"] = NumChannels;

			// Channel types summary
			sol::table ChTypes = Lua.create_table();
			int32 Idx = 0;
			if (ChannelsPtr)
			{
				for (const TObjectPtr<UPoseSearchFeatureChannel>& Ch : *ChannelsPtr)
				{
					if (!Ch) continue;
					ChTypes[++Idx] = ChannelTypeString(Ch);
				}
			}
			Result["channel_types"] = ChTypes;
#else
			Result["num_channels"] = 0;
#endif // ENGINE_MINOR_VERSION >= 6

#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 5
			Session.Log(FString::Printf(TEXT("[OK] info() -> %d channels, %d skeletons, sample_rate=%d"),
				Result.get<int>("num_channels"), Skeletons.Num(), Schema->SampleRate));
#else
			Session.Log(FString::Printf(TEXT("[OK] info() -> %d channels, sample_rate=%d"),
				Result.get<int>("num_channels"), Schema->SampleRate));
#endif
			return Result;
		});
	});

	// ========================================================================
	// DATABASE enrichment
	// ========================================================================
	Lua.set_function("_enrich_pose_search_database", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UPoseSearchDatabase* Database = LoadObject<UPoseSearchDatabase>(nullptr, *FPath);
		if (!Database) return;


#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list/configure:\n"
			"  animation — animation asset entry (AnimSequence, BlendSpace, AnimComposite, AnimMontage)\n"
			"  tag — metadata tag (FName)\n"
			"\n"
			"add(type, params):\n"
			"  add(\"animation\", {asset=\"/Game/Anims/Walk\", enabled=true, mirror_option=\"UnmirroredOnly\", sampling_range={0,0}})\n"
			"  add(\"tag\", {name=\"Locomotion\"})\n"
			"\n"
			"remove(type, index):\n"
			"  remove(\"animation\", 1) — 1-based index\n"
			"  remove(\"tag\", 1) — 1-based index\n"
			"\n"
			"configure(\"animation\", {index=1, enabled=false, mirror_option=\"MirroredOnly\", sampling_range={0.1, 2.5}})\n"
			"configure({search_mode=\"PCAKDTree\", num_principal_components=4, kd_tree_max_leaf_size=16})\n"
			"  search_mode: BruteForce, PCAKDTree, VPTree, EventOnly\n"
			"  Also: schema, kd_tree_query_num_neighbors, continuing_pose_cost_bias, base_cost_bias,\n"
			"    looping_cost_bias, continuing_interaction_cost_bias,\n"
			"    exclude_from_database_parameters={min,max}\n"
			"\n"
			"list(type):\n"
			"  list(\"animations\"), list(\"tags\")\n"
			"\n"
			"rebuild_index() — trigger async index rebuild\n"
			"\n"
			"info() — summary of database\n";

		// ---- add(type, params) ----
		AssetObj.set_function("add", [Database, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<sol::table> Params, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("tag"), ESearchCase::IgnoreCase))
			{
				if (!Params.has_value())
				{
					Session.Log(TEXT("[FAIL] add(\"tag\") -> params required: {name=\"TagName\"}"));
					return sol::lua_nil;
				}
				std::string TagName = Params.value().get_or<std::string>("name", "");
				if (TagName.empty())
				{
					Session.Log(TEXT("[FAIL] add(\"tag\") -> 'name' required"));
					return sol::lua_nil;
				}

				const FScopedTransaction Txn(NSLOCTEXT("AIK", "AddDBTag", "AI Add Database Tag"));
				Database->Modify();
				Database->Tags.AddUnique(FName(UTF8_TO_TCHAR(TagName.c_str())));
				Database->PostEditChange();
				Database->GetPackage()->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] add(\"tag\", \"%s\")"), UTF8_TO_TCHAR(TagName.c_str())));
				return sol::make_object(Lua, true);
			}

			if (!FType.Equals(TEXT("animation"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: animation, tag"), *FType));
				return sol::lua_nil;
			}

			if (!Params.has_value())
			{
				Session.Log(TEXT("[FAIL] add(\"animation\") -> params required: {asset=\"...\"}"));
				return sol::lua_nil;
			}

			sol::table P = Params.value();
			std::string AssetPath = P.get_or<std::string>("asset", "");
			if (AssetPath.empty())
			{
				Session.Log(TEXT("[FAIL] add(\"animation\") -> 'asset' path required"));
				return sol::lua_nil;
			}

			FString FullPath = UTF8_TO_TCHAR(AssetPath.c_str());
			if (!FullPath.StartsWith(TEXT("/"))) FullPath = TEXT("/Game/") + FullPath;

			UObject* AnimAsset = LoadObject<UObject>(nullptr, *FullPath);
			if (!AnimAsset)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"animation\") -> asset not found: %s"), *FullPath));
				return sol::lua_nil;
			}

			const FScopedTransaction Txn(NSLOCTEXT("AIK", "AddDBAnimation", "AI Add Database Animation"));
			Database->Modify();

			FPoseSearchDatabaseAnimationAsset Entry;
			Entry.AnimAsset = AnimAsset;
#if WITH_EDITORONLY_DATA
			Entry.bEnabled = P.get<sol::optional<bool>>("enabled").value_or(true);

			std::string MirrorStr = P.get_or<std::string>("mirror_option", "UnmirroredOnly");
			Entry.MirrorOption = ParseMirrorOption(MirrorStr);

			sol::optional<sol::table> RangeOpt = P.get<sol::optional<sol::table>>("sampling_range");
			if (RangeOpt.has_value())
			{
				sol::table R = RangeOpt.value();
				float Min = static_cast<float>(R.get<sol::optional<double>>(1).value_or(0.0));
				float Max = static_cast<float>(R.get<sol::optional<double>>(2).value_or(0.0));
				Entry.SamplingRange = FFloatInterval(Min, Max);
			}
#endif

			Database->AddAnimationAsset(Entry);
			Database->PostEditChange();
			Database->GetPackage()->MarkPackageDirty();
			RequestDatabaseRebuild(Database);

			int32 NewIndex = Database->GetNumAnimationAssets();
			Session.Log(FString::Printf(TEXT("[OK] add(\"animation\", \"%s\") -> index %d"), *AnimAsset->GetName(), NewIndex));
			return sol::make_object(Lua, NewIndex); // 1-based for Lua
		});

		// ---- remove(type, index) ----
		AssetObj.set_function("remove", [Database, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (FType.Equals(TEXT("tag"), ESearchCase::IgnoreCase))
			{
				if (!Id.is<int>())
				{
					Session.Log(TEXT("[FAIL] remove(\"tag\") -> 1-based index required"));
					return sol::lua_nil;
				}
				int32 LuaIdx = Id.as<int>();
				int32 Idx = LuaIdx - 1;
				if (Idx < 0 || Idx >= Database->Tags.Num())
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"tag\", %d) -> index out of range (1..%d)"), LuaIdx, Database->Tags.Num()));
					return sol::lua_nil;
				}

				const FScopedTransaction Txn(NSLOCTEXT("AIK", "RemoveDBTag", "AI Remove Database Tag"));
				Database->Modify();
				Database->Tags.RemoveAt(Idx);
				Database->PostEditChange();
				Database->GetPackage()->MarkPackageDirty();

				Session.Log(FString::Printf(TEXT("[OK] remove(\"tag\", %d)"), LuaIdx));
				return sol::make_object(Lua, true);
			}

			if (!FType.Equals(TEXT("animation"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: animation, tag"), *FType));
				return sol::lua_nil;
			}

			// Support single index or table of indices
			TArray<int32> Indices;
			if (Id.is<int>())
			{
				Indices.Add(Id.as<int>());
			}
			else if (Id.is<sol::table>())
			{
				sol::table T = Id.as<sol::table>();
				for (auto& Kv : T)
				{
					sol::optional<int> V = Kv.second.as<sol::optional<int>>();
					if (V.has_value()) Indices.AddUnique(V.value());
				}
			}
			else
			{
				Session.Log(TEXT("[FAIL] remove(\"animation\") -> 1-based index or table of indices required"));
				return sol::lua_nil;
			}

			// Sort descending to safely remove without index shifting
			Indices.Sort([](int32 A, int32 B) { return A > B; });

			const FScopedTransaction Txn(NSLOCTEXT("AIK", "RemoveDBAnimation", "AI Remove Database Animation"));
			Database->Modify();

			int32 Removed = 0;
			for (int32 LuaIdx : Indices)
			{
				int32 Idx = LuaIdx - 1; // 1-based to 0-based
				if (Idx >= 0 && Idx < Database->GetNumAnimationAssets())
				{
					Database->RemoveAnimationAssetAt(Idx);
					Session.Log(FString::Printf(TEXT("[OK] remove(\"animation\", %d)"), LuaIdx));
					Removed++;
				}
				else
				{
					Session.Log(FString::Printf(TEXT("[FAIL] remove(\"animation\", %d) -> index out of range"), LuaIdx));
				}
			}

			if (Removed > 0)
			{
				Database->PostEditChange();
				Database->GetPackage()->MarkPackageDirty();
				RequestDatabaseRebuild(Database);
			}

			return Removed > 0 ? sol::make_object(Lua, Removed) : sol::lua_nil;
		});

		// ---- configure("animation", {..}) or configure({search_mode=..}) ----
		AssetObj.set_function("configure", [Database, &Session](sol::table /*self*/,
			sol::object FirstArg, sol::optional<sol::table> SecondArg, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);

			// ---- configure("animation", {index=.., ...}) form ----
			if (FirstArg.is<std::string>())
			{
				FString FType = UTF8_TO_TCHAR(FirstArg.as<std::string>().c_str());
				if (!FType.Equals(TEXT("animation"), ESearchCase::IgnoreCase))
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"%s\") -> unknown type. Valid: animation"), *FType));
					return sol::lua_nil;
				}
				if (!SecondArg.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"animation\") -> params table required"));
					return sol::lua_nil;
				}
				sol::table Params = SecondArg.value();

				sol::optional<int> IndexOpt = Params.get<sol::optional<int>>("index");
				if (!IndexOpt.has_value())
				{
					Session.Log(TEXT("[FAIL] configure(\"animation\") -> 'index' (1-based) required"));
					return sol::lua_nil;
				}

				int32 Idx = IndexOpt.value() - 1;
				FPoseSearchDatabaseAnimationAsset* Entry = Database->GetMutableDatabaseAnimationAsset(Idx);
				if (!Entry)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(\"animation\", index=%d) -> out of range"), IndexOpt.value()));
					return sol::lua_nil;
				}

				const FScopedTransaction Txn(NSLOCTEXT("AIK", "ConfigureDBAnimation", "AI Configure Database Animation"));
				Database->Modify();
				int32 Changes = 0;

#if WITH_EDITORONLY_DATA
				sol::optional<bool> EnabledOpt = Params.get<sol::optional<bool>>("enabled");
				if (EnabledOpt.has_value()) { Entry->bEnabled = EnabledOpt.value(); Changes++; }

				sol::optional<std::string> MirrorOpt = Params.get<sol::optional<std::string>>("mirror_option");
				if (MirrorOpt.has_value()) { Entry->MirrorOption = ParseMirrorOption(MirrorOpt.value()); Changes++; }

				sol::optional<sol::table> RangeOpt = Params.get<sol::optional<sol::table>>("sampling_range");
				if (RangeOpt.has_value())
				{
					sol::table R = RangeOpt.value();
					float Min = static_cast<float>(R.get<sol::optional<double>>(1).value_or(0.0));
					float Max = static_cast<float>(R.get<sol::optional<double>>(2).value_or(0.0));
					Entry->SamplingRange = FFloatInterval(Min, Max);
					Changes++;
				}

				sol::optional<bool> DisableReselOpt = Params.get<sol::optional<bool>>("disable_reselection");
				if (DisableReselOpt.has_value()) { Entry->bDisableReselection = DisableReselOpt.value(); Changes++; }
#endif

				if (Changes > 0)
				{
					Database->PostEditChange();
					Database->GetPackage()->MarkPackageDirty();
					RequestDatabaseRebuild(Database);
					Session.Log(FString::Printf(TEXT("[OK] configure(\"animation\", index=%d) -> %d changes"), IndexOpt.value(), Changes));
				}
				else
				{
					Session.Log(TEXT("[FAIL] configure(\"animation\") -> no valid parameters. Use: enabled, mirror_option, sampling_range, disable_reselection"));
					return sol::lua_nil;
				}
				return sol::make_object(Lua, Changes);
			}

			// ---- configure({search_mode=.., ...}) form for database-level settings ----
			if (!FirstArg.is<sol::table>())
			{
				Session.Log(TEXT("[FAIL] configure() -> pass a table {search_mode=..} or (\"animation\", {index=..})"));
				return sol::lua_nil;
			}
			sol::table Params = FirstArg.as<sol::table>();

			const FScopedTransaction Txn(NSLOCTEXT("AIK", "ConfigureDatabase", "AI Configure PoseSearch Database"));
			Database->Modify();
			int32 Changes = 0;

			// Schema assignment
			sol::optional<std::string> SchemaOpt = Params.get<sol::optional<std::string>>("schema");
			if (SchemaOpt.has_value())
			{
				FString SchemaPath = UTF8_TO_TCHAR(SchemaOpt.value().c_str());
				if (!SchemaPath.StartsWith(TEXT("/"))) SchemaPath = TEXT("/Game/") + SchemaPath;
				UPoseSearchSchema* NewSchema = LoadObject<UPoseSearchSchema>(nullptr, *SchemaPath);
				if (!NewSchema)
				{
					Session.Log(FString::Printf(TEXT("[FAIL] configure(schema) -> not found: %s"), *SchemaPath));
					return sol::lua_nil;
				}
				// Schema is TObjectPtr<const UPoseSearchSchema> — use reflection to assign
				FProperty* SchemaProp = UPoseSearchDatabase::StaticClass()->FindPropertyByName(TEXT("Schema"));
				if (SchemaProp)
				{
					if (FObjectProperty* ObjProp = CastField<FObjectProperty>(SchemaProp))
						ObjProp->SetObjectPropertyValue_InContainer(Database, NewSchema);
				}
				Session.Log(FString::Printf(TEXT("[OK] configure(schema=\"%s\")"), *NewSchema->GetName()));
				Changes++;
			}

			// Search mode
			sol::optional<std::string> ModeOpt = Params.get<sol::optional<std::string>>("search_mode");
			if (ModeOpt.has_value())
			{
				Database->PoseSearchMode = ParseSearchMode(ModeOpt.value());
				Session.Log(FString::Printf(TEXT("[OK] configure(search_mode=\"%s\")"), UTF8_TO_TCHAR(ModeOpt.value().c_str())));
				Changes++;
			}

			// Cost biases
			sol::optional<double> CPCBOpt = Params.get<sol::optional<double>>("continuing_pose_cost_bias");
			if (CPCBOpt.has_value()) { Database->ContinuingPoseCostBias = static_cast<float>(CPCBOpt.value()); Changes++; }

			sol::optional<double> BCBOpt = Params.get<sol::optional<double>>("base_cost_bias");
			if (BCBOpt.has_value()) { Database->BaseCostBias = static_cast<float>(BCBOpt.value()); Changes++; }

			sol::optional<double> LCBOpt = Params.get<sol::optional<double>>("looping_cost_bias");
			if (LCBOpt.has_value()) { Database->LoopingCostBias = static_cast<float>(LCBOpt.value()); Changes++; }

			sol::optional<double> CICBOpt = Params.get<sol::optional<double>>("continuing_interaction_cost_bias");
			if (CICBOpt.has_value()) { Database->ContinuingInteractionCostBias = static_cast<float>(CICBOpt.value()); Changes++; }

			// KD-Tree query neighbors
			sol::optional<int> KDQNOpt = Params.get<sol::optional<int>>("kd_tree_query_num_neighbors");
			if (KDQNOpt.has_value()) { Database->KDTreeQueryNumNeighbors = FMath::Clamp(KDQNOpt.value(), 1, 600); Changes++; }

			// Editor-only performance settings
#if WITH_EDITORONLY_DATA
			sol::optional<int> NPCOpt = Params.get<sol::optional<int>>("num_principal_components");
			if (NPCOpt.has_value()) { Database->NumberOfPrincipalComponents = FMath::Clamp(NPCOpt.value(), 1, 64); Changes++; }

			sol::optional<int> KDMLSOpt = Params.get<sol::optional<int>>("kd_tree_max_leaf_size");
			if (KDMLSOpt.has_value()) { Database->KDTreeMaxLeafSize = FMath::Clamp(KDMLSOpt.value(), 1, 256); Changes++; }

			// Exclude from database parameters
			sol::optional<sol::table> ExclOpt = Params.get<sol::optional<sol::table>>("exclude_from_database_parameters");
			if (ExclOpt.has_value())
			{
				sol::table R = ExclOpt.value();
				float Min = static_cast<float>(R.get<sol::optional<double>>(1).value_or(0.0));
				float Max = static_cast<float>(R.get<sol::optional<double>>(2).value_or(-0.3));
				Database->ExcludeFromDatabaseParameters = FFloatInterval(Min, Max);
				Changes++;
			}
#endif

			if (Changes > 0)
			{
				Database->PostEditChange();
				Database->GetPackage()->MarkPackageDirty();
				RequestDatabaseRebuild(Database);
				Session.Log(FString::Printf(TEXT("[OK] configure() -> %d database-level changes"), Changes));
			}
			else
			{
				Session.Log(TEXT("[FAIL] configure() -> no valid parameters. Use: schema, search_mode, continuing_pose_cost_bias, base_cost_bias, looping_cost_bias, continuing_interaction_cost_bias, kd_tree_query_num_neighbors, num_principal_components, kd_tree_max_leaf_size, exclude_from_database_parameters"));
				return sol::lua_nil;
			}

			return sol::make_object(Lua, Changes);
		});

		// ---- list(type?) ----
		AssetObj.set_function("list", [Database, &Session](sol::table self,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = self["info"];
				if (InfoFn.valid()) return InfoFn(self);
				return sol::lua_nil;
			}

			if (FType.Equals(TEXT("animations"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("animation"), ESearchCase::IgnoreCase))
			{
				int32 Count = Database->GetNumAnimationAssets();
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Count; i++)
				{
					const FPoseSearchDatabaseAnimationAsset* Entry = Database->GetDatabaseAnimationAsset(i);
					if (!Entry) continue;

					sol::table E = Lua.create_table();
					E["index"] = i + 1; // 1-based Lua index
					UObject* AnimAsset = Entry->GetAnimationAsset();
					E["asset"] = AnimAsset ? TCHAR_TO_UTF8(*AnimAsset->GetName()) : "None";
					E["asset_path"] = AnimAsset ? TCHAR_TO_UTF8(*AnimAsset->GetPathName()) : "None";
					E["asset_class"] = AnimAsset ? TCHAR_TO_UTF8(*AnimAsset->GetClass()->GetName()) : "Unknown";
#if WITH_EDITORONLY_DATA
					E["enabled"] = Entry->bEnabled;
					E["mirror_option"] = MirrorOptionToString(Entry->MirrorOption);
					E["disable_reselection"] = Entry->bDisableReselection;
#endif

					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"animations\") -> %d"), Count));
				return Result;
			}

			if (FType.Equals(TEXT("tags"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("tag"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < Database->Tags.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = i + 1;
					E["name"] = TCHAR_TO_UTF8(*Database->Tags[i].ToString());
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"tags\") -> %d"), Database->Tags.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: animations, tags"), *FType));
			return sol::lua_nil;
		});

		// ---- rebuild_index() ----
		AssetObj.set_function("rebuild_index", [Database, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			RequestDatabaseRebuild(Database);
			Session.Log(TEXT("[OK] rebuild_index() -> async rebuild requested"));
			return sol::make_object(Lua, true);
		});

		// ---- info() — override default ----
		AssetObj.set_function("info", [Database, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			Result["num_animations"] = Database->GetNumAnimationAssets();
			Result["schema"] = Database->Schema ? TCHAR_TO_UTF8(*Database->Schema->GetName()) : "None";
			Result["schema_path"] = Database->Schema ? TCHAR_TO_UTF8(*Database->Schema->GetPathName()) : "None";

			const char* ModeStr = SearchModeToString(Database->PoseSearchMode);
			Result["search_mode"] = ModeStr;
			Result["continuing_pose_cost_bias"] = Database->ContinuingPoseCostBias;
			Result["base_cost_bias"] = Database->BaseCostBias;
			Result["looping_cost_bias"] = Database->LoopingCostBias;
			Result["continuing_interaction_cost_bias"] = Database->ContinuingInteractionCostBias;
			Result["kd_tree_query_num_neighbors"] = Database->KDTreeQueryNumNeighbors;
			Result["num_tags"] = static_cast<int>(Database->Tags.Num());
#if WITH_EDITORONLY_DATA
			Result["num_principal_components"] = Database->NumberOfPrincipalComponents;
			Result["kd_tree_max_leaf_size"] = Database->KDTreeMaxLeafSize;
			Result["exclude_from_database_min"] = Database->ExcludeFromDatabaseParameters.Min;
			Result["exclude_from_database_max"] = Database->ExcludeFromDatabaseParameters.Max;
#endif

			AddDatabaseEditorOnlyInfo(Database, Result);

			Session.Log(FString::Printf(TEXT("[OK] info() -> %d animations, schema=%s, mode=%s"),
				Database->GetNumAnimationAssets(),
				Database->Schema ? *Database->Schema->GetName() : TEXT("None"),
				UTF8_TO_TCHAR(ModeStr)));
			return Result;
		});
#else
		AssetObj.set_function("info", [&Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();
			Result["error"] = "PoseSearch database editing requires UE 5.7+";
			Session.Log(TEXT("[FAIL] PoseSearch database editing requires UE 5.7+"));
			return Result;
		});
#endif
	});

	// ========================================================================
	// NORMALIZATION SET enrichment
	// ========================================================================
	Lua.set_function("_enrich_pose_search_normalization_set", [&Session](sol::table AssetObj, sol::this_state S)
	{
		sol::state_view LuaView(S);
		std::string PathStr = AssetObj.get<std::string>("path");
		FString FPath = UTF8_TO_TCHAR(PathStr.c_str());
		UPoseSearchNormalizationSet* NormSet = LoadObject<UPoseSearchNormalizationSet>(nullptr, *FPath);
		if (!NormSet) return;

		// ---- help text ----
		AssetObj["_help_text"] =
			"Element types for add/remove/list:\n"
			"  database — UPoseSearchDatabase reference\n"
			"\n"
			"add(type, path):\n"
			"  add(\"database\", \"/Game/MotionMatching/DB_Locomotion\")\n"
			"\n"
			"remove(type, index):\n"
			"  remove(\"database\", 1) — 1-based index\n"
			"\n"
			"list(type):\n"
			"  list(\"databases\")\n"
			"\n"
			"info() — summary of normalization set\n";

		// ---- add("database", path) ----
		AssetObj.set_function("add", [NormSet, &Session](sol::table /*self*/,
			const std::string& Type, sol::optional<std::string> PathOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (!FType.Equals(TEXT("database"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"%s\") -> unknown type. Valid: database"), *FType));
				return sol::lua_nil;
			}

			if (!PathOpt.has_value() || PathOpt.value().empty())
			{
				Session.Log(TEXT("[FAIL] add(\"database\") -> path required"));
				return sol::lua_nil;
			}

			FString DBPath = UTF8_TO_TCHAR(PathOpt.value().c_str());
			if (!DBPath.StartsWith(TEXT("/"))) DBPath = TEXT("/Game/") + DBPath;

			UPoseSearchDatabase* DB = LoadObject<UPoseSearchDatabase>(nullptr, *DBPath);
			if (!DB)
			{
				Session.Log(FString::Printf(TEXT("[FAIL] add(\"database\") -> not found: %s"), *DBPath));
				return sol::lua_nil;
			}

			const FScopedTransaction Txn(NSLOCTEXT("AIK", "AddNormDB", "AI Add Normalization Set Database"));
			NormSet->Modify();
			NormSet->Databases.AddUnique(DB);
			NormSet->PostEditChange();
			NormSet->GetPackage()->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] add(\"database\", \"%s\")"), *DB->GetName()));
			return sol::make_object(Lua, true);
		});

		// ---- remove("database", index) ----
		AssetObj.set_function("remove", [NormSet, &Session](sol::table /*self*/,
			const std::string& Type, sol::object Id, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = UTF8_TO_TCHAR(Type.c_str());

			if (!FType.Equals(TEXT("database"), ESearchCase::IgnoreCase))
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"%s\") -> unknown type. Valid: database"), *FType));
				return sol::lua_nil;
			}

			if (!Id.is<int>())
			{
				Session.Log(TEXT("[FAIL] remove(\"database\") -> 1-based index required"));
				return sol::lua_nil;
			}

			int32 LuaIdx = Id.as<int>();
			int32 Idx = LuaIdx - 1;

			if (Idx < 0 || Idx >= NormSet->Databases.Num())
			{
				Session.Log(FString::Printf(TEXT("[FAIL] remove(\"database\", %d) -> index out of range (1..%d)"), LuaIdx, NormSet->Databases.Num()));
				return sol::lua_nil;
			}

			const FScopedTransaction Txn(NSLOCTEXT("AIK", "RemoveNormDB", "AI Remove Normalization Set Database"));
			NormSet->Modify();
			NormSet->Databases.RemoveAt(Idx);
			NormSet->PostEditChange();
			NormSet->GetPackage()->MarkPackageDirty();

			Session.Log(FString::Printf(TEXT("[OK] remove(\"database\", %d)"), LuaIdx));
			return sol::make_object(Lua, true);
		});

		// ---- list(type?) ----
		AssetObj.set_function("list", [NormSet, &Session](sol::table self,
			sol::optional<std::string> TypeOpt, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			FString FType = TypeOpt.has_value() ? UTF8_TO_TCHAR(TypeOpt.value().c_str()) : TEXT("all");

			if (FType.Equals(TEXT("all"), ESearchCase::IgnoreCase))
			{
				sol::protected_function InfoFn = self["info"];
				if (InfoFn.valid()) return InfoFn(self);
				return sol::lua_nil;
			}

			if (FType.Equals(TEXT("databases"), ESearchCase::IgnoreCase) || FType.Equals(TEXT("database"), ESearchCase::IgnoreCase))
			{
				sol::table Result = Lua.create_table();
				for (int32 i = 0; i < NormSet->Databases.Num(); i++)
				{
					sol::table E = Lua.create_table();
					E["index"] = i + 1;
					const UPoseSearchDatabase* DB = NormSet->Databases[i];
					E["name"] = DB ? TCHAR_TO_UTF8(*DB->GetName()) : "None";
					E["path"] = DB ? TCHAR_TO_UTF8(*DB->GetPathName()) : "None";
					Result[i + 1] = E;
				}
				Session.Log(FString::Printf(TEXT("[OK] list(\"databases\") -> %d"), NormSet->Databases.Num()));
				return Result;
			}

			Session.Log(FString::Printf(TEXT("[FAIL] list(\"%s\") -> unknown type. Valid: databases"), *FType));
			return sol::lua_nil;
		});

		// ---- info() — override default ----
		AssetObj.set_function("info", [NormSet, &Session](sol::table /*self*/, sol::this_state S) -> sol::object
		{
			sol::state_view Lua(S);
			sol::table Result = Lua.create_table();

			Result["num_databases"] = static_cast<int>(NormSet->Databases.Num());

			sol::table DBNames = Lua.create_table();
			for (int32 i = 0; i < NormSet->Databases.Num(); i++)
			{
				const UPoseSearchDatabase* DB = NormSet->Databases[i];
				DBNames[i + 1] = DB ? TCHAR_TO_UTF8(*DB->GetName()) : "None";
			}
			Result["database_names"] = DBNames;

			Session.Log(FString::Printf(TEXT("[OK] info() -> %d databases"), NormSet->Databases.Num()));
			return Result;
		});
	});
}

REGISTER_LUA_BINDING(PoseSearch, PoseSearchDocs, [](sol::state& Lua, FLuaSessionData& Session)
{
	BindPoseSearch(Lua, Session);
});
