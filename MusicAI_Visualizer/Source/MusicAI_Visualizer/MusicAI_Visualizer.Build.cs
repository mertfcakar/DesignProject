using UnrealBuildTool;

public class MusicAI_Visualizer : ModuleRules
{
    public MusicAI_Visualizer(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[] {
            "Core",
            "CoreUObject",
            "Engine",
            "InputCore",
            "EnhancedInput",
            "NNE",
            "Niagara",
            "AudioCapture",
            "AudioMixer",
            "SignalProcessing"
        });
    }
}