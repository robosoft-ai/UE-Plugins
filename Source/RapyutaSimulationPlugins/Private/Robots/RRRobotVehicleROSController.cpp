// Copyright 2020-2021 Rapyuta Robotics Co., Ltd.

#include "Robots/RRRobotVehicleROSController.h"

// UE
#include "Kismet/GameplayStatics.h"

// rclUE
#include "Msgs/ROS2JointStateMsg.h"
#include "Msgs/ROS2TwistMsg.h"
#include "ROS2Node.h"

// RapyutaSimulationPlugins
#include "Core/RRConversionUtils.h"
#include "Core/RRGeneralUtils.h"
#include "Robots/RobotVehicle.h"
#include "Sensors/RR2DLidarComponent.h"
#include "Tools/RRROS2OdomPublisher.h"
#include "Tools/RRROS2TFPublisher.h"

void ARRRobotVehicleROSController::InitRobotROS2Node(APawn* InPawn)
{
    if (nullptr == RobotROS2Node)
    {
        RobotROS2Node = GetWorld()->SpawnActor<AROS2Node>();
    }
    RobotROS2Node->AttachToActor(InPawn, FAttachmentTransformRules::KeepRelativeTransform);
    // GUID is to make sure the node name is unique, even for multiple Sims?
    RobotROS2Node->Name = URRGeneralUtils::GetNewROS2NodeName(InPawn->GetName());

    // Set robot's [ROS2Node] namespace from spawn parameters if existing
    UROS2Spawnable* rosSpawnParameters = InPawn->FindComponentByClass<UROS2Spawnable>();
    if (rosSpawnParameters)
    {
        RobotROS2Node->Namespace = rosSpawnParameters->GetNamespace();
    }
    else
    {
        RobotROS2Node->Namespace = CastChecked<ARobotVehicle>(InPawn)->RobotUniqueName;
    }
    RobotROS2Node->Init();
}

bool ARRRobotVehicleROSController::InitPublishers(APawn* InPawn)
{
    if (false == IsValid(RobotROS2Node))
    {
        return false;
    }

    // OdomPublisher (with TF)
    if (bPublishOdom)
    {
        if (nullptr == OdomPublisher)
        {
            OdomPublisher = NewObject<URRROS2OdomPublisher>(this);
            OdomPublisher->SetupUpdateCallback();
            OdomPublisher->bPublishOdomTf = bPublishOdomTf;
        }
        OdomPublisher->InitializeWithROS2(RobotROS2Node);
        OdomPublisher->RobotVehicle = CastChecked<ARobotVehicle>(InPawn);
    }
    return true;
}

void ARRRobotVehicleROSController::OnPossess(APawn* InPawn)
{
    Super::OnPossess(InPawn);

    // Track InPawn's initial pose
    InitialPosition = InPawn->GetActorLocation();
    InitialOrientation = InPawn->GetActorRotation();
    InitialOrientation.Yaw += 180.f;

    // Instantiate a ROS2 node for each possessed [InPawn]
    InitRobotROS2Node(InPawn);

    // Initialize Pawn's sensors (lidar, etc.)
    verify(CastChecked<ARobotVehicle>(InPawn)->InitSensors(RobotROS2Node));

    // Refresh TF, Odom publishers
    verify(InitPublishers(InPawn));

    SubscribeToMovementCommandTopic(CmdVelTopicName);
    SubscribeToJointsCommandTopic(JointsCmdTopicName);
}

void ARRRobotVehicleROSController::OnUnPossess()
{
    if (bPublishOdom && OdomPublisher)
    {
        OdomPublisher->RevokeUpdateCallback();
        OdomPublisher->RobotVehicle = nullptr;
    }
    Super::OnUnPossess();
}

// movement command topic
void ARRRobotVehicleROSController::SubscribeToMovementCommandTopic(const FString& InTopicName)
{
    if (InTopicName.IsEmpty())
    {
        UE_LOG(
            LogTemp,
            Warning,
            TEXT(
                "[%s] [RRRobotVehicleROSController] [SubscribeToMovementCommandTopic] TopicName is empty. Do not subscribe topic."),
            *GetName());
        return;
    }

    // Subscription with callback to enqueue vehicle spawn info.
    if (ensure(IsValid(RobotROS2Node)))
    {
        FSubscriptionCallback cb;
        cb.BindDynamic(this, &ARRRobotVehicleROSController::MovementCallback);
        RobotROS2Node->AddSubscription(InTopicName, UROS2TwistMsg::StaticClass(), cb);
    }
}

void ARRRobotVehicleROSController::MovementCallback(const UROS2GenericMsg* Msg)
{
    const UROS2TwistMsg* twistMsg = Cast<UROS2TwistMsg>(Msg);
    if (IsValid(twistMsg))
    {
        // TODO refactoring will be needed to put units and system of reference conversions in a consistent location
        // probably should not stay in msg though
        FROSTwist twist;
        twistMsg->GetMsg(twist);
        const FVector linear(ConversionUtils::VectorROSToUE(twist.linear));
        const FVector angular(ConversionUtils::RotationROSToUE(twist.angular));

        // (Note) In this callback, which could be invoked from a ROS working thread,
        // the ROSController itself (this) could have been garbage collected,
        // thus any direct referencing to its member in this GameThread lambda needs to be verified.
        AsyncTask(ENamedThreads::GameThread,
                  [linear, angular, vehicle = CastChecked<ARobotVehicle>(GetPawn())]
                  {
                      if (IsValid(vehicle))
                      {
                          vehicle->SetLinearVel(linear);
                          vehicle->SetAngularVel(angular);
                      }
                  });
    }
}

// joint state command
void ARRRobotVehicleROSController::SubscribeToJointsCommandTopic(const FString& InTopicName)
{
    if (InTopicName.IsEmpty())
    {
        UE_LOG(
            LogTemp,
            Warning,
            TEXT(
                "[%s] [RRRobotVehicleROSController] [SubscribeToMovementCommandTopic] TopicName is empty. Do not subscribe topic."),
            *GetName());
        return;
    }

    // Subscription with callback to enqueue vehicle spawn info.
    if (ensure(IsValid(RobotROS2Node)))
    {
        FSubscriptionCallback cb;
        cb.BindDynamic(this, &ARRRobotVehicleROSController::JointStateCallback);
        RobotROS2Node->AddSubscription(InTopicName, UROS2JointStateMsg::StaticClass(), cb);
    }
}

void ARRRobotVehicleROSController::JointStateCallback(const UROS2GenericMsg* Msg)
{
    const UROS2JointStateMsg* jointStateMsg = Cast<UROS2JointStateMsg>(Msg);
    if (IsValid(jointStateMsg))
    {
        // TODO refactoring will be needed to put units and system of reference conversions in a consistent location
        // probably should not stay in msg though
        FROSJointState jointState;
        jointStateMsg->GetMsg(jointState);

        auto vehicle = CastChecked<ARobotVehicle>(GetPawn());

        // Check Joint type. should be different function?
        EJointControlType jointControlType;
        if (jointState.name.Num() == jointState.position.Num())
        {
            jointControlType = EJointControlType::POSITION;
        }
        else if (jointState.name.Num() == jointState.velocity.Num())
        {
            jointControlType = EJointControlType::VELOCITY;
        }
        else if (jointState.name.Num() == jointState.effort.Num())
        {
            jointControlType = EJointControlType::EFFORT;
            UE_LOG(LogTemp,
                   Warning,
                   TEXT("[%s] [RRRobotVehicleROSController] [JointStateCallback] Effort control is not supported."),
                   *GetName());
            return;
        }
        else
        {
            UE_LOG(LogTemp,
                   Warning,
                   TEXT("[%s] [RRRobotVehicleROSController] [JointStateCallback] position, velocity or effort array must be same "
                        "size of name array"),
                   *GetName());
            return;
        }

        // Calculate input, ROS to UE conversion.
        TMap<FString, TArray<float>> joints;
        for (unsigned int i = 0; i < jointState.name.Num(); i++)
        {
            if (!vehicle->Joints.Contains(jointState.name[i]))
            {
                UE_LOG(LogTemp,
                       Warning,
                       TEXT("[%s] [RRRobotVehicleROSController] [JointStateCallback] vehicle do not have joint named %s."),
                       *GetName(),
                       *jointState.name[i]);
                continue;
            }

            TArray<float> input;
            if (jointControlType == EJointControlType::POSITION)
            {
                input.Add(jointState.position[i]);
            }
            else if (jointControlType == EJointControlType::VELOCITY)
            {
                input.Add(jointState.velocity[i]);
            }
            else
            {
                UE_LOG(
                    LogTemp,
                    Warning,
                    TEXT("[%s] [RRRobotVehicleROSController] [JointStateCallback] position, velocity or effort array must be same "
                         "size of name array"),
                    *GetName());
                continue;
            }

            // ROS To UE conversion
            if (vehicle->Joints[jointState.name[i]]->LinearDOF == 1)
            {
                input[0] *= 100;    // todo add conversion to conversion util
            }
            else if (vehicle->Joints[jointState.name[i]]->RotationalDOF == 1)
            {
                input[0] *= 180 / M_PI;    // todo add conversion to conversion util
            }
            else
            {
                UE_LOG(LogTemp,
                       Warning,
                       TEXT("[%s] [RRRobotVehicleROSController] [JointStateCallback] Supports only single DOF joint. %s has %d "
                            "linear DOF and %d rotational DOF"),
                       *jointState.name[i],
                       vehicle->Joints[jointState.name[i]]->LinearDOF,
                       vehicle->Joints[jointState.name[i]]->RotationalDOF);
            }

            joints.Emplace(jointState.name[i], input);
        }

        // (Note) In this callback, which could be invoked from a ROS working thread,
        // the ROSController itself (this) could have been garbage collected,
        // thus any direct referencing to its member in this GameThread lambda needs to be verified.
        AsyncTask(ENamedThreads::GameThread,
                  [joints, jointControlType, vehicle]
                  {
                      if (IsValid(vehicle))
                      {
                          vehicle->SetJointState(joints, jointControlType);
                      }
                  });
    }
}
