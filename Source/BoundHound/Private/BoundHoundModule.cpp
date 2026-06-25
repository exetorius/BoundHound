// Copyright (c) 2026 exetorius. Released under the MIT License.

#include "Modules/ModuleManager.h"
#include "CoreGlobals.h"
#include "Misc/CoreDelegates.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "Editor.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "ToolsetRegistry/ToolsetDefinition.h"

DEFINE_LOG_CATEGORY_STATIC(LogBoundHound, Log, All);

// Collect every non-abstract UToolsetDefinition subclass defined in THIS module (/Script/BoundHound)
// that exposes at least one AICallable tool. Reflection-based -- mirrors VibeUE's registration so
// the toolset registers without manually naming the class.
static void GatherBoundHoundToolsetClasses(TArray<UClass*>& OutClasses)
{
	TArray<UClass*> Derived;
	GetDerivedClasses(UToolsetDefinition::StaticClass(), Derived, /*bRecursive*/ true);

	const UPackage* BoundHoundPackage = FindPackage(nullptr, TEXT("/Script/BoundHound"));
	for (UClass* Class : Derived)
	{
		if (!Class || Class->HasAnyClassFlags(CLASS_Abstract) || Class->GetOutermost() != BoundHoundPackage)
		{
			continue;
		}

		bool bHasAICallable = false;
		for (TFieldIterator<UFunction> It(Class); It && !bHasAICallable; ++It)
		{
			const TValueOrError<bool, FString> Result = UToolsetDefinition::IsFunctionAICallable(*It);
			bHasAICallable = Result.HasValue() && Result.GetValue();
		}
		if (bHasAICallable)
		{
			OutClasses.Add(Class);
		}
	}
}

class FBoundHoundModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// ToolsetRegistry needs GEditor; register now if it's already up, else defer to PostEngineInit.
		if (GEditor)
		{
			RegisterToolsets();
		}
		else
		{
			FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FBoundHoundModule::RegisterToolsets);
		}
	}

	void RegisterToolsets()
	{
		if (UToolsetRegistry::IsAvailable())
		{
			TArray<UClass*> ToolsetClasses;
			GatherBoundHoundToolsetClasses(ToolsetClasses);
			for (UClass* Class : ToolsetClasses)
			{
				UToolsetRegistry::RegisterToolsetClass(Class);
			}
			UE_LOG(LogBoundHound, Display, TEXT("BoundHound: registered %d toolset(s) with ToolsetRegistry."), ToolsetClasses.Num());
		}
		else
		{
			UE_LOG(LogBoundHound, Warning, TEXT("BoundHound: ToolsetRegistry not available; toolsets not registered."));
		}
	}
};

IMPLEMENT_MODULE(FBoundHoundModule, BoundHound)
