#include "Raid/RaidRoomActor.h"
#include "Components/BoxComponent.h"
#include "Components/TextRenderComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Raid/RaidCombatSubsystem.h"
#include "Raid/RaidChapterConfig.h"
#include "Raid/RaidLootRegistry.h"
#include "Raid/RaidRegionBannerWidget.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PhysicsVolume.h"
#include "TimerManager.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialTypes.h"
#include "Camera/PlayerCameraManager.h"

namespace
{
    TWeakObjectPtr<URaidRegionBannerWidget> GSharedRegionBannerWidget;
    float GLastRegionBannerShowTime = -1000.0f;
    FTimerHandle GRegionBannerTimerHandle;

    bool IsOutdoorStyleRoom(const FLevelNodeRow& NodeRow)
    {
        const FString Meta = (NodeRow.EnvType + TEXT(" ") + NodeRow.Theme + TEXT(" ") + NodeRow.NodeTags + TEXT(" ") + NodeRow.RoomRole).ToLower();
        const bool bForceIndoor =
            Meta.Contains(TEXT("tarkov")) ||
            Meta.Contains(TEXT("cqb")) ||
            Meta.Contains(TEXT("indoor")) ||
            Meta.Contains(TEXT("factory")) ||
            Meta.Contains(TEXT("warehouse")) ||
            Meta.Contains(TEXT("mall")) ||
            Meta.Contains(TEXT("실내")) ||
            Meta.Contains(TEXT("타르코프"));
        const bool bForceOutdoor =
            Meta.Contains(TEXT("openworld")) ||
            Meta.Contains(TEXT("open world")) ||
            Meta.Contains(TEXT("outdoor")) ||
            Meta.Contains(TEXT("오픈월드")) ||
            Meta.Contains(TEXT("야외"));
        const bool bEnvOutdoor =
            NodeRow.EnvType.Equals(TEXT("Jungle"), ESearchCase::IgnoreCase) ||
            NodeRow.EnvType.Equals(TEXT("Nature"), ESearchCase::IgnoreCase) ||
            NodeRow.EnvType.Equals(TEXT("NatureVillage"), ESearchCase::IgnoreCase);
        return bForceOutdoor || (!bForceIndoor && bEnvOutdoor);
    }

    bool IsInsideWaterPhysicsVolume(UWorld* World, const FVector& Location, float SphereRadius = 20.0f)
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

    bool IsWaterHit(const FHitResult& Hit)
    {
        if (const AActor* HitActor = Hit.GetActor())
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

        if (const UPrimitiveComponent* HitComp = Hit.GetComponent())
        {
            if (HitComp->ComponentHasTag(TEXT("Water"))) return true;
            const FString CompClass = HitComp->GetClass()->GetName();
            if (CompClass.Contains(TEXT("Water"), ESearchCase::IgnoreCase)) return true;
        }
        return false;
    }

    bool IsTreeLikeMeshName(const FString& InName)
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

    bool IsLikelyWindPhaseParamName(const FString& LowerParamName)
    {
        const bool bHasPhaseLikeKeyword =
            LowerParamName.Contains(TEXT("phase")) ||
            LowerParamName.Contains(TEXT("timeoffset")) ||
            LowerParamName.Contains(TEXT("random")) ||
            LowerParamName.Contains(TEXT("offset")) ||
            LowerParamName.Contains(TEXT("variation"));
        if (!bHasPhaseLikeKeyword)
        {
            return false;
        }

        // Never randomize amplitude/speed/strength controls: that causes unstable, unnatural motion.
        if (LowerParamName.Contains(TEXT("strength")) ||
            LowerParamName.Contains(TEXT("intensity")) ||
            LowerParamName.Contains(TEXT("speed")) ||
            LowerParamName.Contains(TEXT("sway")) ||
            LowerParamName.Contains(TEXT("bend")) ||
            LowerParamName.Contains(TEXT("amplitude")) ||
            LowerParamName.Contains(TEXT("weight")) ||
            LowerParamName.Contains(TEXT("gust")))
        {
            return false;
        }

        return
            LowerParamName.Contains(TEXT("wind")) ||
            LowerParamName.Contains(TEXT("tree")) ||
            LowerParamName.Contains(TEXT("foliage")) ||
            LowerParamName.Contains(TEXT("phase")) ||
            LowerParamName.Contains(TEXT("random"));
    }

    void GatherLikelyWindPhaseScalarParams(UMaterialInterface* Material, TArray<FName>& OutParamNames)
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
            if (IsLikelyWindPhaseParamName(LowerName))
            {
                OutParamNames.AddUnique(Info.Name);
            }
        }
    }

    float ResolveWindDesyncValueForParam(const FString& LowerParamName, FRandomStream& Stream)
    {
        if (LowerParamName.Contains(TEXT("phase")))
        {
            return Stream.FRandRange(-PI, PI);
        }
        if (LowerParamName.Contains(TEXT("timeoffset")) || LowerParamName.Contains(TEXT("offset")))
        {
            return Stream.FRandRange(-1.0f, 1.0f);
        }
        if (LowerParamName.Contains(TEXT("random")) || LowerParamName.Contains(TEXT("variation")))
        {
            return Stream.FRandRange(0.0f, 1.0f);
        }
        return Stream.FRandRange(0.0f, 1.0f);
    }

    void ApplyRoomTreeWindPhaseDesync(UStaticMeshComponent* MeshComp, FRandomStream& Stream)
    {
        if (!IsValid(MeshComp))
        {
            return;
        }

        // Keep CPD values normalized (0..1). Some foliage materials remap these to
        // phase internally, and large raw values can cause unstable motion.
        MeshComp->SetCustomPrimitiveDataFloat(0, Stream.FRandRange(0.0f, 1.0f));
        MeshComp->SetCustomPrimitiveDataFloat(1, Stream.FRandRange(0.0f, 1.0f));

        static const FName FallbackParamNames[] = {
            TEXT("WindPhaseOffset"),
            TEXT("WindPhase"),
            TEXT("WindTimeOffset"),
            TEXT("PerInstanceRandom"),
            TEXT("TreeWindOffset"),
            TEXT("FoliageRandom")
        };

        const int32 MaterialCount = MeshComp->GetNumMaterials();
        for (int32 MatIndex = 0; MatIndex < MaterialCount; ++MatIndex)
        {
            UMaterialInterface* BaseMat = MeshComp->GetMaterial(MatIndex);
            if (!BaseMat)
            {
                continue;
            }

            TArray<FName> ParamNamesToSet;
            GatherLikelyWindPhaseScalarParams(BaseMat, ParamNamesToSet);
            if (ParamNamesToSet.Num() == 0)
            {
                for (const FName ParamName : FallbackParamNames)
                {
                    ParamNamesToSet.Add(ParamName);
                }
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

            for (const FName ParamName : ParamNamesToSet)
            {
                const FString LowerParamName = ParamName.ToString().ToLower();
                const float ParamValue = ResolveWindDesyncValueForParam(LowerParamName, Stream);
                MID->SetScalarParameterValue(ParamName, ParamValue);
            }
        }
    }
}

ARaidRoomActor::ARaidRoomActor()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;

    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
    SceneRoot->SetMobility(EComponentMobility::Static);
    RootComponent = SceneRoot;

    Trigger = CreateDefaultSubobject<UBoxComponent>(TEXT("Trigger"));
    Trigger->SetMobility(EComponentMobility::Movable);
    Trigger->SetupAttachment(RootComponent);
    Trigger->OnComponentBeginOverlap.AddDynamic(this, &ARaidRoomActor::OnOverlap);
    Trigger->ShapeColor = FColor::Green; Trigger->SetLineThickness(5.0f);
    Trigger->SetGenerateOverlapEvents(true);
    Trigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    Trigger->SetCanEverAffectNavigation(false);
    // Keep trigger out of typical WorldDynamic object traces used by weapon systems.
    Trigger->SetCollisionObjectType(ECC_Destructible);
    Trigger->SetCollisionResponseToAllChannels(ECR_Ignore);
    Trigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
    Trigger->SetCollisionResponseToChannel(ECC_Vehicle, ECR_Overlap);

    StatusText = CreateDefaultSubobject<UTextRenderComponent>(TEXT("StatusText"));
    StatusText->SetMobility(EComponentMobility::Movable);
    StatusText->SetupAttachment(RootComponent);

    // =========================================================================
    // 1. 글씨 크기 50% 축소: 기존 800.0f -> 400.0f
    StatusText->SetWorldSize(400.0f);

    // 2. 높이 20m & 중심점 맞춤: X=0, Y=0 (정중앙), Z=2000.0f (20미터 상공)
    StatusText->SetRelativeLocation(FVector(0.0f, 0.0f, 2000.0f));
    // =========================================================================

    StatusText->SetHorizontalAlignment(EHTA_Center);
    StatusText->SetVerticalAlignment(EVRTA_TextCenter);
    StatusText->SetTextRenderColor(FColor(244, 244, 170, 255));
    StatusText->bAlwaysRenderAsText = true;
    StatusText->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    StatusText->SetCollisionResponseToAllChannels(ECR_Ignore);
}

// 카메라 빌보드(Billboard) 로직 (가장 가벼운 연산)
void ARaidRoomActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (StatusText && GetWorld())
    {
        // 첫 번째 플레이어(또는 에디터 뷰포트)의 카메라를 찾음.
        APlayerCameraManager* CamManager = GetWorld()->GetFirstPlayerController() ? GetWorld()->GetFirstPlayerController()->PlayerCameraManager : nullptr;
        if (CamManager)
        {
            FVector CamLoc = CamManager->GetCameraLocation();
            FVector TextLoc = StatusText->GetComponentLocation();

            // 카메라를 쳐다보는 각도 계산 (현재 프로젝트 텍스트 전면 기준에 맞춤)
            FRotator LookAtRot = (CamLoc - TextLoc).Rotation();
            LookAtRot.Yaw += 360.0f;
            LookAtRot.Pitch = 0.0f; // 좌우로만 회전하게 고정 (위아래로 누우면 찌그러져 보임)

            StatusText->SetWorldRotation(LookAtRot);
        }
    }
}
void ARaidRoomActor::BeginPlay() { Super::BeginPlay(); }

void ARaidRoomActor::SetNodeData(int32 InNodeId, const FLevelNodeRow& InNodeRow, const URaidChapterConfig* InConfig)
{
    NodeId = InNodeId; NodeRow = InNodeRow; ChapterConfigRef = InConfig; CurrentRoomType = RaidRoomParsing::ParseRoomType(NodeRow.RoomType); RoomRandomStream.Initialize(NodeRow.Seed);
    const FString Size = NodeRow.RoomSize;
    if (Size.Equals(TEXT("Small"), ESearchCase::IgnoreCase)) GridSize = 9;
    else if (Size.Equals(TEXT("Medium"), ESearchCase::IgnoreCase)) GridSize = 13;
    else if (Size.Equals(TEXT("Large"), ESearchCase::IgnoreCase)) GridSize = 21;
    else if (Size.Equals(TEXT("Massive"), ESearchCase::IgnoreCase)) GridSize = 31;
    else GridSize = 13;
    bNodeDataInitialized = true; bLootAlreadySpawned = false;
}

void ARaidRoomActor::MaybeEnableNaniteForMesh(UStaticMesh* Mesh)
{
#if WITH_EDITOR
    if (!bAutoEnableNaniteInEditor || !Mesh) return;
    const FString MeshPath = Mesh->GetPathName();
    if (!MeshPath.StartsWith(TEXT("/Game/"))) return;
    if (Mesh->GetNumTriangles(0) < NaniteTriangleThreshold) return;
    if (Mesh->NaniteSettings.bEnabled) return;
    Mesh->Modify(); Mesh->NaniteSettings.bEnabled = true; Mesh->MarkPackageDirty();
#endif
}

void ARaidRoomActor::ApplyISMCOptimization(UHierarchicalInstancedStaticMeshComponent* ISMC, int32 MeshType) const
{
    if (!ISMC) return;
    ISMC->SetMobility(EComponentMobility::Static);
    if (MeshType == 3 || MeshType == 7) ISMC->SetCollisionProfileName(TEXT("NoCollision"));
    else ISMC->SetCollisionProfileName(TEXT("BlockAll"));

    if (bAutoOptimizeInstancedMeshes)
    {
        if (MeshType == 0 || MeshType == 1) ISMC->SetCullDistances(0, 0);
        else ISMC->SetCullDistances(FMath::RoundToInt(FMath::Max(0.0f, DetailCullStartDistance)), FMath::RoundToInt(FMath::Max(0.0f, DetailCullEndDistance)));
        ISMC->SetCastShadow(MeshType <= 2); ISMC->bCastDynamicShadow = (MeshType <= 1); ISMC->SetReceivesDecals(false); ISMC->SetCanEverAffectNavigation(MeshType <= 1); ISMC->bEnableDensityScaling = true;
    }
}

void ARaidRoomActor::ClearAllMeshInstances()
{
    for (AActor* Spawned : SpawnedDynamicActors) { if (IsValid(Spawned)) Spawned->Destroy(); }
    SpawnedDynamicActors.Empty(); SpawnedDoorActors.Empty();
    DynamicISMC_Pool.RemoveAll([](UHierarchicalInstancedStaticMeshComponent* Comp) { return !IsValid(Comp); });
    for (UHierarchicalInstancedStaticMeshComponent* ISMC : DynamicISMC_Pool) { if (IsValid(ISMC)) ISMC->ClearInstances(); }
    SemanticMaterialCache.Empty(); TraversalMaterialCache = nullptr;
}

// 메시 타입별로 명확한 화이트박스 색상(직관성 100%)을 지정!
FLinearColor ARaidRoomActor::ResolveSemanticTintForType(int32 MeshType) const
{
    FLinearColor Tint = FLinearColor::White;
    if (MeshType == 0) Tint = FLinearColor(0.2f, 0.2f, 0.2f, 1.0f); // 바닥 (어두운 회색)
    else if (MeshType == 1) Tint = FLinearColor(0.35f, 0.35f, 0.35f, 1.0f); // 벽 (조금 밝은 회색)
    else if (MeshType == 2) Tint = FLinearColor(0.5f, 0.5f, 0.5f, 1.0f); // 건물/매스 (회색)
    else if (MeshType == 3) Tint = FLinearColor(0.8f, 0.7f, 0.3f, 1.0f); // 소품 (노란빛)
    else if (MeshType == 6) Tint = FLinearColor(0.15f, 0.4f, 0.15f, 1.0f); // 나무 (녹색)
    else if (MeshType == 7) Tint = FLinearColor(0.3f, 0.6f, 0.2f, 1.0f); // 풀/덤불 (밝은 녹색)
    else if (MeshType == 8) Tint = FLinearColor(0.4f, 0.25f, 0.15f, 1.0f); // 돌/바위 (갈색)

    // 환경(테마)별 약간의 톤 변화 (색깔이 너무 왜곡되지 않게 살짝만 적용)
    const bool bUrban = NodeRow.EnvType.Equals(TEXT("Urban"), ESearchCase::IgnoreCase);
    const bool bVillage = NodeRow.EnvType.Equals(TEXT("NatureVillage"), ESearchCase::IgnoreCase);
    if (bUrban) Tint *= FLinearColor(0.95f, 0.97f, 1.05f, 1.0f);
    else if (bVillage) Tint *= FLinearColor(1.05f, 0.98f, 0.90f, 1.0f);

    Tint.R = FMath::Clamp(Tint.R, 0.05f, 0.95f); Tint.G = FMath::Clamp(Tint.G, 0.05f, 0.95f); Tint.B = FMath::Clamp(Tint.B, 0.05f, 0.95f); Tint.A = 1.0f;
    return Tint;
}

UMaterialInterface* ARaidRoomActor::GetSemanticMaterialForType(int32 MeshType)
{
    if (!bUseSemanticWhiteboxColors) return nullptr;
    if (TObjectPtr<UMaterialInterface>* Found = SemanticMaterialCache.Find(MeshType)) return Found->Get();
    UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    if (!BaseMaterial) return nullptr;
    UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
    if (!MID) return nullptr;
    const FLinearColor Tint = ResolveSemanticTintForType(MeshType);
    MID->SetVectorParameterValue(TEXT("Color"), Tint); MID->SetVectorParameterValue(TEXT("BaseColor"), Tint); MID->SetVectorParameterValue(TEXT("Tint"), Tint);
    SemanticMaterialCache.Add(MeshType, MID);
    return MID;
}

UMaterialInterface* ARaidRoomActor::GetTraversalMaterial()
{
    if (!bUseSemanticWhiteboxColors) return nullptr;
    if (TraversalMaterialCache) return TraversalMaterialCache;
    UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    if (!BaseMaterial) return nullptr;
    TraversalMaterialCache = UMaterialInstanceDynamic::Create(BaseMaterial, this);
    if (!TraversalMaterialCache) return nullptr;
    TraversalMaterialCache->SetVectorParameterValue(TEXT("Color"), TraversalTint); TraversalMaterialCache->SetVectorParameterValue(TEXT("BaseColor"), TraversalTint); TraversalMaterialCache->SetVectorParameterValue(TEXT("Tint"), TraversalTint);
    return TraversalMaterialCache;
}

AActor* ARaidRoomActor::SpawnProceduralDoorBlocker(const FModularMeshKit& ThemeKit, const FVector& LocalLocation, float LocalYaw)
{
    UWorld* World = GetWorld();
    if (!World) return nullptr;

    const FVector DoorScale(1.8f, 0.45f, 2.6f);
    const float DoorHalfHeight = 50.0f * DoorScale.Z;
    const float DoorYawOffset = 90.0f;

    FVector WorldDoorLocation = GetActorTransform().TransformPosition(LocalLocation);

    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(RaidDoorBlockerGroundSnap), false);
    QueryParams.bTraceComplex = false;
    QueryParams.AddIgnoredActor(this);

    FHitResult GroundHit;
    const bool bHitGround = World->LineTraceSingleByChannel(
        GroundHit,
        WorldDoorLocation + FVector(0.0f, 0.0f, 120000.0f),
        WorldDoorLocation + FVector(0.0f, 0.0f, -120000.0f),
        ECC_WorldStatic,
        QueryParams);

    if (bHitGround)
    {
        if (IsWaterHit(GroundHit) || IsInsideWaterPhysicsVolume(World, GroundHit.ImpactPoint, 80.0f))
        {
            return nullptr;
        }
        WorldDoorLocation.Z = GroundHit.ImpactPoint.Z + DoorHalfHeight;
    }
    else
    {
        WorldDoorLocation.Z = GetActorLocation().Z + DoorHalfHeight;
    }

    const FTransform WorldDoorTransform(
        FRotator(0.0f, LocalYaw + DoorYawOffset, 0.0f),
        WorldDoorLocation,
        DoorScale);

    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));

    AStaticMeshActor* DoorActor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), WorldDoorTransform, Params);
    if (!DoorActor) return nullptr;

    if (UStaticMeshComponent* MeshComp = DoorActor->GetStaticMeshComponent())
    {
        MeshComp->SetStaticMesh(CubeMesh);
        MeshComp->SetCollisionProfileName(TEXT("BlockAll"));
    }

    DoorActor->Tags.AddUnique(FName(TEXT("RaidDoorBlocker")));
    DoorActor->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepWorldTransform);
    return DoorActor;
}

AActor* ARaidRoomActor::AddMeshInstance(const FMeshVariation& Variation, const FTransform& BaseTransform, int32 MeshType, UMaterialInterface* MaterialOverride)
{
    if (Variation.Mesh.IsNull() && Variation.BlueprintPrefab.IsNull()) return nullptr;
    FTransform FinalTransform = Variation.GetRandomizedTransform(BaseTransform, RoomRandomStream);
    FTransform WorldTransform = FinalTransform * GetActorTransform();

    const bool bOutdoorStyle = IsOutdoorStyleRoom(NodeRow);
    const bool bTerrainConformType = (MeshType == 0 || MeshType == 2 || MeshType == 3 || MeshType == 6 || MeshType == 7 || MeshType == 8);
    const bool bShouldSnapToLandscape = bOutdoorStyle && bTerrainConformType;
    const bool bShouldAlignToSlope = bOutdoorStyle && (MeshType == 6 || MeshType == 7 || MeshType == 8);

    if (UWorld* World = GetWorld())
    {
        if (bOutdoorStyle && IsInsideWaterPhysicsVolume(World, WorldTransform.GetLocation(), 120.0f))
        {
            return nullptr;
        }

        if (bShouldSnapToLandscape)
        {
            FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(RaidRoomObjectGroundSnap), false);
            QueryParams.bTraceComplex = false;
            QueryParams.AddIgnoredActor(this);

            const FVector QueryLocation = WorldTransform.GetLocation();
            FHitResult GroundHit;
            if (World->LineTraceSingleByChannel(
                GroundHit,
                QueryLocation + FVector(0.0f, 0.0f, 120000.0f),
                QueryLocation + FVector(0.0f, 0.0f, -120000.0f),
                ECC_WorldStatic,
                QueryParams))
            {
                if (IsWaterHit(GroundHit) || IsInsideWaterPhysicsVolume(World, GroundHit.ImpactPoint, 120.0f))
                {
                    return nullptr;
                }

                const float HeightOffsetFromRoomOrigin = WorldTransform.GetLocation().Z - GetActorLocation().Z;
                WorldTransform.SetLocation(GroundHit.ImpactPoint + FVector(0.0f, 0.0f, HeightOffsetFromRoomOrigin));

                if (bShouldAlignToSlope)
                {
                    const float PreservedYaw = WorldTransform.GetRotation().Rotator().Yaw;
                    FRotator SlopeRot = FRotationMatrix::MakeFromZ(GroundHit.ImpactNormal).Rotator();
                    SlopeRot.Yaw = PreservedYaw;
                    WorldTransform.SetRotation(SlopeRot.Quaternion());
                }
            }
        }
    }

    if (!Variation.BlueprintPrefab.IsNull())
    {
        if (UClass* LoadedClass = Variation.BlueprintPrefab.LoadSynchronous())
        {
            FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            if (AActor* SpawnedActor = GetWorld()->SpawnActor<AActor>(LoadedClass, WorldTransform, Params))
            {
                TInlineComponentArray<USceneComponent*> SceneComps; SpawnedActor->GetComponents(SceneComps);
                for (USceneComponent* Comp : SceneComps) { if (Comp) Comp->SetMobility(EComponentMobility::Movable); }
                SpawnedActor->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepWorldTransform);
                SpawnedDynamicActors.Add(SpawnedActor); return SpawnedActor;
            }
        }
        return nullptr;
    }

    UStaticMesh* LoadedMesh = Variation.Mesh.LoadSynchronous();
    if (!LoadedMesh) return nullptr;
    MaybeEnableNaniteForMesh(LoadedMesh);

    UMaterialInterface* EffectiveMaterial = MaterialOverride;

    if (!EffectiveMaterial && bUseSemanticWhiteboxColors)
    {
        if (LoadedMesh->GetPathName().StartsWith(TEXT("/Engine/")))
        {
            EffectiveMaterial = GetSemanticMaterialForType(MeshType);
        }
    }

    const bool bTreeLikeMeshAsset = IsTreeLikeMeshName(LoadedMesh->GetPathName());
    const bool bSpawnAsTreeActor =
        (MeshType == 6) &&
        bSpawnWindAnimatedRoomTreesAsActors &&
        bTreeLikeMeshAsset;

    if (bSpawnAsTreeActor)
    {
        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        if (AStaticMeshActor* TreeActor = GetWorld()->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), WorldTransform, SpawnParams))
        {
            if (UStaticMeshComponent* MeshComp = TreeActor->GetStaticMeshComponent())
            {
                MeshComp->SetStaticMesh(LoadedMesh);
                MeshComp->SetCollisionProfileName(TEXT("BlockAll"));
                MeshComp->SetMobility(EComponentMobility::Static);
                if (EffectiveMaterial)
                {
                    MeshComp->SetMaterial(0, EffectiveMaterial);
                }
                ApplyRoomTreeWindPhaseDesync(MeshComp, RoomRandomStream);
            }

            SpawnedDynamicActors.Add(TreeActor);
            return TreeActor;
        }
    }

    UHierarchicalInstancedStaticMeshComponent* TargetISMC = nullptr;
    for (UHierarchicalInstancedStaticMeshComponent* ISMC : DynamicISMC_Pool)
    {
        if (!IsValid(ISMC) || ISMC->GetStaticMesh() != LoadedMesh) continue;
        const bool bTypeMatch = ISMC->ComponentTags.Contains(FName(*FString::Printf(TEXT("MeshType_%d"), MeshType)));
        UMaterialInterface* CurrentMat = ISMC->GetMaterial(0);
        const bool bHasOverrideTag = ISMC->ComponentTags.Contains(FName(TEXT("MatOverride")));
        const bool bMaterialMatch = EffectiveMaterial ? (CurrentMat == EffectiveMaterial) : !bHasOverrideTag;
        if (bTypeMatch && bMaterialMatch) { TargetISMC = ISMC; break; }
    }

    if (!TargetISMC)
    {
        TargetISMC = NewObject<UHierarchicalInstancedStaticMeshComponent>(this);
        TargetISMC->CreationMethod = EComponentCreationMethod::Instance; TargetISMC->SetMobility(EComponentMobility::Movable); TargetISMC->SetStaticMesh(LoadedMesh); TargetISMC->SetupAttachment(RootComponent);
        TargetISMC->ComponentTags.AddUnique(FName(*FString::Printf(TEXT("MeshType_%d"), MeshType)));
        ApplyISMCOptimization(TargetISMC, MeshType);
        if (EffectiveMaterial) { TargetISMC->SetMaterial(0, EffectiveMaterial); TargetISMC->ComponentTags.AddUnique(FName(TEXT("MatOverride"))); }
        TargetISMC->RegisterComponent(); AddInstanceComponent(TargetISMC); DynamicISMC_Pool.Add(TargetISMC);
    }
    TargetISMC->AddInstance(WorldTransform, true);
    return nullptr;
}

void ARaidRoomActor::GenerateTraversalWhiteboxKit(float RoomRadius, const FModularMeshKit* ThemeKit)
{
    if (!bEnableTraversalWhiteboxKit) return;
    FRandomStream Rng(NodeRow.Seed ^ (NodeId * 9973));
    UMaterialInterface* TraversalMat = GetTraversalMaterial();
    const bool bHasTraversalMeshOverride = !TraversalMeshOverride.IsNull();
    TMap<FSoftObjectPath, float> MeshBaseLiftCache;

    auto PickThemeVariationForMeshType = [&](int32 MeshType) -> const FMeshVariation*
        {
            if (!bUseThemeMeshForTraversalKit || !ThemeKit) return nullptr;

            const TArray<FMeshVariation>* CandidatePool = nullptr;
            if (MeshType == 1)
            {
                CandidatePool = &ThemeKit->WallVariations;
            }
            else if (MeshType == 0)
            {
                CandidatePool = &ThemeKit->FloorVariations;
            }
            else
            {
                CandidatePool = &ThemeKit->ObstacleVariations;
            }

            if (CandidatePool && CandidatePool->Num() > 0)
            {
                return RaidMeshUtils::PickRandomVariation(*CandidatePool, Rng);
            }
            return nullptr;
        };

    auto ResolveMeshBaseLift = [&](const FMeshVariation& VariationToMeasure) -> float
        {
            if (VariationToMeasure.Mesh.IsNull()) return 50.0f;

            const FSoftObjectPath MeshPath = VariationToMeasure.Mesh.ToSoftObjectPath();
            if (const float* Found = MeshBaseLiftCache.Find(MeshPath))
            {
                return *Found;
            }

            float Lift = 50.0f;
            if (UStaticMesh* Mesh = VariationToMeasure.Mesh.LoadSynchronous())
            {
                const FBoxSphereBounds Bounds = Mesh->GetBounds();
                Lift = -(Bounds.Origin.Z - Bounds.BoxExtent.Z);
            }
            MeshBaseLiftCache.Add(MeshPath, Lift);
            return Lift;
        };

    // 기본 도형을 스폰하는 람다 헬퍼 함수
    auto SpawnBox = [&](const FVector& Loc, const FVector& Scale, float Yaw, int32 MeshType) {
        FMeshVariation V;
        bool bUsesThemeVariation = false;
        if (const FMeshVariation* ThemeVar = PickThemeVariationForMeshType(MeshType))
        {
            V = *ThemeVar;
            bUsesThemeVariation = true;
        }
        else if (bHasTraversalMeshOverride)
        {
            V.Mesh = TraversalMeshOverride;
            V.Offset = FTransform::Identity;
        }
        else
        {
            // Do not spawn a hardcoded fallback cube.
            return;
        }

        FVector BaseScale = V.Offset.GetScale3D();
        if (BaseScale.IsNearlyZero()) BaseScale = FVector(1.0f, 1.0f, 1.0f);
        V.Offset.SetScale3D(BaseScale * Scale);

        FVector SpawnLoc = Loc;
        const float CubeBaselineLift = 50.0f * Scale.Z;
        const float MeshBaseLift = ResolveMeshBaseLift(V);
        const float ActualLift = MeshBaseLift * V.Offset.GetScale3D().Z;
        SpawnLoc.Z += (ActualLift - CubeBaselineLift);

        UMaterialInterface* MaterialOverrideToUse = nullptr;
        if (!bUsesThemeVariation && !V.Mesh.IsNull())
        {
            const FString MeshPath = V.Mesh.ToSoftObjectPath().ToString();
            if (MeshPath.StartsWith(TEXT("/Engine/")))
            {
                MaterialOverrideToUse = TraversalMat;
            }
        }

        AddMeshInstance(V, FTransform(FRotator(0.0f, Yaw, 0.0f), SpawnLoc), MeshType, MaterialOverrideToUse);
        };

    // EnvType + Theme + NodeTags를 함께 읽어 오픈월드/실내전투 스타일을 판정한다.
    const FString Env = NodeRow.EnvType;
    const FString Meta = (NodeRow.EnvType + TEXT(" ") + NodeRow.Theme + TEXT(" ") + NodeRow.NodeTags + TEXT(" ") + NodeRow.RoomRole).ToLower();
    const bool bForceIndoor =
        Meta.Contains(TEXT("tarkov")) ||
        Meta.Contains(TEXT("cqb")) ||
        Meta.Contains(TEXT("indoor")) ||
        Meta.Contains(TEXT("factory")) ||
        Meta.Contains(TEXT("warehouse")) ||
        Meta.Contains(TEXT("mall")) ||
        Meta.Contains(TEXT("실내")) ||
        Meta.Contains(TEXT("타르코프"));
    const bool bForceOutdoor =
        Meta.Contains(TEXT("openworld")) ||
        Meta.Contains(TEXT("open world")) ||
        Meta.Contains(TEXT("outdoor")) ||
        Meta.Contains(TEXT("오픈월드")) ||
        Meta.Contains(TEXT("야외"));
    const bool bEnvOutdoor =
        Env.Equals(TEXT("Jungle"), ESearchCase::IgnoreCase) ||
        Env.Equals(TEXT("Nature"), ESearchCase::IgnoreCase) ||
        Env.Equals(TEXT("NatureVillage"), ESearchCase::IgnoreCase);
    const bool bIsOpenWorld = bForceOutdoor || (!bForceIndoor && bEnvOutdoor);
    const float Half = RoomRadius - 200.0f; // 외곽 여백

    // =========================================================================
    // 🌳 [TRACK 1: 오픈월드/자연] 외벽 없음, 규칙 없음, 무작위 산포(Scatter)
    // =========================================================================
    if (bIsOpenWorld)
    {
        int32 ScatterCount = (GridSize >= 21) ? 12 : ((GridSize >= 13) ? 7 : 4);
        ScatterCount += FMath::RoundToInt(FMath::Clamp(NodeRow.ObstacleDensity, 0.0f, 1.0f) * 6.0f);
        ScatterCount += FMath::Clamp(NodeRow.TraversalLaneSeeds, 1, 6) - 1;
        for (int32 i = 0; i < ScatterCount; ++i)
        {
            FVector RandomLoc(Rng.FRandRange(-Half + 300, Half - 300), Rng.FRandRange(-Half + 300, Half - 300), 0.0f);
            if (RandomLoc.Size2D() < 800.0f) continue; // 중앙은 교전을 위해 비워둠

            float ScaleX = Rng.FRandRange(1.5f, 3.5f);
            float ScaleY = Rng.FRandRange(1.5f, 3.5f);
            float Height = Rng.FRandRange(1.5f, 4.0f);
            float RandomYaw = Rng.FRandRange(0.0f, 360.0f); // 360도 무작위 각도
            SpawnBox(RandomLoc + FVector(0, 0, Height * 50.f), FVector(ScaleX, ScaleY, Height), RandomYaw, 2);
        }
    }
    // =========================================================================
    // 🏢 [TRACK 2: 현대 전술 CQB] 밀봉된 외벽 + 스마트 문 뚫기 + 전술 엄폐물
    // =========================================================================
    else
    {
        float WallThickness = 0.5f; // 벽 두께 50cm
        float WallHeight = 4.0f;    // 층고 4m
        float DoorWidth = FMath::Lerp(2.4f, 3.8f, FMath::Clamp(NodeRow.EnterableBuildingRatio, 0.0f, 1.0f)); // 데이터 기반 문 폭

        // 스마트 벽 깎기 알고리즘 (Smart Edge Carving)
        auto BuildSmartWall = [&](bool bHasDoor, FVector CenterOffset, float Yaw) {
            if (!bHasDoor) {
                // 꽉 막힌 솔리드 벽 스폰
                SpawnBox(CenterOffset + FVector(0, 0, WallHeight * 50.0f), FVector((Half * 2.0f) / 100.0f, WallThickness, WallHeight), Yaw, 1);
            }
            else {
                // 문이 있는 경우, 양옆으로 벽을 쪼개서 스폰하고 중앙을 비움!
                float SideWidth = ((Half * 2.0f) - (DoorWidth * 100.0f)) / 2.0f;
                float LeftOffset = DoorWidth * 50.0f + SideWidth / 2.0f;

                FTransform BaseWallTrans(FRotator(0, Yaw, 0), CenterOffset);
                FVector LeftLoc = BaseWallTrans.TransformPosition(FVector(-LeftOffset, 0, WallHeight * 50.0f));
                FVector RightLoc = BaseWallTrans.TransformPosition(FVector(LeftOffset, 0, WallHeight * 50.0f));

                SpawnBox(LeftLoc, FVector(SideWidth / 100.0f, WallThickness, WallHeight), Yaw, 1);
                SpawnBox(RightLoc, FVector(SideWidth / 100.0f, WallThickness, WallHeight), Yaw, 1);

                // 문 위쪽 헤더(인방) 막아주기 (문 높이는 3m, 층고는 4m이므로 위쪽 1m를 덮음)
                FVector HeaderLoc = BaseWallTrans.TransformPosition(FVector(0, 0, WallHeight * 100.0f - 50.0f));
                SpawnBox(HeaderLoc, FVector(DoorWidth, WallThickness, 1.0f), Yaw, 1);
            }
            };

        // 매니저가 지정해준 연결 방향에만 물리적으로 문을 뚫음
        BuildSmartWall(bDoorNorth, FVector(Half, 0, 0), 0.0f);
        BuildSmartWall(bDoorSouth, FVector(-Half, 0, 0), 0.0f);
        BuildSmartWall(bDoorEast, FVector(0, Half, 0), 90.0f);
        BuildSmartWall(bDoorWest, FVector(0, -Half, 0), 90.0f);

        // 내부 전술 엄폐물 세팅 (십자 도로 대신 L자 벽, 기둥 산개 배치)
        int32 CoverCount = (GridSize >= 15) ? 7 : 3;
        CoverCount += FMath::Clamp(NodeRow.TraversalLaneSeeds, 1, 6);
        CoverCount += FMath::RoundToInt(FMath::Clamp(NodeRow.ObstacleDensity, 0.0f, 1.0f) * 4.0f);
        CoverCount = FMath::Clamp(CoverCount, 4, 20);
        for (int i = 0; i < CoverCount; ++i) {
            FVector CoverLoc(Rng.FRandRange(-Half + 400, Half - 400), Rng.FRandRange(-Half + 400, Half - 400), WallHeight * 50.0f);

            // 문 앞(중앙 크로스 라인)은 사격 통제선(Fatal Funnel)이므로 비워둠
            if (FMath::Abs(CoverLoc.X) < 400.0f || FMath::Abs(CoverLoc.Y) < 400.0f) continue;

            // 기둥 생성
            SpawnBox(CoverLoc, FVector(1.0f, 1.0f, WallHeight), 0.0f, 2);

            // 50% 확률로 기둥 옆에 벽을 덧대어 L자형 사각지대(코너) 생성
            if (Rng.FRand() < 0.5f) {
                if (Rng.FRand() < 0.5f) SpawnBox(CoverLoc + FVector(150.f, 0, 0), FVector(2.0f, 0.5f, WallHeight), 0.f, 2);
                else SpawnBox(CoverLoc + FVector(0, 150.f, 0), FVector(0.5f, 2.0f, WallHeight), 0.f, 2);
            }
        }

        // 무작위 박스/책상/엄폐물 배치
        const int32 PropCount = FMath::Clamp(4 + NodeRow.TraversalLaneSeeds + FMath::RoundToInt(NodeRow.ObstacleDensity * 4.0f), 4, 14);
        for (int i = 0; i < PropCount; ++i) {
            FVector BoxLoc(Rng.FRandRange(-Half + 300, Half - 300), Rng.FRandRange(-Half + 300, Half - 300), 50.0f);
            if (FMath::Abs(BoxLoc.X) < 300.0f || FMath::Abs(BoxLoc.Y) < 300.0f) continue;
            SpawnBox(BoxLoc, FVector(1.5f, 1.0f, 1.0f), Rng.FRandRange(0.f, 360.f), 2);
        }
    }
}

void ARaidRoomActor::GenerateRoomLayout()
{
    if (!bNodeDataInitialized) return;
    ClearAllMeshInstances();
    const FModularMeshKit* ThemeKit = nullptr; FString ResolvedThemeKey;
    if (ChapterConfigRef) {
        auto TryFindThemeByKey = [&](const FString& RawKey) -> const FModularMeshKit* {
            const FString Key = RawKey.TrimStartAndEnd(); if (Key.IsEmpty()) return nullptr;
            if (const FModularMeshKit* Exact = ChapterConfigRef->ThemeRegistry.Find(Key)) { ResolvedThemeKey = Key; return Exact; }
            for (const TPair<FString, FModularMeshKit>& Pair : ChapterConfigRef->ThemeRegistry) { if (Pair.Key.Equals(Key, ESearchCase::IgnoreCase)) { ResolvedThemeKey = Pair.Key; return &Pair.Value; } } return nullptr;
            };
        ThemeKit = TryFindThemeByKey(NodeRow.EnvType); if (!ThemeKit) ThemeKit = TryFindThemeByKey(NodeRow.Theme);
        if (!ThemeKit && ChapterConfigRef->ThemeRegistry.Num() > 0) { auto It = ChapterConfigRef->ThemeRegistry.CreateConstIterator(); if (It) { ResolvedThemeKey = It.Key(); ThemeKit = &It.Value(); } }
    }
    FString TypeStr = UEnum::GetDisplayValueAsText(CurrentRoomType).ToString();
    const FString ThemeLabel = ResolvedThemeKey.IsEmpty() ? NodeRow.EnvType : ResolvedThemeKey;
    StatusText->SetText(FText::FromString(FString::Printf(TEXT("< Room %d : %s >\n[%s] Zone %d"), NodeId, *TypeStr, *ThemeLabel, NodeRow.ZoneId)));
    float RoomRadius = (GridSize * TileSize) / 2.0f;
    Trigger->SetBoxExtent(FVector(RoomRadius, RoomRadius, 5000.0f));

    if (!ThemeKit)
    {
        static FModularMeshKit FallbackKit; static bool bFallbackInit = false;
        if (!bFallbackInit) { bFallbackInit = true; FallbackKit.bIsOrganicTheme = false; FMeshVariation FloorV; FloorV.Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube"))); FloorV.Offset.SetScale3D(FVector(4.4f, 4.4f, 0.12f)); FallbackKit.FloorVariations.Add(FloorV); FMeshVariation WallV; WallV.Mesh = TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Engine/BasicShapes/Cube.Cube"))); WallV.Offset.SetScale3D(FVector(4.4f, 0.20f, 2.6f)); FallbackKit.WallVariations.Add(WallV); }
        ThemeKit = &FallbackKit;
    }

    float Offset = RoomRadius - (TileSize / 2.0f); int32 Center = GridSize / 2; TArray<uint8> ActiveMask; ActiveMask.Init(1, GridSize * GridSize);
    auto ToIndex = [this](int32 X, int32 Y) { return X * GridSize + Y; }; auto IsInside = [this](int32 X, int32 Y) { return X >= 0 && X < GridSize && Y >= 0 && Y < GridSize; }; auto IsActiveTile = [&ActiveMask, &ToIndex](int32 X, int32 Y) { return ActiveMask[ToIndex(X, Y)] != 0; };
    const bool bUrban = NodeRow.EnvType.Equals(TEXT("Urban"), ESearchCase::IgnoreCase);

    for (int32 X = 0; X < GridSize; ++X)
    {
        for (int32 Y = 0; Y < GridSize; ++Y)
        {
            if (!IsActiveTile(X, Y)) continue;
            FVector Location = FVector(X * TileSize - Offset, Y * TileSize - Offset, 0.0f);
            if (bEnableRoomShellGeometry) { if (const FMeshVariation* RandomFloor = RaidMeshUtils::PickRandomVariation(ThemeKit->FloorVariations, RoomRandomStream)) { AddMeshInstance(*RandomFloor, FTransform(FRotator::ZeroRotator, Location), 0); } }

            bool bExposeNorth = !IsInside(X + 1, Y) || !IsActiveTile(X + 1, Y); bool bExposeSouth = !IsInside(X - 1, Y) || !IsActiveTile(X - 1, Y); bool bExposeEast = !IsInside(X, Y + 1) || !IsActiveTile(X, Y + 1); bool bExposeWest = !IsInside(X, Y - 1) || !IsActiveTile(X, Y - 1);
            bool bNorthBoundary = (X == GridSize - 1); bool bSouthBoundary = (X == 0); bool bEastBoundary = (Y == GridSize - 1); bool bWestBoundary = (Y == 0);

            auto SpawnEdge = [&](bool bExpose, float Yaw, bool bDoorOnEdge, bool bSkipForNeighbor) {
                if (!bExpose || bSkipForNeighbor) return;
                float WallSurvivalChance = bUrban ? 0.20f : 0.0f; bool bSpawnWall = RoomRandomStream.FRand() < WallSurvivalChance;
                if (bDoorOnEdge) { const bool bLockDoorForCombatFlow = !bCombatCleared && CurrentRoomType != ERaidRoomType::Start && CurrentRoomType != ERaidRoomType::Exit; if (bLockDoorForCombatFlow) { if (AActor* DoorBlocker = SpawnProceduralDoorBlocker(*ThemeKit, Location, Yaw)) { SpawnedDoorActors.Add(DoorBlocker); } } }
                else if (bSpawnWall) { if (const FMeshVariation* RandomWall = RaidMeshUtils::PickRandomVariation(ThemeKit->WallVariations, RoomRandomStream)) { AddMeshInstance(*RandomWall, FTransform(FRotator(0, Yaw, 0), Location), 1); } }
                };

            if (bEnableRoomShellGeometry) { SpawnEdge(bExposeNorth, 180.0f, bNorthBoundary && bDoorNorth && Y == Center, false); SpawnEdge(bExposeSouth, 0.0f, bSouthBoundary && bDoorSouth && Y == Center, false); SpawnEdge(bExposeEast, -90.0f, bEastBoundary && bDoorEast && X == Center, false); SpawnEdge(bExposeWest, 90.0f, bWestBoundary && bDoorWest && X == Center, false); }

            int32 ExposedCount = (bExposeNorth ? 1 : 0) + (bExposeSouth ? 1 : 0) + (bExposeEast ? 1 : 0) + (bExposeWest ? 1 : 0);
            if (bEnableRoomInteriorGeometry && ExposedCount == 0)
            {
                if (CurrentRoomType == ERaidRoomType::Combat || CurrentRoomType == ERaidRoomType::Boss || CurrentRoomType == ERaidRoomType::Loot) {
                    FVector RandomOffset(RoomRandomStream.FRandRange(-160.0f, 160.0f), RoomRandomStream.FRandRange(-160.0f, 160.0f), 0.0f);
                    if (ThemeKit->ObstacleVariations.Num() > 0 && RoomRandomStream.FRand() < NodeRow.ObstacleDensity) { if (const FMeshVariation* RandomObs = RaidMeshUtils::PickRandomVariation(ThemeKit->ObstacleVariations, RoomRandomStream)) { AddMeshInstance(*RandomObs, FTransform(FRotator(0, RoomRandomStream.RandRange(0, 3) * 90.0f, 0), Location + RandomOffset), 2); } }
                }
            }
        }
    }

    if (bEnableRoomShellGeometry || bEnableRoomInteriorGeometry) GenerateTraversalWhiteboxKit(RoomRadius, ThemeKit);

    TArray<FVector> BuildingWorldLocs;
    for (UHierarchicalInstancedStaticMeshComponent* ISMC : DynamicISMC_Pool)
    {
        if (IsValid(ISMC) && (ISMC->ComponentTags.Contains(TEXT("MeshType_1")) || ISMC->ComponentTags.Contains(TEXT("MeshType_2"))))
        {
            for (int32 i = 0; i < ISMC->GetInstanceCount(); ++i)
            {
                FTransform Trans; ISMC->GetInstanceTransform(i, Trans, true);
                BuildingWorldLocs.Add(Trans.GetLocation());
            }
        }
    }

    if (bEnableRoomOrganicClusters && ThemeKit->bIsOrganicTheme && ThemeKit->FoliageClusters.Num() > 0)
    {
        TArray<FVector> LocalSpawnedLocations;
        for (const FMeshCluster& Cluster : ThemeKit->FoliageClusters)
        {
            if (Cluster.Variations.Num() == 0) continue;
            int32 TargetSpawnCount = Cluster.CalculateSpawnCount(RoomRadius, RoomRandomStream);
            int32 Spawned = 0;

            // [분기 처리] 6번 나무, 7번 풀, 8번 바위 분리!
            int32 FoliageMeshType = 6;
            if (Cluster.ClusterName.Contains(TEXT("NoCol")) || Cluster.ClusterName.Contains(TEXT("Bush"))) FoliageMeshType = 7;
            else if (Cluster.ClusterName.Contains(TEXT("Rock"))) FoliageMeshType = 8;

            float BuildingClearanceSq = FMath::Square(800.0f); // 나무는 건물에서 8m 배척
            if (FoliageMeshType == 7) BuildingClearanceSq = FMath::Square(350.0f); // 풀은 3.5m 배척
            else if (FoliageMeshType == 8) BuildingClearanceSq = FMath::Square(400.0f); // 바위는 4m 배척

            float MinDistSq = FMath::Max(FMath::Square(Cluster.MinDistanceBetweenInstances), BuildingClearanceSq * 0.5f);

            for (int32 i = 0; i < TargetSpawnCount * 10 && Spawned < TargetSpawnCount; ++i)
            {
                FVector FoliageLoc = FVector(RoomRandomStream.FRandRange(-RoomRadius + 150.f, RoomRadius - 150.f), RoomRandomStream.FRandRange(-RoomRadius + 150.f, RoomRadius - 150.f), 0.0f);

                bool bOverlaps = false;
                for (const FVector& Loc : LocalSpawnedLocations) { if (FVector::DistSquaredXY(Loc, FoliageLoc) < MinDistSq) { bOverlaps = true; break; } }
                if (bOverlaps) continue;

                bool bHitsBuilding = false;
                FVector WorldFoliageLoc = GetActorTransform().TransformPosition(FoliageLoc);
                for (const FVector& BldgLoc : BuildingWorldLocs) {
                    if (FVector::DistSquaredXY(BldgLoc, WorldFoliageLoc) < BuildingClearanceSq) { bHitsBuilding = true; break; }
                }
                if (bHitsBuilding) continue;

                if (const FMeshVariation* RandomFoliage = RaidMeshUtils::PickRandomVariation(Cluster.Variations, RoomRandomStream))
                {
                    FTransform FinalTrans = Cluster.GetClusterRandomizedTransform(*RandomFoliage, FTransform(FRotator::ZeroRotator, FoliageLoc), RoomRandomStream);
                    AddMeshInstance(*RandomFoliage, FinalTrans, FoliageMeshType);
                    LocalSpawnedLocations.Add(FoliageLoc); Spawned++;
                }
            }
        }
    }
}

void ARaidRoomActor::InternalSpawnLoot()
{
    if (bLootAlreadySpawned) return; bLootAlreadySpawned = true;
    if (ChapterConfigRef && ChapterConfigRef->LootRegistry)
    {
        int32 LCount = NodeRow.LootCount > 0 ? NodeRow.LootCount : 3; FVector CenterLoc = GetActorLocation();
        bool bIsCentral = NodeRow.LootStrategy.Equals(TEXT("Central_Cache"), ESearchCase::IgnoreCase);
        float MinDistance = bIsCentral ? 100.0f : 300.0f; float MaxDistance = bIsCentral ? 250.0f : ((GridSize * TileSize) / 2.0f - 200.0f); float AngleStep = 360.0f / (float)FMath::Max(1, LCount);

        for (int32 i = 0; i < LCount; ++i)
        {
            if (const FRaidLootCandidate* Candidate = ChapterConfigRef->LootRegistry->GetRandomCandidate(NodeRow.LootLevel))
            {
                if (Candidate->ItemClass)
                {
                    float Radian = FMath::DegreesToRadians(i * AngleStep + RoomRandomStream.FRandRange(-20.0f, 20.0f)); float Distance = RoomRandomStream.FRandRange(MinDistance, MaxDistance);
                    FVector Offset(FMath::Cos(Radian) * Distance, FMath::Sin(Radian) * Distance, 0.0f);
                    FVector StartLoc = CenterLoc + Offset + FVector(0.0f, 0.0f, 1000.0f); FVector EndLoc = CenterLoc + Offset + FVector(0.0f, 0.0f, -500.0f);

                    FHitResult HitResult; FCollisionQueryParams QueryParams; QueryParams.bTraceComplex = true;
                    FVector FinalSpawnLoc = CenterLoc + Offset + FVector(0.0f, 0.0f, 150.0f);
                    FRotator FinalRotation = FRotator(0.0f, RoomRandomStream.FRandRange(0.0f, 360.0f), 0.0f);

                    if (GetWorld()->LineTraceSingleByChannel(HitResult, StartLoc, EndLoc, ECC_WorldStatic, QueryParams)) {
                        FinalSpawnLoc = HitResult.ImpactPoint + FVector(0.0f, 0.0f, 10.0f); // 바닥에서 살짝 위
                        FRotator AlignedRot = FRotationMatrix::MakeFromZX(HitResult.ImpactNormal, FVector(FMath::Cos(FinalRotation.Yaw), FMath::Sin(FinalRotation.Yaw), 0.0f)).Rotator();
                        FinalRotation = (Candidate->Category == ERaidLootCategory::Rifle || Candidate->Category == ERaidLootCategory::Pistol) ? (AlignedRot.Quaternion() * FRotator(90.0f, 0.0f, 0.0f).Quaternion()).Rotator() : AlignedRot;
                    }

                    FActorSpawnParameters SpawnParams;
                    // 오브젝트 겹침 방지: 메쉬나 건물과 겹치면 엔진이 알아서 위치를 살짝 비켜서 안전하게 스폰!
                    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

                    if (AActor* SpawnedItem = GetWorld()->SpawnActor<AActor>(Candidate->ItemClass, FinalSpawnLoc, FinalRotation, SpawnParams))
                    {
                        // 엔진이 위치를 비키면서 공중에 띄워버렸을 경우를 대비해, 다시 바닥으로 내려주는 안전장치
                        FHitResult GroundHit;
                        if (GetWorld()->LineTraceSingleByChannel(GroundHit, SpawnedItem->GetActorLocation(), SpawnedItem->GetActorLocation() - FVector(0.0f, 0.0f, 500.0f), ECC_WorldStatic, QueryParams))
                        {
                            SpawnedItem->SetActorLocation(GroundHit.ImpactPoint + FVector(0.0f, 0.0f, 5.0f));
                        }
                    }
                }
            }
        }
    }
    StatusText->SetTextRenderColor(FColor::Green); StatusText->SetText(FText::FromString(TEXT("CLEARED!"))); OpenRoom();
}

void ARaidRoomActor::SetCombatCleared(bool bCleared) { bCombatCleared = bCleared; if (bCleared) { StatusText->SetTextRenderColor(FColor::Green); StatusText->SetText(FText::FromString(TEXT("CLEARED!"))); OpenRoom(); } }
void ARaidRoomActor::OpenRoom() { for (AActor* Door : SpawnedDoorActors) { if (IsValid(Door)) Door->Destroy(); } SpawnedDoorActors.Empty(); }
FVector ARaidRoomActor::GetRoomExtent() const { return FVector((GridSize * TileSize) / 2.0f); }
void ARaidRoomActor::TryShowRegionBanner(APawn* OverlappingPawn)
{
    if (!OverlappingPawn || bEntryBannerShown || !OverlappingPawn->IsPlayerControlled())
    {
        return;
    }

    APlayerController* PC = Cast<APlayerController>(OverlappingPawn->GetController());
    if (!PC || !PC->IsLocalController())
    {
        return;
    }

    FString TitleStr = NodeRow.NodeTags;
    FString SubStr = NodeRow.RoomRole;
    if (TitleStr.IsEmpty() || TitleStr.Contains(TEXT("[")))
    {
        TitleStr = TEXT("미확인 구역 (Unknown Sector)");
    }
    if (SubStr.IsEmpty())
    {
        SubStr = FString::Printf(TEXT("[%s] 진입함"), *NodeRow.EnvType);
    }

    const float NowTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
    const float CooldownRemaining = 0.55f - (NowTime - GLastRegionBannerShowTime);
    if (CooldownRemaining > 0.0f)
    {
        if (UWorld* World = GetWorld())
        {
            TWeakObjectPtr<ARaidRoomActor> WeakThis(this);
            TWeakObjectPtr<APawn> WeakPawn(OverlappingPawn);
            FTimerHandle RetryHandle;
            World->GetTimerManager().SetTimer(
                RetryHandle,
                FTimerDelegate::CreateLambda([WeakThis, WeakPawn]()
                    {
                        if (ARaidRoomActor* Room = WeakThis.Get())
                        {
                            if (APawn* Pawn = WeakPawn.Get())
                            {
                                Room->TryShowRegionBanner(Pawn);
                            }
                        }
                    }),
                FMath::Max(0.05f, CooldownRemaining + 0.02f),
                false);
        }
        return;
    }

    if (URaidRegionBannerWidget* OldWidget = GSharedRegionBannerWidget.Get())
    {
        OldWidget->RemoveFromParent();
        GSharedRegionBannerWidget.Reset();
    }

    UClass* WidgetClass = RegionBannerWidgetClass.LoadSynchronous();
    if (!WidgetClass)
    {
        WidgetClass = LoadClass<URaidRegionBannerWidget>(nullptr, TEXT("/Game/Game/AILevel/Blueprints/Interfaces/WBP_RaidRegionBanner.WBP_RaidRegionBanner_C"));
    }
    if (!WidgetClass)
    {
        return;
    }

    ActiveRegionBannerWidget = CreateWidget<URaidRegionBannerWidget>(PC, WidgetClass);
    if (!ActiveRegionBannerWidget)
    {
        return;
    }

    bEntryBannerShown = true;
    GSharedRegionBannerWidget = ActiveRegionBannerWidget;
    ActiveRegionBannerWidget->AddToViewport(25);
    ActiveRegionBannerWidget->ShowRegionTitle(FText::FromString(TitleStr), FText::FromString(SubStr), 4.0f);
    GLastRegionBannerShowTime = NowTime;

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(GRegionBannerTimerHandle);
        World->GetTimerManager().SetTimer(
            GRegionBannerTimerHandle,
            FTimerDelegate::CreateLambda([]()
                {
                    if (URaidRegionBannerWidget* Widget = GSharedRegionBannerWidget.Get())
                    {
                        if (Widget->IsInViewport())
                        {
                            Widget->RemoveFromParent();
                        }
                    }
                }),
            5.5f,
            false);
    }
}
void ARaidRoomActor::OnOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
    if (APawn* OverlappingPawn = Cast<APawn>(OtherActor)) {
        TryShowRegionBanner(OverlappingPawn);
        if (OverlappingPawn->IsPlayerControlled() && !bCombatStarted && !bCombatCleared) {
            if (URaidCombatSubsystem* CombatSubsystem = GetWorld()->GetSubsystem<URaidCombatSubsystem>()) { CombatSubsystem->StartCombatForRoom(this); Trigger->SetCollisionEnabled(ECollisionEnabled::NoCollision); }
        }
    }
}

void ARaidRoomActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);

    // 🔥 1. 트리거(마커 감지 영역)를 오픈월드 스케일에 맞게 거대하게 확장
    if (Trigger)
    {
        float RoomRadius = (GridSize * TileSize) / 2.0f;
        Trigger->SetBoxExtent(FVector(RoomRadius, RoomRadius, 10000.0f));
        Trigger->SetRelativeLocation(FVector(0.0f, 0.0f, 2000.0f));
    }

    // 🔥 2. 기존에 있던 룸 레이아웃 생성 호출 유지
    GenerateRoomLayout();
}

