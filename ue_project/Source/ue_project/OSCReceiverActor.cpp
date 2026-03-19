#include "OSCReceiverActor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"

AOSCReceiverActor::AOSCReceiverActor()
{
	PrimaryActorTick.bCanEverTick = true;

	// Instantiating the OSC Server component
	OSCServer = CreateDefaultSubobject<UOSCServer>(TEXT("OSCServer"));
}

void AOSCReceiverActor::BeginPlay()
{
	Super::BeginPlay();

	// Generating a unique filename with a timestamp to prevent overwriting previous session data
	FString FileName = FString::Printf(TEXT("TelemetryLog_%s.csv"), *FDateTime::Now().ToString());
	OutputFilePath = FPaths::ProjectSavedDir() / TEXT("TelemetryLogs") / FileName;
	
	// Initialize the CSV headers
	TelemetryBuffer.Empty();
	TelemetryBuffer.Add(TEXT("GameTime(s),F1,F2,F3,F4"));

	if (OSCServer)
	{
		// Binding to 0.0.0.0 to bypass macOS localhost loopback restrictions
		OSCServer->SetAddress(TEXT("0.0.0.0"), 8000);
		OSCServer->OnOscMessageReceived.AddDynamic(this, &AOSCReceiverActor::CatchAudioData);
		OSCServer->Listen();

		UE_LOG(LogTemp, Warning, TEXT("Telemetry Bridge Online: Listening on UDP 8000"));
	}
}

void AOSCReceiverActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (OSCServer)
	{
		// Explicitly tearing down the socket to prevent segmentation faults during Actor destruction
		OSCServer->Stop();
		OSCServer->OnOscMessageReceived.RemoveDynamic(this, &AOSCReceiverActor::CatchAudioData);
		
		UE_LOG(LogTemp, Warning, TEXT("Telemetry Bridge Offline: Socket safely closed."));
	}

	// Flush the RAM buffer to the SSD in a single operation to minimize disk I/O latency
	if (TelemetryBuffer.Num() > 1) 
	{
		FFileHelper::SaveStringArrayToFile(TelemetryBuffer, *OutputFilePath);
		UE_LOG(LogTemp, Warning, TEXT("Session data safely logged to: %s"), *OutputFilePath);
	}

	Super::EndPlay(EndPlayReason);
}

void AOSCReceiverActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
}

void AOSCReceiverActor::CatchAudioData(const FOSCMessage& Message, const FString& IPAddress, int32 Port)
{
	TArray<float> PayloadValues;
	UOSCManager::GetAllFloats(Message, PayloadValues);

	// We now expect 4 values from the AI, not 2
	if (PayloadValues.Num() >= 4)
	{
		float F1 = PayloadValues[0];
		float F2 = PayloadValues[1];
		float F3 = PayloadValues[2];
		float F4 = PayloadValues[3];

		// Record the new AI data to our RAM buffer
		float CurrentTime = GetWorld()->GetTimeSeconds();
		FString DataRow = FString::Printf(TEXT("%.3f,%.3f,%.3f,%.3f,%.3f"), CurrentTime, F1, F2, F3, F4);
		TelemetryBuffer.Add(DataRow);

		// Real-time viewport debug verification
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Purple, FString::Printf(TEXT("AI Features: %.2f | %.2f | %.2f | %.2f"), F1, F2, F3, F4));
		}
	}
}