#include "ForgeAISettings.h"
#include "Misc/Paths.h"

UForgeAISettings::UForgeAISettings()
{
}

FString UForgeAISettings::GetAbsoluteContextDirectory() const
{
	if (FPaths::IsRelative(ContextDirectory))
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / ContextDirectory);
	}
	return ContextDirectory;
}
