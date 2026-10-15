using UnrealBuildTool;

public class Y01Tests : ModuleRules
{
    public Y01Tests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        // 各测试夹具拥有文件局部辅助函数，禁止Unity重新分组造成匿名命名空间冲突。
        bUseUnity = false;
        if (Target.Configuration == UnrealTargetConfiguration.Shipping || Target.Configuration == UnrealTargetConfiguration.Test)
        {
            throw new BuildException("Y01Tests 仅供开发配置使用");
        }
        PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "Y01", "GameplayAbilities", "GameplayTags", "GameplayTasks", "EnhancedInput", "InputCore", "UMG", "SlateCore", "Slate", "Json", "RHI", "OnlineSubsystemUtils" });
        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new string[] { "UnrealEd", "AssetRegistry", "RenderCore", "UMGEditor" });
        }
    }
}
