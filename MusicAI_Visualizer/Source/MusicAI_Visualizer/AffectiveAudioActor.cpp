#include "AffectiveAudioActor.h"
#include "NiagaraComponent.h"
#include "Async/Async.h"
#include "NNE.h"
#include "NNERuntimeCPU.h"
#include "NNEModelData.h"

// CRITICAL: Standard C++ Math Libraries
#include <complex> 
#include <cmath>

// =========================================================================
// HELPER MATH & DSP FUNCTIONS
// =========================================================================

void ComputeFFT(TArray<std::complex<float>>& Buffer)
{
    int32 N = Buffer.Num();
    if (N <= 1) return;

    TArray<std::complex<float>> Even, Odd;
    Even.Reserve(N / 2); Odd.Reserve(N / 2);
    for (int32 i = 0; i < N; i++) {
        if (i % 2 == 0) Even.Add(Buffer[i]);
        else Odd.Add(Buffer[i]);
    }

    ComputeFFT(Even);
    ComputeFFT(Odd);

    for (int32 k = 0; k < N / 2; k++) {
        // PI is a UE macro, std::polar requires <complex>
        std::complex<float> t = std::polar(1.0f, -2.0f * (float)PI * k / N) * Odd[k];
        Buffer[k] = Even[k] + t;
        Buffer[k + N / 2] = Even[k] - t;
    }
}

float HzToMel(float Hz) { return 2595.0f * std::log10(1.0f + Hz / 700.0f); }
float MelToHz(float Mel) { return 700.0f * (std::pow(10.0f, Mel / 2595.0f) - 1.0f); }

// =========================================================================
// ACTOR CORE LOGIC
// =========================================================================

AAffectiveAudioActor::AAffectiveAudioActor()
{
    PrimaryActorTick.bCanEverTick = true;
    AI_AudioBuffer.Reserve(16000);
    VGGishEmbeddings.SetNumZeroed(128);
    AestheticScores.SetNumZeroed(5);
}

void AAffectiveAudioActor::BeginPlay()
{
    Super::BeginPlay();

    TWeakInterfacePtr<INNERuntimeCPU> Runtime = UE::NNE::GetRuntime<INNERuntimeCPU>(TEXT("NNERuntimeORTCpu"));

    if (!Runtime.IsValid()) {
        UE_LOG(LogTemp, Error, TEXT("AI ERROR: NNE Runtime not found! Check your plugins."));
        return;
    }

    if (VGGishModelData && AestheticModelData)
    {
        // 1. Setup VGGish
        TSharedPtr<UE::NNE::IModelCPU> VGGishModel = Runtime->CreateModelCPU(VGGishModelData);
        if (VGGishModel.IsValid()) {
            VGGishInstance = VGGishModel->CreateModelInstanceCPU();
            if (VGGishInstance.IsValid()) {
                // VGGish expects: 1 Batch, 96 Frames, 64 Mel Bins
                TArray<uint32> S; S.Add(1); S.Add(96); S.Add(64);
                TArray<UE::NNE::FTensorShape> Shapes;
                Shapes.Add(UE::NNE::FTensorShape::Make(S));

                auto Status = VGGishInstance->SetInputTensorShapes(Shapes);
                if (Status != UE::NNE::IModelInstanceCPU::ESetInputTensorShapesStatus::Ok) {
                    UE_LOG(LogTemp, Error, TEXT("AI ERROR: VGGish failed to set shapes!"));
                }
                else {
                    UE_LOG(LogTemp, Warning, TEXT("AI SUCCESS: VGGish shapes locked!"));
                }
            }
        }

        // 2. Setup Aesthetic Brain
        TSharedPtr<UE::NNE::IModelCPU> AestheticModel = Runtime->CreateModelCPU(AestheticModelData);
        if (AestheticModel.IsValid()) {
            AestheticInstance = AestheticModel->CreateModelInstanceCPU();
            if (AestheticInstance.IsValid()) {
                // Aesthetic Brain expects: 1 Batch, 128 Embeddings
                TArray<uint32> S; S.Add(1); S.Add(128);
                TArray<UE::NNE::FTensorShape> Shapes;
                Shapes.Add(UE::NNE::FTensorShape::Make(S));

                auto Status = AestheticInstance->SetInputTensorShapes(Shapes);
                if (Status != UE::NNE::IModelInstanceCPU::ESetInputTensorShapesStatus::Ok) {
                    UE_LOG(LogTemp, Error, TEXT("AI ERROR: Aesthetic failed to set shapes!"));
                }
                else {
                    UE_LOG(LogTemp, Warning, TEXT("AI SUCCESS: Aesthetic shapes locked!"));
                }
            }
        }
    }
    else {
        UE_LOG(LogTemp, Error, TEXT("AI ERROR: Model Data is NULL! Assign them in the Editor Details panel."));
    }

    // Audio listener setup remains same...
    if (SubmixToAnalyze) {
        if (FAudioDeviceHandle AudioDevice = GetWorld()->GetAudioDevice()) {
            ListenerProxy = MakeShared<FAudioAnalyzerListener, ESPMode::ThreadSafe>(AudioFeatureQueue, CurrentRMS);
            AudioDevice->RegisterSubmixBufferListener(ListenerProxy.Get(), SubmixToAnalyze);
        }
    }
}

void AAffectiveAudioActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    TArray<float> ReceivedData;
    while (AudioFeatureQueue.Dequeue(ReceivedData))
    {
        TArray<float> Downsampled = DownsampleAudio(ReceivedData);
        AI_AudioBuffer.Append(Downsampled);

        if (AI_AudioBuffer.Num() >= 16000)
        {
            ProcessAIInference();
            AI_AudioBuffer.RemoveAt(0, 8000);
        }
    }

    if (UNiagaraComponent* NiagaraComp = FindComponentByClass<UNiagaraComponent>())
    {
        static float InterpScores[5] = { 0,0,0,0,0 };

        const float Sensitivity = 40.0f;
        const float EnergyFloor = 3.0f;
        const float BassFloor = 1.5f;

        for (int i = 0; i < 3; i++) {
            float RawScore = FMath::Abs(AestheticScores[i]);
            float Floor = (i == 0) ? EnergyFloor : BassFloor;
            float CleanScore = FMath::Max(0.0f, (RawScore - Floor) * Sensitivity);
            InterpScores[i] = FMath::FInterpTo(InterpScores[i], CleanScore, DeltaTime, 8.0f);
        }

        NiagaraComp->SetNiagaraVariableFloat(TEXT("AI_Energy"), InterpScores[0]);
        NiagaraComp->SetNiagaraVariableFloat(TEXT("Bass"), InterpScores[1]);
        NiagaraComp->SetNiagaraVariableFloat(TEXT("Vibe"), InterpScores[2]);

        if (GEngine) {
            GEngine->AddOnScreenDebugMessage(1, 0.0f, FColor::Cyan,
                FString::Printf(TEXT("Energy: %.2f | Bass: %.2f"), InterpScores[0], InterpScores[1]));
        }
    }
}

void AAffectiveAudioActor::ProcessAIInference()
{
    AsyncTask(ENamedThreads::AnyBackgroundHiPriTask, [this]()
        {
            if (!VGGishInstance.IsValid() || !AestheticInstance.IsValid()) return;

            TArray<float> Spectrogram = ComputeMelSpectrogram(AI_AudioBuffer);

            TArray<UE::NNE::FTensorBindingCPU> InB, OutB;
            UE::NNE::FTensorBindingCPU BIn; BIn.Data = Spectrogram.GetData(); BIn.SizeInBytes = Spectrogram.Num() * sizeof(float); InB.Add(BIn);

            TArray<float> DummyData; DummyData.SetNumZeroed(10);
            while (InB.Num() < 3) {
                UE::NNE::FTensorBindingCPU DB; DB.Data = DummyData.GetData(); DB.SizeInBytes = DummyData.Num() * sizeof(float); InB.Add(DB);
            }

            UE::NNE::FTensorBindingCPU BOut; BOut.Data = VGGishEmbeddings.GetData(); BOut.SizeInBytes = VGGishEmbeddings.Num() * sizeof(float); OutB.Add(BOut);
            VGGishInstance->RunSync(InB, OutB);

            TArray<UE::NNE::FTensorBindingCPU> InB2, OutB2;
            UE::NNE::FTensorBindingCPU BIn2; BIn2.Data = VGGishEmbeddings.GetData(); BIn2.SizeInBytes = VGGishEmbeddings.Num() * sizeof(float); InB2.Add(BIn2);
            UE::NNE::FTensorBindingCPU BOut2; BOut2.Data = AestheticScores.GetData(); BOut2.SizeInBytes = AestheticScores.Num() * sizeof(float); OutB2.Add(BOut2);
            AestheticInstance->RunSync(InB2, OutB2);
        });
}

TArray<float> AAffectiveAudioActor::DownsampleAudio(const TArray<float>& InputAudio)
{
    TArray<float> Downsampled;
    Downsampled.Reserve(InputAudio.Num() / 3);
    for (int32 i = 0; i < InputAudio.Num(); i += 3) {
        float Sum = InputAudio[i];
        int32 Count = 1;
        if (i + 1 < InputAudio.Num()) { Sum += InputAudio[i + 1]; Count++; }
        if (i + 2 < InputAudio.Num()) { Sum += InputAudio[i + 2]; Count++; }
        Downsampled.Add(Sum / Count);
    }
    return Downsampled;
}

TArray<float> AAffectiveAudioActor::ComputeMelSpectrogram(const TArray<float>& RawAudio)
{
    const int32 NumFrames = 96, WindowSize = 400, HopSize = 160, FFTSize = 512, NumMelBins = 64;
    TArray<float> MelSpectrogram; MelSpectrogram.SetNumZeroed(NumFrames * NumMelBins);

    TArray<float> Hann;
    for (int32 i = 0; i < WindowSize; i++) Hann.Add(0.5f * (1.0f - FMath::Cos((2.0f * (float)PI * i) / WindowSize)));

    for (int32 f = 0; f < NumFrames; f++) {
        int32 Start = f * HopSize;
        TArray<std::complex<float>> CB; CB.SetNumZeroed(FFTSize);
        for (int32 i = 0; i < WindowSize; i++) {
            if (Start + i < RawAudio.Num()) CB[i] = std::complex<float>(RawAudio[Start + i] * Hann[i], 0.0f);
        }

        ComputeFFT(CB);

        for (int32 Bin = 0; Bin < NumMelBins; Bin++) {
            float Hz = MelToHz(HzToMel(125.f) + (Bin + 1) * ((HzToMel(7500.f) - HzToMel(125.f)) / (NumMelBins + 1)));
            int32 FFTBin = FMath::Clamp(FMath::RoundToInt((Hz / 16000.f) * FFTSize), 0, FFTSize / 2);
            float Power = (CB[FFTBin].real() * CB[FFTBin].real()) + (CB[FFTBin].imag() * CB[FFTBin].imag());
            MelSpectrogram[f * NumMelBins + Bin] = FMath::Loge(Power + 0.01f);
        }
    }
    return MelSpectrogram;
}

void FAudioAnalyzerListener::OnNewSubmixBuffer(const USoundSubmix* OwningSubmix, float* AudioData, int32 NumSamples, int32 NumChannels, const int32 SampleRate, double AudioClock)
{
    if (!AudioData || NumSamples == 0) return;
    TArray<float> Mono;
    for (int32 i = 0; i < NumSamples; i += NumChannels) Mono.Add(AudioData[i]);
    OutQueue.Enqueue(MoveTemp(Mono));
}

void AAffectiveAudioActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (FAudioDeviceHandle AudioDevice = GetWorld()->GetAudioDevice()) {
        if (ListenerProxy.IsValid()) AudioDevice->UnregisterSubmixBufferListener(ListenerProxy.Get(), SubmixToAnalyze);
    }
    Super::EndPlay(EndPlayReason);
}