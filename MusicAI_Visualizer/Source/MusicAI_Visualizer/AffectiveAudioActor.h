#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AudioDevice.h"
#include "Sound/SoundSubmix.h"
#include "ISubmixBufferListener.h"
#include "Containers/Queue.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Templates/UniquePtr.h"
#include <atomic>
#include "NNE.h"
#include "NNERuntimeCPU.h"
#include "NNEModelData.h"
#include "Networking.h"
#include "AffectiveAudioActor.generated.h"

class FSocket;
class FUdpSocketReceiver;

class FAudioAnalyzerListener : public ISubmixBufferListener
{
public:
    FAudioAnalyzerListener(TQueue<TArray<float>, EQueueMode::Spsc>& InQueue, std::atomic<float>& InRMS)
        : OutQueue(InQueue), OutRMS(InRMS) {}

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

    const TArray<float>& GetAffectiveScores() const { return AestheticScores; }
    const TArray<float>& GetTagPredictions() const { return TagPredictions; }
    const TArray<FString>& GetTagNames() const { return TagNames; }

    // Python sidecar accessors (lighting code reads these for genre-aware scenes)
    bool   IsPythonSidecarActive() const;
    FString GetTopGenre() const;
    FString GetTopMood() const;
    FString GetTopInstrument() const;
    float   GetTopGenreConfidence() const;
    float   GetTopMoodConfidence() const;
    float   GetTopInstrumentConfidence() const;

    // Top-N lists from the Python sidecar.
    void GetTopGenres(TArray<FString>& OutNames, TArray<float>& OutPcts) const;
    void GetTopMoods(TArray<FString>& OutNames, TArray<float>& OutPcts) const;
    void GetTopInstruments(TArray<FString>& OutNames, TArray<float>& OutPcts) const;

    // Unified top-N accessors that prefer the sidecar (richer top-15 list) but FALL BACK
    // to the in-engine ONNX inference (SmoothedTagProbs filtered by tag prefix) when the
    // sidecar UDP isn't reaching us. The lighting code uses these so coloring works whether
    // or not the Python sidecar is connected. K = how many top tags to return.
    void GetTopGenresAny(TArray<FString>& OutNames, TArray<float>& OutPcts, int32 K = 15) const;
    void GetTopMoodsAny(TArray<FString>& OutNames, TArray<float>& OutPcts, int32 K = 15) const;
    void GetTopInstrumentsAny(TArray<FString>& OutNames, TArray<float>& OutPcts, int32 K = 15) const;

    UPROPERTY(EditAnywhere, Category = "AI | Sidecar")
    int32 PythonSidecarPort = 17777;

    float GetBassLevel() const { return BassLevel.load(); }
    float GetMidLevel() const { return MidLevel.load(); }
    float GetTrebleLevel() const { return TrebleLevel.load(); }
    double GetLastKickOnset() const { return LastKickOnset.load(); }
    double GetLastSnareOnset() const { return LastSnareOnset.load(); }
    double GetLastHiHatOnset() const { return LastHiHatOnset.load(); }

private:
    TSharedPtr<UE::NNE::IModelInstanceCPU> VGGishInstance;
    TSharedPtr<UE::NNE::IModelInstanceCPU> AestheticInstance;

    TArray<float> AI_AudioBuffer;
    TArray<float> VGGishEmbeddings;
    TArray<float> AestheticScores;
    TArray<float> TagPredictions;
    TArray<FString> TagNames;
    TArray<float> SmoothedTagProbs;
    TArray<float> LstmHiddenState;
    TArray<float> LstmCellState;
    bool bModelHasTagOutput = false;

    FString CsvFilePath;
    TUniquePtr<IFileHandle> CsvFileHandle;
    FCriticalSection CsvCS;
    double SessionStartTime = 0.0;

    std::atomic<float> BassLevel{ 0.0f };
    std::atomic<float> MidLevel{ 0.0f };
    std::atomic<float> TrebleLevel{ 0.0f };
    std::atomic<double> LastKickOnset{ 0.0 };
    std::atomic<double> LastSnareOnset{ 0.0 };
    std::atomic<double> LastHiHatOnset{ 0.0 };
    float PrevBassEnergy = 0.0f;
    float PrevMidEnergy = 0.0f;
    float PrevTrebleEnergy = 0.0f;
    double LastKickTime = 0.0;
    double LastSnareTime = 0.0;
    double LastHiHatTime = 0.0;

    TSharedPtr<FAudioAnalyzerListener, ESPMode::ThreadSafe> ListenerProxy;
    TQueue<TArray<float>, EQueueMode::Spsc> AudioFeatureQueue;
    std::atomic<float> CurrentRMS{ 0.0f };

    TArray<float> DownsampleAudio(const TArray<float>& InputAudio);
    void ProcessAIInference();
    TArray<float> ComputeMelSpectrogram(const TArray<float>& RawAudio);
    void LoadTagNames();

    void StartPythonSidecarReceiver();
    void StopPythonSidecarReceiver();
    void OnSidecarPacket(const FArrayReaderPtr& Data, const FIPv4Endpoint& Endpoint);

    FSocket* SidecarSocket = nullptr;
    FUdpSocketReceiver* SidecarReceiver = nullptr;

    mutable FCriticalSection SidecarStateCS;
    TArray<float> SidecarScores;
    FString  SidecarTopGenre;
    FString  SidecarTopMood;
    FString  SidecarTopInstrument;
    float    SidecarTopGenrePct = 0.0f;
    float    SidecarTopMoodPct = 0.0f;
    float    SidecarTopInstrumentPct = 0.0f;
    // Full top-3 lists (parallel arrays). Index 0 == Top1 (already mirrored above).
    TArray<FString> SidecarTopGenresList;
    TArray<float>   SidecarTopGenresPcts;
    TArray<FString> SidecarTopMoodsList;
    TArray<float>   SidecarTopMoodsPcts;
    TArray<FString> SidecarTopInstrumentsList;
    TArray<float>   SidecarTopInstrumentsPcts;
    double   SidecarLastRecvTime = 0.0;
};
