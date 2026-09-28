// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class Y01EditorTarget : TargetRules
{
	public Y01EditorTarget( TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
		ExtraModuleNames.Add("Y01");
		if (Configuration != UnrealTargetConfiguration.Shipping && Configuration != UnrealTargetConfiguration.Test)
		{
			ExtraModuleNames.Add("Y01Tests");
		}
	}
}
