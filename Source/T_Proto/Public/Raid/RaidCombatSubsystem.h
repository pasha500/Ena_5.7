#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "RaidCombatSubsystem.generated.h"

class AActor;
class APawn;
class ARaidRoomActor;

USTRUCT(BlueprintType)
struct FRaidPOI
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, Category = "Raid|Compass")
    FVector Location = FVector::ZeroVector;

    UPROPERTY(BlueprintReadWrite, Category = "Raid|Compass")
    FName MarkerType = TEXT("Default");
};

USTRUCT(BlueprintType)
struct FRaidGuidanceSignal
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadWrite, Category = "Raid|Guidance")
    bool bValid = false;

    UPROPERTY(BlueprintReadWrite, Category = "Raid|Guidance")
    FVector TargetLocation = FVector::ZeroVector;

    UPROPERTY(BlueprintReadWrite, Category = "Raid|Guidance")
    float Urgency = 0.0f;

    UPROPERTY(BlueprintReadWrite, Category = "Raid|Guidance")
    bool bUseStrongCue = false;

    UPROPERTY(BlueprintReadWrite, Category = "Raid|Guidance")
    FName CueStyle = TEXT("Subtle");
};

UCLASS()
class T_PROTO_API URaidCombatSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category = "Raid|Room")
    void RegisterRoom(ARaidRoomActor* Room);

    // Backward-compatible entry point used by existing BPs/scripts.
    UFUNCTION(BlueprintCallable, Category = "Raid|Room")
    void RegisterRoomAsPOI(ARaidRoomActor* InRoom);

    UFUNCTION(BlueprintCallable, Category = "Raid|Room")
    void ResetSubsystem();

    UFUNCTION(BlueprintCallable, Category = "Raid|Combat")
    void StartCombatForRoom(ARaidRoomActor* Room);

    // Optional compatibility hooks for external systems.
    UFUNCTION(BlueprintCallable, Category = "Raid|Combat")
    void OnEnemySpawned(APawn* Enemy, int32 RoomId);

    UFUNCTION(BlueprintCallable, Category = "Raid|Combat")
    void OnEnemyKilled(APawn* Enemy);

    UFUNCTION(BlueprintPure, Category = "Raid|Compass")
    const TArray<FRaidPOI>& GetActivePOIs() const { return ActivePOIs; }

    UFUNCTION(BlueprintCallable, Category = "Raid|Compass")
    void AddPOI(const FVector& Loc, FName Type);

    UFUNCTION(BlueprintCallable, Category = "Raid|Compass")
    void ClearPOIs();

    UFUNCTION(BlueprintCallable, Category = "Raid|Compass")
    void UpdateCompassForNextRooms(ARaidRoomActor* ClearedRoom);

    UFUNCTION(BlueprintCallable, Category = "Raid|Guidance")
    FRaidGuidanceSignal GetGuidanceSignalForPlayer(APawn* PlayerPawn);

    UFUNCTION(BlueprintPure, Category = "Raid|Guidance")
    int32 GetCurrentObjectiveRoomId() const { return CurrentObjectiveRoomId; }

    UFUNCTION(BlueprintPure, Category = "Raid|Guidance")
    FVector GetCurrentObjectiveLocation() const { return CurrentObjectiveLocation; }

    UFUNCTION(BlueprintPure, Category = "Raid|Guidance")
    float GetRoomUtility(const ARaidRoomActor* Room, const FVector& PlayerLoc, bool bHasPendingBoss) const;

private:
    void SpawnEnemiesForRoom(ARaidRoomActor* Room);
    void SanitizeProceduralFoliageCollisionForTraces();
    void StartTraceCollisionEnforcer();
    void StopTraceCollisionEnforcer();
    void EnforceTraceCollisionOnAllAIPawns();

    UFUNCTION()
    void OnEnemyDestroyed(AActor* DestroyedActor);

    void HandleRoomCleared(int32 RoomId);
    APawn* GetPrimaryPlayerPawn() const;
    ARaidRoomActor* FindStartRoom() const;
    bool IsPawnInsideRoomBounds2D(const APawn* Pawn, const ARaidRoomActor* Room) const;
    int32 ResolvePrimaryProgressionRoomId(const ARaidRoomActor* StartRoom) const;
    void RefreshStartRoomProgressState(APawn* PlayerPawn);
    void ForceObjectiveToRoom(ARaidRoomActor* Room, FName MarkerType = NAME_None);
    void AddNearbyOptionalPOIsFromStart(const ARaidRoomActor* StartRoom, int32 PrimaryRoomId);
    void ReevaluateObjectiveByPlayer(APawn* PlayerPawn);
    float EvaluateObjectiveUtility(const ARaidRoomActor* Room, const FVector& PlayerLoc, bool bHasPendingBoss) const;

private:
    UPROPERTY()
    TMap<int32, TObjectPtr<ARaidRoomActor>> RoomById;

    UPROPERTY()
    TMap<int32, int32> AliveByRoomId;

    UPROPERTY()
    TMap<AActor*, int32> EnemyToRoomMap;

    UPROPERTY()
    TArray<FRaidPOI> ActivePOIs;

    UPROPERTY()
    bool bInternalClearing = false;

    UPROPERTY()
    int32 CurrentObjectiveRoomId = -1;

    UPROPERTY()
    FVector CurrentObjectiveLocation = FVector::ZeroVector;

    UPROPERTY()
    float LastProgressTimeSeconds = 0.0f;

    UPROPERTY()
    float LastDistanceToObjective = TNumericLimits<float>::Max();

    UPROPERTY()
    float WrongDirectionScore = 0.0f;

    UPROPERTY()
    bool bFoliageTraceCollisionSanitized = false;

    UPROPERTY()
    int32 StartRoomId = -1;

    UPROPERTY()
    bool bStartFlowInitialized = false;

    UPROPERTY()
    bool bPlayerSpawnedInsideStartRoom = false;

    UPROPERTY()
    bool bStartPendingClearOnExit = false;

    FTimerHandle TraceCollisionEnforcerHandle;

    UPROPERTY(EditAnywhere, Category = "Raid|Guidance", meta = (ClampMin = "1.0", ClampMax = "120.0"))
    float GentleNudgeDelay = 10.0f;

    UPROPERTY(EditAnywhere, Category = "Raid|Guidance", meta = (ClampMin = "2.0", ClampMax = "180.0"))
    float StrongNudgeDelay = 22.0f;

    UPROPERTY(EditAnywhere, Category = "Raid|Guidance", meta = (ClampMin = "0.0", ClampMax = "300.0"))
    float ObjectiveSwitchHysteresis = 45.0f;

    UPROPERTY(EditAnywhere, Category = "Raid|Guidance", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float ObjectiveProximityWeight = 1.0f;

    UPROPERTY(EditAnywhere, Category = "Raid|Guidance", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float ObjectiveValueWeight = 1.0f;

    UPROPERTY(EditAnywhere, Category = "Raid|Guidance", meta = (ClampMin = "0.0", ClampMax = "2.0"))
    float ObjectiveSafetyWeight = 1.0f;

    UPROPERTY(EditAnywhere, Category = "Raid|Compass", meta = (ClampMin = "1000.0", ClampMax = "100000.0"))
    float StartOptionalPOIRadius = 18000.0f;

    UPROPERTY(EditAnywhere, Category = "Raid|Compass", meta = (ClampMin = "0.0", ClampMax = "1000.0"))
    float RoomInsideCheckPadding = 150.0f;

    UPROPERTY(EditAnywhere, Category = "Raid|Combat", meta = (ClampMin = "1", ClampMax = "64"))
    int32 MaxEnemiesPerRoom = 6;
};
