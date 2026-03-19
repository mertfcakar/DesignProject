#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OSCServer.h"
#include "OSCManager.h"
#include "OSCReceiverActor.generated.h"

UCLASS()
class UE_PROJECT_API AOSCReceiverActor : public AActor
{
	GENERATED_BODY()
	
public:	
	AOSCReceiverActor();

protected:
	virtual void BeginPlay() override;

	// Overriding EndPlay to ensure a graceful socket shutdown and 
	// prevent garbage collection race conditions on the network thread.
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:	
	virtual void Tick(float DeltaTime) override;

	// Background listener for Python DSP telemetry
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Telemetry")
	UOSCServer* OSCServer;

	// Callback bound to incoming UDP packets
	UFUNCTION()
	void CatchAudioData(const FOSCMessage& Message, const FString& IPAddress, int32 Port);

	// RAM buffer to hold high-frequency data before dumping to disk. 
	// This avoids severe I/O bottlenecks during runtime.
	TArray<FString> TelemetryBuffer;

	// Absolute path for the output CSV file
	FString OutputFilePath;
};