#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "Types/PhysXInstancedTypes.h"
#include "PhysXInstanceEvents.generated.h"

UINTERFACE(BlueprintType)
class UPhysXInstanceEvents : public UInterface
{
	GENERATED_BODY()
};

class IPhysXInstanceEvents
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintNativeEvent, Category="PhysX Instance|Events")
	void OnInstancePreRemove(FPhysXInstanceID ID, EPhysXInstanceRemoveReason Reason, const FTransform& WorldTransform);

	UFUNCTION(BlueprintNativeEvent, Category="PhysX Instance|Events")
	void OnInstancePostRemove(FPhysXInstanceID ID, EPhysXInstanceRemoveReason Reason, const FTransform& WorldTransform);

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="PhysX Instance|Events")
	void OnInstancePreConvert(
		FPhysXInstanceID ID,
		EPhysXInstanceConvertReason Reason,
		APhysXInstancedMeshActor* FromActor,
		APhysXInstancedMeshActor* ToActor,
		const FTransform& WorldTransform);

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="PhysX Instance|Events")
	void OnInstancePostConvert(
		FPhysXInstanceID ID,
		EPhysXInstanceConvertReason Reason,
		APhysXInstancedMeshActor* FromActor,
		APhysXInstancedMeshActor* ToActor,
		const FTransform& WorldTransform);

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="PhysX Instance|Events")
	void OnInstancePrePhysics(FPhysXInstanceID ID, bool bEnable, bool bDestroyBodyIfDisabling);

	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="PhysX Instance|Events")
	void OnInstancePostPhysics(FPhysXInstanceID ID, bool bEnable, bool bDestroyBodyIfDisabling, bool bSuccess);
};
