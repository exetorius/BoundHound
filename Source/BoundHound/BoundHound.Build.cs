// Copyright (c) 2026 exetorius. Released under the MIT License.

using UnrealBuildTool;

public class BoundHound : ModuleRules
{
	public BoundHound(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
		IWYUSupport = IWYUSupport.None;
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Json",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UnrealEd",            // GEditor, FRequestPlaySessionParams, FEditorDelegates
				"RenderCore",          // GGameThreadTime / GRenderThreadTime / GRHIThreadTime
				"RHI",                 // RHIGetGPUFrameCycles
				"TraceServices",       // ITraceServicesModule / IAnalysisService (Analyse)
				"ToolsetRegistry",     // UE 5.8 native AI toolset registry (UToolsetDefinition base + registration)
			}
		);
	}
}
