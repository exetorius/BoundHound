// Copyright (c) 2026 exetorius. Released under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/AgentSkill.h"
#include "BoundHoundTriageSkill.generated.h"

/**
 * Native AgentSkill that teaches the engine's AI assistant HOW to use BoundHound -- the strategy layer
 * on top of the per-tool descriptions. Ships with the plugin: because ToolsetRegistry discovers skills by
 * scanning UAgentSkill subclasses (native classes included) and the default allow/block lists are empty,
 * this is auto-registered on plugin load with zero project setup. It shows up in ListSkills and the
 * assistant reads its Instructions via GetSkills.
 *
 * Description/Instructions are set on the CDO in the constructor, which is exactly what the read path
 * (UAgentSkillToolset::ListSkills / GetSkills) inspects.
 */
UCLASS()
class UBoundHoundTriageSkill : public UAgentSkill
{
	GENERATED_BODY()

public:
	UBoundHoundTriageSkill();
};
