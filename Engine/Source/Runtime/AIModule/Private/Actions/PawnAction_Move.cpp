// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

#include "AIModulePrivate.h"
#include "Actions/PawnAction_Move.h"

UPawnAction_Move::UPawnAction_Move(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, GoalLocation(FAISystem::InvalidLocation)
	, AcceptableRadius(30.f)
	, bFinishOnOverlap(true)
	, bUsePathfinding(true)
	, bAllowPartialPath(true)
	, bProjectGoalToNavigation(false)
	, bUpdatePathToGoal(true)
	, bAbortChildActionOnPathChange(false)
{
	bShouldPauseMovement = true;
}

void UPawnAction_Move::BeginDestroy()
{
	ClearPath();
	Super::BeginDestroy();
}

UPawnAction_Move* UPawnAction_Move::CreateAction(UWorld& World, AActor* GoalActor, EPawnActionMoveMode::Type Mode)
{
	if (GoalActor == NULL)
	{
		return NULL;
	}

	UPawnAction_Move* Action = UPawnAction::CreateActionInstance<UPawnAction_Move>(World);
	if (Action)
	{
		Action->GoalActor = GoalActor;
		Action->bUsePathfinding = (Mode == EPawnActionMoveMode::UsePathfinding);
	}

	return Action;
}

UPawnAction_Move* UPawnAction_Move::CreateAction(UWorld& World, const FVector& GoalLocation, EPawnActionMoveMode::Type Mode)
{
	if (FAISystem::IsValidLocation(GoalLocation) == false)
	{
		return NULL;
	}

	UPawnAction_Move* Action = UPawnAction::CreateActionInstance<UPawnAction_Move>(World);
	if (Action)
	{
		Action->GoalLocation = GoalLocation;
		Action->bUsePathfinding = (Mode == EPawnActionMoveMode::UsePathfinding);
	}

	return Action;
}

bool UPawnAction_Move::Start()
{
	bool bResult = Super::Start();
	if (bResult)
	{
		bResult = PerformMoveAction();
	}

	return bResult;
}

EPathFollowingRequestResult::Type UPawnAction_Move::RequestMove(AAIController& Controller)
{
	EPathFollowingRequestResult::Type RequestResult = EPathFollowingRequestResult::Failed;
	
	FAIMoveRequest MoveReq;
	MoveReq.SetUsePathfinding(bUsePathfinding);
	MoveReq.SetAllowPartialPath(bAllowPartialPath);
	MoveReq.SetProjectGoalLocation(bProjectGoalToNavigation);
	MoveReq.SetNavigationFilter(FilterClass);
	MoveReq.SetAcceptanceRadius(AcceptableRadius);
	MoveReq.SetStopOnOverlap(bFinishOnOverlap);
	MoveReq.SetCanStrafe(bAllowStrafe);

	if (GoalActor != NULL)
	{
		const bool bAtGoal = CheckAlreadyAtGoal(Controller, *GoalActor, AcceptableRadius);
		if (bUpdatePathToGoal)
		{
			MoveReq.SetGoalActor(GoalActor);
		}
		else
		{
			MoveReq.SetGoalLocation(GoalActor->GetActorLocation());
		}

		RequestResult = bAtGoal ? EPathFollowingRequestResult::AlreadyAtGoal : Controller.MoveTo(MoveReq);
	}
	else if (FAISystem::IsValidLocation(GoalLocation))
	{
		const bool bAtGoal = CheckAlreadyAtGoal(Controller, GoalLocation, AcceptableRadius);
		MoveReq.SetGoalLocation(GoalLocation);
		
		RequestResult = bAtGoal ? EPathFollowingRequestResult::AlreadyAtGoal : Controller.MoveTo(MoveReq);
	}
	else
	{
		UE_VLOG(&Controller, LogPawnAction, Warning, TEXT("UPawnAction_Move::Start: no valid move goal set"));
	}

	return RequestResult;
}

bool UPawnAction_Move::PerformMoveAction()
{
	AAIController* MyController = Cast<AAIController>(GetController());
	if (MyController == NULL)
	{
		return false;
	}

	if (bUsePathfinding && MyController->ShouldPostponePathUpdates())
	{
		UE_VLOG(MyController, LogPawnAction, Log, TEXT("Can't path right now, waiting..."));
		MyController->GetWorldTimerManager().SetTimer(TimerHandle_DeferredPerformMoveAction, this, &UPawnAction_Move::DeferredPerformMoveAction, 0.1f);
		return true;
	}

	const EPathFollowingRequestResult::Type RequestResult = RequestMove(*MyController);
	bool bResult = true;

	if (RequestResult == EPathFollowingRequestResult::RequestSuccessful)
	{
		RequestID = MyController->GetCurrentMoveRequestID();
		WaitForMessage(UBrainComponent::AIMessage_MoveFinished, RequestID);
		WaitForMessage(UBrainComponent::AIMessage_RepathFailed);
	}
	else if (RequestResult == EPathFollowingRequestResult::AlreadyAtGoal)
	{
		// note that this will result in latently notifying actions component about finishing
		// so it's no problem we run it from withing Start function
		Finish(EPawnActionResult::Success);
	}
	else
	{
		bResult = false;
	}

	return bResult;
}

void UPawnAction_Move::DeferredPerformMoveAction()
{
	const bool bResult = PerformMoveAction();
	if (!bResult)
	{
		Finish(EPawnActionResult::Failed);
	}
}

bool UPawnAction_Move::Pause(const UPawnAction* PausedBy)
{
	bool bResult = Super::Pause(PausedBy);
	if (bResult)
	{
		AAIController* MyController = Cast<AAIController>(GetController());
		if (MyController)
		{
			bResult = MyController->PauseMove(RequestID);
		}
	}
	return bResult;
}

bool UPawnAction_Move::Resume()
{
	if (GoalActor != NULL && GoalActor->IsPendingKillPending())
	{
		return false;
	}

	bool bResult = Super::Resume();
	if (bResult)
	{
		bResult = false;
		AAIController* MyController = Cast<AAIController>(GetController());
		if (MyController)
		{
			if (MyController->ResumeMove(RequestID) == false)
			{
				// try requesting a new move
				UE_VLOG(MyController, LogPawnAction, Log, TEXT("Resume move failed, requesting a new one."));
				StopWaitingForMessages();
				
				bResult = PerformMoveAction();
			}
			else
			{
				bResult = true;
			}
		}
	}
	return bResult;
}

EPawnActionAbortState::Type UPawnAction_Move::PerformAbort(EAIForceParam::Type ShouldForce)
{
	ClearPath();

	AAIController* MyController = Cast<AAIController>(GetController());

	if (MyController && MyController->GetPathFollowingComponent())
	{
		MyController->GetPathFollowingComponent()->AbortMove(TEXT("BehaviorTree abort"), RequestID);
	}

	return Super::PerformAbort(ShouldForce);
}

void UPawnAction_Move::HandleAIMessage(UBrainComponent*, const FAIMessage& Message)
{
	if (Message.MessageName == UBrainComponent::AIMessage_MoveFinished && Message.HasFlag(EPathFollowingMessage::OtherRequest))
	{
		// move was aborted by another request from different action, don't finish yet
		return;
	}

	const bool bFail = Message.MessageName == UBrainComponent::AIMessage_RepathFailed
		|| Message.Status == FAIMessage::Failure;

	Finish(bFail ? EPawnActionResult::Failed : EPawnActionResult::Success);
}

void UPawnAction_Move::OnFinished(EPawnActionResult::Type WithResult)
{
	ClearPath();
	Super::OnFinished(WithResult);
}

void UPawnAction_Move::ClearPath()
{
	ClearPendingRepath();
	if (Path.IsValid())
	{
		Path->RemoveObserver(PathObserverDelegateHandle);
		Path = NULL;
	}
}

void UPawnAction_Move::SetPath(FNavPathSharedRef InPath)
{
	if (InPath != Path)
	{
		ClearPath();
		Path = InPath;
		PathObserverDelegateHandle = Path->AddObserver(FNavigationPath::FPathObserverDelegate::FDelegate::CreateUObject(this, &UPawnAction_Move::OnPathUpdated));

		// skip auto updates, it will be handled manually to include controller's ShouldPostponePathUpdates()
		Path->EnableRecalculationOnInvalidation(false);
	}
}

void UPawnAction_Move::OnPathUpdated(FNavigationPath* UpdatedPath, ENavPathEvent::Type Event)
{
	const AController* MyOwner = GetController();
	if (MyOwner == NULL)
	{
		return;
	}

	UE_VLOG(MyOwner, LogPawnAction, Log, TEXT("%s> Path updated!"), *GetName());
	
	if (bAbortChildActionOnPathChange && GetChildAction())
	{
		UE_VLOG(MyOwner, LogPawnAction, Log, TEXT(">> aborting child action: %s"), *GetNameSafe(GetChildAction()));
		
		GetOwnerComponent()->AbortAction(*GetChildAction());
	}

	if (Event == ENavPathEvent::Invalidated)
	{
		TryToRepath();
	}

	// log new path when action is paused, otherwise it will be logged by path following component's update
	if (Event == ENavPathEvent::UpdatedDueToGoalMoved || Event == ENavPathEvent::UpdatedDueToNavigationChanged)
	{
		if (IsPaused() && UpdatedPath)
		{
			UPathFollowingComponent::LogPathHelper(MyOwner, UpdatedPath, UpdatedPath->GetGoalActor());
		}
	}
}

void UPawnAction_Move::TryToRepath()
{
	if (Path.IsValid())
	{
		AAIController* MyController = Cast<AAIController>(GetController());
		if (MyController == NULL || !MyController->ShouldPostponePathUpdates())
		{
			ANavigationData* NavData = Path->GetNavigationDataUsed();
			if (NavData)
			{
				NavData->RequestRePath(Path, ENavPathUpdateType::NavigationChanged);
			}
		}
		else if (GetWorld())
		{
			GetWorld()->GetTimerManager().SetTimer(TimerHandle_TryToRepath, this, &UPawnAction_Move::TryToRepath, 0.25f);
		}
	}
}

void UPawnAction_Move::ClearPendingRepath()
{
	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(TimerHandle_TryToRepath);
	}
}

bool UPawnAction_Move::CheckAlreadyAtGoal(AAIController& Controller, const FVector& TestLocation, float Radius)
{
	const bool bAlreadyAtGoal = Controller.GetPathFollowingComponent()->HasReached(TestLocation, Radius);
	if (bAlreadyAtGoal)
	{
		Controller.GetPathFollowingComponent()->AbortMove(TEXT("Aborting move due to new move request finishing with AlreadyAtGoal"), FAIRequestID::AnyRequest, true, false, EPathFollowingMessage::OtherRequest);
		Controller.GetPathFollowingComponent()->SetLastMoveAtGoal(true);
	}

	return bAlreadyAtGoal;
}

bool UPawnAction_Move::CheckAlreadyAtGoal(AAIController& Controller, const AActor& TestGoal, float Radius)
{
	const bool bAlreadyAtGoal = Controller.GetPathFollowingComponent()->HasReached(TestGoal, Radius);
	if (bAlreadyAtGoal)
	{
		Controller.GetPathFollowingComponent()->AbortMove(TEXT("Aborting move due to new move request finishing with AlreadyAtGoal"), FAIRequestID::AnyRequest, true, false, EPathFollowingMessage::OtherRequest);
		Controller.GetPathFollowingComponent()->SetLastMoveAtGoal(true);
	}

	return bAlreadyAtGoal;
}
