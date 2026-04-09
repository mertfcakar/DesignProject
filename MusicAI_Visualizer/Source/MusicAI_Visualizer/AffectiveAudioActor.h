#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AudioDevice.h"
#include "Sound/SoundSubmix.h"
#include "ISubmixBufferListener.h"
#include "Containers/Queue.h"
#include <atomic> // <--- CRITICAL: Added for std::atomic
#include "NNE.h"
#include "NNERuntimeCPU.h"
#include "NNEModelData.h"
#include "AffectiveAudioActor.generated.h"

class FAudioAnalyzerListener : public ISubmixBufferListener
{
public:
    FAudioAnalyzerListener(TQueue<TArray<float>, EQueueMode::Spsc>& InQueue, std::atomic<float>& InRMS)
        : OutQueue(InQueue), OutRMS(InRMS) {
    }

    virtual void OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData, int32 NumSamples, int32 NumChannels, const int32 SampleRate, double AudioClock) override;

private:
    TQueue<TArray<float>, EQueueMode::Spsc>& OutQueue;
    std::atomic<float>& OutRMS;
};

UCLASS()
class MUSICAI_VISUALIZER_API AAffectiveAudioActor : public AActor
{
    GENERATED_BODY()

public:
    AAffectiveAudioActor();

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
    virtual void Tick(float DeltaTime) override;

    UPROPERTY(EditAnywhere, Category = "Audio")
    TObjectPtr<USoundSubmix> SubmixToAnalyze;

    UPROPERTY(EditAnywhere, Category = "AI | Models")
    TObjectPtr<UNNEModelData> VGGishModelData;

    UPROPERTY(EditAnywhere, Category = "AI | Models")
    TObjectPtr<UNNEModelData> AestheticModelData;

private:
    TSharedPtr<UE::NNE::IModelInstanceCPU> VGGishInstance;
    TSharedPtr<UE::NNE::IModelInstanceCPU> AestheticInstance;

    TArray<float> AI_AudioBuffer;
    TArray<float> VGGishEmbeddings;
    TArray<float> AestheticScores;

    TSharedPtr<FAudioAnalyzerListener, ESPMode::ThreadSafe> ListenerProxy;
    TQueue<TArray<float>, EQueueMode::Spsc> AudioFeatureQueue;
    std::atomic<float> CurrentRMS{ 0.0f };

    TArray<float> DownsampleAudio(const TArray<float>& InputAudio);
    void ProcessAIInference();
    TArray<float> ComputeMelSpectrogram(const TArray<float>& RawAudio);
};