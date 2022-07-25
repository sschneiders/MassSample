﻿// Fill out your copyright notice in the Description page of Project Settings.


#include "RTSFormationSubsystem.h"

#include "MassAgentComponent.h"
#include "MassCommonFragments.h"
#include "MassEntitySubsystem.h"
#include "MassMovementFragments.h"
#include "MassNavigationFragments.h"
#include "MassSignalSubsystem.h"
#include "MassSpawnerSubsystem.h"
#include "RTSAgentTraits.h"
#include "RTSFormationProcessors.h"
#include "Kismet/GameplayStatics.h"

void URTSFormationSubsystem::DestroyEntity(UMassAgentComponent* Entity)
{
	UMassEntitySubsystem* EntitySubsystem = GetWorld()->GetSubsystem<UMassEntitySubsystem>();
	check(EntitySubsystem);
	
	EntitySubsystem->Defer().DestroyEntity(Entity->GetEntityHandle());
}

void URTSFormationSubsystem::UpdateUnitPosition(const FVector& NewPosition, int UnitIndex)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("UpdateUnitPosition"))
	if (!ensure(Units.IsValidIndex(UnitIndex))) { return; }

	// Convenience variables
	FUnitInfo& Unit = Units[UnitIndex];
	UMassEntitySubsystem* EntitySubsystem = UWorld::GetSubsystem<UMassEntitySubsystem>(GetWorld());
	TMap<int, FVector>& NewPositions = Unit.NewPositions;

	// Empty NewPositions Map to make room for new calculations
	NewPositions.Empty();
	NewPositions.Reserve(Unit.Entities.Num());

	// Calculate entity positions for new destination
	const FVector CenterOffset = FVector((Unit.Entities.Num()/Unit.FormationLength/2) * Unit.BufferDistance, (Unit.FormationLength/2) * Unit.BufferDistance, 0.f);
	for(int i=0;i<Unit.Entities.Num();++i)
	{
		const int w = i / Unit.FormationLength;
		const int l = i % Unit.FormationLength;
		
		FVector Position = FVector(w,l,0.f);
		Position *= Unit.BufferDistance;
		Position -= CenterOffset;
		Position = Position.RotateAngleAxis(Unit.Angle, FVector(0.f,0.f,Unit.TurnDirection));
		Position += NewPosition;
		NewPositions.Emplace(i, Position);
	}

	// The position to order entities/positions is based on the furthest destination location
	Unit.FarCorner = NewPosition;
	if (NewPositions.Num())
	{
		TArray<FVector> NewArray;
		NewArray.Reserve(NewPositions.Num());
		NewPositions.GenerateValueArray(NewArray);
		Unit.FarCorner = NewArray[0];
	}
	
	
	Unit.Entities.Sort([&EntitySubsystem, &Unit](const FMassEntityHandle& A, const FMassEntityHandle& B)
	{
		//@todo Find if theres a way to move this logic to a processor, most of the cost is coming from retrieving the location
		const FVector& LocA = EntitySubsystem->GetFragmentDataChecked<FTransformFragment>(A).GetTransform().GetLocation();
		const FVector& LocB = EntitySubsystem->GetFragmentDataChecked<FTransformFragment>(B).GetTransform().GetLocation();
		return FVector::DistSquared2D(LocA, Unit.FarCorner) > FVector::DistSquared2D(LocB, Unit.FarCorner);
	});
	
	NewPositions.ValueSort([&Unit](const FVector& A, const FVector& B)
	{
		return FVector::DistSquared2D(A, Unit.FarCorner) > FVector::DistSquared2D(B, Unit.FarCorner);
	});

	// Update the entities new index gradually using signals. This allows the bulk of processing to be spread out between frames
	FMassExecutionContext Context = EntitySubsystem->CreateExecutionContext(GetWorld()->GetDeltaSeconds());
	TArray<FMassEntityHandle> Entities = Unit.Entities.Array();
	for(int i=0;i<Unit.Entities.Num();++i)
	{
		GetWorld()->GetSubsystem<UMassSignalSubsystem>()->DelaySignalEntity(UpdateIndex, Entities[i], 0.01*(i/Unit.FormationLength));
	} 
	
}

void URTSFormationSubsystem::MoveEntities(int UnitIndex)
{
	FUnitInfo& Unit = Units[UnitIndex];
	UMassEntitySubsystem* EntitySubsystem = UWorld::GetSubsystem<UMassEntitySubsystem>(GetWorld());
	
	// Final sort to ensure that entities are signaled from front to back
	Unit.Entities.Sort([&EntitySubsystem, &Unit](const FMassEntityHandle& A, const FMassEntityHandle& B)
	{
		// Find if theres a way to move this logic to a processor, most of the cost is coming from retrieving the location
		const FVector& LocA = EntitySubsystem->GetFragmentDataChecked<FTransformFragment>(A).GetTransform().GetLocation();
		const FVector& LocB = EntitySubsystem->GetFragmentDataChecked<FTransformFragment>(B).GetTransform().GetLocation();
		return FVector::DistSquared2D(LocA, Unit.FarCorner) < FVector::DistSquared2D(LocB, Unit.FarCorner);
	});
	
	CurrentIndex = 0;

	// Signal entities to begin moving
	FMassExecutionContext Context = EntitySubsystem->CreateExecutionContext(GetWorld()->GetDeltaSeconds());
	TArray<FMassEntityHandle> Entities = Unit.Entities.Array();
	for(int i=0;i<Unit.Entities.Num();++i)
	{
		GetWorld()->GetSubsystem<UMassSignalSubsystem>()->DelaySignalEntity(FormationUpdated, Entities[i], 0.1*(i/Unit.FormationLength));
	} 
}

void URTSFormationSubsystem::SetUnitPosition(const FVector& NewPosition, int UnitIndex)
{
	DrawDebugDirectionalArrow(GetWorld(), NewPosition, NewPosition+((NewPosition-Units[UnitIndex].InterpolatedDestination).GetSafeNormal()*250.f), 150.f, FColor::Red, false, 5.f, 0, 25.f);

	FUnitInfo& Unit = Units[UnitIndex];
	// Calculate turn direction and angle for entities in unit
	Unit.TurnDirection = (NewPosition-Units[UnitIndex].InterpolatedDestination).GetSafeNormal().Y > 0 ? 1.f : -1.f;
	Unit.Angle = FMath::RadiansToDegrees(acosf(FVector::DotProduct((NewPosition-Units[UnitIndex].InterpolatedDestination).GetSafeNormal(),FVector::ForwardVector)));
	Unit.Angle += 180.f; // Resolve unit facing the wrong direction

	UMassEntitySubsystem* EntitySubsystem = GetWorld()->GetSubsystem<UMassEntitySubsystem>();
	for(const FMassEntityHandle& Entity : Unit.Entities)
	{
		EntitySubsystem->GetFragmentDataPtr<FMassMoveTargetFragment>(Entity)->CreateNewAction(EMassMovementAction::Stand, *GetWorld());
		EntitySubsystem->GetFragmentDataPtr<FMassVelocityFragment>(Entity)->Value = FVector::Zero();
	}
	
	Unit.UnitPosition = NewPosition;

	// Instantly set the angle since entity indexes will change
	Unit.InterpolatedAngle = Units[UnitIndex].Angle;
	
	UpdateUnitPosition(NewPosition, UnitIndex);
}

void URTSFormationSubsystem::SpawnEntitiesForUnit(int UnitIndex, const UMassEntityConfigAsset* EntityConfig, int Count)
{
	if (!ensure(Units.IsValidIndex(UnitIndex))) { return; }

	UMassEntitySubsystem* EntitySubsystem = GetWorld()->GetSubsystem<UMassEntitySubsystem>();

	// Reserve space for the new units, the space will be filled in a processor
	Units[UnitIndex].Entities.Reserve(Units[UnitIndex].Entities.Num()+Count);
	
	TArray<FMassEntityHandle> Entities;
	const FMassEntityTemplate* EntityTemplate = EntityConfig->GetConfig().GetOrCreateEntityTemplate(*UGameplayStatics::GetPlayerPawn(this, 0), *EntityConfig);

	// We are doing a little bit of work here since we are setting the unit index manually
	// Otherwise, using SpawnEntities would be perfectly fine
	// @todo find if there is a better way to modify templates in code
	TArray<FMassEntityHandle> SpawnedEntities;
	TSharedRef<UMassEntitySubsystem::FEntityCreationContext> CreationContext = EntitySubsystem->BatchCreateEntities(EntityTemplate->GetArchetype(), Count, SpawnedEntities);

	// Set the template default values for the entities
	TConstArrayView<FInstancedStruct> FragmentInstances = EntityTemplate->GetInitialFragmentValues();
	EntitySubsystem->BatchSetEntityFragmentsValues(CreationContext->GetChunkCollection(), FragmentInstances);

	// Set unit index for entities
	FRTSFormationAgent FormationAgent;
	FormationAgent.UnitIndex = UnitIndex;
	
	TArray<FInstancedStruct> Fragments;
	Fragments.Add(FConstStructView::Make(FormationAgent));
	EntitySubsystem->BatchSetEntityFragmentsValues(CreationContext->GetChunkCollection(), Fragments);
}

int URTSFormationSubsystem::SpawnNewUnit(const UMassEntityConfigAsset* EntityConfig, int Count, const FVector& Position)
{
	int UnitIndex = Units.Num();
	Units.AddDefaulted(1);
	Units[UnitIndex].UnitPosition = Position;
	
	SpawnEntitiesForUnit(UnitIndex, EntityConfig, Count);
	return UnitIndex;
}

void URTSFormationSubsystem::Tick(float DeltaTime)
{
	for(int i=0;i<Units.Num();++i)
	{
		FUnitInfo& Unit = Units[i];
		Unit.InterpolatedAngle = FMath::FInterpConstantTo(Unit.InterpolatedAngle, Unit.Angle, DeltaTime, 50.f);
		Unit.InterpolatedDestination = FMath::VInterpConstantTo(Unit.InterpolatedDestination, Unit.UnitPosition, DeltaTime, 150.f);
	}
}

bool URTSFormationSubsystem::IsTickable() const
{
	return true;
}

TStatId URTSFormationSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(URTSFormationSubsystem, STATGROUP_Tickables);
}

void URTSFormationSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	
}
