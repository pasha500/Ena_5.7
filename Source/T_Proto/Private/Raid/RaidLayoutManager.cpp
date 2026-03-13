#include "Raid/RaidLayoutManager.h"
#include "Raid/RaidRoomActor.h"
#include "Raid/RaidChapterConfig.h"
#include "Raid/LevelNodeRow.h"
#include "Raid/RoomPrefabRegistry.h"
#include "Raid/RaidCombatSubsystem.h"
#include "Raid/RaidEditorPipelineLibrary.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SplineComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/EngineTypes.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "GameFramework/PhysicsVolume.h"
#include "EngineUtils.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialTypes.h"
#include "Misc/Paths.h"

namespace
{
    bool IsTreeLikeName(const FString& InName)
    {
        const FString Lower = InName.ToLower();
        static const TCHAR* Keywords[] = {
            TEXT("tree"), TEXT("sapling"), TEXT("pine"), TEXT("oak"), TEXT("beech"),
            TEXT("birch"), TEXT("fir"), TEXT("spruce"), TEXT("palm"), TEXT("cypress"),
            TEXT("willow"), TEXT("trunk")
        };

        for (const TCHAR* Keyword : Keywords)
        {
            if (Lower.Contains(Keyword))
            {
                return true;
            }
        }
        return false;
    }

    bool IsTreeLikeVariation(const FMeshVariation& Variation)
    {
        if (Variation.Mesh.IsNull())
        {
            return false;
        }

        const FString PathString = Variation.Mesh.ToSoftObjectPath().ToString();
        return IsTreeLikeName(PathString);
    }

    bool ClusterContainsTreeLikeVariation(const FMeshCluster& Cluster)
    {
        for (const FMeshVariation& Variation : Cluster.Variations)
        {
            if (IsTreeLikeVariation(Variation))
            {
                return true;
            }
        }
        return false;
    }

    bool IsLikelyWindScalarParamName(const FString& LowerParamName)
    {
        return
            LowerParamName.Contains(TEXT("wind")) ||
            LowerParamName.Contains(TEXT("phase")) ||
            LowerParamName.Contains(TEXT("gust")) ||
            LowerParamName.Contains(TEXT("sway")) ||
            LowerParamName.Contains(TEXT("bend")) ||
            LowerParamName.Contains(TEXT("offset"));
    }

    void GatherLikelyWindScalarParams(UMaterialInterface* Material, TArray<FName>& OutParamNames)
    {
        if (!Material)
        {
            return;
        }

        TArray<FMaterialParameterInfo> ScalarInfos;
        TArray<FGuid> ScalarIds;
        Material->GetAllScalarParameterInfo(ScalarInfos, ScalarIds);
        for (const FMaterialParameterInfo& Info : ScalarInfos)
        {
            const FString LowerName = Info.Name.ToString().ToLower();
            if (IsLikelyWindScalarParamName(LowerName))
            {
                OutParamNames.AddUnique(Info.Name);
            }
        }
    }

    void ApplyWindPhaseDesync(UStaticMeshComponent* MeshComp, FRandomStream& Stream, bool bCreateMIDPerTree)
    {
        if (!IsValid(MeshComp))
        {
            return;
        }

        // Cheap path for materials using custom primitive data.
        MeshComp->SetCustomPrimitiveDataFloat(0, Stream.FRandRange(0.0f, 1.0f));
        MeshComp->SetCustomPrimitiveDataFloat(1, Stream.FRandRange(0.0f, 6.283185f));

        if (!bCreateMIDPerTree)
        {
            return;
        }

        // Fallback path for materials exposing scalar wind-phase parameters.
        static const FName FallbackParamNames[] = {
            TEXT("WindPhaseOffset"),
            TEXT("WindPhase"),
            TEXT("WindOffset"),
            TEXT("WindTimeOffset"),
            TEXT("WindVariation"),
            TEXT("PerInstanceRandom"),
            TEXT("TreeWindOffset"),
            TEXT("GustPhase")
        };

        const float RandomPhase = Stream.FRandRange(-1.0f, 1.0f);
        const int32 MaterialCount = MeshComp->GetNumMaterials();
        for (int32 MatIndex = 0; MatIndex < MaterialCount; ++MatIndex)
        {
            UMaterialInterface* BaseMat = MeshComp->GetMaterial(MatIndex);
            if (!BaseMat)
            {
                continue;
            }

            UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(BaseMat);
            if (!MID)
            {
                MID = UMaterialInstanceDynamic::Create(BaseMat, MeshComp);
                if (!MID)
                {
                    continue;
                }
                MeshComp->SetMaterial(MatIndex, MID);
            }

            TArray<FName> ParamNamesToSet;
            GatherLikelyWindScalarParams(BaseMat, ParamNamesToSet);
            if (ParamNamesToSet.Num() == 0)
            {
                for (const FName ParamName : FallbackParamNames)
                {
                    ParamNamesToSet.Add(ParamName);
                }
            }

            for (const FName ParamName : ParamNamesToSet)
            {
                MID->SetScalarParameterValue(ParamName, RandomPhase);
            }
        }
    }

    bool IsWaterActorOrComponent(const AActor* HitActor, const UPrimitiveComponent* HitComp)
    {
        if (HitActor)
        {
            if (const APhysicsVolume* PhysicsVolume = Cast<APhysicsVolume>(HitActor))
            {
                if (PhysicsVolume->bWaterVolume) return true;
            }
            if (HitActor->ActorHasTag(TEXT("Water"))) return true;

            const FString ActorClass = HitActor->GetClass()->GetName();
            if (ActorClass.Contains(TEXT("Water"), ESearchCase::IgnoreCase) ||
                ActorClass.Contains(TEXT("Lake"), ESearchCase::IgnoreCase) ||
                ActorClass.Contains(TEXT("River"), ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        if (HitComp)
        {
            if (HitComp->ComponentHasTag(TEXT("Water"))) return true;

            const FString CompClass = HitComp->GetClass()->GetName();
            if (CompClass.Contains(TEXT("Water"), ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    bool IsInsideWaterPhysicsVolume(UWorld* World, const FVector& Location, float SphereRadius)
    {
        if (!World) return false;

        for (FConstPhysicsVolumeIterator It = World->GetNonDefaultPhysicsVolumeIterator(); It; ++It)
        {
            const TWeakObjectPtr<APhysicsVolume>& WeakVolume = *It;
            const APhysicsVolume* PhysicsVolume = WeakVolume.Get();
            if (!PhysicsVolume || !PhysicsVolume->bWaterVolume) continue;
            if (PhysicsVolume->EncompassesPoint(Location, SphereRadius)) return true;
        }
        return false;
    }

    bool IsHitWaterLocation(const FHitResult& Hit)
    {
        return IsWaterActorOrComponent(Hit.GetActor(), Hit.GetComponent());
    }

    bool IsLandscapeLikeHit(const FHitResult& Hit)
    {
        const AActor* HitActor = Hit.GetActor();
        const UPrimitiveComponent* HitComp = Hit.GetComponent();
        if (!HitActor && !HitComp) return false;

        const FString ActorClass = HitActor ? HitActor->GetClass()->GetName() : FString();
        const FString CompClass = HitComp ? HitComp->GetClass()->GetName() : FString();
        return
            ActorClass.Contains(TEXT("Landscape"), ESearchCase::IgnoreCase) ||
            CompClass.Contains(TEXT("Landscape"), ESearchCase::IgnoreCase) ||
            ActorClass.Contains(TEXT("Terrain"), ESearchCase::IgnoreCase) ||
            CompClass.Contains(TEXT("Terrain"), ESearchCase::IgnoreCase);
    }

    bool TryResolveGroundHit(
        UWorld* World,
        const FVector& XYLocation,
        bool bPreferLandscape,
        bool bIgnoreRaidRooms,
        const FCollisionQueryParams& QueryParams,
        FHitResult& OutHit)
    {
        if (!World) return false;

        TArray<FHitResult> Hits;
        const FVector TraceStart(XYLocation.X, XYLocation.Y, 120000.0f);
        const FVector TraceEnd(XYLocation.X, XYLocation.Y, -120000.0f);
        if (!World->LineTraceMultiByChannel(Hits, TraceStart, TraceEnd, ECC_WorldStatic, QueryParams))
        {
            return false;
        }

        const FHitResult* BestLandscape = nullptr;
        const FHitResult* BestGeneral = nullptr;

        for (const FHitResult& Hit : Hits)
        {
            if (!Hit.bBlockingHit) continue;
            if (IsHitWaterLocation(Hit)) continue;
            if (bIgnoreRaidRooms && Hit.GetActor() && Hit.GetActor()->IsA(ARaidRoomActor::StaticClass())) continue;

            if (!BestGeneral || Hit.Distance < BestGeneral->Distance)
            {
                BestGeneral = &Hit;
            }
            if (IsLandscapeLikeHit(Hit))
            {
                if (!BestLandscape || Hit.Distance < BestLandscape->Distance)
                {
                    BestLandscape = &Hit;
                }
            }
        }

        const FHitResult* SelectedHit = (bPreferLandscape && BestLandscape) ? BestLandscape : BestGeneral;
        if (!SelectedHit)
        {
            return false;
        }

        OutHit = *SelectedHit;
        return true;
    }

    bool IsLocationNearWater(
        UWorld* World,
        const FVector& Location,
        float AvoidanceRadius,
        const FCollisionQueryParams& QueryParams)
    {
        if (!World || AvoidanceRadius <= 1.0f)
        {
            return false;
        }

        if (IsInsideWaterPhysicsVolume(World, Location, AvoidanceRadius))
        {
            return true;
        }

        FCollisionObjectQueryParams ObjQuery;
        ObjQuery.AddObjectTypesToQuery(ECC_WorldStatic);
        ObjQuery.AddObjectTypesToQuery(ECC_WorldDynamic);

        TArray<FOverlapResult> Overlaps;
        if (World->OverlapMultiByObjectType(
            Overlaps,
            Location,
            FQuat::Identity,
            ObjQuery,
            FCollisionShape::MakeSphere(AvoidanceRadius),
            QueryParams))
        {
            for (const FOverlapResult& Overlap : Overlaps)
            {
                const AActor* Actor = Overlap.GetActor();
                const UPrimitiveComponent* Comp = Overlap.Component.Get();
                if (IsWaterActorOrComponent(Actor, Comp))
                {
                    return true;
                }
            }
        }

        constexpr int32 RingSamples = 6;
        for (int32 Index = 0; Index <= RingSamples; ++Index)
        {
            const float Dist = (Index == 0) ? 0.0f : AvoidanceRadius;
            const float Angle = (Index == 0) ? 0.0f : (2.0f * PI * (float)(Index - 1) / (float)RingSamples);
            const FVector SamplePoint = Location + FVector(FMath::Cos(Angle) * Dist, FMath::Sin(Angle) * Dist, 0.0f);

            if (IsInsideWaterPhysicsVolume(World, SamplePoint, 30.0f))
            {
                return true;
            }

            FHitResult Hit;
            if (World->LineTraceSingleByChannel(
                Hit,
                SamplePoint + FVector(0.0f, 0.0f, 100000.0f),
                SamplePoint + FVector(0.0f, 0.0f, -100000.0f),
                ECC_WorldStatic,
                QueryParams))
            {
                if (IsHitWaterLocation(Hit))
                {
                    return true;
                }
            }
        }

        return false;
    }
}

ARaidLayoutManager::ARaidLayoutManager()
{
    PrimaryActorTick.bCanEverTick = false;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    if (USceneComponent* SceneRoot = Cast<USceneComponent>(RootComponent))
    {
        SceneRoot->SetMobility(EComponentMobility::Static);
    }
}
void ARaidLayoutManager::BeginPlay() { Super::BeginPlay(); SpawnRaidLayout(); }

// 🔥 [신규] CSV 기반 맞춤형 더미 자동 생성 함수
void ARaidLayoutManager::AutoGenerateWhiteboxFromCSV()
{
    if (!ChapterConfig) { UE_LOG(LogTemp, Error, TEXT("ChapterConfig가 비어있습니다!")); return; }
    ChapterConfig->Modify(); Modify();

    TSoftObjectPtr<UStaticMesh> Cube(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")));
    TSoftObjectPtr<UStaticMesh> Sphere(FSoftObjectPath(TEXT("/Engine/BasicShapes/Sphere.Sphere")));
    TSoftObjectPtr<UStaticMesh> Cylinder(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cylinder.Cylinder")));
    TSoftObjectPtr<UStaticMesh> Plane(FSoftObjectPath(TEXT("/Engine/BasicShapes/Plane.Plane")));

    auto MakeVar = [](TSoftObjectPtr<UStaticMesh> InMesh, FVector InScale, bool bRand = false) {
        FMeshVariation V; V.Mesh = InMesh; V.Offset.SetScale3D(InScale);
        V.bUseRandomScale = bRand; if (bRand) { V.RandomScaleMin = 0.7f; V.RandomScaleMax = 1.4f; }
        return V;
        };

    TSet<FString> UsedThemes;
    if (ChapterConfig->LevelDataTable) {
        TArray<FLevelNodeRow*> Rows; ChapterConfig->LevelDataTable->GetAllRows<FLevelNodeRow>(TEXT(""), Rows);
        for (FLevelNodeRow* Row : Rows) { if (Row && !Row->Theme.IsEmpty()) UsedThemes.Add(Row->Theme); }
    }
    if (UsedThemes.Num() == 0) { UsedThemes.Add(TEXT("Jungle")); UsedThemes.Add(TEXT("Urban")); }

    ChapterConfig->ThemeRegistry.Empty();
    for (const FString& ThemeName : UsedThemes) {
        FModularMeshKit Kit;
        Kit.bIsOrganicTheme = ThemeName.Contains(TEXT("Jungle")) || ThemeName.Contains(TEXT("Nature"));
        Kit.FloorVariations.Add(MakeVar(Plane, FVector(4.f, 4.f, 1.f)));
        Kit.ObstacleVariations.Add(MakeVar(Cube, FVector(1.f, 2.f, 1.5f), true));
        if (Kit.bIsOrganicTheme) {
            FMeshCluster TreeCls; TreeCls.ClusterName = TEXT("Foliage_Trees"); TreeCls.Variations.Add(MakeVar(Cylinder, FVector(0.5f, 0.5f, 2.f), true));
            FMeshCluster RockCls; RockCls.ClusterName = TEXT("Foliage_Rocks"); RockCls.Variations.Add(MakeVar(Sphere, FVector(1.2f, 1.2f, 1.2f), true));
            Kit.FoliageClusters.Add(TreeCls); Kit.FoliageClusters.Add(RockCls);
        }
        else {
            Kit.WallVariations.Add(MakeVar(Cube, FVector(4.f, 0.2f, 3.f)));
        }
        ChapterConfig->ThemeRegistry.Add(ThemeName, Kit);
    }

    BackgroundClusters.Empty();
    FMeshCluster BgTree; BgTree.ClusterName = TEXT("Background_Trees"); BgTree.SpawnRadius = BackgroundRadius; BgTree.MinDistanceBetweenInstances = 1500.0f; BgTree.Variations.Add(MakeVar(Cylinder, FVector(1.f, 1.f, 4.f), true));
    FMeshCluster BgRock; BgRock.ClusterName = TEXT("Background_Rocks"); BgRock.SpawnRadius = BackgroundRadius; BgRock.MinDistanceBetweenInstances = 3000.0f; BgRock.Variations.Add(MakeVar(Sphere, FVector(3.f, 3.f, 2.f), true));
    FMeshCluster BgBush; BgBush.ClusterName = TEXT("Background_Bushes_NoCol"); BgBush.SpawnRadius = BackgroundRadius; BgBush.MinDistanceBetweenInstances = 800.0f; BgBush.Variations.Add(MakeVar(Sphere, FVector(1.f, 1.f, 0.5f), true));
    FMeshCluster BgStruct; BgStruct.ClusterName = TEXT("Background_Structures"); BgStruct.SpawnRadius = BackgroundRadius; BgStruct.MinDistanceBetweenInstances = 4000.0f; BgStruct.Variations.Add(MakeVar(Cube, FVector(2.f, 2.f, 4.f), true));
    BackgroundClusters.Add(BgTree); BackgroundClusters.Add(BgRock); BackgroundClusters.Add(BgBush); BackgroundClusters.Add(BgStruct);

    UE_LOG(LogTemp, Warning, TEXT("[Raid UX] CSV 기반 테마 및 배경 슬롯 자동 생성 완료!"));
}

void ARaidLayoutManager::ClearAllRooms()
{
    if (UWorld* World = GetWorld()) {
        for (TActorIterator<ARaidRoomActor> It(World); It; ++It) { if (ARaidRoomActor* Room = *It) { Room->ClearAllMeshInstances(); Room->Destroy(); } }
        for (TActorIterator<AStaticMeshActor> It(World); It; ++It) { if (It->Tags.Contains(TEXT("RaidDoorBlocker"))) It->Destroy(); }
    }
    SpawnedRooms.Empty(); ClearBackgroundScenery();
    for (AActor* Road : SpawnedRoadActors) { if (IsValid(Road)) Road->Destroy(); }
    SpawnedRoadActors.Empty();
}

void ARaidLayoutManager::SpawnRaidLayout()
{
    if (!ChapterConfig || !ChapterConfig->LevelDataTable) return;
    ClearAllRooms();
    EnsureBackgroundClustersInitialized();
    URaidCombatSubsystem* CombatSub = GetWorld()->GetSubsystem<URaidCombatSubsystem>();
    if (CombatSub) CombatSub->ResetSubsystem();

    TArray<FLevelNodeRow*> Rows; ChapterConfig->LevelDataTable->GetAllRows<FLevelNodeRow>(TEXT(""), Rows);
    Rows.Sort([](const FLevelNodeRow& A, const FLevelNodeRow& B)
        {
            return A.NodeId < B.NodeId;
        });

    float MinX = TNumericLimits<float>::Max();
    float MaxX = -TNumericLimits<float>::Max();
    float MinY = TNumericLimits<float>::Max();
    float MaxY = -TNumericLimits<float>::Max();
    int32 ValidCount = 0;
    int32 OutdoorHints = 0;
    int32 IndoorHints = 0;

    for (FLevelNodeRow* Row : Rows)
    {
        if (!Row) continue;
        ValidCount++;
        MinX = FMath::Min(MinX, Row->PosX);
        MaxX = FMath::Max(MaxX, Row->PosX);
        MinY = FMath::Min(MinY, Row->PosY);
        MaxY = FMath::Max(MaxY, Row->PosY);

        const FString Meta = (Row->EnvType + TEXT(" ") + Row->Theme + TEXT(" ") + Row->NodeTags + TEXT(" ") + Row->RoomRole).ToLower();
        if (Meta.Contains(TEXT("openworld")) || Meta.Contains(TEXT("open world")) || Meta.Contains(TEXT("outdoor")) ||
            Meta.Contains(TEXT("오픈월드")) || Meta.Contains(TEXT("야외")) || Row->EnvType.Equals(TEXT("Jungle"), ESearchCase::IgnoreCase) ||
            Row->EnvType.Equals(TEXT("NatureVillage"), ESearchCase::IgnoreCase))
        {
            OutdoorHints++;
        }
        if (Meta.Contains(TEXT("tarkov")) || Meta.Contains(TEXT("cqb")) || Meta.Contains(TEXT("indoor")) ||
            Meta.Contains(TEXT("factory")) || Meta.Contains(TEXT("warehouse")) || Meta.Contains(TEXT("mall")) ||
            Meta.Contains(TEXT("실내")) || Meta.Contains(TEXT("타르코프")) || Row->EnvType.Equals(TEXT("Urban"), ESearchCase::IgnoreCase))
        {
            IndoorHints++;
        }
    }

    const float SpanX = (ValidCount > 0) ? (MaxX - MinX) : 0.0f;
    const float SpanY = (ValidCount > 0) ? (MaxY - MinY) : 0.0f;
    const bool bLooksCollapsed = ValidCount >= 3 && SpanX < 1000.0f && SpanY < 1000.0f;
    const bool bOpenWorldIntent = OutdoorHints >= IndoorHints;

    TMap<int32, FVector2D> AutoPosOverrides;
    if (bLooksCollapsed)
    {
        const float Spread = bOpenWorldIntent ? 32000.0f : 12000.0f;
        int32 Index = 0;
        for (FLevelNodeRow* Row : Rows)
        {
            if (!Row) continue;

            const float T = (float)Index / (float)FMath::Max(1, Rows.Num() - 1);
            const float Angle = (float)Index * 2.39996323f;
            const float Radius = bOpenWorldIntent
                ? FMath::Sqrt(FMath::Max(0.08f, T)) * Spread
                : FMath::Lerp(2200.0f, Spread * 0.88f, T);
            FVector2D Pos(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius);

            if (Row->RoomType.Equals(TEXT("Start"), ESearchCase::IgnoreCase))
            {
                Pos = FVector2D(-Spread * 0.48f, -Spread * 0.12f);
            }
            else if (Row->NodeId == 1 && !Row->RoomType.Equals(TEXT("Start"), ESearchCase::IgnoreCase))
            {
                // 1번 룸은 Start에서 가장 먼저 접근 가능한 초반 목표가 되도록 가깝게 배치.
                Pos = FVector2D(-Spread * 0.36f, -Spread * 0.08f);
            }
            else if (Row->RoomType.Equals(TEXT("Exit"), ESearchCase::IgnoreCase))
            {
                Pos = FVector2D(Spread * 0.48f, Spread * 0.12f);
            }
            else if (Row->RoomType.Equals(TEXT("Boss"), ESearchCase::IgnoreCase))
            {
                Pos = FVector2D(Spread * 0.34f, bOpenWorldIntent ? Spread * 0.24f : Spread * 0.06f);
            }

            AutoPosOverrides.Add(Row->NodeId, Pos);
            Index++;
        }

        UE_LOG(LogTemp, Warning, TEXT("[RaidLayout] Auto redistributed collapsed node positions (%s mode)."),
            bOpenWorldIntent ? TEXT("OpenWorld") : TEXT("CQB"));
    }

    auto ResolvePlannedXY = [&](const FLevelNodeRow* Row) -> FVector2D
        {
            if (!Row)
            {
                return FVector2D::ZeroVector;
            }
            if (const FVector2D* Overridden = AutoPosOverrides.Find(Row->NodeId))
            {
                return *Overridden;
            }
            return FVector2D(Row->PosX, Row->PosY);
        };

    FVector2D LayoutCenter2D = FVector2D::ZeroVector;
    float LayoutScale = 1.0f;
    if (ValidCount > 0)
    {
        float LocalMinX = TNumericLimits<float>::Max();
        float LocalMaxX = -TNumericLimits<float>::Max();
        float LocalMinY = TNumericLimits<float>::Max();
        float LocalMaxY = -TNumericLimits<float>::Max();

        for (const FLevelNodeRow* Row : Rows)
        {
            if (!Row) continue;
            const FVector2D Planned = ResolvePlannedXY(Row);
            LocalMinX = FMath::Min(LocalMinX, Planned.X);
            LocalMaxX = FMath::Max(LocalMaxX, Planned.X);
            LocalMinY = FMath::Min(LocalMinY, Planned.Y);
            LocalMaxY = FMath::Max(LocalMaxY, Planned.Y);
        }

        LayoutCenter2D = FVector2D((LocalMinX + LocalMaxX) * 0.5f, (LocalMinY + LocalMaxY) * 0.5f);

        float MaxDistFromCenter = 0.0f;
        for (const FLevelNodeRow* Row : Rows)
        {
            if (!Row) continue;
            const FVector2D Planned = ResolvePlannedXY(Row);
            MaxDistFromCenter = FMath::Max(MaxDistFromCenter, FVector2D::Distance(Planned, LayoutCenter2D));
        }

        const float AllowedRadius = FMath::Max(5000.0f, RoomLayoutMaxRadius);
        if (MaxDistFromCenter > AllowedRadius)
        {
            LayoutScale = AllowedRadius / MaxDistFromCenter;
            UE_LOG(LogTemp, Warning, TEXT("[RaidLayout] Room layout scaled down to %.3f (MaxDist=%.1f, Allowed=%.1f)"),
                LayoutScale, MaxDistFromCenter, AllowedRadius);
        }
    }

    auto ToWorldIdealLocation = [&](const FVector2D& PlannedXY) -> FVector
        {
            FVector2D LocalXY = PlannedXY;
            if (bCenterRoomLayoutAroundManager)
            {
                LocalXY -= LayoutCenter2D;
            }
            LocalXY *= LayoutScale;
            return GetActorLocation() + FVector(LocalXY.X, LocalXY.Y, 0.0f);
        };

    auto TryFindSafeGroundAround = [&](const FVector& Origin, float SearchRadius, const FCollisionQueryParams& QueryParams, FVector& OutLocation) -> bool
        {
            const float SafeRadius = FMath::Max(500.0f, SearchRadius);
            constexpr int32 Rings = 10;
            for (int32 Ring = 0; Ring <= Rings; ++Ring)
            {
                const float Alpha = (float)Ring / (float)Rings;
                const float Radius = Alpha * SafeRadius;
                const int32 SampleCount = FMath::Max(8, 10 + Ring * 6);
                for (int32 Sample = 0; Sample < SampleCount; ++Sample)
                {
                    const float Angle = ((2.0f * PI * (float)Sample) / (float)SampleCount) + (Ring * 0.37f);
                    const FVector TestLoc = Origin + FVector(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, 0.0f);
                    FHitResult HitResult;
                    if (!TryResolveGroundHit(GetWorld(), TestLoc, bPreferLandscapeGroundHit, true, QueryParams, HitResult))
                    {
                        continue;
                    }
                    if (IsLocationNearWater(GetWorld(), HitResult.ImpactPoint, WaterAvoidanceRadius, QueryParams))
                    {
                        continue;
                    }

                    OutLocation = HitResult.ImpactPoint;
                    return true;
                }
            }
            return false;
        };

    ARaidRoomActor* StartRoom = nullptr;

    for (FLevelNodeRow* Row : Rows)
    {
        if (!Row || SpawnedRooms.Contains(Row->NodeId)) continue;
        TSubclassOf<ARaidRoomActor> ClassToSpawn = ChapterConfig->RoomClass;
        if (ChapterConfig->PrefabRegistry && !Row->RoomPrefabId.IsEmpty()) {
            if (TSubclassOf<ARaidRoomActor> PrefabClass = ChapterConfig->PrefabRegistry->Resolve(Row->RoomPrefabId)) ClassToSpawn = PrefabClass;
        }
        if (!ClassToSpawn) continue;

        const FVector2D SpawnXY = ResolvePlannedXY(Row);
        const FVector IdealLoc = ToWorldIdealLocation(SpawnXY);
        FVector FinalSpawnLoc = IdealLoc;
        FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(RaidRoomSpawnGround), false);
        QueryParams.bTraceComplex = false;
        QueryParams.AddIgnoredActor(this);
        for (const TPair<int32, TObjectPtr<ARaidRoomActor>>& Existing : SpawnedRooms)
        {
            if (Existing.Value) QueryParams.AddIgnoredActor(Existing.Value);
        }

        bool bFoundSafeSpot = false;

        for (int32 Radius = 0; Radius <= 8000; Radius += 1000) {
            int32 AngleStep = (Radius == 0) ? 360 : 45;
            for (int32 Angle = 0; Angle < 360; Angle += AngleStep) {
                float Rad = FMath::DegreesToRadians((float)Angle);
                FVector TestLoc = IdealLoc + FVector(FMath::Cos(Rad) * Radius, FMath::Sin(Rad) * Radius, 0.0f);
                FHitResult HitResult;
                if (TryResolveGroundHit(GetWorld(), TestLoc, bPreferLandscapeGroundHit, true, QueryParams, HitResult))
                {
                    const FVector HitPoint = HitResult.ImpactPoint;
                    const bool bNearWater = IsLocationNearWater(GetWorld(), HitPoint, WaterAvoidanceRadius, QueryParams);
                    if (!bNearWater)
                    {
                        FinalSpawnLoc = HitPoint;
                        bFoundSafeSpot = true;
                        break;
                    }
                }
            }
            if (bFoundSafeSpot) break;
        }

        if (!bFoundSafeSpot) {
            if (!TryFindSafeGroundAround(IdealLoc, FMath::Max(12000.0f, RoomLayoutMaxRadius * 0.35f), QueryParams, FinalSpawnLoc))
            {
                if (!TryFindSafeGroundAround(GetActorLocation(), FMath::Max(RoomLayoutMaxRadius, BackgroundRadius * 0.75f), QueryParams, FinalSpawnLoc))
                {
                    UE_LOG(LogTemp, Warning, TEXT("[RaidLayout] Failed to place Room %d on safe ground. Skipping spawn."),
                        Row->NodeId);
                    continue;
                }
            }
        }

        FTransform SpawnTransform(FRotator::ZeroRotator, FinalSpawnLoc);
        if (ARaidRoomActor* NewRoom = GetWorld()->SpawnActorDeferred<ARaidRoomActor>(ClassToSpawn, SpawnTransform)) {
            NewRoom->SetNodeData(Row->NodeId, *Row, ChapterConfig);
            NewRoom->FinishSpawning(SpawnTransform);
            SpawnedRooms.Add(Row->NodeId, NewRoom);
            if (CombatSub) CombatSub->RegisterRoom(NewRoom);
            if (Row->RoomType.Equals(TEXT("Start"), ESearchCase::IgnoreCase))
            {
                StartRoom = NewRoom;
            }
#if WITH_EDITOR
            NewRoom->SetActorLabel(FString::Printf(TEXT("Room_%02d_[%s]"), Row->NodeId, *Row->RoomType));
#endif
        }
    }
    ConnectRoomDoors(); GenerateRoadSplineNetwork(TEXT("Default")); ScatterBackgroundScenery();
    // =========================================================
    // [핵심 추가] 맵 생성이 끝나면 콤파스 시스템을 강제로 1회 초기화!
    // =========================================================
    if (CombatSub)
    {
        if (!StartRoom)
        {
            for (const TPair<int32, TObjectPtr<ARaidRoomActor>>& Pair : SpawnedRooms)
            {
                ARaidRoomActor* Room = Pair.Value.Get();
                if (!Room) continue;
                if (Room->GetNodeRow().RoomType.Equals(TEXT("Start"), ESearchCase::IgnoreCase))
                {
                    StartRoom = Room;
                    break;
                }
            }
        }
        CombatSub->UpdateCompassForNextRooms(StartRoom);
    }
}

void ARaidLayoutManager::ConnectRoomDoors()
{
    for (auto& Pair : SpawnedRooms) { if (ARaidRoomActor* Room = Pair.Value) { Room->NeighborNorth = Room->NeighborSouth = Room->NeighborEast = Room->NeighborWest = -1; Room->bDoorNorth = Room->bDoorSouth = Room->bDoorEast = Room->bDoorWest = false; } }
    for (auto& Pair : SpawnedRooms) {
        ARaidRoomActor* Room = Pair.Value; if (!Room) continue;
        const FVector MyLoc = Room->GetActorLocation(); const TArray<int32> Connections = Room->GetNodeRow().GetConnectionIds();
        for (int32 ConnectedId : Connections) {
            TObjectPtr<ARaidRoomActor>* ConnectedRoomPtr = SpawnedRooms.Find(ConnectedId);
            if (!ConnectedRoomPtr || !ConnectedRoomPtr->Get()) continue;
            ARaidRoomActor* ConnectedRoom = ConnectedRoomPtr->Get();
            const FVector Dir = ConnectedRoom->GetActorLocation() - MyLoc;
            if (FMath::Abs(Dir.X) >= FMath::Abs(Dir.Y)) { if (Dir.X > 0) { Room->NeighborNorth = ConnectedId; Room->bDoorNorth = true; } else { Room->NeighborSouth = ConnectedId; Room->bDoorSouth = true; } }
            else { if (Dir.Y > 0) { Room->NeighborEast = ConnectedId; Room->bDoorEast = true; } else { Room->NeighborWest = ConnectedId; Room->bDoorWest = true; } }
        }
    }
    for (auto& Pair : SpawnedRooms) { if (ARaidRoomActor* Room = Pair.Value) Room->GenerateRoomLayout(); }
}

void ARaidLayoutManager::GenerateRoadSplineNetwork(const FString& DominantEnv)
{
    if (SpawnedRooms.Num() < 2) return;

    int32 OutdoorVotes = 0;
    int32 UrbanVotes = 0;
    int32 IndoorKeywordVotes = 0;
    float MinX = TNumericLimits<float>::Max();
    float MaxX = -TNumericLimits<float>::Max();
    float MinY = TNumericLimits<float>::Max();
    float MaxY = -TNumericLimits<float>::Max();
    for (const TPair<int32, TObjectPtr<ARaidRoomActor>>& Pair : SpawnedRooms)
    {
        ARaidRoomActor* Room = Pair.Value.Get();
        if (!Room) continue;
        const FLevelNodeRow& Row = Room->GetNodeRow();
        const FVector Loc = Room->GetActorLocation();
        MinX = FMath::Min(MinX, Loc.X);
        MaxX = FMath::Max(MaxX, Loc.X);
        MinY = FMath::Min(MinY, Loc.Y);
        MaxY = FMath::Max(MaxY, Loc.Y);
        const FString Meta = (Row.EnvType + TEXT(" ") + Row.Theme + TEXT(" ") + Row.NodeTags + TEXT(" ") + Row.RoomRole).ToLower();

        if (Meta.Contains(TEXT("openworld")) || Meta.Contains(TEXT("open world")) || Meta.Contains(TEXT("outdoor")) ||
            Meta.Contains(TEXT("오픈월드")) || Meta.Contains(TEXT("야외")) ||
            Row.EnvType.Equals(TEXT("Jungle"), ESearchCase::IgnoreCase) || Row.EnvType.Equals(TEXT("NatureVillage"), ESearchCase::IgnoreCase))
        {
            OutdoorVotes++;
        }
        if (Row.EnvType.Equals(TEXT("Urban"), ESearchCase::IgnoreCase))
        {
            UrbanVotes++;
        }
        if (Meta.Contains(TEXT("tarkov")) || Meta.Contains(TEXT("cqb")) || Meta.Contains(TEXT("indoor")) ||
            Meta.Contains(TEXT("factory")) || Meta.Contains(TEXT("warehouse")) || Meta.Contains(TEXT("mall")) ||
            Meta.Contains(TEXT("실내")) || Meta.Contains(TEXT("타르코프")))
        {
            IndoorKeywordVotes++;
        }
    }

    const float SpanX = MaxX - MinX;
    const float SpanY = MaxY - MinY;
    const bool bCompactLayout = SpanX < 35000.0f && SpanY < 35000.0f;
    const bool bIndoorFocused = (IndoorKeywordVotes > 0) || ((UrbanVotes > OutdoorVotes + 1) && bCompactLayout);
    if (bIndoorFocused)
    {
        UE_LOG(LogTemp, Warning, TEXT("[RaidLayout] Skipping outdoor road splines for indoor/CQB-focused layout."));
        return;
    }

    for (auto& Pair : SpawnedRooms) {
        ARaidRoomActor* RoomA = Pair.Value; if (!RoomA) continue;
        int32 IdA = RoomA->GetNodeId(); TArray<int32> ConnectedIds = RoomA->GetNodeRow().GetConnectionIds();
        for (int32 IdB : ConnectedIds) {
            if (IdA >= IdB || !SpawnedRooms.Contains(IdB)) continue;
            ARaidRoomActor* RoomB = SpawnedRooms[IdB]; if (!RoomB) continue;
            FVector StartPos = RoomA->GetActorLocation(); FVector EndPos = RoomB->GetActorLocation();
            float Distance = FVector::Dist2D(StartPos, EndPos);
            if (Distance > 20000.0f || Distance < 1000.0f) continue;

            FActorSpawnParameters SpawnParams; SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            AActor* RoadActor = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
            if (!RoadActor) continue;
#if WITH_EDITOR
            RoadActor->SetActorLabel(FString::Printf(TEXT("Road_Path_%02d_to_%02d"), IdA, IdB));
#endif
            USplineComponent* RoadSpline = NewObject<USplineComponent>(RoadActor);
            RoadActor->SetRootComponent(RoadSpline); RoadSpline->RegisterComponent(); RoadSpline->SetMobility(EComponentMobility::Static); RoadSpline->ClearSplinePoints();
            SpawnedRoadActors.Add(RoadActor);

            int32 NumSteps = FMath::Max(2, FMath::CeilToInt(Distance / 500.0f));
            for (int32 step = 0; step <= NumSteps; ++step) {
                float Alpha = (float)step / (float)NumSteps;
                FVector InterpPos = FMath::Lerp(StartPos, EndPos, Alpha);
                if (step > 0 && step < NumSteps) { InterpPos.X += FMath::RandRange(-300.0f, 300.0f); InterpPos.Y += FMath::RandRange(-300.0f, 300.0f); }

                FHitResult HitResult; FCollisionQueryParams QueryParams; QueryParams.bTraceComplex = true;
                for (auto& RoomPair : SpawnedRooms) { if (RoomPair.Value) QueryParams.AddIgnoredActor(RoomPair.Value); }

                if (GetWorld()->LineTraceSingleByChannel(HitResult, InterpPos + FVector(0, 0, 100000.0f), InterpPos + FVector(0, 0, -100000.0f), ECC_WorldStatic, QueryParams)) {
                    if (IsHitWaterLocation(HitResult)) { InterpPos.Z = FMath::Lerp(StartPos.Z, EndPos.Z, Alpha) + 150.0f; }
                    else if (HitResult.GetActor() && !HitResult.GetActor()->IsA(ARaidRoomActor::StaticClass())) { InterpPos.Z = HitResult.ImpactPoint.Z + 20.0f; }
                }
                RoadSpline->AddSplinePoint(InterpPos, ESplineCoordinateSpace::World, true);
            }
            for (int32 pt = 0; pt < RoadSpline->GetNumberOfSplinePoints(); ++pt) RoadSpline->SetSplinePointType(pt, ESplinePointType::Curve, true);
            RoadSpline->UpdateSpline();
        }
    }
}

void ARaidLayoutManager::ScatterBackgroundScenery()
{
    ClearBackgroundScenery();
    EnsureBackgroundClustersInitialized();
    if (BackgroundClusters.Num() == 0) return;

    auto IsWindTreeCluster = [this](const FMeshCluster& Cluster) -> bool
        {
            if (
                Cluster.ClusterName.Contains(TEXT("Tree"), ESearchCase::IgnoreCase) ||
                Cluster.ClusterName.Contains(TEXT("Sapling"), ESearchCase::IgnoreCase))
            {
                return true;
            }

            // Theme-generated cluster names may not include "Tree" in the label.
            return ClusterContainsTreeLikeVariation(Cluster);
        };

    for (const FMeshCluster& Cluster : BackgroundClusters) {
        bool bNoCollision = Cluster.ClusterName.Contains(TEXT("NoCol"));
        for (const FMeshVariation& Var : Cluster.Variations) {
            if (Var.Mesh.IsNull()) continue;
            UStaticMesh* LoadedMesh = Var.Mesh.LoadSynchronous(); if (!LoadedMesh) continue;
            bool bExists = false;
            for (UHierarchicalInstancedStaticMeshComponent* ISMC : BackgroundISMC_Pool) { if (IsValid(ISMC) && ISMC->GetStaticMesh() == LoadedMesh) { bExists = true; break; } }
            if (!bExists) {
                UHierarchicalInstancedStaticMeshComponent* ISMC = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
                ISMC->CreationMethod = EComponentCreationMethod::Instance; ISMC->SetStaticMesh(LoadedMesh); ISMC->SetupAttachment(RootComponent);
                ISMC->SetMobility(EComponentMobility::Static);
                ISMC->SetCollisionProfileName(bNoCollision ? TEXT("NoCollision") : TEXT("BlockAll"));
                ISMC->SetNumCustomDataFloats(2);
                if (LoadedMesh->GetPathName().StartsWith(TEXT("/Engine/"))) {
                    UMaterialInterface* BaseMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
                    if (BaseMat) {
                        UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMat, this);
                        FLinearColor Tint = FLinearColor(0.15f, 0.4f, 0.15f, 1.0f);
                        if (Cluster.ClusterName.Contains(TEXT("Rock"))) Tint = FLinearColor(0.4f, 0.25f, 0.15f, 1.0f);
                        else if (Cluster.ClusterName.Contains(TEXT("Bush"))) Tint = FLinearColor(0.3f, 0.6f, 0.2f, 1.0f);
                        else if (Cluster.ClusterName.Contains(TEXT("Structure"))) Tint = FLinearColor(0.4f, 0.4f, 0.45f, 1.0f);
                        MID->SetVectorParameterValue(TEXT("Color"), Tint); MID->SetVectorParameterValue(TEXT("BaseColor"), Tint); MID->SetVectorParameterValue(TEXT("Tint"), Tint);
                        ISMC->SetMaterial(0, MID);
                    }
                }
                ISMC->RegisterComponent(); BackgroundISMC_Pool.Add(ISMC);
            }
        }
    }

    int32 SpawnedTreeActorCount = 0;
    int32 SpawnedInstancedCount = 0;
    int32 WindActorFallbackByBudget = 0;
    int32 WindActorFallbackByDistance = 0;
    int32 WindActorBudgetLeft = bSpawnWindAnimatedTreesAsActors ? FMath::Max(0, WindTreeActorMaxCount) : 0;
    const float WindActorRadiusSq = FMath::Square(FMath::Max(1000.0f, WindTreeActorSpawnRadius));
    const FVector LayoutCenter = GetActorLocation();
    FRandomStream Stream(FMath::Rand());
    for (const FMeshCluster& Cluster : BackgroundClusters) {
        if (Cluster.Variations.Num() == 0) continue;
        const bool bWindTreeCluster = IsWindTreeCluster(Cluster);
        int32 TargetSpawnCount = Cluster.CalculateSpawnCount(BackgroundRadius, Stream);
        if (TargetSpawnCount <= 5) {
            float AreaSq = FMath::Square(BackgroundRadius * 2.0f);
            float DistSq = FMath::Max(40000.0f, FMath::Square(Cluster.MinDistanceBetweenInstances));
            TargetSpawnCount = FMath::Clamp(FMath::RoundToInt((AreaSq / DistSq) * 0.15f), 50, 5000);
        }
        TargetSpawnCount = FMath::Max(1, FMath::RoundToInt((float)TargetSpawnCount * FMath::Max(0.2f, BackgroundAutoDensityScale)));

        bool bNoCollision = Cluster.ClusterName.Contains(TEXT("NoCol"));
        float MinDistSq = FMath::Max(FMath::Square(Cluster.MinDistanceBetweenInstances), FMath::Square(bNoCollision ? 300.0f : 1200.0f));
        float ExclusionMargin = bNoCollision ? 600.0f : 1500.0f;
        int32 Spawned = 0; TArray<FVector> SpawnedLocations;
        int32 MaxAttempts = TargetSpawnCount * 20;

        for (int32 i = 0; i < MaxAttempts && Spawned < TargetSpawnCount; ++i) {
            FVector Point = GetActorLocation() + FVector(Stream.FRandRange(-BackgroundRadius, BackgroundRadius), Stream.FRandRange(-BackgroundRadius, BackgroundRadius), 0.0f);

            bool bInsideRoom = false;
            for (auto& Pair : SpawnedRooms) {
                if (ARaidRoomActor* Room = Pair.Value) {
                    if (FMath::Abs(Point.X - Room->GetActorLocation().X) < (Room->GetRoomExtent().X + ExclusionMargin) && FMath::Abs(Point.Y - Room->GetActorLocation().Y) < (Room->GetRoomExtent().Y + ExclusionMargin)) {
                        bInsideRoom = true; break;
                    }
                }
            }
            if (bInsideRoom) continue;

            bool bOverlaps = false;
            for (const FVector& ExistingLoc : SpawnedLocations) { if (FVector::DistSquaredXY(ExistingLoc, Point) < MinDistSq) { bOverlaps = true; break; } }
            if (bOverlaps) continue;

            FHitResult Hit;
            FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(RaidBackgroundScatterGround), false);
            TraceParams.bTraceComplex = false;
            TraceParams.AddIgnoredActor(this);
            for (const TPair<int32, TObjectPtr<ARaidRoomActor>>& RoomPair : SpawnedRooms)
            {
                if (RoomPair.Value) TraceParams.AddIgnoredActor(RoomPair.Value);
            }
            for (UHierarchicalInstancedStaticMeshComponent* ExistingISMC : BackgroundISMC_Pool)
            {
                if (IsValid(ExistingISMC)) TraceParams.AddIgnoredComponent(ExistingISMC);
            }
            for (AActor* ExistingBackgroundActor : SpawnedBackgroundActors)
            {
                if (IsValid(ExistingBackgroundActor)) TraceParams.AddIgnoredActor(ExistingBackgroundActor);
            }

            if (TryResolveGroundHit(GetWorld(), Point, bPreferLandscapeGroundHit, true, TraceParams, Hit)) {
                if (IsLocationNearWater(GetWorld(), Hit.ImpactPoint, WaterAvoidanceRadius, TraceParams)) continue;
                if (const FMeshVariation* RandomVar = RaidMeshUtils::PickRandomVariation(Cluster.Variations, Stream)) {
                    FRotator BaseRot = FRotator::ZeroRotator;
                    if (bNoCollision || Cluster.ClusterName.Contains(TEXT("Rock"))) { BaseRot = FRotationMatrix::MakeFromZ(Hit.ImpactNormal).Rotator(); BaseRot.Yaw = Stream.FRandRange(0.0f, 360.0f); }
                    else { BaseRot = FRotator(0.0f, Stream.FRandRange(0.0f, 360.0f), 0.0f); }

                    FTransform HitTransform(BaseRot, Hit.ImpactPoint);
                    FTransform FinalTrans = Cluster.GetClusterRandomizedTransform(*RandomVar, HitTransform, Stream);

                    if (!RandomVar->Mesh.IsNull()) {
                        if (UStaticMesh* LoadedMesh = RandomVar->Mesh.LoadSynchronous()) {
                            const bool bWithinWindRadius = FVector::DistSquaredXY(Hit.ImpactPoint, LayoutCenter) <= WindActorRadiusSq;
                            const bool bSpawnAsWindActor =
                                bSpawnWindAnimatedTreesAsActors &&
                                bWindTreeCluster &&
                                (WindActorBudgetLeft > 0) &&
                                bWithinWindRadius;

                            if (bSpawnAsWindActor)
                            {
                                FActorSpawnParameters SpawnParams;
                                SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
                                if (AStaticMeshActor* NewBackgroundActor = GetWorld()->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FinalTrans, SpawnParams))
                                {
                                    if (UStaticMeshComponent* MeshComp = NewBackgroundActor->GetStaticMeshComponent())
                                    {
                                        MeshComp->SetStaticMesh(LoadedMesh);
                                        MeshComp->SetCollisionProfileName(bNoCollision ? TEXT("NoCollision") : TEXT("BlockAll"));
                                        MeshComp->SetMobility(EComponentMobility::Static);
                                        ApplyWindPhaseDesync(MeshComp, Stream, bForceUniqueWindPhasePerTree);
                                    }
                                    NewBackgroundActor->Tags.AddUnique(FName(TEXT("RaidBackgroundScenery")));
                                    SpawnedBackgroundActors.Add(NewBackgroundActor);
                                    ++SpawnedTreeActorCount;
                                    --WindActorBudgetLeft;
                                }
                            }
                            else
                            {
                                if (bSpawnWindAnimatedTreesAsActors && bWindTreeCluster)
                                {
                                    if (WindActorBudgetLeft <= 0) ++WindActorFallbackByBudget;
                                    else if (!bWithinWindRadius) ++WindActorFallbackByDistance;
                                }

                                for (UHierarchicalInstancedStaticMeshComponent* ISMC : BackgroundISMC_Pool)
                                {
                                    if (IsValid(ISMC) && ISMC->GetStaticMesh() == LoadedMesh)
                                    {
                                        const int32 InstanceIndex = ISMC->AddInstance(FinalTrans, true);
                                        if (InstanceIndex != INDEX_NONE && bWindTreeCluster)
                                        {
                                            ISMC->SetCustomDataValue(InstanceIndex, 0, Stream.FRandRange(0.0f, 1.0f), false);
                                            ISMC->SetCustomDataValue(InstanceIndex, 1, Stream.FRandRange(0.0f, 6.283185f), true);
                                        }
                                        ++SpawnedInstancedCount;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    SpawnedLocations.Add(Hit.ImpactPoint); Spawned++;
                }
            }
        }
    }

    UE_LOG(
        LogTemp,
        Warning,
        TEXT("[RaidLayout] Background scatter complete. TreeActors=%d Instanced=%d (WindTreeActorMode=%s, UniqueWindPhase=%s, MaxActors=%d, ActorRadius=%.0f, FallbackBudget=%d, FallbackDistance=%d)"),
        SpawnedTreeActorCount,
        SpawnedInstancedCount,
        bSpawnWindAnimatedTreesAsActors ? TEXT("On") : TEXT("Off"),
        bForceUniqueWindPhasePerTree ? TEXT("On") : TEXT("Off"),
        WindTreeActorMaxCount,
        WindTreeActorSpawnRadius,
        WindActorFallbackByBudget,
        WindActorFallbackByDistance);
}

void ARaidLayoutManager::EnsureBackgroundClustersInitialized()
{
    auto MakeVar = [](TSoftObjectPtr<UStaticMesh> InMesh, FVector InScale, bool bRand = false) {
        FMeshVariation V;
        V.Mesh = InMesh;
        V.Offset.SetScale3D(InScale);
        V.bUseRandomScale = bRand;
        if (bRand)
        {
            V.RandomScaleMin = 0.7f;
            V.RandomScaleMax = 1.4f;
        }
        return V;
    };

    auto MakeFallbackClusters = [&]() {
        if (BackgroundClusters.Num() > 0) return;

        TSoftObjectPtr<UStaticMesh> Cube(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube")));
        TSoftObjectPtr<UStaticMesh> Sphere(FSoftObjectPath(TEXT("/Engine/BasicShapes/Sphere.Sphere")));
        TSoftObjectPtr<UStaticMesh> Cylinder(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cylinder.Cylinder")));

        FMeshCluster BgTree;
        BgTree.ClusterName = TEXT("Background_Trees");
        BgTree.SpawnRadius = BackgroundRadius;
        BgTree.MinDistanceBetweenInstances = 1500.0f;
        BgTree.Variations.Add(MakeVar(Cylinder, FVector(1.f, 1.f, 4.f), true));

        FMeshCluster BgRock;
        BgRock.ClusterName = TEXT("Background_Rocks");
        BgRock.SpawnRadius = BackgroundRadius;
        BgRock.MinDistanceBetweenInstances = 3000.0f;
        BgRock.Variations.Add(MakeVar(Sphere, FVector(3.f, 3.f, 2.f), true));

        FMeshCluster BgBush;
        BgBush.ClusterName = TEXT("Background_Bushes_NoCol");
        BgBush.SpawnRadius = BackgroundRadius;
        BgBush.MinDistanceBetweenInstances = 800.0f;
        BgBush.Variations.Add(MakeVar(Sphere, FVector(1.f, 1.f, 0.5f), true));

        FMeshCluster BgStruct;
        BgStruct.ClusterName = TEXT("Background_Structures");
        BgStruct.SpawnRadius = BackgroundRadius;
        BgStruct.MinDistanceBetweenInstances = 4000.0f;
        BgStruct.Variations.Add(MakeVar(Cube, FVector(2.f, 2.f, 4.f), true));

        BackgroundClusters.Add(BgTree);
        BackgroundClusters.Add(BgRock);
        BackgroundClusters.Add(BgBush);
        BackgroundClusters.Add(BgStruct);

        UE_LOG(LogTemp, Warning, TEXT("[RaidLayout] BackgroundClusters fallback auto-generated (4 default clusters)."));
    };

    auto NormalizeCluster = [&](FMeshCluster& Cluster) {
        Cluster.SpawnRadius = FMath::Max(Cluster.SpawnRadius, BackgroundRadius);
        Cluster.MinDistanceBetweenInstances = FMath::Max(100.0f, Cluster.MinDistanceBetweenInstances);
        Cluster.SpawnCountMin = FMath::Max(0.1f, Cluster.SpawnCountMin);
        Cluster.SpawnCountMax = FMath::Max(Cluster.SpawnCountMin, Cluster.SpawnCountMax);
    };

    if (!ChapterConfig)
    {
        MakeFallbackClusters();
        return;
    }

    if (!bAutoBuildBackgroundClustersFromThemes)
    {
        for (FMeshCluster& ExistingCluster : BackgroundClusters)
        {
            NormalizeCluster(ExistingCluster);
        }
        MakeFallbackClusters();
        return;
    }

    TArray<FMeshCluster> GeneratedClusters;
    GeneratedClusters.Reserve(16);

    for (const TPair<FString, FModularMeshKit>& ThemePair : ChapterConfig->ThemeRegistry)
    {
        const FString& ThemeName = ThemePair.Key;
        const FModularMeshKit& ThemeKit = ThemePair.Value;

        for (const FMeshCluster& SourceCluster : ThemeKit.FoliageClusters)
        {
            if (SourceCluster.Variations.Num() == 0) continue;

            FMeshCluster NewCluster = SourceCluster;
            if (NewCluster.ClusterName.IsEmpty())
            {
                NewCluster.ClusterName = FString::Printf(TEXT("%s_Foliage"), *ThemeName);
            }
            else if (!NewCluster.ClusterName.StartsWith(ThemeName))
            {
                NewCluster.ClusterName = FString::Printf(TEXT("%s_%s"), *ThemeName, *NewCluster.ClusterName);
            }
            NormalizeCluster(NewCluster);
            GeneratedClusters.Add(NewCluster);
        }

        if (ThemeKit.ObstacleVariations.Num() > 0)
        {
            FMeshCluster StructCluster;
            StructCluster.ClusterName = FString::Printf(TEXT("%s_AutoStructures"), *ThemeName);
            StructCluster.MinDistanceBetweenInstances = 2400.0f;
            StructCluster.SpawnRadius = BackgroundRadius;
            StructCluster.SpawnCountMin = 1.0f;
            StructCluster.SpawnCountMax = 2.0f;
            StructCluster.Variations = ThemeKit.ObstacleVariations;
            NormalizeCluster(StructCluster);
            GeneratedClusters.Add(StructCluster);
        }
    }

    if (BackgroundClusters.Num() == 0)
    {
        BackgroundClusters = MoveTemp(GeneratedClusters);
        for (FMeshCluster& ExistingCluster : BackgroundClusters)
        {
            NormalizeCluster(ExistingCluster);
        }
        MakeFallbackClusters();
        if (BackgroundClusters.Num() > 0)
        {
            UE_LOG(LogTemp, Warning, TEXT("[RaidLayout] BackgroundClusters auto-built from ThemeRegistry (%d clusters)."), BackgroundClusters.Num());
        }
        return;
    }

    for (FMeshCluster& ExistingCluster : BackgroundClusters)
    {
        NormalizeCluster(ExistingCluster);
    }

    int32 AddedClusters = 0;
    for (FMeshCluster& AutoCluster : GeneratedClusters)
    {
        bool bExists = false;
        for (const FMeshCluster& ExistingCluster : BackgroundClusters)
        {
            if (ExistingCluster.ClusterName.Equals(AutoCluster.ClusterName, ESearchCase::IgnoreCase))
            {
                bExists = true;
                break;
            }
        }
        if (!bExists)
        {
            NormalizeCluster(AutoCluster);
            BackgroundClusters.Add(AutoCluster);
            AddedClusters++;
        }
    }

    if (AddedClusters > 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("[RaidLayout] Added %d background clusters from ThemeRegistry for easier setup."), AddedClusters);
    }
}

void ARaidLayoutManager::ClearBackgroundScenery()
{
    for (AActor* BackgroundActor : SpawnedBackgroundActors)
    {
        if (IsValid(BackgroundActor)) BackgroundActor->Destroy();
    }
    SpawnedBackgroundActors.Empty();

    for (UHierarchicalInstancedStaticMeshComponent* ISMC : BackgroundISMC_Pool)
    {
        if (IsValid(ISMC)) ISMC->DestroyComponent();
    }
    BackgroundISMC_Pool.Empty();
}
void ARaidLayoutManager::ApplyProceduralLandscapeDeformation(const FString& DominantEnv) {}
void ARaidLayoutManager::AutoSetupPrototypeRaid() { AutoGenerateWhiteboxFromCSV(); SpawnRaidLayout(); }
void ARaidLayoutManager::AutoFinalizeImportedData() { SpawnRaidLayout(); }
void ARaidLayoutManager::OneClickCsvImportBuild() {
#if WITH_EDITOR
    FString CsvPath; if (URaidEditorPipelineLibrary::PickCsvFile(CsvPath)) { FString OutMsg; URaidEditorPipelineLibrary::OneClickImportAndBuild(CsvPath, TEXT("/Game/Raid/Data/DT_AI_Raid_Design"), ChapterConfig, this, true, true, false, OutMsg); }
#endif
}
void ARaidLayoutManager::RunFullContentAuditAndRepair() {
#if WITH_EDITOR
    FString OutReport, OutSummary; URaidEditorPipelineLibrary::AuditAllProjectContent(true, false, OutReport, OutSummary);
#endif
}
bool ARaidLayoutManager::ApplyOpenWorldSpecFromCsvPath(const FString& CsvPath) { LastOpenWorldSpecDirectory = FPaths::GetPath(CsvPath); return true; }
