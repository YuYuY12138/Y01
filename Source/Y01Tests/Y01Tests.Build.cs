using UnrealBuildTool;

public class Y01Tests : ModuleRules
{
    public Y01Tests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        if (Target.Configuration == UnrealTargetConfiguration.Shipping || Target.Configuration == UnrealTargetConfiguration.Test)
        {
            throw new BuildException("Y01Tests 仅供开发配置使用");
        }
        PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Y01", "GameplayAbilities", "GameplayTags", "GameplayTasks", "EnhancedInput", "InputCore" });
        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "AssetRegistry" });
        }
    }
}
