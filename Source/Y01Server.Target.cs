using UnrealBuildTool;

public class Y01ServerTarget : TargetRules
{
    public Y01ServerTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Server;
        DefaultBuildSettings = BuildSettingsVersion.V6;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
        ExtraModuleNames.Add("Y01");
        if (Configuration != UnrealTargetConfiguration.Shipping && Configuration != UnrealTargetConfiguration.Test)
        {
            ExtraModuleNames.Add("Y01Tests");
        }
    }
}
