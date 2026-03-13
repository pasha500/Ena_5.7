#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h" 
#include "Math/RandomStream.h"
#include "RaidChapterConfig.generated.h"

class ARaidRoomActor;
class URaidEnemyPresetRegistry;
class URaidLootRegistry;
class URoomPrefabRegistry;

USTRUCT(BlueprintType)
struct T_PROTO_API FMeshVariation {
    GENERATED_BODY()

    // [최적화 적용] Hard Reference -> Soft Reference 교체 완료
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Variation|Asset")
    TSoftObjectPtr<UStaticMesh> Mesh;

    // [최적화 적용] TSubclassOf -> TSoftClassPtr 교체 완료
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Variation|Asset")
    TSoftClassPtr<AActor> BlueprintPrefab;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Variation|Spawn")
    float SpawnWeight = 1.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Variation|Transform")
    FTransform Offset = FTransform::Identity;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Variation|Random")
    bool bUseRandomScale = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Variation|Random", meta = (EditCondition = "bUseRandomScale"))
    float RandomScaleMin = 0.8f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Variation|Random", meta = (EditCondition = "bUseRandomScale"))
    float RandomScaleMax = 1.2f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Variation|Random")
    bool bUseRandomRotation = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Variation|Random", meta = (EditCondition = "bUseRandomRotation"))
    float RandomRotationYawMin = 0.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Variation|Random", meta = (EditCondition = "bUseRandomRotation"))
    float RandomRotationYawMax = 360.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Variation|Random")
    bool bUseRandomLocationJitter = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Variation|Random", meta = (EditCondition = "bUseRandomLocationJitter"))
    float JitterRadius = 50.0f;

    FTransform GetRandomizedTransform(const FTransform& BaseTransform, const FRandomStream& Stream) const
    {
        FTransform FinalTrans = Offset * BaseTransform;
        if (bUseRandomLocationJitter) {
            FVector Jitter(Stream.FRandRange(-JitterRadius, JitterRadius), Stream.FRandRange(-JitterRadius, JitterRadius), 0.0f);
            FinalTrans.AddToTranslation(Jitter);
        }
        if (bUseRandomRotation) {
            FRotator RandRot(0.0f, Stream.FRandRange(RandomRotationYawMin, RandomRotationYawMax), 0.0f);
            FinalTrans.ConcatenateRotation(RandRot.Quaternion());
        }
        if (bUseRandomScale) {
            float RandScale = Stream.FRandRange(RandomScaleMin, RandomScaleMax);
            FinalTrans.SetScale3D(FinalTrans.GetScale3D() * RandScale);
        }
        return FinalTrans;
    }
};

namespace RaidMeshUtils
{
    inline const FMeshVariation* PickRandomVariation(const TArray<FMeshVariation>& Variations, const FRandomStream& Stream)
    {
        if (Variations.Num() == 0) return nullptr;
        float TotalWeight = 0.0f;
        for (const auto& Var : Variations) TotalWeight += FMath::Max(0.0f, Var.SpawnWeight);
        if (TotalWeight <= 0.0f) return &Variations[Stream.RandRange(0, Variations.Num() - 1)];
        float RandVal = Stream.FRandRange(0.0f, TotalWeight);
        for (const auto& Var : Variations)
        {
            RandVal -= FMath::Max(0.0f, Var.SpawnWeight);
            if (RandVal <= 0.0f) return &Var;
        }
        return &Variations.Last();
    }
}

USTRUCT(BlueprintType)
struct T_PROTO_API FMeshCluster {
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster")
    FString ClusterName = TEXT("New Cluster Set");

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster|Spawning")
    bool bUseAreaSpawning = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster|Spawning", meta = (EditCondition = "bUseAreaSpawning"))
    float SpawnCountMin = 1.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster|Spawning", meta = (EditCondition = "bUseAreaSpawning"))
    float SpawnCountMax = 1.0f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster|Spawning", meta = (EditCondition = "bUseAreaSpawning"))
    float SpawnRadius = 1000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster|Spawning")
    float MinDistanceBetweenInstances = 200.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster|Overrides")
    bool bOverrideRandomization = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster|Overrides", meta = (EditCondition = "bOverrideRandomization"))
    bool bUseRandomScale = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster|Overrides", meta = (EditCondition = "bOverrideRandomization"))
    float RandomScaleMin = 0.8f;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster|Overrides", meta = (EditCondition = "bOverrideRandomization"))
    float RandomScaleMax = 1.5f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster|Overrides", meta = (EditCondition = "bOverrideRandomization"))
    bool bUseRandomRotation = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster|Overrides", meta = (EditCondition = "bOverrideRandomization"))
    bool bUseRandomLocationJitter = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster|Overrides", meta = (EditCondition = "bOverrideRandomization"))
    float JitterRadius = 300.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cluster|Items")
    TArray<FMeshVariation> Variations;

    int32 CalculateSpawnCount(float TotalAreaRadius, const FRandomStream& Stream) const
    {
        if (!bUseAreaSpawning) return 1;
        float SafeSpawnRadius = FMath::Max(100.0f, SpawnRadius);
        float AreaRatio = (TotalAreaRadius * TotalAreaRadius) / (SafeSpawnRadius * SafeSpawnRadius);
        float RandomDensity = Stream.FRandRange(SpawnCountMin, SpawnCountMax);
        float TotalSpawnsF = RandomDensity * AreaRatio;
        int32 Count = FMath::FloorToInt(TotalSpawnsF);
        if (Stream.FRand() < (TotalSpawnsF - Count)) Count++;
        return Count;
    }

    FTransform GetClusterRandomizedTransform(const FMeshVariation& Var, const FTransform& BaseTransform, const FRandomStream& Stream) const
    {
        FTransform FinalTrans = Var.Offset * BaseTransform;
        bool bJitter = bOverrideRandomization ? bUseRandomLocationJitter : Var.bUseRandomLocationJitter;
        float JRad = bOverrideRandomization ? JitterRadius : Var.JitterRadius;
        if (bJitter) {
            FVector Jitter(Stream.FRandRange(-JRad, JRad), Stream.FRandRange(-JRad, JRad), 0.0f);
            FinalTrans.AddToTranslation(Jitter);
        }
        bool bRot = bOverrideRandomization ? bUseRandomRotation : Var.bUseRandomRotation;
        if (bRot) {
            FRotator RandRot(0.0f, Stream.FRandRange(0.0f, 360.0f), 0.0f);
            FinalTrans.ConcatenateRotation(RandRot.Quaternion());
        }
        bool bScale = bOverrideRandomization ? bUseRandomScale : Var.bUseRandomScale;
        float SMin = bOverrideRandomization ? RandomScaleMin : Var.RandomScaleMin;
        float SMax = bOverrideRandomization ? RandomScaleMax : Var.RandomScaleMax;
        if (bScale) {
            float RandScale = Stream.FRandRange(SMin, SMax);
            FinalTrans.SetScale3D(FinalTrans.GetScale3D() * RandScale);
        }
        return FinalTrans;
    }
};

USTRUCT(BlueprintType)
struct T_PROTO_API FModularMeshKit {
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular|Base")
    TArray<FMeshVariation> FloorVariations;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular|Base")
    TArray<FMeshVariation> WallVariations;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular|Base")
    TArray<FMeshVariation> CornerVariations;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular|Base")
    TArray<FMeshVariation> DoorwayVariations;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular|Base")
    TArray<FMeshVariation> DoorBlockerVariations;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular|Props")
    TArray<FMeshVariation> ObstacleVariations;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular|Props")
    TArray<FMeshVariation> DecorationVariations;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular|Nature")
    bool bIsOrganicTheme = false;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Modular|Nature")
    TArray<FMeshCluster> FoliageClusters;
};

UCLASS(BlueprintType)
class T_PROTO_API URaidChapterConfig : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout Config")
    TObjectPtr<UDataTable> LevelDataTable;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout Config")
    TSubclassOf<ARaidRoomActor> RoomClass;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Layout Config|Themes")
    TMap<FString, FModularMeshKit> ThemeRegistry;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Registry")
    TObjectPtr<URoomPrefabRegistry> PrefabRegistry;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Registry")
    TObjectPtr<URaidEnemyPresetRegistry> EnemyPresetRegistry;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Registry")
    TObjectPtr<URaidLootRegistry> LootRegistry;
};
