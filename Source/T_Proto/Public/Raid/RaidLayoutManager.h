#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Raid/RaidChapterConfig.h" 
#include "Raid/LevelNodeRow.h"
#include "RaidLayoutManager.generated.h"

class ARaidRoomActor;
class URaidChapterConfig;
class UHierarchicalInstancedStaticMeshComponent;
class USplineComponent;

UCLASS(Blueprintable, BlueprintType, Placeable, ClassGroup = (Raid), meta = (DisplayName = "Raid Layout Manager"))
class T_PROTO_API ARaidLayoutManager : public AActor
{
    GENERATED_BODY()

public:
    ARaidLayoutManager();

protected:
    virtual void BeginPlay() override;

public:
    // =========================================================================
    // 🔥 초직관적 3-STEP 프로세스로 완벽 개편!
    // =========================================================================

    // --- STEP 1: 핵심 데이터 설정 ---
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Raid|Step 1. Config")
    TObjectPtr<URaidChapterConfig> ChapterConfig;

    // --- STEP 2: 배경 스펙 및 테마 셋업 ---
    UPROPERTY(EditAnywhere, Category = "Raid|Step 2. Environment Setup")
    float BackgroundRadius = 100000.0f; // 기본 1km 반경

    UFUNCTION(BlueprintCallable, CallInEditor, Category = "Raid|Step 2. Environment Setup")
    void AutoGenerateWhiteboxFromCSV();

    UPROPERTY(EditAnywhere, Category = "Raid|Step 2. Environment Setup")
    TArray<FMeshCluster> BackgroundClusters;

    // true면 ThemeRegistry/Foliage 데이터를 읽어 BackgroundClusters를 자동으로 구성.
    UPROPERTY(EditAnywhere, Category = "Raid|Step 2. Environment Setup")
    bool bAutoBuildBackgroundClustersFromThemes = true;

    UPROPERTY(EditAnywhere, Category = "Raid|Step 2. Environment Setup", meta = (ClampMin = "0.2", ClampMax = "4.0"))
    float BackgroundAutoDensityScale = 1.0f;

    // Tree meshes that rely on wind WPO can look synchronized when heavily instanced.
    // Enable this to spawn those tree clusters as individual actors for per-object wind phase.
    UPROPERTY(EditAnywhere, Category = "Raid|Step 2. Environment Setup")
    bool bSpawnWindAnimatedTreesAsActors = true;

    // Wind actor mode can be extremely expensive when thousands of trees are spawned.
    // Trees beyond this budget automatically fall back to instanced rendering.
    UPROPERTY(EditAnywhere, Category = "Raid|Step 2. Environment Setup", meta = (ClampMin = "0", ClampMax = "5000"))
    int32 WindTreeActorMaxCount = 320;

    // Only trees within this radius from manager origin are allowed to spawn as actors.
    // Outside this radius, trees fall back to instanced rendering for performance.
    UPROPERTY(EditAnywhere, Category = "Raid|Step 2. Environment Setup", meta = (ClampMin = "1000.0", ClampMax = "300000.0"))
    float WindTreeActorSpawnRadius = 28000.0f;

    // Forces unique wind phase data per spawned tree actor (custom primitive data + MID scalar parameters).
    UPROPERTY(EditAnywhere, Category = "Raid|Step 2. Environment Setup")
    bool bForceUniqueWindPhasePerTree = true;

    // 수역 근처 룸/배경 스폰 차단 반경(uu)
    UPROPERTY(EditAnywhere, Category = "Raid|Step 2. Environment Setup", meta = (ClampMin = "100.0"))
    float WaterAvoidanceRadius = 2200.0f;

    // true면 지면 탐색 시 Landscape/Terrain 히트를 우선 선택.
    UPROPERTY(EditAnywhere, Category = "Raid|Step 2. Environment Setup")
    bool bPreferLandscapeGroundHit = true;

    // true면 CSV 좌표 분포의 중심을 LayoutManager 좌표 원점으로 재정렬해 한쪽 치우침을 방지.
    UPROPERTY(EditAnywhere, Category = "Raid|Step 2. Environment Setup")
    bool bCenterRoomLayoutAroundManager = true;

    // 재정렬된 룸 레이아웃의 최대 반경(uu). 초과 시 전체를 자동 축소해 랜드스케이프 이탈 스폰을 줄임.
    UPROPERTY(EditAnywhere, Category = "Raid|Step 2. Environment Setup", meta = (ClampMin = "5000.0", ClampMax = "500000.0"))
    float RoomLayoutMaxRadius = 85000.0f;

    // --- STEP 3: 스폰 및 클리어 ---
    UFUNCTION(BlueprintCallable, CallInEditor, Category = "Raid|Step 3. Actions")
    void OneClickCsvImportBuild(); // 🔥 원클릭 빌드 부활!

    UFUNCTION(BlueprintCallable, CallInEditor, Category = "Raid|Step 3. Actions")
    void SpawnRaidLayout();

    UFUNCTION(BlueprintCallable, CallInEditor, Category = "Raid|Step 3. Actions")
    void ClearAllRooms();

    // =========================================================================

    UPROPERTY(VisibleInstanceOnly, Category = "Raid|State")
    TMap<int32, TObjectPtr<ARaidRoomActor>> SpawnedRooms;

    UPROPERTY(VisibleInstanceOnly, Category = "Raid|State")
    TArray<TObjectPtr<AActor>> SpawnedRoadActors;

public:
    void AutoSetupPrototypeRaid();
    void AutoFinalizeImportedData();
    void RunFullContentAuditAndRepair();
    bool ApplyOpenWorldSpecFromCsvPath(const FString& CsvPath);

private:
    void ConnectRoomDoors();
    void ScatterBackgroundScenery();
    void ClearBackgroundScenery();
    void GenerateRoadSplineNetwork(const FString& DominantEnv);
    void ApplyProceduralLandscapeDeformation(const FString& DominantEnv);
    void EnsureBackgroundClustersInitialized();

    UPROPERTY()
    TArray<UHierarchicalInstancedStaticMeshComponent*> BackgroundISMC_Pool;

    UPROPERTY()
    TArray<TObjectPtr<AActor>> SpawnedBackgroundActors;

    FString LastOpenWorldSpecDirectory;
};
