#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Raid/LevelNodeRow.h"
#include "Raid/RaidRoomType.h"
#include "Math/RandomStream.h"
#include "Raid/RaidChapterConfig.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "RaidRoomActor.generated.h"

class UBoxComponent;
class UTextRenderComponent;
class URaidChapterConfig;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UStaticMesh;
class URaidRegionBannerWidget;
class APawn;

UCLASS()
class T_PROTO_API ARaidRoomActor : public AActor
{
    GENERATED_BODY()

public:
    ARaidRoomActor();

protected:
    virtual void BeginPlay() override;
    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void Tick(float DeltaTime) override;

public:
    void ClearAllMeshInstances();

    void SetNodeData(int32 InNodeId, const FLevelNodeRow& InNodeRow, const URaidChapterConfig* InConfig);
    void InternalSpawnLoot();
    void SetCombatStarted(bool bStarted) { bCombatStarted = bStarted; }
    void SetCombatCleared(bool bCleared);
    void OpenRoom();

    int32 GetNodeId() const { return NodeId; }
    const FLevelNodeRow& GetNodeRow() const { return NodeRow; }
    bool IsCleared() const { return bCombatCleared; }
    bool HasCombatStarted() const { return bCombatStarted; }
    const URaidChapterConfig* GetChapterConfig() const { return ChapterConfigRef; }
    void GenerateRoomLayout();
    FVector GetRoomExtent() const;

    UFUNCTION()
    void OnOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

    void TryShowRegionBanner(APawn* OverlappingPawn);

    // 컴뱃 서브시스템이 접근할 수 있도록 public으로 개방!
    UPROPERTY(EditAnywhere, Category = "Room|Config") float TileSize = 400.0f;
    UPROPERTY(EditAnywhere, Category = "Room|Config") int32 GridSize = 11;

    int32 NeighborNorth = -1, NeighborSouth = -1, NeighborEast = -1, NeighborWest = -1;
    bool bDoorNorth = false, bDoorSouth = false, bDoorEast = false, bDoorWest = false;

protected:
    int32 NodeId = 0;
    FLevelNodeRow NodeRow;
    ERaidRoomType CurrentRoomType = ERaidRoomType::Unknown;
    FRandomStream RoomRandomStream;

    bool bCombatStarted = false;
    bool bCombatCleared = false;

    UPROPERTY(EditAnywhere, Category = "Room|Geometry") bool bEnableRoomShellGeometry = true;
    UPROPERTY(EditAnywhere, Category = "Room|Geometry") bool bEnableRoomInteriorGeometry = true;
    UPROPERTY(EditAnywhere, Category = "Room|Geometry") bool bEnableRoomOrganicClusters = true;
    UPROPERTY(EditAnywhere, Category = "Room|Geometry") bool bEnableTraversalWhiteboxKit = true;
    UPROPERTY(EditAnywhere, Category = "Room|Geometry") bool bSpawnWindAnimatedRoomTreesAsActors = true;
    UPROPERTY(EditAnywhere, Category = "Room|Whitebox") bool bUseSemanticWhiteboxColors = true;

    UPROPERTY(EditAnywhere, Category = "Room|Whitebox|Colors") FLinearColor FloorTint = FLinearColor(0.1f, 0.1f, 0.15f);
    UPROPERTY(EditAnywhere, Category = "Room|Whitebox|Colors") FLinearColor WallTint = FLinearColor(0.2f, 0.2f, 0.2f);
    UPROPERTY(EditAnywhere, Category = "Room|Whitebox|Colors") FLinearColor ObstacleTint = FLinearColor(0.25f, 0.3f, 0.35f);
    UPROPERTY(EditAnywhere, Category = "Room|Whitebox|Colors") FLinearColor DecorationTint = FLinearColor(0.4f, 0.3f, 0.1f);
    UPROPERTY(EditAnywhere, Category = "Room|Whitebox|Colors") FLinearColor TraversalTint = FLinearColor(0.12f, 0.15f, 0.12f);
    UPROPERTY(EditAnywhere, Category = "Room|Whitebox|Meshes")
    TSoftObjectPtr<UStaticMesh> TraversalMeshOverride;
    UPROPERTY(EditAnywhere, Category = "Room|Whitebox|Meshes")
    bool bUseThemeMeshForTraversalKit = true;

    UPROPERTY(EditAnywhere, Category = "Room|Optimization") bool bAutoOptimizeInstancedMeshes = true;
    UPROPERTY(EditAnywhere, Category = "Room|Optimization") float DetailCullStartDistance = 8000.0f;
    UPROPERTY(EditAnywhere, Category = "Room|Optimization") float DetailCullEndDistance = 15000.0f;
    UPROPERTY(EditAnywhere, Category = "Room|Optimization") bool bAutoEnableNaniteInEditor = true;
    UPROPERTY(EditAnywhere, Category = "Room|Optimization") int32 NaniteTriangleThreshold = 1000;

    friend class ARaidLayoutManager;

    void MaybeEnableNaniteForMesh(UStaticMesh* Mesh);
    void ApplyISMCOptimization(UHierarchicalInstancedStaticMeshComponent* ISMC, int32 MeshType) const;
    FLinearColor ResolveSemanticTintForType(int32 MeshType) const;
    UMaterialInterface* GetSemanticMaterialForType(int32 MeshType);
    UMaterialInterface* GetTraversalMaterial();
    AActor* SpawnProceduralDoorBlocker(const FModularMeshKit& ThemeKit, const FVector& LocalLocation, float LocalYaw);
    void GenerateTraversalWhiteboxKit(float RoomRadius, const FModularMeshKit* ThemeKit);

    // 누락되었던 AddMeshInstance 함수 선언 추가!
    AActor* AddMeshInstance(const FMeshVariation& Variation, const FTransform& BaseTransform, int32 MeshType, UMaterialInterface* MaterialOverride = nullptr);

    UPROPERTY() TArray<UHierarchicalInstancedStaticMeshComponent*> DynamicISMC_Pool;
    UPROPERTY(Transient) TArray<TObjectPtr<AActor>> SpawnedDynamicActors;
    UPROPERTY(Transient) TArray<TObjectPtr<AActor>> SpawnedDoorActors;
    UPROPERTY(Transient) TMap<int32, TObjectPtr<UMaterialInterface>> SemanticMaterialCache;
    UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> TraversalMaterialCache;
    UPROPERTY(EditAnywhere, Category = "Room|UI") TSoftClassPtr<URaidRegionBannerWidget> RegionBannerWidgetClass;
    UPROPERTY(Transient) TObjectPtr<URaidRegionBannerWidget> ActiveRegionBannerWidget;

    bool bEntryBannerShown = false;
    bool bNodeDataInitialized = false;
    bool bLootAlreadySpawned = false;

    UPROPERTY(VisibleAnywhere) TObjectPtr<USceneComponent> SceneRoot;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UBoxComponent> Trigger;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UTextRenderComponent> StatusText;

    const URaidChapterConfig* ChapterConfigRef = nullptr;
};
