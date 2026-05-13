#include "AffectiveAudioActor.h"
#include "NiagaraComponent.h"
#include "Async/Async.h"
#include "NNE.h"
#include "NNERuntimeCPU.h"
#include "NNEModelData.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/ArrayReader.h"
#include "NormalizationConstants.h"
#include "VGGishPCAConstants.h"
#include "VGGishMelConstants.h"
#include "Common/UdpSocketBuilder.h"
#include "Common/UdpSocketReceiver.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include <complex>
#include <cmath>

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
        std::complex<float> t = std::polar(1.0f, -2.0f * (float)PI * k / N) * Odd[k];
        Buffer[k] = Even[k] + t;
        Buffer[k + N / 2] = Even[k] - t;
    }
}

float HzToMel(float Hz) { return 2595.0f * std::log10(1.0f + Hz / 700.0f); }
float MelToHz(float Mel) { return 700.0f * (std::pow(10.0f, Mel / 2595.0f) - 1.0f); }

AAffectiveAudioActor::AAffectiveAudioActor()
{
    PrimaryActorTick.bCanEverTick = true;
    AI_AudioBuffer.Reserve(16000);
    VGGishEmbeddings.SetNumZeroed(128);
    AestheticScores.SetNumZeroed(5);
}

void AAffectiveAudioActor::LoadTagNames()
{
    TagNames.Reset();
    FString JsonString;
    const FString FilePath = FPaths::ProjectContentDir() / TEXT("AI/tag_names.json");
    if (!FFileHelper::LoadFileToString(JsonString, *FilePath))
    {
        UE_LOG(LogTemp, Warning, TEXT("AI TAGS: Could not load %s"), *FilePath);
        return;
    }

    TSharedPtr<FJsonValue> JsonValue;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
    if (!FJsonSerializer::Deserialize(Reader, JsonValue) || !JsonValue.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("AI TAGS: Failed to parse JSON in %s"), *FilePath);
        return;
    }

    const TArray<TSharedPtr<FJsonValue>>* JsonArray = nullptr;
    if (JsonValue->TryGetArray(JsonArray))
    {
        TagNames.Reserve(JsonArray->Num());
        for (const TSharedPtr<FJsonValue>& V : *JsonArray)
        {
            TagNames.Add(V->AsString());
        }
        UE_LOG(LogTemp, Warning, TEXT("AI TAGS: Loaded %d tag names"), TagNames.Num());
    }
}

void AAffectiveAudioActor::StartPythonSidecarReceiver()
{
    StopPythonSidecarReceiver();

    FIPv4Endpoint Endpoint(FIPv4Address::Any, (uint16)PythonSidecarPort);
    SidecarSocket = FUdpSocketBuilder(TEXT("PythonSidecarSocket"))
        .AsNonBlocking()
        .AsReusable()
        .BoundToEndpoint(Endpoint)
        .WithReceiveBufferSize(2 * 1024 * 1024);

    if (!SidecarSocket)
    {
        UE_LOG(LogTemp, Warning, TEXT("AI SIDECAR: Failed to bind UDP socket on port %d"), PythonSidecarPort);
        return;
    }

    SidecarReceiver = new FUdpSocketReceiver(SidecarSocket, FTimespan::FromMilliseconds(50), TEXT("PythonSidecarReceiver"));
    SidecarReceiver->OnDataReceived().BindUObject(this, &AAffectiveAudioActor::OnSidecarPacket);
    SidecarReceiver->Start();
    UE_LOG(LogTemp, Warning, TEXT("AI SIDECAR: Listening for UDP JSON on port %d"), PythonSidecarPort);
}

void AAffectiveAudioActor::StopPythonSidecarReceiver()
{
    if (SidecarReceiver)
    {
        SidecarReceiver->Stop();
        delete SidecarReceiver;
        SidecarReceiver = nullptr;
    }
    if (SidecarSocket)
    {
        SidecarSocket->Close();
        ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        if (SS) SS->DestroySocket(SidecarSocket);
        SidecarSocket = nullptr;
    }
}

void AAffectiveAudioActor::OnSidecarPacket(const FArrayReaderPtr& Data, const FIPv4Endpoint& Endpoint)
{
    if (!Data.IsValid() || Data->Num() == 0) return;

    FString JsonStr = FString::ConstructFromPtrSize((const UTF8CHAR*)Data->GetData(), Data->Num());

    TSharedPtr<FJsonObject> JsonObj;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
    if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid()) return;

    // Read ALL [name, pct] pairs in the named field into parallel arrays.
    // Also writes index 0 into the legacy Top1 OutName/OutPct outputs for back-compat.
    auto ReadAllPairs = [&](const FString& FieldName,
                            FString& OutTop1Name, float& OutTop1Pct,
                            TArray<FString>& OutAllNames, TArray<float>& OutAllPcts) {
        OutTop1Name.Reset();
        OutTop1Pct = 0.0f;
        OutAllNames.Reset();
        OutAllPcts.Reset();
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!JsonObj->TryGetArrayField(FieldName, Arr)) return;
        for (const TSharedPtr<FJsonValue>& V : *Arr) {
            const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
            if (!V.IsValid() || !V->TryGetArray(Pair) || Pair->Num() != 2) continue;
            OutAllNames.Add((*Pair)[0]->AsString());
            OutAllPcts.Add((float)(*Pair)[1]->AsNumber());
        }
        if (OutAllNames.Num() > 0) {
            OutTop1Name = OutAllNames[0];
            OutTop1Pct  = OutAllPcts[0];
        }
    };

    FScopeLock Lock(&SidecarStateCS);

    const TArray<TSharedPtr<FJsonValue>>* ScoresArr = nullptr;
    if (JsonObj->TryGetArrayField(TEXT("scores"), ScoresArr) && ScoresArr->Num() == 5)
    {
        SidecarScores.SetNum(5);
        for (int32 i = 0; i < 5; i++) SidecarScores[i] = (float)(*ScoresArr)[i]->AsNumber();
    }

    ReadAllPairs(TEXT("top_genres"),      SidecarTopGenre,      SidecarTopGenrePct,
                 SidecarTopGenresList,     SidecarTopGenresPcts);
    ReadAllPairs(TEXT("top_moods"),       SidecarTopMood,       SidecarTopMoodPct,
                 SidecarTopMoodsList,      SidecarTopMoodsPcts);
    ReadAllPairs(TEXT("top_instruments"), SidecarTopInstrument, SidecarTopInstrumentPct,
                 SidecarTopInstrumentsList, SidecarTopInstrumentsPcts);

    SidecarLastRecvTime = FPlatformTime::Seconds();
}

bool AAffectiveAudioActor::IsPythonSidecarActive() const
{
    FScopeLock Lock(&SidecarStateCS);
    return (FPlatformTime::Seconds() - SidecarLastRecvTime) < 3.0;
}

FString AAffectiveAudioActor::GetTopGenre() const
{
    FScopeLock Lock(&SidecarStateCS);
    return SidecarTopGenre;
}

FString AAffectiveAudioActor::GetTopMood() const
{
    FScopeLock Lock(&SidecarStateCS);
    return SidecarTopMood;
}

FString AAffectiveAudioActor::GetTopInstrument() const
{
    FScopeLock Lock(&SidecarStateCS);
    return SidecarTopInstrument;
}

float AAffectiveAudioActor::GetTopGenreConfidence() const
{
    FScopeLock Lock(&SidecarStateCS);
    return SidecarTopGenrePct;
}

float AAffectiveAudioActor::GetTopMoodConfidence() const
{
    FScopeLock Lock(&SidecarStateCS);
    return SidecarTopMoodPct;
}

float AAffectiveAudioActor::GetTopInstrumentConfidence() const
{
    FScopeLock Lock(&SidecarStateCS);
    return SidecarTopInstrumentPct;
}

void AAffectiveAudioActor::GetTopGenres(TArray<FString>& OutNames, TArray<float>& OutPcts) const
{
    FScopeLock Lock(&SidecarStateCS);
    OutNames = SidecarTopGenresList;
    OutPcts  = SidecarTopGenresPcts;
}

void AAffectiveAudioActor::GetTopMoods(TArray<FString>& OutNames, TArray<float>& OutPcts) const
{
    FScopeLock Lock(&SidecarStateCS);
    OutNames = SidecarTopMoodsList;
    OutPcts  = SidecarTopMoodsPcts;
}

void AAffectiveAudioActor::GetTopInstruments(TArray<FString>& OutNames, TArray<float>& OutPcts) const
{
    FScopeLock Lock(&SidecarStateCS);
    OutNames = SidecarTopInstrumentsList;
    OutPcts  = SidecarTopInstrumentsPcts;
}

// Helper: build top-K from local SmoothedTagProbs filtered by tag prefix.
// "genre---" / "mood/theme---" / "instrument---" are the prefixes used by tag_names.json.
// The returned names are stripped of their prefix (e.g. "genre---rock" -> "rock") so the
// downstream classifier doesn't have to know about the prefix.
static void BuildTopKFromLocal(const TArray<FString>& TagNames,
                               const TArray<float>& Probs,
                               const FString& Prefix,
                               int32 K,
                               TArray<FString>& OutNames,
                               TArray<float>& OutPcts)
{
    OutNames.Reset();
    OutPcts.Reset();
    if (TagNames.Num() == 0 || TagNames.Num() != Probs.Num()) return;

    TArray<TPair<float, FString>> Filtered;
    Filtered.Reserve(TagNames.Num());
    for (int32 i = 0; i < TagNames.Num(); i++) {
        if (TagNames[i].StartsWith(Prefix)) {
            Filtered.Emplace(Probs[i], TagNames[i].RightChop(Prefix.Len()));
        }
    }
    Filtered.Sort([](const TPair<float, FString>& A, const TPair<float, FString>& B){
        return A.Key > B.Key;
    });
    const int32 N = FMath::Min(K, Filtered.Num());
    for (int32 i = 0; i < N; i++) {
        OutPcts.Add(Filtered[i].Key);
        OutNames.Add(Filtered[i].Value);
    }
}

void AAffectiveAudioActor::GetTopGenresAny(TArray<FString>& OutNames, TArray<float>& OutPcts, int32 K) const
{
    if (IsPythonSidecarActive()) {
        FScopeLock Lock(&SidecarStateCS);
        if (SidecarTopGenresList.Num() > 0) {
            OutNames = SidecarTopGenresList;
            OutPcts  = SidecarTopGenresPcts;
            return;
        }
    }
    // Fallback: local ONNX inference.
    BuildTopKFromLocal(TagNames, SmoothedTagProbs, TEXT("genre---"), K, OutNames, OutPcts);
}

void AAffectiveAudioActor::GetTopMoodsAny(TArray<FString>& OutNames, TArray<float>& OutPcts, int32 K) const
{
    if (IsPythonSidecarActive()) {
        FScopeLock Lock(&SidecarStateCS);
        if (SidecarTopMoodsList.Num() > 0) {
            OutNames = SidecarTopMoodsList;
            OutPcts  = SidecarTopMoodsPcts;
            return;
        }
    }
    BuildTopKFromLocal(TagNames, SmoothedTagProbs, TEXT("mood/theme---"), K, OutNames, OutPcts);
}

void AAffectiveAudioActor::GetTopInstrumentsAny(TArray<FString>& OutNames, TArray<float>& OutPcts, int32 K) const
{
    if (IsPythonSidecarActive()) {
        FScopeLock Lock(&SidecarStateCS);
        if (SidecarTopInstrumentsList.Num() > 0) {
            OutNames = SidecarTopInstrumentsList;
            OutPcts  = SidecarTopInstrumentsPcts;
            return;
        }
    }
    BuildTopKFromLocal(TagNames, SmoothedTagProbs, TEXT("instrument---"), K, OutNames, OutPcts);
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
        TSharedPtr<UE::NNE::IModelCPU> VGGishModel = Runtime->CreateModelCPU(VGGishModelData);
        if (VGGishModel.IsValid()) {
            VGGishInstance = VGGishModel->CreateModelInstanceCPU();
            if (VGGishInstance.IsValid()) {
                TConstArrayView<UE::NNE::FTensorDesc> VGGishDescs = VGGishInstance->GetInputTensorDescs();

                UE_LOG(LogTemp, Warning, TEXT("AI DIAG: VGGish has %d input tensor(s)"), VGGishDescs.Num());
                for (int32 i = 0; i < VGGishDescs.Num(); i++) {
                    FString Dims;
                    for (int32 Dim : VGGishDescs[i].GetShape().GetData()) Dims += FString::Printf(TEXT("%d "), Dim);
                    UE_LOG(LogTemp, Warning, TEXT("AI DIAG: VGGish input[%d] '%s' shape: [%s]"), i, *VGGishDescs[i].GetName(), *Dims);
                }

                TArray<UE::NNE::FTensorShape> VGGishShapes;
                for (const UE::NNE::FTensorDesc& Desc : VGGishDescs) {
                    VGGishShapes.Add(UE::NNE::FTensorShape::MakeFromSymbolic(Desc.GetShape()));
                }

                auto Status = VGGishInstance->SetInputTensorShapes(VGGishShapes);
                if (Status != UE::NNE::IModelInstanceCPU::ESetInputTensorShapesStatus::Ok) {
                    UE_LOG(LogTemp, Error, TEXT("AI ERROR: VGGish failed to set shapes!"));
                } else {
                    UE_LOG(LogTemp, Warning, TEXT("AI SUCCESS: VGGish shapes locked!"));
                }
            }
        }

        LoadTagNames();

        TSharedPtr<UE::NNE::IModelCPU> AestheticModel = Runtime->CreateModelCPU(AestheticModelData);
        if (AestheticModel.IsValid()) {
            AestheticInstance = AestheticModel->CreateModelInstanceCPU();
            if (AestheticInstance.IsValid()) {
                TConstArrayView<UE::NNE::FTensorDesc> OutputDescsCheck = AestheticInstance->GetOutputTensorDescs();
                bModelHasTagOutput = (OutputDescsCheck.Num() >= 4);
                if (bModelHasTagOutput) {
                    UE::NNE::FTensorShape TagShape = UE::NNE::FTensorShape::MakeFromSymbolic(OutputDescsCheck[0].GetShape());
                    int32 NumTags = (int32)TagShape.Volume();
                    TagPredictions.SetNumZeroed(NumTags);
                    SmoothedTagProbs.SetNumZeroed(NumTags);
                    UE_LOG(LogTemp, Warning, TEXT("AI MODEL: v2 detected. Tag output dim = %d"), NumTags);
                } else {
                    UE_LOG(LogTemp, Warning, TEXT("AI MODEL: v1 detected (no tag output)"));
                }

                TConstArrayView<UE::NNE::FTensorDesc> InputDescs = AestheticInstance->GetInputTensorDescs();

                UE_LOG(LogTemp, Warning, TEXT("AI DIAG: AestheticBrain has %d input tensor(s)"), InputDescs.Num());
                for (int32 i = 0; i < InputDescs.Num(); i++) {
                    FString Dims;
                    for (int32 Dim : InputDescs[i].GetShape().GetData()) Dims += FString::Printf(TEXT("%d "), Dim);
                    UE_LOG(LogTemp, Warning, TEXT("AI DIAG: AestheticBrain input[%d] '%s' shape: [%s]"), i, *InputDescs[i].GetName(), *Dims);
                }

                TArray<UE::NNE::FTensorShape> AestheticShapes;
                for (const UE::NNE::FTensorDesc& Desc : InputDescs) {
                    AestheticShapes.Add(UE::NNE::FTensorShape::MakeFromSymbolic(Desc.GetShape()));
                }

                if (InputDescs.Num() >= 3) {
                    uint64 HiddenElements = UE::NNE::FTensorShape::MakeFromSymbolic(InputDescs[1].GetShape()).Volume();
                    LstmHiddenState.SetNumZeroed((int32)HiddenElements);
                    LstmCellState.SetNumZeroed((int32)HiddenElements);
                    UE_LOG(LogTemp, Warning, TEXT("AI DIAG: LSTM state allocated %llu elements"), HiddenElements);
                }

                auto Status = AestheticInstance->SetInputTensorShapes(AestheticShapes);
                if (Status != UE::NNE::IModelInstanceCPU::ESetInputTensorShapesStatus::Ok) {
                    UE_LOG(LogTemp, Error, TEXT("AI ERROR: Aesthetic failed to set shapes!"));
                } else {
                    UE_LOG(LogTemp, Warning, TEXT("AI SUCCESS: Aesthetic shapes locked!"));
                }
            }
        }
    }
    else {
        UE_LOG(LogTemp, Error, TEXT("AI ERROR: Model Data is NULL! Assign them in the Editor Details panel."));
    }

    if (SubmixToAnalyze) {
        if (FAudioDeviceHandle AudioDevice = GetWorld()->GetAudioDevice()) {
            ListenerProxy = MakeShared<FAudioAnalyzerListener, ESPMode::ThreadSafe>(AudioFeatureQueue, CurrentRMS);
            AudioDevice->RegisterSubmixBufferListener(ListenerProxy.Get(), SubmixToAnalyze);
        }
    }

    StartPythonSidecarReceiver();

    SessionStartTime = FPlatformTime::Seconds();
    FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
    CsvFilePath = FPaths::ProjectSavedDir() / TEXT("MusicAI_Logs") / FString::Printf(TEXT("AffectiveLog_%s.csv"), *Stamp);
    IPlatformFile& Pf = FPlatformFileManager::Get().GetPlatformFile();
    Pf.CreateDirectoryTree(*FPaths::GetPath(CsvFilePath));
    CsvFileHandle.Reset(Pf.OpenWrite(*CsvFilePath));
    if (CsvFileHandle.IsValid()) {
        FString Header = TEXT("Time,RMS,Arousal,Valence,Timbre,Rhythm,Intensity,TopGenre,GenrePct,TopMood,MoodPct,TopInstrument,InstrumentPct\r\n");
        FTCHARToUTF8 Utf8(*Header);
        CsvFileHandle->Write((const uint8*)Utf8.Get(), Utf8.Length());
        UE_LOG(LogTemp, Warning, TEXT("AI CSV: Logging to %s"), *CsvFilePath);
    } else {
        UE_LOG(LogTemp, Error, TEXT("AI CSV: Failed to open %s"), *CsvFilePath);
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

    if (GEngine) {
        float BufferRms = 0.0f;
        for (float S : AI_AudioBuffer) BufferRms += S * S;
        BufferRms = AI_AudioBuffer.Num() > 0 ? FMath::Sqrt(BufferRms / AI_AudioBuffer.Num()) : 0.0f;
        GEngine->AddOnScreenDebugMessage(3, 0.0f, FColor::Yellow,
            FString::Printf(TEXT("BUFFER: %d/16000 | RMS: %.6f | Queue: %s"),
                AI_AudioBuffer.Num(), BufferRms,
                AudioFeatureQueue.IsEmpty() ? TEXT("EMPTY") : TEXT("DATA")));
    }

    // Display top tags. Prefer Python sidecar (more accurate). Fall back to in-engine ONNX.
    const bool bSidecar = IsPythonSidecarActive();
    if (GEngine && bSidecar)
    {
        FString Genre, Mood, Instr;
        float GP, MP, IP;
        {
            FScopeLock Lock(&SidecarStateCS);
            Genre = SidecarTopGenre; GP = SidecarTopGenrePct;
            Mood  = SidecarTopMood;  MP = SidecarTopMoodPct;
            Instr = SidecarTopInstrument; IP = SidecarTopInstrumentPct;
        }
        GEngine->AddOnScreenDebugMessage(10, 0.0f, FColor::Green,
            FString::Printf(TEXT("[PY] GENRE: %s %d%%"), *Genre.ToUpper(), FMath::RoundToInt(GP * 100.0f)));
        GEngine->AddOnScreenDebugMessage(11, 0.0f, FColor::Magenta,
            FString::Printf(TEXT("[PY] MOOD:  %s %d%%"), *Mood.ToUpper(),  FMath::RoundToInt(MP * 100.0f)));
        GEngine->AddOnScreenDebugMessage(12, 0.0f, FColor::Orange,
            FString::Printf(TEXT("[PY] INSTR: %s %d%%"), *Instr.ToUpper(), FMath::RoundToInt(IP * 100.0f)));
    }
    else if (GEngine && bModelHasTagOutput && SmoothedTagProbs.Num() > 0 && TagNames.Num() == SmoothedTagProbs.Num())
    {
        auto TopForCategory = [this](const FString& Prefix, int32 K) -> FString {
            TArray<TPair<float, FString>> Items;
            for (int32 i = 0; i < TagNames.Num(); i++) {
                if (TagNames[i].StartsWith(Prefix)) {
                    Items.Add(TPair<float, FString>(SmoothedTagProbs[i], TagNames[i].RightChop(Prefix.Len())));
                }
            }
            Items.Sort([](const TPair<float, FString>& A, const TPair<float, FString>& B){ return A.Key > B.Key; });
            FString Out;
            for (int32 i = 0; i < FMath::Min(K, Items.Num()); i++) {
                if (i > 0) Out += TEXT("  ");
                Out += FString::Printf(TEXT("%s %d%%"), *Items[i].Value.ToUpper(), FMath::RoundToInt(Items[i].Key * 100.0f));
            }
            return Out;
        };

        GEngine->AddOnScreenDebugMessage(10, 0.0f, FColor::Green,
            FString::Printf(TEXT("GENRE: %s"), *TopForCategory(TEXT("genre---"), 3)));
        GEngine->AddOnScreenDebugMessage(11, 0.0f, FColor::Magenta,
            FString::Printf(TEXT("MOOD:  %s"), *TopForCategory(TEXT("mood/theme---"), 3)));
        GEngine->AddOnScreenDebugMessage(12, 0.0f, FColor::Orange,
            FString::Printf(TEXT("INSTR: %s"), *TopForCategory(TEXT("instrument---"), 3)));
    }

    // When sidecar is active, override AestheticScores with sidecar values.
    // Lighting and Niagara code reads AestheticScores so this is a transparent swap.
    if (bSidecar) {
        FScopeLock Lock(&SidecarStateCS);
        if (SidecarScores.Num() == 5) {
            for (int32 i = 0; i < 5; i++) AestheticScores[i] = SidecarScores[i];
        }
    }

    if (UNiagaraComponent* NiagaraComp = FindComponentByClass<UNiagaraComponent>())
    {
        static float InterpScores[5] = { 0,0,0,0,0 };
        static float OldEnergy = 0.0f, OldBass = 0.0f, OldVibe = 0.0f;

        for (int i = 0; i < 5; i++) {
            float Normalized = (FMath::Tanh(AestheticScores[i] * 0.4f) + 1.0f) * 0.5f;
            InterpScores[i] = FMath::FInterpTo(InterpScores[i], Normalized, DeltaTime, 6.0f);
        }

        const float Sensitivity = 40.0f;
        const float EnergyFloor = 3.0f;
        const float BassFloor = 1.5f;
        float TargetEnergy = FMath::Max(0.0f, (FMath::Abs(AestheticScores[0]) - EnergyFloor) * Sensitivity);
        float TargetBass   = FMath::Max(0.0f, (FMath::Abs(AestheticScores[1]) - BassFloor)   * Sensitivity);
        float TargetVibe   = FMath::Max(0.0f, (FMath::Abs(AestheticScores[2]) - BassFloor)   * Sensitivity);
        OldEnergy = FMath::FInterpTo(OldEnergy, TargetEnergy, DeltaTime, 8.0f);
        OldBass   = FMath::FInterpTo(OldBass,   TargetBass,   DeltaTime, 8.0f);
        OldVibe   = FMath::FInterpTo(OldVibe,   TargetVibe,   DeltaTime, 8.0f);

        float ArousalDamp = 1.0f - 0.75f * InterpScores[0];
        NiagaraComp->SetVariableFloat(FName(TEXT("AI_Energy")), OldEnergy * ArousalDamp);
        NiagaraComp->SetVariableFloat(FName(TEXT("Bass")),      OldBass   * ArousalDamp);
        NiagaraComp->SetVariableFloat(FName(TEXT("Vibe")),      OldVibe   * ArousalDamp);
        NiagaraComp->SetVariableFloat(FName(TEXT("AI_Calm")),   1.0f - InterpScores[0]);

        NiagaraComp->SetVariableFloat(FName(TEXT("AI_Arousal")),   InterpScores[0]);
        NiagaraComp->SetVariableFloat(FName(TEXT("AI_Valence")),   InterpScores[1]);
        NiagaraComp->SetVariableFloat(FName(TEXT("AI_Timbre")),    InterpScores[2]);
        NiagaraComp->SetVariableFloat(FName(TEXT("AI_Rhythm")),    InterpScores[3]);
        NiagaraComp->SetVariableFloat(FName(TEXT("AI_Intensity")), InterpScores[4]);

        if (GEngine) {
            GEngine->AddOnScreenDebugMessage(1, 0.0f, FColor::Cyan,
                FString::Printf(TEXT("A:%.2f V:%.2f T:%.2f R:%.2f I:%.2f"),
                    InterpScores[0], InterpScores[1], InterpScores[2], InterpScores[3], InterpScores[4]));
        }
    }
}

void AAffectiveAudioActor::ProcessAIInference()
{
    AsyncTask(ENamedThreads::AnyBackgroundHiPriTask, [this]()
    {
        if (!VGGishInstance.IsValid() || !AestheticInstance.IsValid()) return;

        // Skip if buffer underrun (prevents division by zero from alt-tab glitches)
        if (AI_AudioBuffer.Num() < 8000) return;

        float RmsSum = 0.0f;
        for (float Sample : AI_AudioBuffer) RmsSum += Sample * Sample;
        float Rms = FMath::Sqrt(RmsSum / AI_AudioBuffer.Num());

        // Reject corrupted RMS values (prevents the 592541 spike bug)
        if (!FMath::IsFinite(Rms) || Rms > 1.0f) return;

        if (Rms < 0.0005f) {
            FMemory::Memzero(LstmHiddenState.GetData(), LstmHiddenState.Num() * sizeof(float));
            FMemory::Memzero(LstmCellState.GetData(), LstmCellState.Num() * sizeof(float));
            FMemory::Memzero(AestheticScores.GetData(), AestheticScores.Num() * sizeof(float));
            return;
        }

        TArray<float> Spectrogram = ComputeMelSpectrogram(AI_AudioBuffer);

        TArray<UE::NNE::FTensorBindingCPU> InB, OutB;

        UE::NNE::FTensorBindingCPU BIn;
        BIn.Data = Spectrogram.GetData();
        BIn.SizeInBytes = Spectrogram.Num() * sizeof(float);
        InB.Add(BIn);

        UE::NNE::FTensorBindingCPU BOut;
        BOut.Data = VGGishEmbeddings.GetData();
        BOut.SizeInBytes = VGGishEmbeddings.Num() * sizeof(float);
        OutB.Add(BOut);

        VGGishInstance->RunSync(InB, OutB);

        // VGGish PCA postprocessing: matmul with eigen vectors + clip + 8-bit quantize.
        // Without this, AestheticBrain receives features outside its training distribution
        // and collapses to a constant prediction (verified by ai_backend/diagnostic_pca.py).
        {
            float Postprocessed[128];
            for (int32 i = 0; i < 128; i++) {
                float Sum = 0.0f;
                for (int32 j = 0; j < 128; j++) {
                    Sum += VGGISH_PCA_EIGEN[i * 128 + j] * (VGGishEmbeddings[j] - VGGISH_PCA_MEANS[j]);
                }
                Postprocessed[i] = Sum;
            }
            const float QScale = 255.0f / (VGGISH_QUANTIZE_MAX - VGGISH_QUANTIZE_MIN);
            for (int32 i = 0; i < 128; i++) {
                const float Clipped = FMath::Clamp(Postprocessed[i], VGGISH_QUANTIZE_MIN, VGGISH_QUANTIZE_MAX);
                VGGishEmbeddings[i] = FMath::RoundToFloat((Clipped - VGGISH_QUANTIZE_MIN) * QScale);
            }
        }

        // Z-score normalization with the per-dim stats computed during training.
        // The AestheticBrain expects (x - mean) / std applied to the postprocessed embeddings.
        for (int32 i = 0; i < 128; i++) {
            VGGishEmbeddings[i] = (VGGishEmbeddings[i] - VGGISH_MEAN[i]) / VGGISH_STD[i];
        }

        // Stateless mode: zero state each frame. Trained LSTM weights cause output saturation when state accumulates;
        // analyzing each audio chunk independently produces meaningful dynamic values across all 5 outputs.
        FMemory::Memzero(LstmHiddenState.GetData(), LstmHiddenState.Num() * sizeof(float));
        FMemory::Memzero(LstmCellState.GetData(), LstmCellState.Num() * sizeof(float));

        TArray<float> LstmHiddenOut;
        TArray<float> LstmCellOut;
        LstmHiddenOut.SetNumZeroed(LstmHiddenState.Num());
        LstmCellOut.SetNumZeroed(LstmCellState.Num());

        TArray<UE::NNE::FTensorBindingCPU> InB2, OutB2;

        UE::NNE::FTensorBindingCPU BInput;
        BInput.Data = VGGishEmbeddings.GetData();
        BInput.SizeInBytes = VGGishEmbeddings.Num() * sizeof(float);
        InB2.Add(BInput);

        if (LstmHiddenState.Num() > 0) {
            UE::NNE::FTensorBindingCPU BHidden;
            BHidden.Data = LstmHiddenState.GetData();
            BHidden.SizeInBytes = LstmHiddenState.Num() * sizeof(float);
            InB2.Add(BHidden);

            UE::NNE::FTensorBindingCPU BCell;
            BCell.Data = LstmCellState.GetData();
            BCell.SizeInBytes = LstmCellState.Num() * sizeof(float);
            InB2.Add(BCell);
        }

        if (bModelHasTagOutput && TagPredictions.Num() > 0) {
            UE::NNE::FTensorBindingCPU BTags;
            BTags.Data = TagPredictions.GetData();
            BTags.SizeInBytes = TagPredictions.Num() * sizeof(float);
            OutB2.Add(BTags);
        }

        UE::NNE::FTensorBindingCPU BScore;
        BScore.Data = AestheticScores.GetData();
        BScore.SizeInBytes = AestheticScores.Num() * sizeof(float);
        OutB2.Add(BScore);

        if (LstmHiddenState.Num() > 0) {
            UE::NNE::FTensorBindingCPU BHiddenOut;
            BHiddenOut.Data = LstmHiddenOut.GetData();
            BHiddenOut.SizeInBytes = LstmHiddenOut.Num() * sizeof(float);
            OutB2.Add(BHiddenOut);

            UE::NNE::FTensorBindingCPU BCellOut;
            BCellOut.Data = LstmCellOut.GetData();
            BCellOut.SizeInBytes = LstmCellOut.Num() * sizeof(float);
            OutB2.Add(BCellOut);
        }

        AestheticInstance->RunSync(InB2, OutB2);

        // Loosen clamp back to ±10 so we can see the raw model output without state influence
        for (int32 i = 0; i < AestheticScores.Num(); i++) {
            if (!FMath::IsFinite(AestheticScores[i])) AestheticScores[i] = 0.0f;
            AestheticScores[i] = FMath::Clamp(AestheticScores[i], -10.0f, 10.0f);
        }

        // Smooth tag probabilities over time (EMA) for stable top-K selection
        if (bModelHasTagOutput && TagPredictions.Num() == SmoothedTagProbs.Num()) {
            const float Alpha = 0.15f;
            for (int32 i = 0; i < TagPredictions.Num(); i++) {
                float P = FMath::IsFinite(TagPredictions[i]) ? FMath::Clamp(TagPredictions[i], 0.0f, 1.0f) : 0.0f;
                SmoothedTagProbs[i] = SmoothedTagProbs[i] * (1.0f - Alpha) + P * Alpha;
            }
        }

        {
            FScopeLock Lock(&CsvCS);
            if (CsvFileHandle.IsValid()) {
                double T = FPlatformTime::Seconds() - SessionStartTime;

                FString TopGenre = TEXT(""), TopMood = TEXT(""), TopInstr = TEXT("");
                float TopGenreP = 0.0f, TopMoodP = 0.0f, TopInstrP = 0.0f;
                if (bModelHasTagOutput && SmoothedTagProbs.Num() > 0 && TagNames.Num() == SmoothedTagProbs.Num()) {
                    for (int32 i = 0; i < TagNames.Num(); i++) {
                        const FString& Name = TagNames[i];
                        const float P = SmoothedTagProbs[i];
                        if (Name.StartsWith(TEXT("genre---")) && P > TopGenreP) {
                            TopGenreP = P; TopGenre = Name.RightChop(8);
                        } else if (Name.StartsWith(TEXT("mood/theme---")) && P > TopMoodP) {
                            TopMoodP = P; TopMood = Name.RightChop(13);
                        } else if (Name.StartsWith(TEXT("instrument---")) && P > TopInstrP) {
                            TopInstrP = P; TopInstr = Name.RightChop(13);
                        }
                    }
                }

                FString Row = FString::Printf(
                    TEXT("%.3f,%.6f,%.4f,%.4f,%.4f,%.4f,%.4f,%s,%.1f,%s,%.1f,%s,%.1f\r\n"),
                    T, Rms, AestheticScores[0], AestheticScores[1],
                    AestheticScores[2], AestheticScores[3], AestheticScores[4],
                    *TopGenre, TopGenreP * 100.0f,
                    *TopMood,  TopMoodP  * 100.0f,
                    *TopInstr, TopInstrP * 100.0f);
                FTCHARToUTF8 Utf8(*Row);
                CsvFileHandle->Write((const uint8*)Utf8.Get(), Utf8.Length());
            }
        }

        AsyncTask(ENamedThreads::GameThread, [this]() {
            if (GEngine) {
                GEngine->AddOnScreenDebugMessage(2, 2.0f, FColor::Green,
                    FString::Printf(TEXT("RAW: A=%.3f V=%.3f T=%.3f R=%.3f I=%.3f"),
                        AestheticScores[0], AestheticScores[1], AestheticScores[2],
                        AestheticScores[3], AestheticScores[4]));
            }
        });

        if (LstmHiddenState.Num() > 0) {
            LstmHiddenState = MoveTemp(LstmHiddenOut);
            LstmCellState = MoveTemp(LstmCellOut);
        }
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
    // Exact VGGish mel spectrogram pipeline:
    //   1. STFT with Hann window, 400-sample window, 160-sample hop, 512-point FFT.
    //   2. Magnitude (NOT power) of FFT output, first 257 bins.
    //   3. Multiply by precomputed triangular mel filterbank (257 -> 64 bins).
    //   4. log(mel + 0.01) - no extra normalization.
    //   5. Output layout is time-major: [frame0_mel0..mel63, frame1_mel0..mel63, ...]
    //      because VGGish ONNX expects [batch, 1, time=96, mels=64] in NCHW order.
    const int32 NumFrames = VGGISH_MEL_NUM_FRAMES;        // 96
    const int32 WindowSize = VGGISH_MEL_WINDOW_LEN;       // 400
    const int32 HopSize = VGGISH_MEL_HOP_LEN;             // 160
    const int32 FFTSize = VGGISH_MEL_FFT_SIZE;            // 512
    const int32 SpecBins = VGGISH_MEL_SPEC_BINS;          // 257
    const int32 NumMelBins = VGGISH_MEL_NUM_BINS;         // 64

    TArray<float> MelSpectrogram;
    MelSpectrogram.SetNumZeroed(NumFrames * NumMelBins);

    TArray<float> Hann;
    Hann.SetNumUninitialized(WindowSize);
    for (int32 i = 0; i < WindowSize; i++) {
        Hann[i] = 0.5f * (1.0f - FMath::Cos((2.0f * (float)PI * i) / WindowSize));
    }

    TArray<float> Magnitude;
    Magnitude.SetNumZeroed(SpecBins);

    // For DSP band-energy / onset detection, accumulate average mel energy in 3 bands.
    double BassAcc = 0.0, MidAcc = 0.0, TrebleAcc = 0.0;
    int32  BassN = 0, MidN = 0, TrebleN = 0;

    for (int32 f = 0; f < NumFrames; f++) {
        int32 Start = f * HopSize;

        TArray<std::complex<float>> CB;
        CB.SetNumZeroed(FFTSize);
        for (int32 i = 0; i < WindowSize; i++) {
            const int32 Idx = Start + i;
            if (Idx < RawAudio.Num()) {
                CB[i] = std::complex<float>(RawAudio[Idx] * Hann[i], 0.0f);
            }
        }

        ComputeFFT(CB);

        // Magnitude of first SpecBins = FFTSize/2 + 1 bins (real-FFT range).
        for (int32 s = 0; s < SpecBins; s++) {
            const float Re = CB[s].real();
            const float Im = CB[s].imag();
            Magnitude[s] = FMath::Sqrt(Re * Re + Im * Im);
        }

        // Triangular mel filterbank: mel[m] = sum over s of magnitude[s] * weight[s, m].
        for (int32 m = 0; m < NumMelBins; m++) {
            float Sum = 0.0f;
            for (int32 s = 0; s < SpecBins; s++) {
                Sum += Magnitude[s] * VGGISH_MEL_MATRIX[s * NumMelBins + m];
            }
            const float LogMel = FMath::Loge(Sum + VGGISH_MEL_LOG_OFFSET);
            // Store in time-major layout for VGGish: row = frame, col = mel bin.
            MelSpectrogram[f * NumMelBins + m] = LogMel;

            // DSP side-channel: accumulate per-band energy for visualization.
            const float Energy = Sum;
            if (m < 8)               { BassAcc   += Energy; BassN++; }
            else if (m >= 16 && m < 40) { MidAcc    += Energy; MidN++; }
            else if (m >= 48)        { TrebleAcc += Energy; TrebleN++; }
        }
    }

    const float Bass   = BassN   > 0 ? (float)(BassAcc   / BassN)   : 0.0f;
    const float Mid    = MidN    > 0 ? (float)(MidAcc    / MidN)    : 0.0f;
    const float Treble = TrebleN > 0 ? (float)(TrebleAcc / TrebleN) : 0.0f;

    BassLevel.store(FMath::Clamp(FMath::Loge(Bass * 20.0f + 1.0f) / 4.0f, 0.0f, 1.0f));
    MidLevel.store(FMath::Clamp(FMath::Loge(Mid * 20.0f + 1.0f) / 4.0f, 0.0f, 1.0f));
    TrebleLevel.store(FMath::Clamp(FMath::Loge(Treble * 20.0f + 1.0f) / 4.0f, 0.0f, 1.0f));

    const double Now = FPlatformTime::Seconds();
    if ((Bass - PrevBassEnergy) > FMath::Max(PrevBassEnergy * 0.35f, 0.0008f) && (Now - LastKickTime) > 0.18) {
        LastKickOnset.store(Now);
        LastKickTime = Now;
    }
    if ((Mid - PrevMidEnergy) > FMath::Max(PrevMidEnergy * 0.30f, 0.0005f) && (Now - LastSnareTime) > 0.12) {
        LastSnareOnset.store(Now);
        LastSnareTime = Now;
    }
    if ((Treble - PrevTrebleEnergy) > FMath::Max(PrevTrebleEnergy * 0.25f, 0.0003f) && (Now - LastHiHatTime) > 0.06) {
        LastHiHatOnset.store(Now);
        LastHiHatTime = Now;
    }
    PrevBassEnergy = Bass;
    PrevMidEnergy = Mid;
    PrevTrebleEnergy = Treble;

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
    StopPythonSidecarReceiver();

    if (FAudioDeviceHandle AudioDevice = GetWorld()->GetAudioDevice()) {
        if (ListenerProxy.IsValid()) AudioDevice->UnregisterSubmixBufferListener(ListenerProxy.Get(), SubmixToAnalyze);
    }

    {
        FScopeLock Lock(&CsvCS);
        CsvFileHandle.Reset();
    }

    Super::EndPlay(EndPlayReason);
}
