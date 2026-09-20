// CaveSystemGenerator.h
//
// Ported from a Godot 4.x GDScript (CaveSystemGenerator_v3.gd) to Unreal
// Engine C++. Procedurally grows a branching network of organic worm-tunnels
// and extrudes each into a tube mesh with collision, using
// UProceduralMeshComponent.
//
// PORTING NOTES (read before using):
//
// 1. UNITS: Unreal's default world unit is 1 centimeter, while the original
//    Godot project was authored in meters. All distance-related defaults
//    below (SegmentLength, MinRadius, MaxRadius, the primary-tunnel spawn
//    spread, etc.) have already been multiplied by 100 so the generator
//    produces sensibly-sized tunnels out of the box. If you retune these,
//    remember you're working in centimeters, not meters.
//
// 2. NOISE: Godot's FastNoiseLite (simplex) has been replaced with
//    FMath::PerlinNoise3D, which is what ships in Core. It's qualitatively
//    similar (smooth continuous 3D noise) but not numerically identical, so
//    the same seed will NOT reproduce the same cave shape as the Godot
//    version. NoiseFrequency has been rescaled accordingly (see comment on
//    that property).
//
// 3. CLEANUP SAFETY: the Godot version's v2->v3 fix was to stop deleting
//    ALL child nodes (which could nuke a Camera3D/DirectionalLight3D placed
//    under the same node) and instead only delete name-tagged children. This
//    C++ port sidesteps that whole class of bug structurally: generated
//    components are tracked directly in GeneratedMeshComponents and only
//    those pointers are ever destroyed, regardless of what else might be
//    attached to this actor.
//
// 4. MODULE DEPENDENCY: this uses UProceduralMeshComponent, which lives in
//    the "ProceduralMeshComponent" module. Add it to your .Build.cs:
//
//        PublicDependencyModuleNames.AddRange(new string[] {
//            "Core", "CoreUObject", "Engine", "InputCore",
//            "ProceduralMeshComponent"
//        });
//
// 5. USAGE: place a BP or C++ instance of ACaveSystemGenerator in the level.
//    It generates on BeginPlay, and GenerateCaves() is also exposed as
//    CallInEditor so you can hit the button in the Details panel to
//    regenerate at edit time.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProceduralMeshComponent.h"
#include "CaveSystemGenerator.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogCaveGen, Log, All);

/** One tunnel walk: an ordered list of centerline points and their radii. */
struct FTunnelPath
{
	TArray<FVector> Points;
	TArray<float> Radii;
};

UCLASS()
class DIVE1_API ACaveSystemGenerator : public AActor
{
	GENERATED_BODY()

public:
	ACaveSystemGenerator();

	/** Clears and regenerates the full cave network. Also runs on BeginPlay. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Cave Generation")
	void GenerateCaves();

	/** End of every leaf tunnel - candidates for level exits / chamber placement. */
	UFUNCTION(BlueprintPure, Category = "Cave Generation")
	const TArray<FVector>& GetExitPoints() const { return ExitPoints; }

protected:
	virtual void BeginPlay() override;

public:
	// ---------------- Generation Settings ----------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generation Settings")
	bool bUseRandomSeed = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generation Settings")
	int32 RandomSeed = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generation Settings")
	int32 NumPrimaryTunnels = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generation Settings")
	int32 MinSegments = 40;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generation Settings")
	int32 MaxSegments = 70;

	/** In cm. Godot default was 3.0 (meters); this is that x100. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generation Settings")
	float SegmentLength = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generation Settings", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float TurnNoiseStrength = 0.6f;

	/** In cm. Godot default was 1.5 (meters); this is that x100. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generation Settings")
	float MinRadius = 150.0f;

	/** In cm. Godot default was 3.0 (meters); this is that x100. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generation Settings")
	float MaxRadius = 300.0f;

	/**
	 * Frequency multiplier applied to world-space coordinates before
	 * sampling Perlin noise. Godot's FastNoiseLite used 0.15 tuned for
	 * meter-scale steps; since this port works in centimeters, that value
	 * is rescaled by /100 here. Tune to taste.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Generation Settings")
	float NoiseFrequency = 0.0015f;

	// ---------------- Chambers ----------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chambers", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ChamberChance = 0.04f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chambers")
	float ChamberRadiusMultiplier = 2.5f;

	/** How many neighboring points blend toward the chamber size. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Chambers")
	int32 ChamberFalloffSegments = 3;

	// ---------------- Branching ----------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Branching", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BranchChancePerSegment = 0.02f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Branching")
	int32 MaxBranchesPerTree = 6;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Branching", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BranchMinParentProgress = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Branching")
	int32 MaxBranchDepth = 3;

	// ---------------- Mesh ----------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
	int32 RingSides = 10;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
	bool bGenerateCollision = true;

	/** Optional material applied to every generated tunnel mesh section. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Mesh")
	TObjectPtr<UMaterialInterface> TunnelMaterial = nullptr;

	// ---------------- Debug ----------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bDebugLogging = true;

	/** Draws a debug sphere at every path point. Expensive - one-off inspection only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bDebugDrawPathPoints = false;

	/** Moves the first ACameraActor found in the level to frame the generated geometry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bDebugAutofitCamera = false;

private:
	void ClearGeneratedComponents();

	void GrowTunnel(const FVector& StartPos, const FVector& StartDir, int32 SegmentCount, int32 BranchesLeft, int32 Depth, int32 WalkIndex);

	void BlendChamberIntoNeighbors(FTunnelPath& Path, float ChamberRadius);

	void BuildAllMeshes();

	void BuildTunnelMesh(const FTunnelPath& Path, int32 PathIndex);

	void DebugDrawPathPoints();

	void DebugAutofitCamera();

	void DebugLog(const FString& Message) const;

	float SampleNoise(float X, float Y, float Z) const;

	FRandomStream Rng;

	TArray<FTunnelPath> Paths;

	/** End of every leaf tunnel - see GetExitPoints(). */
	TArray<FVector> ExitPoints;

	/** Every UProceduralMeshComponent this actor has generated, tracked so
	 *  cleanup only ever destroys components we created ourselves. */
	UPROPERTY()
	TArray<TObjectPtr<UProceduralMeshComponent>> GeneratedMeshComponents;

	UPROPERTY()
	TObjectPtr<USceneComponent> CaveRoot = nullptr;
};
