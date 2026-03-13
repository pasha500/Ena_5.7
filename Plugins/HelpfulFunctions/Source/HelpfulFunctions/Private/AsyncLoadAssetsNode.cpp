


#include "AsyncLoadAssetsNode.h"
#include "Engine/AssetManager.h"

UAsyncLoadAssetsNode::UAsyncLoadAssetsNode()
{
}

UAsyncLoadAssetsNode* UAsyncLoadAssetsNode::AsyncLoadAssetArray(const TArray<TSoftObjectPtr<UObject>>& AssetsToLoad)
{
    UAsyncLoadAssetsNode* Node = NewObject<UAsyncLoadAssetsNode>();
    Node->Assets = AssetsToLoad;
    return Node;
}

void UAsyncLoadAssetsNode::Activate()
{
    if (Assets.Num() == 0)
    {
        // Jeœli lista zasobów jest pusta, natychmiast zakoñcz
        OnCompleted.Broadcast(LoadedAssets);
        return;
    }

    LoadNextAsset();
}

void UAsyncLoadAssetsNode::LoadNextAsset()
{
    if (Assets.Num() == 0)
    {
        // Wszystkie zasoby zosta³y za³adowane
        OnCompleted.Broadcast(LoadedAssets);
        return;
    }

    // Pobierz pierwszy zasób z listy
    TSoftObjectPtr<UObject> AssetToLoad = Assets[0];
    Assets.RemoveAt(0);

    if (AssetToLoad.IsValid())
    {
        // Jeœli zasób jest ju¿ za³adowany, dodaj go do wyników
        LoadedAssets.Add(AssetToLoad.Get());
        LoadNextAsset();
    }
    else
    {
        // Asynchronicznie za³aduj zasób i wywo³aj callback po zakoñczeniu
        StreamableManager.RequestAsyncLoad(
            AssetToLoad.ToSoftObjectPath(),
            FStreamableDelegate::CreateUObject(this, &UAsyncLoadAssetsNode::OnAssetLoaded, AssetToLoad)
        );
    }
}

void UAsyncLoadAssetsNode::OnAssetLoaded(TSoftObjectPtr<UObject> LoadedAsset)
{
    if (LoadedAsset.IsValid())
    {
        UObject* LoadedObject = LoadedAsset.Get();
        if (LoadedObject)
        {
            LoadedAssets.Add(LoadedObject);
        }
    }

    // PrzejdŸ do nastêpnego zasobu
    LoadNextAsset();
}

