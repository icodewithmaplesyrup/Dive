// CaveSystemGenerator.cpp
//
// See CaveSystemGenerator.h for porting notes (units, noise substitution,
// module dependency).

#include "CaveSystemGenerator.h"
#include "Camera/CameraActor.h"
#include "Kismet/GameplayStatics.h"
#include "DrawDebugHelpers.h"

DEFINE_LOG_CATEGORY(LogCaveGen);

ACaveSystemGenerator::ACaveSystemGenerator()
{
	PrimaryActorTick.bCanEverTick = false;

	CaveRoot = CreateDefaultSubobject<USceneComponent>(TEXT("CaveRoot"));
	RootComponent = CaveRoot;
}

void ACaveSystemGenerator::BeginPlay()
{
	Super::BeginPlay();
	GenerateCaves();
}

void ACaveSystemGenerator::GenerateCaves()
{
	const double StartTime = FPlatformTime::Seconds();

	const int32 SeedToUse = bUseRandomSeed ? FMath::Rand() : RandomSeed;
	Rng.Initialize(SeedToUse);

	DebugLog(FString::Printf(TEXT("=== GenerateCaves() start (seed: %d) ==="), SeedToUse));

	Paths.Empty();
	ExitPoints.Empty();
	ClearGeneratedComponents();

	for (int32 I = 0; I < NumPrimaryTunnels; ++I)
	{
		FVector StartPos = FVector::ZeroVector;
		if (I > 0)
		{
			// X/Y = horizontal spread, Z = vertical spread (cm).
			StartPos = FVector(
				Rng.FRandRange(-600.0f, 600.0f),
				Rng.FRandRange(-600.0f, 600.0f),
				Rng.FRandRange(-300.0f, 100.0f));
		}

		// Keep tunnels roughly horizontal-biased; tweak the Z range for verticality.
		const FVector StartDir = FVector(
			Rng.FRandRange(-1.0f, 1.0f),
			Rng.FRandRange(-1.0f, 1.0f),
			Rng.FRandRange(-0.25f, 0.25f)).GetSafeNormal();

		DebugLog(FString::Printf(TEXT("Starting primary tunnel %d at %s, dir %s"), I, *StartPos.ToString(), *StartDir.ToString()));
		GrowTunnel(StartPos, StartDir, Rng.RandRange(MinSegments, MaxSegments), MaxBranchesPerTree, 0, I);
	}

	DebugLog(FString::Printf(TEXT("Walk phase complete: %d total paths, %d exit points"), Paths.Num(), ExitPoints.Num()));
	for (int32 I = 0; I < Paths.Num(); ++I)
	{
		const FTunnelPath& P = Paths[I];
		if (P.Points.Num() == 0)
		{
			DebugLog(FString::Printf(TEXT("  [WARNING] path %d has zero points"), I));
		}
		else
		{
			DebugLog(FString::Printf(TEXT("  path %d: %d points, starts %s, ends %s"),
				I, P.Points.Num(), *P.Points[0].ToString(), *P.Points.Last().ToString()));
		}
	}

	BuildAllMeshes();

	if (bDebugDrawPathPoints)
	{
		DebugDrawPathPoints();
	}

	if (bDebugAutofitCamera)
	{
		DebugAutofitCamera();
	}

	const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;
	DebugLog(FString::Printf(TEXT("=== GenerateCaves() complete in %.1f ms ==="), ElapsedMs));
}

// Only destroys components THIS actor generated (tracked directly in
// GeneratedMeshComponents), so anything else attached to the actor - a
// camera, a light, hand-placed geometry - is never touched.
void ACaveSystemGenerator::ClearGeneratedComponents()
{
	int32 Removed = 0;
	for (UProceduralMeshComponent* Comp : GeneratedMeshComponents)
	{
		if (IsValid(Comp))
		{
			Comp->DestroyComponent();
			++Removed;
		}
	}
	GeneratedMeshComponents.Empty();

	// Debug path-point spheres use the engine's global persistent debug-line
	// system rather than tracked components (see DebugDrawPathPoints), so
	// clear those here too. Note this flushes ALL persistent debug lines in
	// the world, not just this actor's - fine for a standalone debug tool,
	// worth knowing if something else in your project relies on persistent
	// debug draws.
	if (UWorld* World = GetWorld())
	{
		FlushPersistentDebugLines(World);
	}

	if (Removed > 0)
	{
		DebugLog(FString::Printf(TEXT("Cleared %d previously generated mesh components"), Removed));
	}
}

// Recursively walks a tunnel path, occasionally spawning branch tunnels.
// BranchesLeft is intentionally passed by value (matching the original
// GDScript's semantics): it decrements across sibling branch spawns within
// one call's segment loop, but a child subtree's own decrements never
// propagate back up to the parent. It's a soft, best-effort cap on branch
// count per tree, not a hard global one.
void ACaveSystemGenerator::GrowTunnel(const FVector& StartPos, const FVector& StartDir, int32 SegmentCount, int32 BranchesLeft, int32 Depth, int32 WalkIndex)
{
	if (Depth > MaxBranchDepth)
	{
		return;
	}

	FTunnelPath Path;
	FVector Pos = StartPos;
	FVector Dir = StartDir.GetSafeNormal();
	Path.Points.Add(Pos);
	Path.Radii.Add(Rng.FRandRange(MinRadius, MaxRadius));

	// Unique noise offset per walk so parallel tunnels don't curve identically.
	const FVector NoiseOffset(
		Rng.FRandRange(0.0f, 10000.0f),
		Rng.FRandRange(0.0f, 10000.0f),
		Rng.FRandRange(0.0f, 10000.0f));

	int32 BranchesSpawned = 0;

	for (int32 S = 0; S < SegmentCount; ++S)
	{
		const float T = static_cast<float>(S) * 0.12f;
		FVector Turn(
			SampleNoise(NoiseOffset.X + T, 0.0f, 0.0f),
			SampleNoise(0.0f, NoiseOffset.Y + T, 0.0f) * 0.5f, // dampen vertical steering
			SampleNoise(0.0f, 0.0f, NoiseOffset.Z + T));
		Turn *= TurnNoiseStrength;

		Dir = (Dir + Turn * 0.3f).GetSafeNormal();
		Pos += Dir * SegmentLength;

		float Radius = Rng.FRandRange(MinRadius, MaxRadius);
		if (Rng.FRand() < ChamberChance)
		{
			Radius *= ChamberRadiusMultiplier;
			BlendChamberIntoNeighbors(Path, Radius);
		}

		Path.Points.Add(Pos);
		Path.Radii.Add(Radius);

		// Chance to spawn a branch tunnel from this point.
		if (BranchesLeft > 0 && S > SegmentCount * BranchMinParentProgress && Rng.FRand() < BranchChancePerSegment)
		{
			--BranchesLeft;
			++BranchesSpawned;

			const float YawAngle = Rng.FRandRange(-PI * 0.6f, PI * 0.6f);
			FVector BranchDir = FQuat(FVector::UpVector, YawAngle).RotateVector(Dir);

			FVector SideAxis = FVector::CrossProduct(Dir, FVector::UpVector);
			if (SideAxis.Size() > 0.001f)
			{
				SideAxis.Normalize();
				const float PitchAngle = Rng.FRandRange(-0.4f, 0.4f);
				BranchDir = FQuat(SideAxis, PitchAngle).RotateVector(BranchDir);
			}

			GrowTunnel(Pos, BranchDir, static_cast<int32>(SegmentCount * 0.6f), BranchesLeft, Depth + 1, WalkIndex);
		}
	}

	if (BranchesSpawned > 0)
	{
		DebugLog(FString::Printf(TEXT("  walk %d (depth %d) spawned %d branches"), WalkIndex, Depth, BranchesSpawned));
	}

	ExitPoints.Add(Pos);
	Paths.Add(Path);
}

// Softens a sudden chamber radius spike into the last few points so the
// transition reads as a widening cavern instead of a jarring balloon.
void ACaveSystemGenerator::BlendChamberIntoNeighbors(FTunnelPath& Path, float ChamberRadius)
{
	const int32 Count = Path.Radii.Num();
	const int32 LoopCount = FMath::Min(ChamberFalloffSegments, Count);
	for (int32 I = 0; I < LoopCount; ++I)
	{
		const int32 Idx = Count - 1 - I;
		const float Blend = 1.0f - (static_cast<float>(I) / ChamberFalloffSegments);
		Path.Radii[Idx] = FMath::Lerp(Path.Radii[Idx], ChamberRadius, Blend * 0.5f);
	}
}

void ACaveSystemGenerator::BuildAllMeshes()
{
	int32 Built = 0;
	int32 Skipped = 0;
	for (int32 I = 0; I < Paths.Num(); ++I)
	{
		if (Paths[I].Points.Num() < 2)
		{
			++Skipped;
			continue;
		}
		BuildTunnelMesh(Paths[I], I);
		++Built;
	}
	DebugLog(FString::Printf(TEXT("Mesh phase complete: %d meshes built, %d paths skipped (too few points)"), Built, Skipped));
}

// Extrudes a path into a tube mesh using a parallel-transport frame (carries
// the "up" vector forward from ring to ring) so the tube doesn't twist as it
// turns. Per-vertex normals point from the wall back toward the tube's
// centerline, since players are inside the tunnel looking at the walls, not
// outside looking at a worm - if a tunnel renders inside-out, negate the
// normals below and swap the winding order of the two triangles per quad.
// UVs are generated per-vertex (U around the circumference, V along the
// tube's length) for texturing/triplanar shaders on the cave walls.
void ACaveSystemGenerator::BuildTunnelMesh(const FTunnelPath& Path, int32 PathIndex)
{
	if (Path.Points.Num() < 2)
	{
		return;
	}

	TArray<TArray<FVector>> Rings;
	TArray<FVector> RingAxisDirs;
	Rings.Reserve(Path.Points.Num());
	RingAxisDirs.Reserve(Path.Points.Num());

	FVector Up = FVector::UpVector;

	for (int32 I = 0; I < Path.Points.Num(); ++I)
	{
		const FVector& Pos = Path.Points[I];
		FVector Tangent;
		if (I == 0)
		{
			Tangent = (Path.Points[I + 1] - Pos).GetSafeNormal();
		}
		else if (I == Path.Points.Num() - 1)
		{
			Tangent = (Pos - Path.Points[I - 1]).GetSafeNormal();
		}
		else
		{
			Tangent = (Path.Points[I + 1] - Path.Points[I - 1]).GetSafeNormal();
		}

		FVector ReferenceUp = Up;
		if (FMath::Abs(FVector::DotProduct(Tangent, ReferenceUp)) > 0.99f)
		{
			ReferenceUp = FVector::RightVector;
		}
		const FVector Right = FVector::CrossProduct(Tangent, ReferenceUp).GetSafeNormal();
		const FVector NewUp = FVector::CrossProduct(Right, Tangent).GetSafeNormal();
		Up = NewUp; // carried forward - this is the parallel-transport step

		TArray<FVector> RingVerts;
		RingVerts.Reserve(RingSides);
		for (int32 S = 0; S < RingSides; ++S)
		{
			const float Angle = (static_cast<float>(S) / RingSides) * (2.0f * PI);
			const FVector Offset = (Right * FMath::Cos(Angle) + Up * FMath::Sin(Angle)) * Path.Radii[I];
			RingVerts.Add(Pos + Offset);
		}
		Rings.Add(MoveTemp(RingVerts));
		RingAxisDirs.Add(Tangent);
	}

	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FProcMeshTangent> Tangents;
	TArray<FLinearColor> VertexColors;

	auto AddVert = [&](const FVector& P, const FVector& Center, const FVector& AxisDir, float U, float V)
	{
		Vertices.Add(P);
		const FVector Normal = (Center - P).GetSafeNormal();
		Normals.Add(Normal);
		UVs.Add(FVector2D(U, V));
		Tangents.Add(FProcMeshTangent(FVector::CrossProduct(Normal, AxisDir).GetSafeNormal(), false));
		Triangles.Add(Vertices.Num() - 1);
	};

	int32 VertexCount = 0;
	for (int32 I = 0; I < Rings.Num() - 1; ++I)
	{
		const TArray<FVector>& RingA = Rings[I];
		const TArray<FVector>& RingB = Rings[I + 1];
		const float V0 = static_cast<float>(I) / (Rings.Num() - 1);
		const float V1 = static_cast<float>(I + 1) / (Rings.Num() - 1);
		const FVector& CenterA = Path.Points[I];
		const FVector& CenterB = Path.Points[I + 1];
		const FVector& AxisA = RingAxisDirs[I];
		const FVector& AxisB = RingAxisDirs[I + 1];

		for (int32 S = 0; S < RingSides; ++S)
		{
			const int32 SNext = (S + 1) % RingSides;
			const float U0 = static_cast<float>(S) / RingSides;
			const float U1 = static_cast<float>(SNext) / RingSides;

			const FVector& A0 = RingA[S];
			const FVector& A1 = RingA[SNext];
			const FVector& B0 = RingB[S];
			const FVector& B1 = RingB[SNext];

			AddVert(A0, CenterA, AxisA, U0, V0);
			AddVert(A1, CenterA, AxisA, U1, V0);
			AddVert(B0, CenterB, AxisB, U0, V1);

			AddVert(A1, CenterA, AxisA, U1, V0);
			AddVert(B1, CenterB, AxisB, U1, V1);
			AddVert(B0, CenterB, AxisB, U0, V1);

			VertexCount += 6;
		}
	}

	VertexColors.Init(FLinearColor::White, Vertices.Num());

	DebugLog(FString::Printf(TEXT("  built tunnel mesh: %d rings, %d verts"), Rings.Num(), VertexCount));

	UProceduralMeshComponent* ProcMesh = NewObject<UProceduralMeshComponent>(
		this, *FString::Printf(TEXT("TunnelMesh_%d"), PathIndex));
	ProcMesh->SetupAttachment(GetRootComponent());
	ProcMesh->RegisterComponent();
	AddInstanceComponent(ProcMesh);

	ProcMesh->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, UVs, VertexColors, Tangents, bGenerateCollision);

	if (TunnelMaterial)
	{
		ProcMesh->SetMaterial(0, TunnelMaterial);
	}

	if (bGenerateCollision)
	{
		ProcMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		ProcMesh->SetCollisionObjectType(ECC_WorldStatic);
	}

	GeneratedMeshComponents.Add(ProcMesh);
}

// Draws a debug sphere at every generated path point - useful for a single
// debug run to visually confirm the walk shape even if the tube mesh itself
// isn't rendering correctly yet. Expensive with large tunnel counts; leave
// bDebugDrawPathPoints off once the mesh is confirmed working.
void ACaveSystemGenerator::DebugDrawPathPoints()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	int32 Count = 0;
	const FTransform& ActorXform = GetActorTransform();
	for (const FTunnelPath& Path : Paths)
	{
		for (const FVector& Point : Path.Points)
		{
			DrawDebugSphere(World, ActorXform.TransformPosition(Point), 15.0f, 8, FColor::Yellow, true);
			++Count;
		}
	}

	DebugLog(FString::Printf(TEXT("Spawned %d debug path point markers"), Count));
}

// Moves the first ACameraActor found in the level to frame the full
// generated cave network, computed from the actual bounding box of every
// path point. Handy while iterating - turn this off once you're placing
// your camera deliberately (e.g. a player rig) instead of just trying to
// see what got generated.
void ACaveSystemGenerator::DebugAutofitCamera()
{
	if (Paths.Num() == 0 || Paths[0].Points.Num() == 0)
	{
		DebugLog(TEXT("[WARNING] Autofit skipped - no paths generated."));
		return;
	}

	FBox Bounds(Paths[0].Points[0], Paths[0].Points[0]);
	for (const FTunnelPath& Path : Paths)
	{
		for (const FVector& Pt : Path.Points)
		{
			Bounds += Pt;
		}
	}

	const FVector Center = Bounds.GetCenter();
	const float Extent = Bounds.GetSize().Size();

	UWorld* World = GetWorld();
	ACameraActor* Cam = World ? Cast<ACameraActor>(UGameplayStatics::GetActorOfClass(World, ACameraActor::StaticClass())) : nullptr;
	if (!Cam)
	{
		DebugLog(TEXT("[WARNING] No ACameraActor found in the level - can't autofit. ")
			TEXT("Place a CameraActor as a sibling (not a child) of the generator."));
		return;
	}

	const float Distance = FMath::Max(Extent * 0.75f, 1000.0f); // 1000cm floor
	const FVector WorldCenter = GetActorTransform().TransformPosition(Center);
	const FVector NewLocation = WorldCenter + FVector(Distance, 0.0f, Extent * 0.25f);

	Cam->SetActorLocation(NewLocation);
	Cam->SetActorRotation((WorldCenter - NewLocation).Rotation());

	DebugLog(FString::Printf(TEXT("Autofit camera: bounds center %s, extent %.1f, camera moved to %s"),
		*Center.ToString(), Extent, *NewLocation.ToString()));
}

void ACaveSystemGenerator::DebugLog(const FString& Message) const
{
	if (bDebugLogging)
	{
		UE_LOG(LogCaveGen, Log, TEXT("[CaveGen] %s"), *Message);
	}
}

float ACaveSystemGenerator::SampleNoise(float X, float Y, float Z) const
{
	return FMath::PerlinNoise3D(FVector(X, Y, Z) * NoiseFrequency);
}
