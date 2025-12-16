using UnrealBuildTool;

public class PhysXInstancedSubsystem : ModuleRules
{
	public PhysXInstancedSubsystem(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Public include paths exposed to modules that depend on this module.
		PublicIncludePaths.AddRange(
			new string[]
			{
				// (intentionally empty)
			}
		);

		// Private include paths used only by this module's implementation.
		PrivateIncludePaths.AddRange(
			new string[]
			{
				// (intentionally empty)
			}
		);

		// Public dependencies required to compile and link against this module.
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"Engine",
				"PhysicsCore", // Required for PhysX include integration on UE4.
				"PhysX",
			}
		);

		// Private dependencies required by this module's internal implementation.
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"CoreUObject",
				"Engine",
				"PhysicsCore",
				"NavigationSystem",
				"Projects", // Required for import resource for Billboard
			}
		);

		// Modules that are loaded at runtime on demand.
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// (none)
			}
		);
	}
}