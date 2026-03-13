#include "Raid/RaidCombatSubsystem.h"
#include "Raid/RaidRoomActor.h"
#include "Raid/RaidChapterConfig.h"
#include "Raid/RaidEnemyPresetRegistry.h"
#include "Engine/EngineTypes.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "NavigationSystem.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Character.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EngineUtils.h"
#include "TimerManager.h"

namespace
{
    void ForceAllGameTraceChannelsToBlock(UPrimitiveComponent* Primitive)
    {
        if (!IsValid(Primitive))
        {
            return;
        }

        for (int32 ChannelIdx = (int32)ECC_GameTraceChannel1; ChannelIdx <= (int32)ECC_GameTraceChannel18; ++ChannelIdx)
        {
            Primitive->SetCollisionResponseToChannel((ECollisionChannel)ChannelIdx, ECR_Block);
        }
    }

    UCapsuleComponent* FindNamedCapsuleComponent(APawn* Enemy, const FName ComponentName)
    {
        if (!IsValid(Enemy))
        {
            return nullptr;
        }

        TInlineComponentArray<UCapsuleComponent*> Capsules;
        Enemy->GetComponents(Capsules);
        for (UCapsuleComponent* Capsule : Capsules)
        {
            if (IsValid(Capsule) && Capsule->GetFName() == ComponentName)
            {
                return Capsule;
            }
        }
        return nullptr;
    }

    void EnsureTraceProxyCapsule(APawn* Enemy, const FName ComponentName, ECollisionChannel ObjectType, float Radius, float HalfHeight)
    {
        if (!IsValid(Enemy))
        {
            return;
        }

        UCapsuleComponent* ProxyCapsule = FindNamedCapsuleComponent(Enemy, ComponentName);
        if (!ProxyCapsule)
        {
            ProxyCapsule = NewObject<UCapsuleComponent>(Enemy, ComponentName);
            if (!IsValid(ProxyCapsule))
            {
                return;
            }

            if (USceneComponent* Root = Enemy->GetRootComponent())
            {
                ProxyCapsule->SetupAttachment(Root);
            }
            ProxyCapsule->RegisterComponent();
        }

        ProxyCapsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        ProxyCapsule->SetCollisionObjectType(ObjectType);
        ProxyCapsule->SetCollisionResponseToAllChannels(ECR_Block);
        ProxyCapsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
        ProxyCapsule->SetCollisionResponseToChannel(ECC_Vehicle, ECR_Ignore);
        ProxyCapsule->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
        ProxyCapsule->SetCollisionResponseToChannel(ECC_Camera, ECR_Block);
        ForceAllGameTraceChannelsToBlock(ProxyCapsule);
        ProxyCapsule->SetCanEverAffectNavigation(false);
        ProxyCapsule->SetGenerateOverlapEvents(false);
        ProxyCapsule->SetHiddenInGame(true);
        ProxyCapsule->SetVisibility(false, true);
        ProxyCapsule->SetCapsuleSize(FMath::Max(20.0f, Radius), FMath::Max(40.0f, HalfHeight), true);
        ProxyCapsule->SetRelativeLocation(FVector::ZeroVector);
    }

    FString BuildTraceChannelSnapshot(const UPrimitiveComponent* Primitive)
    {
        if (!IsValid(Primitive))
        {
            return TEXT("None");
        }

        TArray<FString> BlockedChannels;
        BlockedChannels.Reserve(20);

        if (Primitive->GetCollisionResponseToChannel(ECC_Visibility) == ECR_Block)
        {
            BlockedChannels.Add(TEXT("Visibility"));
        }
        if (Primitive->GetCollisionResponseToChannel(ECC_Camera) == ECR_Block)
        {
            BlockedChannels.Add(TEXT("Camera"));
        }

        for (int32 ChannelIdx = (int32)ECC_GameTraceChannel1; ChannelIdx <= (int32)ECC_GameTraceChannel18; ++ChannelIdx)
        {
            if (Primitive->GetCollisionResponseToChannel((ECollisionChannel)ChannelIdx) == ECR_Block)
            {
                BlockedChannels.Add(FString::Printf(TEXT("GameTrace%d"), ChannelIdx - (int32)ECC_GameTraceChannel1 + 1));
            }
        }

        return FString::Printf(
            TEXT("Enabled=%d ObjType=%d Blocked=[%s]"),
            (int32)Primitive->GetCollisionEnabled(),
            (int32)Primitive->GetCollisionObjectType(),
            *FString::Join(BlockedChannels, TEXT(","))
        );
    }

    void LogEnemyTraceCollisionSnapshot(const APawn* Enemy)
    {
        if (!IsValid(Enemy))
        {
            return;
        }

        const UCapsuleComponent* Capsule = Enemy->FindComponentByClass<UCapsuleComponent>();
        const USkeletalMeshComponent* MeshComp = Enemy->FindComponentByClass<USkeletalMeshComponent>();
        UE_LOG(
            LogTemp,
            Warning,
            TEXT("[RaidCombat] Collision snapshot Enemy='%s' Capsule={%s} Mesh={%s}"),
            *GetNameSafe(Enemy),
            *BuildTraceChannelSnapshot(Capsule),
            *BuildTraceChannelSnapshot(MeshComp));
    }

    void ForceEnemyTraceCollision(APawn* Enemy)
    {
        if (!IsValid(Enemy))
        {
            return;
        }

        Enemy->SetCanBeDamaged(true);
        Enemy->SetActorEnableCollision(true);

        float ProxyRadius = 42.0f;
        float ProxyHalfHeight = 88.0f;

        if (UCapsuleComponent* Capsule = Enemy->FindComponentByClass<UCapsuleComponent>())
        {
            ProxyRadius = Capsule->GetUnscaledCapsuleRadius();
            ProxyHalfHeight = Capsule->GetUnscaledCapsuleHalfHeight();
            Capsule->SetCollisionObjectType(ECC_Pawn);
            Capsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
            Capsule->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
            Capsule->SetCollisionResponseToChannel(ECC_Camera, ECR_Block);
            ForceAllGameTraceChannelsToBlock(Capsule);
        }

        if (USkeletalMeshComponent* MeshComp = Enemy->FindComponentByClass<USkeletalMeshComponent>())
        {
            // Keep one query body as WorldDynamic because many weapon blueprints use Object traces.
            MeshComp->SetCollisionObjectType(ECC_WorldDynamic);
            if (MeshComp->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
            {
                MeshComp->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
            }
            MeshComp->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
            MeshComp->SetCollisionResponseToChannel(ECC_Camera, ECR_Block);
            MeshComp->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
            ForceAllGameTraceChannelsToBlock(MeshComp);
        }

        // Redundant trace proxies to survive collision profile resets from external blueprints/plugins.
        EnsureTraceProxyCapsule(Enemy, TEXT("RaidHitProxy_WorldDynamic"), ECC_WorldDynamic, ProxyRadius, ProxyHalfHeight);
        EnsureTraceProxyCapsule(Enemy, TEXT("RaidHitProxy_PhysicsBody"), ECC_PhysicsBody, ProxyRadius, ProxyHalfHeight);
    }

    int32 GetRoomTypePriority(const FString& RoomType)
    {
        if (RoomType.Equals(TEXT("Exit"), ESearchCase::IgnoreCase)) return 100;
        if (RoomType.Equals(TEXT("Boss"), ESearchCase::IgnoreCase)) return 80;
        if (RoomType.Equals(TEXT("Loot"), ESearchCase::IgnoreCase)) return 60;
        if (RoomType.Equals(TEXT("Combat"), ESearchCase::IgnoreCase)) return 40;
        return 10;
    }

    void BuildPresetCandidates(const FLevelNodeRow& Row, TArray<FName>& OutCandidates)
    {
        OutCandidates.Reset();

        const FString PresetRaw = Row.EnemyPreset.TrimStartAndEnd();
        const bool bRequestedNone = PresetRaw.IsEmpty() || PresetRaw.Equals(TEXT("None"), ESearchCase::IgnoreCase);

        if (!bRequestedNone)
        {
            OutCandidates.AddUnique(FName(*PresetRaw));
        }

        if (Row.RoomType.Equals(TEXT("Boss"), ESearchCase::IgnoreCase))
        {
            OutCandidates.AddUnique(TEXT("BossGuard"));
        }
        else if (Row.EnvType.Equals(TEXT("Jungle"), ESearchCase::IgnoreCase))
        {
            OutCandidates.AddUnique(TEXT("Scavenger"));
            OutCandidates.AddUnique(TEXT("Raider"));
        }
        else if (Row.EnvType.Equals(TEXT("Urban"), ESearchCase::IgnoreCase))
        {
            OutCandidates.AddUnique(TEXT("Raider"));
            OutCandidates.AddUnique(TEXT("Scavenger"));
            OutCandidates.AddUnique(TEXT("Sniper"));
        }
        else
        {
            OutCandidates.AddUnique(TEXT("Scavenger"));
            OutCandidates.AddUnique(TEXT("Raider"));
        }

        OutCandidates.AddUnique(TEXT("Default"));
    }

    bool IsWaterHit(const FHitResult& Hit)
    {
        if (AActor* HitActor = Hit.GetActor())
        {
            if (HitActor->ActorHasTag(TEXT("Water"))) return true;
            const FString ActorClass = HitActor->GetClass()->GetName();
            if (ActorClass.Contains(TEXT("Water"), ESearchCase::IgnoreCase)) return true;
        }

        if (UPrimitiveComponent* HitComp = Hit.GetComponent())
        {
            if (HitComp->ComponentHasTag(TEXT("Water"))) return true;
            const FString CompClass = HitComp->GetClass()->GetName();
            if (CompClass.Contains(TEXT("Water"), ESearchCase::IgnoreCase)) return true;
        }

        return false;
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

    bool IsRoomComponentHitWithTag(const FHitResult& Hit, const ARaidRoomActor* Room, const FName& Tag)
    {
        if (!Room || Hit.GetActor() != Room) return false;
        const UPrimitiveComponent* HitComp = Hit.GetComponent();
        return HitComp && HitComp->ComponentTags.Contains(Tag);
    }

    bool IsRoomFloorHit(const FHitResult& Hit, const ARaidRoomActor* Room)
    {
        return IsRoomComponentHitWithTag(Hit, Room, TEXT("MeshType_0"));
    }

    bool IsRoomObstacleHit(const FHitResult& Hit, const ARaidRoomActor* Room)
    {
        if (!Room || Hit.GetActor() != Room) return false;

        const UPrimitiveComponent* HitComp = Hit.GetComponent();
        if (!HitComp) return false;

        return
            HitComp->ComponentTags.Contains(TEXT("MeshType_1")) ||
            HitComp->ComponentTags.Contains(TEXT("MeshType_2")) ||
            HitComp->ComponentTags.Contains(TEXT("MeshType_3")) ||
            HitComp->ComponentTags.Contains(TEXT("MeshType_6")) ||
            HitComp->ComponentTags.Contains(TEXT("MeshType_7")) ||
            HitComp->ComponentTags.Contains(TEXT("MeshType_8"));
    }

    bool TryResolveAIGroundHit(UWorld* World, ARaidRoomActor* Room, const FVector& XYLocation, FHitResult& OutHit)
    {
        if (!World) return false;

        FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(RaidAIGroundResolve), false);
        QueryParams.bTraceComplex = false;

        TArray<FHitResult> Hits;
        const FVector Start(XYLocation.X, XYLocation.Y, 5000.0f);
        const FVector End(XYLocation.X, XYLocation.Y, -5000.0f);
        if (!World->LineTraceMultiByChannel(Hits, Start, End, ECC_WorldStatic, QueryParams))
        {
            return false;
        }

        const FHitResult* BestRoomFloor = nullptr;
        const FHitResult* BestLandscape = nullptr;
        const FHitResult* BestGeneral = nullptr;

        for (const FHitResult& Hit : Hits)
        {
            if (!Hit.bBlockingHit) continue;
            if (IsWaterHit(Hit)) continue;
            if (IsRoomObstacleHit(Hit, Room)) continue;

            if (IsRoomFloorHit(Hit, Room))
            {
                if (!BestRoomFloor || Hit.Distance < BestRoomFloor->Distance)
                {
                    BestRoomFloor = &Hit;
                }
                continue;
            }

            if (IsLandscapeLikeHit(Hit))
            {
                if (!BestLandscape || Hit.Distance < BestLandscape->Distance)
                {
                    BestLandscape = &Hit;
                }
            }

            if (!BestGeneral || Hit.Distance < BestGeneral->Distance)
            {
                BestGeneral = &Hit;
            }
        }

        const FHitResult* Selected = BestRoomFloor ? BestRoomFloor : (BestLandscape ? BestLandscape : BestGeneral);
        if (!Selected)
        {
            return false;
        }

        OutHit = *Selected;
        return true;
    }

    bool IsNearRoomObstacle(UWorld* World, const ARaidRoomActor* Room, const FVector& Location, float Radius)
    {
        if (!World || !Room) return false;

        FCollisionObjectQueryParams ObjQuery;
        ObjQuery.AddObjectTypesToQuery(ECC_WorldStatic);
        ObjQuery.AddObjectTypesToQuery(ECC_WorldDynamic);

        FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(RaidAIObstacleOverlap), false);
        QueryParams.bTraceComplex = false;

        TArray<FOverlapResult> Overlaps;
        if (!World->OverlapMultiByObjectType(
            Overlaps,
            Location,
            FQuat::Identity,
            ObjQuery,
            FCollisionShape::MakeSphere(Radius),
            QueryParams))
        {
            return false;
        }

        for (const FOverlapResult& Overlap : Overlaps)
        {
            const UPrimitiveComponent* Comp = Overlap.Component.Get();
            const AActor* Owner = Overlap.GetActor();
            if (!Comp) continue;

            const bool bIsRoomOwnedComponent = (Owner == Room);
            const bool bIsRoomOwnedDoorBlocker = Owner && Owner->ActorHasTag(TEXT("RaidDoorBlocker")) && Owner->GetOwner() == Room;
            if (!bIsRoomOwnedComponent && !bIsRoomOwnedDoorBlocker) continue;
            if (Comp->GetCollisionEnabled() == ECollisionEnabled::NoCollision) continue;
            if (Comp->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block) continue;

            if (Comp->ComponentTags.Contains(TEXT("MeshType_1")) ||
                Comp->ComponentTags.Contains(TEXT("MeshType_2")) ||
                Comp->ComponentTags.Contains(TEXT("MeshType_3")) ||
                Comp->ComponentTags.Contains(TEXT("MeshType_6")) ||
                Comp->ComponentTags.Contains(TEXT("MeshType_7")) ||
                Comp->ComponentTags.Contains(TEXT("MeshType_8")) ||
                bIsRoomOwnedDoorBlocker)
            {
                return true;
            }
        }

        return false;
    }

    void ResolvePawnCapsuleSize(TSubclassOf<APawn> EnemyClass, float& OutRadius, float& OutHalfHeight)
    {
        OutRadius = 42.0f;
        OutHalfHeight = 88.0f;

        if (!EnemyClass) return;
        const APawn* DefaultPawn = EnemyClass->GetDefaultObject<APawn>();
        const ACharacter* DefaultCharacter = Cast<ACharacter>(DefaultPawn);
        if (!DefaultCharacter) return;

        const UCapsuleComponent* Capsule = DefaultCharacter->GetCapsuleComponent();
        if (!Capsule) return;

        OutRadius = FMath::Max(20.0f, Capsule->GetScaledCapsuleRadius());
        OutHalfHeight = FMath::Max(40.0f, Capsule->GetScaledCapsuleHalfHeight());
    }

    bool IsCapsuleBlockedForPawn(UWorld* World, const FVector& PawnActorLocation, float CapsuleRadius, float CapsuleHalfHeight)
    {
        if (!World) return true;
        FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(RaidAICapsuleCheck), false);
        QueryParams.bTraceComplex = false;
        return World->OverlapBlockingTestByProfile(
            PawnActorLocation,
            FQuat::Identity,
            TEXT("Pawn"),
            FCollisionShape::MakeCapsule(CapsuleRadius, CapsuleHalfHeight),
            QueryParams);
    }

    bool TryResolveSafeAIPawnSpawnLocation(
        UWorld* World,
        ARaidRoomActor* Room,
        UNavigationSystemV1* NavSys,
        const FVector& XYLocation,
        float CapsuleRadius,
        float CapsuleHalfHeight,
        FVector& OutActorLocation)
    {
        if (!World || !Room) return false;

        FHitResult GroundHit;
        if (!TryResolveAIGroundHit(World, Room, XYLocation, GroundHit))
        {
            return false;
        }

        FVector CandidateGroundLoc = GroundHit.ImpactPoint;
        FVector CandidateGroundNormal = GroundHit.ImpactNormal;

        if (NavSys)
        {
            FNavLocation NavLoc;
            if (NavSys->ProjectPointToNavigation(CandidateGroundLoc + FVector(0.0f, 0.0f, 50.0f), NavLoc, FVector(260.0f, 260.0f, 260.0f)))
            {
                FHitResult NavGroundHit;
                if (TryResolveAIGroundHit(World, Room, NavLoc.Location, NavGroundHit))
                {
                    CandidateGroundLoc = NavGroundHit.ImpactPoint;
                    CandidateGroundNormal = NavGroundHit.ImpactNormal;
                }
                else
                {
                    CandidateGroundLoc = NavLoc.Location;
                }
            }
        }

        // Avoid steep normals that usually mean wall/side-surface hits.
        if (CandidateGroundNormal.Z < 0.55f)
        {
            return false;
        }

        const FVector CandidateActorLoc = CandidateGroundLoc + FVector(0.0f, 0.0f, CapsuleHalfHeight + 6.0f);
        if (IsNearRoomObstacle(World, Room, CandidateActorLoc, CapsuleRadius + 70.0f))
        {
            return false;
        }
        if (IsCapsuleBlockedForPawn(World, CandidateActorLoc, CapsuleRadius, CapsuleHalfHeight))
        {
            return false;
        }

        OutActorLocation = CandidateActorLoc;
        return true;
    }

    bool TryResolveNearbyFallbackSpawnLocation(
        UWorld* World,
        ARaidRoomActor* Room,
        UNavigationSystemV1* NavSys,
        const FVector& SeedActorLocation,
        float CapsuleRadius,
        float CapsuleHalfHeight,
        FRandomStream& Stream,
        FVector& OutActorLocation)
    {
        static const FVector2D DirectionSamples[] =
        {
            FVector2D(1.0f, 0.0f),
            FVector2D(-1.0f, 0.0f),
            FVector2D(0.0f, 1.0f),
            FVector2D(0.0f, -1.0f),
            FVector2D(0.7071f, 0.7071f),
            FVector2D(0.7071f, -0.7071f),
            FVector2D(-0.7071f, 0.7071f),
            FVector2D(-0.7071f, -0.7071f)
        };
        static const float RingRadii[] = { 140.0f, 260.0f, 380.0f };

        const int32 DirectionCount = static_cast<int32>(sizeof(DirectionSamples) / sizeof(DirectionSamples[0]));
        const int32 RingCount = static_cast<int32>(sizeof(RingRadii) / sizeof(RingRadii[0]));
        if (DirectionCount <= 0 || RingCount <= 0)
        {
            return false;
        }

        for (int32 RingIndex = 0; RingIndex < RingCount; ++RingIndex)
        {
            const float RingDistance = RingRadii[RingIndex] + CapsuleRadius;
            const int32 StartDirection = Stream.RandRange(0, DirectionCount - 1);
            for (int32 OffsetIdx = 0; OffsetIdx < DirectionCount; ++OffsetIdx)
            {
                const FVector2D Dir = DirectionSamples[(StartDirection + OffsetIdx) % DirectionCount];
                const FVector XYCandidate(
                    SeedActorLocation.X + Dir.X * RingDistance + Stream.FRandRange(-22.0f, 22.0f),
                    SeedActorLocation.Y + Dir.Y * RingDistance + Stream.FRandRange(-22.0f, 22.0f),
                    SeedActorLocation.Z);

                if (TryResolveSafeAIPawnSpawnLocation(World, Room, NavSys, XYCandidate, CapsuleRadius, CapsuleHalfHeight, OutActorLocation))
                {
                    return true;
                }
            }
        }

        return false;
    }
}

void URaidCombatSubsystem::RegisterRoom(ARaidRoomActor* Room)
{
    if (!Room) return;
    RoomById.Add(Room->GetNodeId(), Room);
    if (Room->GetNodeRow().RoomType.Equals(TEXT("Start"), ESearchCase::IgnoreCase))
    {
        StartRoomId = Room->GetNodeId();
        bStartFlowInitialized = false;
        bPlayerSpawnedInsideStartRoom = false;
        bStartPendingClearOnExit = false;
    }
    StartTraceCollisionEnforcer();
}

void URaidCombatSubsystem::RegisterRoomAsPOI(ARaidRoomActor* InRoom)
{
    if (!InRoom) return;
    RegisterRoom(InRoom);

    const FString Type = InRoom->GetNodeRow().RoomType;
    if (Type.Equals(TEXT("Start"), ESearchCase::IgnoreCase))
    {
        return;
    }
    AddPOI(InRoom->GetActorLocation(), FName(*Type));
}

void URaidCombatSubsystem::OnEnemySpawned(APawn* Enemy, int32 RoomId)
{
    if (!Enemy) return;
    EnemyToRoomMap.Add(Enemy, RoomId);
}

void URaidCombatSubsystem::OnEnemyKilled(APawn* Enemy)
{
    if (!Enemy) return;
    OnEnemyDestroyed(Enemy);
}

void URaidCombatSubsystem::ResetSubsystem()
{
    RoomById.Empty();
    AliveByRoomId.Empty();
    EnemyToRoomMap.Empty();
    ClearPOIs();
    bInternalClearing = false;
    CurrentObjectiveRoomId = -1;
    CurrentObjectiveLocation = FVector::ZeroVector;
    LastProgressTimeSeconds = 0.0f;
    LastDistanceToObjective = TNumericLimits<float>::Max();
    WrongDirectionScore = 0.0f;
    bFoliageTraceCollisionSanitized = false;
    StartRoomId = -1;
    bStartFlowInitialized = false;
    bPlayerSpawnedInsideStartRoom = false;
    bStartPendingClearOnExit = false;
    StopTraceCollisionEnforcer();
}

APawn* URaidCombatSubsystem::GetPrimaryPlayerPawn() const
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return nullptr;
    }

    for (TActorIterator<APawn> It(World); It; ++It)
    {
        APawn* Pawn = *It;
        if (IsValid(Pawn) && Pawn->IsPlayerControlled())
        {
            return Pawn;
        }
    }

    return nullptr;
}

ARaidRoomActor* URaidCombatSubsystem::FindStartRoom() const
{
    if (StartRoomId != -1)
    {
        if (const TObjectPtr<ARaidRoomActor>* Found = RoomById.Find(StartRoomId))
        {
            return Found->Get();
        }
    }

    for (const TPair<int32, TObjectPtr<ARaidRoomActor>>& Pair : RoomById)
    {
        ARaidRoomActor* Room = Pair.Value.Get();
        if (!Room) continue;
        if (Room->GetNodeRow().RoomType.Equals(TEXT("Start"), ESearchCase::IgnoreCase))
        {
            return Room;
        }
    }
    return nullptr;
}

bool URaidCombatSubsystem::IsPawnInsideRoomBounds2D(const APawn* Pawn, const ARaidRoomActor* Room) const
{
    if (!Pawn || !Room)
    {
        return false;
    }

    const FVector PawnLoc = Pawn->GetActorLocation();
    const FVector RoomLoc = Room->GetActorLocation();
    const FVector Extent = Room->GetRoomExtent();
    const float Padding = FMath::Max(0.0f, RoomInsideCheckPadding);

    return
        FMath::Abs(PawnLoc.X - RoomLoc.X) <= (Extent.X + Padding) &&
        FMath::Abs(PawnLoc.Y - RoomLoc.Y) <= (Extent.Y + Padding);
}

int32 URaidCombatSubsystem::ResolvePrimaryProgressionRoomId(const ARaidRoomActor* StartRoom) const
{
    if (const TObjectPtr<ARaidRoomActor>* RoomOnePtr = RoomById.Find(1))
    {
        ARaidRoomActor* RoomOne = RoomOnePtr->Get();
        if (RoomOne && !RoomOne->IsCleared() &&
            !RoomOne->GetNodeRow().RoomType.Equals(TEXT("Start"), ESearchCase::IgnoreCase))
        {
            return 1;
        }
    }

    if (!StartRoom)
    {
        return -1;
    }

    const FVector StartLoc = StartRoom->GetActorLocation();
    float BestDistSq = TNumericLimits<float>::Max();
    int32 BestId = -1;
    for (const TPair<int32, TObjectPtr<ARaidRoomActor>>& Pair : RoomById)
    {
        ARaidRoomActor* Room = Pair.Value.Get();
        if (!Room || Room->IsCleared()) continue;
        if (Room == StartRoom) continue;
        if (Room->GetNodeRow().RoomType.Equals(TEXT("Start"), ESearchCase::IgnoreCase)) continue;

        const float DistSq = FVector::DistSquared2D(StartLoc, Room->GetActorLocation());
        if (DistSq < BestDistSq)
        {
            BestDistSq = DistSq;
            BestId = Pair.Key;
        }
    }
    return BestId;
}

void URaidCombatSubsystem::ForceObjectiveToRoom(ARaidRoomActor* Room, FName MarkerType)
{
    if (!Room)
    {
        return;
    }

    if (MarkerType != NAME_None)
    {
        AddPOI(Room->GetActorLocation(), MarkerType);
    }

    CurrentObjectiveRoomId = Room->GetNodeId();
    CurrentObjectiveLocation = Room->GetActorLocation();
    LastProgressTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
    LastDistanceToObjective = TNumericLimits<float>::Max();
    WrongDirectionScore = 0.0f;
}

void URaidCombatSubsystem::AddNearbyOptionalPOIsFromStart(const ARaidRoomActor* StartRoom, int32 PrimaryRoomId)
{
    if (!StartRoom)
    {
        return;
    }

    const float MaxDistSq = FMath::Square(FMath::Max(1000.0f, StartOptionalPOIRadius));
    const FVector StartLoc = StartRoom->GetActorLocation();
    for (const TPair<int32, TObjectPtr<ARaidRoomActor>>& Pair : RoomById)
    {
        ARaidRoomActor* Room = Pair.Value.Get();
        if (!Room || Room->IsCleared()) continue;
        if (Room->GetNodeId() == PrimaryRoomId) continue;
        if (Room == StartRoom) continue;
        if (Room->GetNodeRow().RoomType.Equals(TEXT("Start"), ESearchCase::IgnoreCase)) continue;

        const float DistSq = FVector::DistSquared2D(StartLoc, Room->GetActorLocation());
        if (DistSq <= MaxDistSq)
        {
            AddPOI(Room->GetActorLocation(), FName(*Room->GetNodeRow().RoomType));
        }
    }
}

void URaidCombatSubsystem::RefreshStartRoomProgressState(APawn* PlayerPawn)
{
    ARaidRoomActor* StartRoom = FindStartRoom();
    if (!StartRoom || !PlayerPawn)
    {
        return;
    }

    if (StartRoom->IsCleared())
    {
        bStartFlowInitialized = true;
        bStartPendingClearOnExit = false;
        return;
    }

    const bool bInsideStart = IsPawnInsideRoomBounds2D(PlayerPawn, StartRoom);
    if (!bStartFlowInitialized)
    {
        bStartFlowInitialized = true;
        bPlayerSpawnedInsideStartRoom = bInsideStart;
        bStartPendingClearOnExit = bInsideStart;
    }

    // Spawn outside Start: clear on first entry.
    if (!bStartPendingClearOnExit)
    {
        if (bInsideStart)
        {
            HandleRoomCleared(StartRoom->GetNodeId());
        }
        return;
    }

    // Spawn inside Start: clear on first exit.
    if (!bInsideStart)
    {
        bStartPendingClearOnExit = false;
        HandleRoomCleared(StartRoom->GetNodeId());
    }
}

void URaidCombatSubsystem::SanitizeProceduralFoliageCollisionForTraces()
{
    if (bFoliageTraceCollisionSanitized)
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    int32 PatchedComponentCount = 0;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Candidate = *It;
        if (!IsValid(Candidate))
        {
            continue;
        }

        const FString ClassName = Candidate->GetClass()->GetName();
        const bool bIsProceduralOrInstancedFoliageActor =
            ClassName.Contains(TEXT("InstancedFoliageActor"), ESearchCase::IgnoreCase) ||
            ClassName.Contains(TEXT("ProceduralFoliage"), ESearchCase::IgnoreCase);
        if (!bIsProceduralOrInstancedFoliageActor)
        {
            continue;
        }

        TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents;
        Candidate->GetComponents(PrimitiveComponents);
        for (UPrimitiveComponent* Primitive : PrimitiveComponents)
        {
            if (!IsValid(Primitive))
            {
                continue;
            }

            Primitive->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
            Primitive->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
            Primitive->SetCollisionResponseToChannel(ECC_GameTraceChannel10, ECR_Ignore); // HitboxTrace
            ++PatchedComponentCount;
        }
    }

    bFoliageTraceCollisionSanitized = true;
    UE_LOG(LogTemp, Warning, TEXT("[RaidCombat] Foliage trace-collision sanitize completed. Patched components: %d"), PatchedComponentCount);
}

void URaidCombatSubsystem::StartCombatForRoom(ARaidRoomActor* Room)
{
    if (!IsValid(Room) || Room->HasCombatStarted() || Room->IsCleared()) return;

    SanitizeProceduralFoliageCollisionForTraces();
    StartTraceCollisionEnforcer();

    RoomById.Add(Room->GetNodeId(), Room);
    Room->SetCombatStarted(true);
    ClearPOIs();

    const FLevelNodeRow& Row = Room->GetNodeRow();

    if (Row.RoomType.Equals(TEXT("Start"), ESearchCase::IgnoreCase))
    {
        if (APawn* PlayerPawn = GetPrimaryPlayerPawn())
        {
            RefreshStartRoomProgressState(PlayerPawn);
        }
        UpdateCompassForNextRooms(nullptr);
        return;
    }

    if (Row.RoomType.Equals(TEXT("Exit"), ESearchCase::IgnoreCase))
    {
        HandleRoomCleared(Room->GetNodeId());
        return;
    }

    if (Row.RoomType.Equals(TEXT("Loot"), ESearchCase::IgnoreCase))
    {
        Room->InternalSpawnLoot();
        HandleRoomCleared(Room->GetNodeId());
    }
    else
    {
        SpawnEnemiesForRoom(Room);
    }
}

void URaidCombatSubsystem::StartTraceCollisionEnforcer()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    FTimerManager& TimerManager = World->GetTimerManager();
    if (TimerManager.IsTimerActive(TraceCollisionEnforcerHandle))
    {
        return;
    }

    TimerManager.SetTimer(
        TraceCollisionEnforcerHandle,
        this,
        &URaidCombatSubsystem::EnforceTraceCollisionOnAllAIPawns,
        0.50f,
        true,
        0.10f);
}

void URaidCombatSubsystem::StopTraceCollisionEnforcer()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }
    World->GetTimerManager().ClearTimer(TraceCollisionEnforcerHandle);
}

void URaidCombatSubsystem::EnforceTraceCollisionOnAllAIPawns()
{
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    int32 FixedCount = 0;
    for (TActorIterator<APawn> It(World); It; ++It)
    {
        APawn* Pawn = *It;
        if (!IsValid(Pawn))
        {
            continue;
        }
        if (Pawn->IsPlayerControlled())
        {
            continue;
        }

        ForceEnemyTraceCollision(Pawn);
        ++FixedCount;
    }

    static int32 SweepCounter = 0;
    ++SweepCounter;
    if (SweepCounter % 20 == 0 && FixedCount > 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("[RaidCombat] Trace-collision enforcer sweep updated %d AI pawns."), FixedCount);
    }
}

void URaidCombatSubsystem::SpawnEnemiesForRoom(ARaidRoomActor* Room)
{
    UWorld* World = GetWorld();
    if (!World || !Room)
    {
        return;
    }

    const FLevelNodeRow& Row = Room->GetNodeRow();
    int32 Id = Room->GetNodeId();

    const URaidChapterConfig* Config = Room->GetChapterConfig();
    if (!Config || !Config->EnemyPresetRegistry)
    {
        UE_LOG(LogTemp, Error, TEXT("[RaidCombat] ERROR: EnemyPresetRegistry is missing in DA_ChapterConfig!"));
        HandleRoomCleared(Id);
        return;
    }

    TArray<FName> PresetCandidates;
    BuildPresetCandidates(Row, PresetCandidates);

    FName EffectivePreset = NAME_None;
    for (const FName Candidate : PresetCandidates)
    {
        FRaidEnemyPreset FoundPreset;
        if (Config->EnemyPresetRegistry->ResolvePreset(Candidate, FoundPreset) && FoundPreset.IsValid())
        {
            EffectivePreset = Candidate;
            break;
        }
    }

    if (EffectivePreset.IsNone())
    {
        UE_LOG(LogTemp, Error, TEXT("[RaidCombat] ERROR: No valid enemy preset found for Room %d (requested='%s')."),
            Id, *Row.EnemyPreset);
        HandleRoomCleared(Id);
        return;
    }

    FRandomStream Stream(Row.Seed ^ (Id * 7919));

    const bool bBossRoom = Row.RoomType.Equals(TEXT("Boss"), ESearchCase::IgnoreCase);
    const bool bCombatLike = Row.RoomType.Equals(TEXT("Combat"), ESearchCase::IgnoreCase) || Row.RoomType.Equals(TEXT("Normal"), ESearchCase::IgnoreCase) || bBossRoom;
    const float SafeDifficulty = FMath::Clamp(Row.Difficulty > 0.01f ? Row.Difficulty : 1.0f, 0.6f, 3.0f);
    const float SafeCombatWeight = FMath::Clamp(Row.CombatWeight > 0.01f ? Row.CombatWeight : 1.0f, 0.6f, 2.6f);
    const int32 BaseSpawnCount = Row.SpawnCount > 0 ? Row.SpawnCount : (bBossRoom ? 3 : 3);
    const FString BotProfile = Row.BotProfile.TrimStartAndEnd();

    int32 FinalSpawnCount = FMath::RoundToInt((float)BaseSpawnCount * (0.75f + SafeDifficulty * 0.45f) * (0.70f + SafeCombatWeight * 0.35f));
    if (bBossRoom) FinalSpawnCount = FMath::Max(2, FinalSpawnCount); // boss + helper
    else if (bCombatLike) FinalSpawnCount = FMath::Max(2, FinalSpawnCount);
    else FinalSpawnCount = FMath::Max(1, FinalSpawnCount);
    FinalSpawnCount = FMath::Clamp(FinalSpawnCount, 1, FMath::Max(1, MaxEnemiesPerRoom));

    int32 SpawnedCount = 0;
    FVector Center = Room->GetActorLocation();
    float SpawnRadius = (Room->GridSize * Room->TileSize) / 2.0f - 200.0f;
    float AngleStep = 360.0f / FinalSpawnCount;

    UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(World);

    for (int32 i = 0; i < FinalSpawnCount; ++i)
    {
        FName SpawnPreset = EffectivePreset;
        // Boss room composition: first unit from boss preset, others from helper presets.
        if (bBossRoom && i > 0)
        {
            TArray<FName> BossHelperCandidates;
            BossHelperCandidates.Add(TEXT("Raider"));
            BossHelperCandidates.Add(TEXT("Scavenger"));
            BossHelperCandidates.Add(TEXT("Sniper"));
            BossHelperCandidates.Add(TEXT("Default"));
            for (const FName HelperPreset : BossHelperCandidates)
            {
                FRaidEnemyPreset FoundPreset;
                if (Config->EnemyPresetRegistry->ResolvePreset(HelperPreset, FoundPreset) && FoundPreset.IsValid())
                {
                    SpawnPreset = HelperPreset;
                    break;
                }
            }
        }

        TSubclassOf<APawn> EnemyClass = Config->EnemyPresetRegistry->ResolveEnemyClassFromPreset(SpawnPreset);
        if (!EnemyClass)
        {
            // 2차 폴백: 후보 프리셋 순차 재시도
            for (const FName Candidate : PresetCandidates)
            {
                if (Candidate == SpawnPreset) continue;
                EnemyClass = Config->EnemyPresetRegistry->ResolveEnemyClassFromPreset(Candidate);
                if (EnemyClass)
                {
                    SpawnPreset = Candidate;
                    break;
                }
            }
            if (!EnemyClass)
            {
                UE_LOG(LogTemp, Error, TEXT("[RaidCombat] ERROR: Failed to resolve EnemyClass for all preset candidates in Room %d."), Id);
                continue;
            }
        }

        float CapsuleRadius = 42.0f;
        float CapsuleHalfHeight = 88.0f;
        ResolvePawnCapsuleSize(EnemyClass, CapsuleRadius, CapsuleHalfHeight);

        float BaseAngle = i * AngleStep;
        float RandomAngle = BaseAngle + Stream.FRandRange(-25.0f, 25.0f);
        float MinSpawnDistance = 300.0f;
        float MaxSpawnDistance = SpawnRadius;
        if (BotProfile.Equals(TEXT("Defensive"), ESearchCase::IgnoreCase))
        {
            MinSpawnDistance = FMath::Max(300.0f, SpawnRadius * 0.55f);
        }
        else if (BotProfile.Equals(TEXT("Tactical"), ESearchCase::IgnoreCase))
        {
            MinSpawnDistance = FMath::Max(300.0f, SpawnRadius * 0.42f);
            MaxSpawnDistance = FMath::Max(MinSpawnDistance + 50.0f, SpawnRadius * 0.88f);
        }
        else if (BotProfile.Equals(TEXT("Aggressive"), ESearchCase::IgnoreCase))
        {
            MaxSpawnDistance = FMath::Max(350.0f, SpawnRadius * 0.70f);
        }

        if (SpawnPreset == TEXT("Sniper"))
        {
            MinSpawnDistance = FMath::Max(MinSpawnDistance, SpawnRadius * 0.60f);
        }

        MaxSpawnDistance = FMath::Max(MinSpawnDistance + 50.0f, MaxSpawnDistance);
        float Distance = Stream.FRandRange(MinSpawnDistance, MaxSpawnDistance);

        FVector FinalSpawnLoc = FVector::ZeroVector;
        bool bFoundSafeSpawn = false;
        constexpr int32 MaxSpawnLocationAttempts = 10;

        for (int32 Attempt = 0; Attempt < MaxSpawnLocationAttempts && !bFoundSafeSpawn; ++Attempt)
        {
            const float CandidateAngle = RandomAngle + ((Attempt == 0) ? 0.0f : Stream.FRandRange(-95.0f, 95.0f));
            const float CandidateDistance = (Attempt == 0) ? Distance : Stream.FRandRange(MinSpawnDistance, MaxSpawnDistance);
            const float Radian = FMath::DegreesToRadians(CandidateAngle);
            const FVector Offset(FMath::Cos(Radian) * CandidateDistance, FMath::Sin(Radian) * CandidateDistance, 0.0f);
            const FVector XYCandidate = Center + Offset;

            FVector CandidateSpawnLoc = FVector::ZeroVector;
            if (!TryResolveSafeAIPawnSpawnLocation(World, Room, NavSys, XYCandidate, CapsuleRadius, CapsuleHalfHeight, CandidateSpawnLoc))
            {
                continue;
            }

            FinalSpawnLoc = CandidateSpawnLoc;
            bFoundSafeSpawn = true;
        }

        if (!bFoundSafeSpawn)
        {
            FVector RecoveryLoc = FVector::ZeroVector;
            if (TryResolveNearbyFallbackSpawnLocation(World, Room, NavSys, Center, CapsuleRadius, CapsuleHalfHeight, Stream, RecoveryLoc))
            {
                FinalSpawnLoc = RecoveryLoc;
                bFoundSafeSpawn = true;
            }
        }

        if (!bFoundSafeSpawn)
        {
            continue;
        }

        FRotator RandomRotation(0.0f, Stream.FRandRange(0.0f, 360.0f), 0.0f);
        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;

        if (APawn* SpawnedEnemy = World->SpawnActor<APawn>(EnemyClass, FinalSpawnLoc, RandomRotation, SpawnParams))
        {
            FVector EffectiveSpawnLoc = SpawnedEnemy->GetActorLocation();
            if (IsCapsuleBlockedForPawn(World, EffectiveSpawnLoc, CapsuleRadius, CapsuleHalfHeight) ||
                IsNearRoomObstacle(World, Room, EffectiveSpawnLoc, CapsuleRadius + 70.0f))
            {
                FVector RecoveryLoc = FVector::ZeroVector;
                if (TryResolveNearbyFallbackSpawnLocation(World, Room, NavSys, EffectiveSpawnLoc, CapsuleRadius, CapsuleHalfHeight, Stream, RecoveryLoc))
                {
                    SpawnedEnemy->SetActorLocation(RecoveryLoc, false, nullptr, ETeleportType::TeleportPhysics);
                    EffectiveSpawnLoc = RecoveryLoc;
                }
                else
                {
                    UE_LOG(
                        LogTemp,
                        Warning,
                        TEXT("[RaidCombat] Rejected blocked spawn for '%s' in Room %d at %s"),
                        *GetNameSafe(EnemyClass),
                        Id,
                        *SpawnedEnemy->GetActorLocation().ToCompactString());
                    SpawnedEnemy->Destroy();
                    continue;
                }
            }

            FString RepairLog;
            const bool bRepaired = Config->EnemyPresetRegistry->TryRepairSpawnedPawn(SpawnedEnemy, RepairLog);
            if (bRepaired)
            {
                UE_LOG(LogTemp, Warning, TEXT("[RaidCombat] Auto-repaired spawned enemy '%s': %s"),
                    *GetNameSafe(SpawnedEnemy->GetClass()), *RepairLog);
            }

            // Some AI blueprints/plugins override collision right after spawn.
            // Re-apply trace collision immediately and with short delayed retries.
            ForceEnemyTraceCollision(SpawnedEnemy);

            SpawnedEnemy->SpawnDefaultController();
            ForceEnemyTraceCollision(SpawnedEnemy);
            LogEnemyTraceCollisionSnapshot(SpawnedEnemy);

            if (World)
            {
                TWeakObjectPtr<APawn> EnemyWeak = SpawnedEnemy;
                for (const float DelaySeconds : { 0.05f, 0.20f, 0.60f })
                {
                    FTimerHandle RetryHandle;
                    World->GetTimerManager().SetTimer(
                        RetryHandle,
                        FTimerDelegate::CreateWeakLambda(this, [EnemyWeak]()
                            {
                                if (APawn* EnemyStrong = EnemyWeak.Get())
                                {
                                    ForceEnemyTraceCollision(EnemyStrong);
                                }
                            }),
                        DelaySeconds,
                        false);
                }
            }

            FString SanitizedProfile = BotProfile;
            SanitizedProfile.ReplaceInline(TEXT(" "), TEXT(""));
            SpawnedEnemy->Tags.AddUnique(FName(*FString::Printf(TEXT("RaidRoom_%d"), Id)));
            SpawnedEnemy->Tags.AddUnique(FName(TEXT("Enemy")));
            SpawnedEnemy->Tags.AddUnique(FName(TEXT("RaidEnemy")));
            if (!SanitizedProfile.IsEmpty())
            {
                SpawnedEnemy->Tags.AddUnique(FName(*FString::Printf(TEXT("BotProfile_%s"), *SanitizedProfile)));
            }
            if (AController* SpawnedController = SpawnedEnemy->GetController())
            {
                SpawnedController->Tags.AddUnique(FName(*FString::Printf(TEXT("RaidRoom_%d"), Id)));
                SpawnedController->Tags.AddUnique(FName(TEXT("Enemy")));
                SpawnedController->Tags.AddUnique(FName(TEXT("RaidEnemy")));
                if (!SanitizedProfile.IsEmpty())
                {
                    SpawnedController->Tags.AddUnique(FName(*FString::Printf(TEXT("BotProfile_%s"), *SanitizedProfile)));
                }
            }

            SpawnedCount++;
            EnemyToRoomMap.Add(SpawnedEnemy, Id);
            SpawnedEnemy->OnDestroyed.AddDynamic(this, &URaidCombatSubsystem::OnEnemyDestroyed);
        }
    }

    if (SpawnedCount > 0)
    {
        AliveByRoomId.Add(Id, SpawnedCount);
        UE_LOG(LogTemp, Warning, TEXT("[RaidCombat] %d Enemies spawned successfully in Room %d"), SpawnedCount, Id);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("[RaidCombat] 0 enemies spawned in Room %d! Auto-clearing."), Id);
        HandleRoomCleared(Id);
    }
}

void URaidCombatSubsystem::OnEnemyDestroyed(AActor* DestroyedActor)
{
    if (bInternalClearing) return;

    if (int32* RoomIdPtr = EnemyToRoomMap.Find(DestroyedActor))
    {
        int32 RId = *RoomIdPtr;
        EnemyToRoomMap.Remove(DestroyedActor);

        if (int32* AlivePtr = AliveByRoomId.Find(RId))
        {
            (*AlivePtr)--;
            if (*AlivePtr <= 0)
            {
                AliveByRoomId.Remove(RId);
                HandleRoomCleared(RId);
            }
        }
    }
}

void URaidCombatSubsystem::HandleRoomCleared(int32 RoomId)
{
    if (TObjectPtr<ARaidRoomActor>* RoomPtr = RoomById.Find(RoomId))
    {
        if (ARaidRoomActor* Room = RoomPtr->Get())
        {
            bInternalClearing = true;
            Room->SetCombatCleared(true);
            bInternalClearing = false;

            if (Room->GetNodeRow().RoomType.Equals(TEXT("Boss"), ESearchCase::IgnoreCase))
            {
                Room->InternalSpawnLoot();
            }

            UpdateCompassForNextRooms(Room);
        }
    }
}

void URaidCombatSubsystem::AddPOI(const FVector& Loc, FName Type)
{
    for (const FRaidPOI& Existing : ActivePOIs)
    {
        if (Existing.Location.Equals(Loc, 5.0f))
        {
            return;
        }
    }

    FRaidPOI NewPOI;
    NewPOI.Location = Loc;
    NewPOI.MarkerType = Type;
    ActivePOIs.Add(NewPOI);
}

void URaidCombatSubsystem::ClearPOIs()
{
    ActivePOIs.Empty();
    CurrentObjectiveRoomId = -1;
    CurrentObjectiveLocation = FVector::ZeroVector;
    LastDistanceToObjective = TNumericLimits<float>::Max();
}

void URaidCombatSubsystem::UpdateCompassForNextRooms(ARaidRoomActor* ClearedRoom)
{
    if (RoomById.Num() == 0)
    {
        ClearPOIs();
        return;
    }

    ClearPOIs();
    APawn* PlayerPawn = GetPrimaryPlayerPawn();
    if (PlayerPawn)
    {
        RefreshStartRoomProgressState(PlayerPawn);
    }

    ARaidRoomActor* StartRoom = FindStartRoom();
    if (StartRoom)
    {
        StartRoomId = StartRoom->GetNodeId();
    }

    bool bHasPendingBoss = false;
    for (const TPair<int32, TObjectPtr<ARaidRoomActor>>& Pair : RoomById)
    {
        ARaidRoomActor* Room = Pair.Value.Get();
        if (!Room) continue;
        if (Room->GetNodeRow().RoomType.Equals(TEXT("Boss"), ESearchCase::IgnoreCase) && !Room->IsCleared())
        {
            bHasPendingBoss = true;
            break;
        }
    }

    const int32 PrimaryRoomId = ResolvePrimaryProgressionRoomId(StartRoom);
    ARaidRoomActor* PrimaryRoom = nullptr;
    if (PrimaryRoomId != -1)
    {
        if (TObjectPtr<ARaidRoomActor>* PrimaryPtr = RoomById.Find(PrimaryRoomId))
        {
            PrimaryRoom = PrimaryPtr->Get();
        }
    }

    // Start room flow:
    // - Spawned outside Start: objective must be Start until entered.
    // - Spawned inside Start: objective starts from primary room (ID 1 if valid).
    if (StartRoom && !StartRoom->IsCleared())
    {
        if (!bPlayerSpawnedInsideStartRoom)
        {
            ForceObjectiveToRoom(StartRoom, TEXT("Start"));
            return;
        }

        if (PrimaryRoom && !PrimaryRoom->IsCleared())
        {
            AddPOI(PrimaryRoom->GetActorLocation(), FName(*PrimaryRoom->GetNodeRow().RoomType));
            AddNearbyOptionalPOIsFromStart(StartRoom, PrimaryRoomId);
            ForceObjectiveToRoom(PrimaryRoom);
            return;
        }

        ForceObjectiveToRoom(StartRoom, TEXT("Start"));
        return;
    }

    // Even if optional rooms were cleared first, keep pulling objective back to primary room
    // until that primary progression room is cleared.
    if (PrimaryRoom && !PrimaryRoom->IsCleared())
    {
        AddPOI(PrimaryRoom->GetActorLocation(), FName(*PrimaryRoom->GetNodeRow().RoomType));
        AddNearbyOptionalPOIsFromStart(StartRoom, PrimaryRoomId);
        ForceObjectiveToRoom(PrimaryRoom);
        return;
    }

    if (ClearedRoom)
    {
        const TArray<int32> ConnectedIds = ClearedRoom->GetNodeRow().GetConnectionIds();
        for (int32 NextId : ConnectedIds)
        {
            if (TObjectPtr<ARaidRoomActor>* NextPtr = RoomById.Find(NextId))
            {
                if (ARaidRoomActor* NextRoom = NextPtr->Get())
                {
                    if (NextRoom->IsCleared()) continue;
                    FName RoomType = FName(*NextRoom->GetNodeRow().RoomType);
                    AddPOI(NextRoom->GetActorLocation(), RoomType);
                }
            }
        }

        int32 Neighbors[4] = { ClearedRoom->NeighborNorth, ClearedRoom->NeighborSouth, ClearedRoom->NeighborEast, ClearedRoom->NeighborWest };
        for (int32 NextId : Neighbors)
        {
            if (NextId == -1) continue;
            if (TObjectPtr<ARaidRoomActor>* NextPtr = RoomById.Find(NextId))
            {
                if (ARaidRoomActor* NextRoom = NextPtr->Get())
                {
                    if (NextRoom->IsCleared()) continue;
                    bool bAlreadyExists = false;
                    for (const FRaidPOI& ExistingPOI : ActivePOIs)
                    {
                        if (ExistingPOI.Location == NextRoom->GetActorLocation())
                        {
                            bAlreadyExists = true;
                            break;
                        }
                    }

                    if (!bAlreadyExists)
                    {
                        FName RoomType = FName(*NextRoom->GetNodeRow().RoomType);
                        AddPOI(NextRoom->GetActorLocation(), RoomType);
                    }
                }
            }
        }
    }
    else
    {
        // 초기 생성 직후(아직 클리어된 방이 없는 상태)에도 목표 마커를 세팅한다.
        for (const TPair<int32, TObjectPtr<ARaidRoomActor>>& Pair : RoomById)
        {
            ARaidRoomActor* Room = Pair.Value.Get();
            if (!Room || Room->IsCleared()) continue;

            const FString RoomType = Room->GetNodeRow().RoomType;
            if (RoomType.Equals(TEXT("Start"), ESearchCase::IgnoreCase)) continue;
            if (bHasPendingBoss && RoomType.Equals(TEXT("Exit"), ESearchCase::IgnoreCase)) continue;

            AddPOI(Room->GetActorLocation(), FName(*RoomType));
        }
    }

    int32 BestRoomId = -1;
    int32 BestScore = TNumericLimits<int32>::Min();
    FVector BestLocation = FVector::ZeroVector;

    for (const FRaidPOI& POI : ActivePOIs)
    {
        int32 CandidateId = -1;
        FString CandidateType = POI.MarkerType.ToString();

        for (const TPair<int32, TObjectPtr<ARaidRoomActor>>& Pair : RoomById)
        {
            if (!Pair.Value) continue;
            if (Pair.Value->GetActorLocation().Equals(POI.Location, 1.0f))
            {
                CandidateId = Pair.Key;
                CandidateType = Pair.Value->GetNodeRow().RoomType;
                break;
            }
        }

        int32 Score = GetRoomTypePriority(CandidateType);
        if (bHasPendingBoss && CandidateType.Equals(TEXT("Exit"), ESearchCase::IgnoreCase))
        {
            Score -= 200;
        }
        if (Score > BestScore)
        {
            BestScore = Score;
            BestRoomId = CandidateId;
            BestLocation = POI.Location;
        }
    }

    CurrentObjectiveRoomId = BestRoomId;
    CurrentObjectiveLocation = BestLocation;
    if (CurrentObjectiveRoomId == -1 || CurrentObjectiveLocation.IsNearlyZero())
    {
        // Fallback: 연결 데이터가 비정상이거나 비어도 가장 우선순위 높은 방 하나를 목표로 잡는다.
        int32 FallbackScore = TNumericLimits<int32>::Min();
        for (const TPair<int32, TObjectPtr<ARaidRoomActor>>& Pair : RoomById)
        {
            ARaidRoomActor* Room = Pair.Value.Get();
            if (!Room) continue;
            if (ClearedRoom && Pair.Key == ClearedRoom->GetNodeId()) continue;

            const FString RoomType = Room->GetNodeRow().RoomType;
            int32 Score = GetRoomTypePriority(RoomType);
            if (bHasPendingBoss && RoomType.Equals(TEXT("Exit"), ESearchCase::IgnoreCase))
            {
                Score -= 200;
            }
            if (Score > FallbackScore)
            {
                FallbackScore = Score;
                CurrentObjectiveRoomId = Pair.Key;
                CurrentObjectiveLocation = Room->GetActorLocation();
            }
        }

        if (CurrentObjectiveRoomId != -1)
        {
            AddPOI(CurrentObjectiveLocation, TEXT("Fallback"));
            UE_LOG(LogTemp, Warning, TEXT("[RaidCombat] Compass fallback objective selected: Room %d"), CurrentObjectiveRoomId);
        }
    }

    LastProgressTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
    LastDistanceToObjective = TNumericLimits<float>::Max();
    WrongDirectionScore = 0.0f;
}

FRaidGuidanceSignal URaidCombatSubsystem::GetGuidanceSignalForPlayer(APawn* PlayerPawn)
{
    FRaidGuidanceSignal Signal;
    if (!PlayerPawn)
    {
        return Signal;
    }

    ReevaluateObjectiveByPlayer(PlayerPawn);
    if (CurrentObjectiveLocation.IsNearlyZero())
    {
        return Signal;
    }

    UWorld* World = GetWorld();
    if (!World) return Signal;

    const float Now = World->GetTimeSeconds();
    const FVector PlayerLoc = PlayerPawn->GetActorLocation();
    const float CurrentDist = FVector::Dist2D(PlayerLoc, CurrentObjectiveLocation);

    if (LastProgressTimeSeconds <= 0.0f)
    {
        LastProgressTimeSeconds = Now;
        LastDistanceToObjective = CurrentDist;
    }

    const float DistDelta = LastDistanceToObjective - CurrentDist;
    if (DistDelta > 120.0f)
    {
        LastProgressTimeSeconds = Now;
        WrongDirectionScore = FMath::Max(0.0f, WrongDirectionScore - 0.3f);
    }
    else if (DistDelta < -60.0f)
    {
        WrongDirectionScore = FMath::Min(1.5f, WrongDirectionScore + 0.2f);
    }

    LastDistanceToObjective = CurrentDist;

    const float StuckSeconds = FMath::Max(0.0f, Now - LastProgressTimeSeconds);
    const float GentleAlpha = FMath::Clamp(StuckSeconds / FMath::Max(1.0f, GentleNudgeDelay), 0.0f, 1.0f);
    const float StrongAlpha = FMath::Clamp((StuckSeconds - GentleNudgeDelay) / FMath::Max(1.0f, StrongNudgeDelay - GentleNudgeDelay), 0.0f, 1.0f);
    const float WrongAlpha = FMath::Clamp(WrongDirectionScore, 0.0f, 1.0f);

    Signal.bValid = true;
    Signal.TargetLocation = CurrentObjectiveLocation;
    Signal.Urgency = FMath::Clamp(FMath::Max3(GentleAlpha * 0.75f, StrongAlpha, WrongAlpha), 0.0f, 1.0f);
    Signal.bUseStrongCue = (Signal.Urgency >= 0.75f);

    if (Signal.Urgency < 0.35f) Signal.CueStyle = TEXT("Subtle");
    else if (Signal.Urgency < 0.75f) Signal.CueStyle = TEXT("Pulse");
    else Signal.CueStyle = TEXT("StrongPulse");

    return Signal;
}

void URaidCombatSubsystem::ReevaluateObjectiveByPlayer(APawn* PlayerPawn)
{
    if (!PlayerPawn) return;

    RefreshStartRoomProgressState(PlayerPawn);

    ARaidRoomActor* StartRoom = FindStartRoom();
    const int32 PrimaryRoomId = ResolvePrimaryProgressionRoomId(StartRoom);
    ARaidRoomActor* PrimaryRoom = nullptr;
    if (PrimaryRoomId != -1)
    {
        if (TObjectPtr<ARaidRoomActor>* PrimaryPtr = RoomById.Find(PrimaryRoomId))
        {
            PrimaryRoom = PrimaryPtr->Get();
        }
    }

    if (StartRoom && !StartRoom->IsCleared())
    {
        if (!bPlayerSpawnedInsideStartRoom)
        {
            const bool bHasStartPOI = ActivePOIs.ContainsByPredicate(
                [StartRoom](const FRaidPOI& POI)
                {
                    return POI.Location.Equals(StartRoom->GetActorLocation(), 5.0f);
                });
            if (CurrentObjectiveRoomId != StartRoom->GetNodeId() || !bHasStartPOI)
            {
                UpdateCompassForNextRooms(nullptr);
            }
            CurrentObjectiveRoomId = StartRoom->GetNodeId();
            CurrentObjectiveLocation = StartRoom->GetActorLocation();
            return;
        }

        if (PrimaryRoom && !PrimaryRoom->IsCleared())
        {
            const bool bHasPrimaryPOI = ActivePOIs.ContainsByPredicate(
                [PrimaryRoom](const FRaidPOI& POI)
                {
                    return POI.Location.Equals(PrimaryRoom->GetActorLocation(), 5.0f);
                });
            if (CurrentObjectiveRoomId != PrimaryRoom->GetNodeId() || !bHasPrimaryPOI)
            {
                UpdateCompassForNextRooms(nullptr);
            }
            CurrentObjectiveRoomId = PrimaryRoom->GetNodeId();
            CurrentObjectiveLocation = PrimaryRoom->GetActorLocation();
            return;
        }
    }

    if (PrimaryRoom && !PrimaryRoom->IsCleared())
    {
        const bool bHasPrimaryPOI = ActivePOIs.ContainsByPredicate(
            [PrimaryRoom](const FRaidPOI& POI)
            {
                return POI.Location.Equals(PrimaryRoom->GetActorLocation(), 5.0f);
            });
        if (CurrentObjectiveRoomId != PrimaryRoom->GetNodeId() || !bHasPrimaryPOI)
        {
            UpdateCompassForNextRooms(nullptr);
        }
        CurrentObjectiveRoomId = PrimaryRoom->GetNodeId();
        CurrentObjectiveLocation = PrimaryRoom->GetActorLocation();
        return;
    }

    bool bHasPendingBoss = false;
    for (const TPair<int32, TObjectPtr<ARaidRoomActor>>& Pair : RoomById)
    {
        ARaidRoomActor* Room = Pair.Value.Get();
        if (!Room) continue;
        if (Room->GetNodeRow().RoomType.Equals(TEXT("Boss"), ESearchCase::IgnoreCase) && !Room->IsCleared())
        {
            bHasPendingBoss = true;
            break;
        }
    }

    const FVector PlayerLoc = PlayerPawn->GetActorLocation();
    int32 BestRoomId = -1;
    FVector BestLocation = FVector::ZeroVector;
    float BestUtility = -TNumericLimits<float>::Max();

    for (const TPair<int32, TObjectPtr<ARaidRoomActor>>& Pair : RoomById)
    {
        ARaidRoomActor* Room = Pair.Value.Get();
        if (!Room || Room->IsCleared()) continue;

        const FString RoomType = Room->GetNodeRow().RoomType;
        if (RoomType.Equals(TEXT("Start"), ESearchCase::IgnoreCase)) continue;
        if (bHasPendingBoss && RoomType.Equals(TEXT("Exit"), ESearchCase::IgnoreCase)) continue;

        const float Utility = EvaluateObjectiveUtility(Room, PlayerLoc, bHasPendingBoss);
        if (Utility > BestUtility)
        {
            BestUtility = Utility;
            BestRoomId = Pair.Key;
            BestLocation = Room->GetActorLocation();
        }
    }

    if (BestRoomId != -1)
    {
        bool bShouldSwitch = (CurrentObjectiveRoomId != BestRoomId);
        if (CurrentObjectiveRoomId != -1 && bShouldSwitch && ObjectiveSwitchHysteresis > 0.0f)
        {
            if (TObjectPtr<ARaidRoomActor>* CurrentRoomPtr = RoomById.Find(CurrentObjectiveRoomId))
            {
                if (ARaidRoomActor* CurrentRoom = CurrentRoomPtr->Get())
                {
                    const float CurrentUtility = EvaluateObjectiveUtility(CurrentRoom, PlayerLoc, bHasPendingBoss);
                    if ((BestUtility - CurrentUtility) < ObjectiveSwitchHysteresis)
                    {
                        bShouldSwitch = false;
                    }
                }
            }
        }

        if (bShouldSwitch)
        {
            LastProgressTimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
            LastDistanceToObjective = TNumericLimits<float>::Max();
            WrongDirectionScore = 0.0f;
            CurrentObjectiveRoomId = BestRoomId;
            CurrentObjectiveLocation = BestLocation;
        }
    }
}

float URaidCombatSubsystem::EvaluateObjectiveUtility(const ARaidRoomActor* Room, const FVector& PlayerLoc, bool bHasPendingBoss) const
{
    if (!Room) return -TNumericLimits<float>::Max();

    const FLevelNodeRow& Row = Room->GetNodeRow();
    const FString RoomType = Row.RoomType;

    // 1) Proximity utility
    const float DistUU = FVector::Dist2D(PlayerLoc, Room->GetActorLocation());
    const float DistNorm = FMath::Clamp(DistUU / 120000.0f, 0.0f, 1.0f);
    const float ProximityUtility = 1.0f - DistNorm;

    // 2) Value utility (boss/loot/combat)
    float ValueUtility = 0.25f;
    if (RoomType.Equals(TEXT("Boss"), ESearchCase::IgnoreCase)) ValueUtility = 1.0f;
    else if (RoomType.Equals(TEXT("Loot"), ESearchCase::IgnoreCase)) ValueUtility = 0.85f;
    else if (RoomType.Equals(TEXT("Combat"), ESearchCase::IgnoreCase) || RoomType.Equals(TEXT("Normal"), ESearchCase::IgnoreCase)) ValueUtility = 0.65f;
    else if (RoomType.Equals(TEXT("Exit"), ESearchCase::IgnoreCase)) ValueUtility = bHasPendingBoss ? 0.0f : 0.55f;

    // 3) Safety utility (inverse of risk)
    const float RawRisk = Row.Difficulty * 0.45f + Row.CombatWeight * 0.35f + (float)FMath::Max(0, Row.SpawnCount) * 0.07f;
    const float RiskNorm = FMath::Clamp(RawRisk / 3.0f, 0.0f, 1.0f);
    const float SafetyUtility = 1.0f - RiskNorm;

    const float Utility =
        (ProximityUtility * ObjectiveProximityWeight * 1000.0f) +
        (ValueUtility * ObjectiveValueWeight * 900.0f) +
        (SafetyUtility * ObjectiveSafetyWeight * 700.0f);

    return Utility;
}

float URaidCombatSubsystem::GetRoomUtility(const ARaidRoomActor* Room, const FVector& PlayerLoc, bool bHasPendingBoss) const
{
    return EvaluateObjectiveUtility(Room, PlayerLoc, bHasPendingBoss);
}
