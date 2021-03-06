// Copyright 1998-2015 Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	TextureRenderTarget2D.cpp: UTextureRenderTarget2D implementation
=============================================================================*/

#include "EnginePrivate.h"
#include "Engine/TextureRenderTarget2D.h"

/*-----------------------------------------------------------------------------
	UTextureRenderTarget2D
-----------------------------------------------------------------------------*/

UTextureRenderTarget2D::UTextureRenderTarget2D(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bHDR = true;
	ClearColor = FLinearColor(0.0f, 1.0f, 0.0f, 1.0f);
	OverrideFormat = PF_Unknown;
	bForceLinearGamma = true;
}

FTextureResource* UTextureRenderTarget2D::CreateResource()
{
	UWorld* World = GetWorld();
	ERHIFeatureLevel::Type FeatureLevel = World != nullptr ? World->FeatureLevel : GMaxRHIFeatureLevel;
	if (FeatureLevel <= ERHIFeatureLevel::ES2)
	{
		EPixelFormat Format = GetFormat();
		if ((!GSupportsRenderTargetFormat_PF_FloatRGBA && (Format == PF_FloatRGBA || Format == PF_FloatRGB))
			|| Format == PF_A16B16G16R16)
		{
			OverrideFormat = PF_B8G8R8A8;
		}
	}
	FTextureRenderTarget2DResource* Result = new FTextureRenderTarget2DResource(this);
	return Result;
}

EMaterialValueType UTextureRenderTarget2D::GetMaterialType()
{
	return MCT_Texture2D;
}

SIZE_T UTextureRenderTarget2D::GetResourceSize(EResourceSizeMode::Type Mode)
{
	// Calculate size based on format.
	EPixelFormat Format = GetFormat();
	int32 BlockSizeX	= GPixelFormats[Format].BlockSizeX;
	int32 BlockSizeY	= GPixelFormats[Format].BlockSizeY;
	int32 BlockBytes	= GPixelFormats[Format].BlockBytes;
	int32 NumBlocksX	= (SizeX + BlockSizeX - 1) / BlockSizeX;
	int32 NumBlocksY	= (SizeY + BlockSizeY - 1) / BlockSizeY;
	int32 NumBytes	= NumBlocksX * NumBlocksY * BlockBytes;

	return NumBytes;
}

void UTextureRenderTarget2D::InitCustomFormat( uint32 InSizeX, uint32 InSizeY, EPixelFormat InOverrideFormat, bool bInForceLinearGamma )
{
	check(InSizeX > 0 && InSizeY > 0);
	check(FTextureRenderTargetResource::IsSupportedFormat(InOverrideFormat));

	// set required size/format
	SizeX = InSizeX;
	SizeY = InSizeY;
	OverrideFormat = InOverrideFormat;
	bForceLinearGamma = bInForceLinearGamma;

	// Recreate the texture's resource.
	UpdateResource();
}

void UTextureRenderTarget2D::InitAutoFormat(uint32 InSizeX, uint32 InSizeY)
{
	check(InSizeX > 0 && InSizeY > 0);

	// set required size
	SizeX = InSizeX;
	SizeY = InSizeY;

	// Recreate the texture's resource.
	UpdateResource();
}

void UTextureRenderTarget2D::UpdateResourceImmediate(bool bClearRenderTarget/*=true*/)
{
	ENQUEUE_UNIQUE_RENDER_COMMAND_TWOPARAMETER(
		UpdateResourceImmediate,
		FRenderResource*,Resource,Resource,
		bool, bClearRenderTarget, bClearRenderTarget,
		{
		static_cast<FTextureRenderTarget2DResource*>(Resource)->UpdateDeferredResource(RHICmdList, bClearRenderTarget);
		}
	);
}

#if WITH_EDITOR
void UTextureRenderTarget2D::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const int32 MaxSize = 2048; 
	EPixelFormat Format = GetFormat();
	SizeX = FMath::Clamp<int32>(SizeX - (SizeX % GPixelFormats[Format].BlockSizeX),1,MaxSize);
	SizeY = FMath::Clamp<int32>(SizeY - (SizeY % GPixelFormats[Format].BlockSizeY),1,MaxSize);

	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif // WITH_EDITOR

void UTextureRenderTarget2D::PostLoad()
{
	if (!FPlatformProperties::SupportsWindowedMode())
	{
		// Clamp the render target size in order to avoid reallocating the scene render targets,
		// before the FTextureRenderTarget2DResource() is created in Super::PostLoad().
		SizeX = FMath::Min<int32>(SizeX, GSystemResolution.ResX);
		SizeY = FMath::Min<int32>(SizeY, GSystemResolution.ResY);
	}

	Super::PostLoad();
}


FString UTextureRenderTarget2D::GetDesc()	
{
	// size and format string
	return FString::Printf( TEXT("Render to Texture %dx%d[%s]"), SizeX, SizeY, GPixelFormats[GetFormat()].Name );
}

UTexture2D* UTextureRenderTarget2D::ConstructTexture2D(UObject* Outer, const FString& NewTexName, EObjectFlags ObjectFlags, uint32 Flags, TArray<uint8>* AlphaOverride)
{
	UTexture2D* Result = NULL;
#if WITH_EDITOR
	// Check render target size is valid and power of two.
	const bool bIsValidSize = (SizeX != 0 && !(SizeX & (SizeX - 1)) &&
		SizeY != 0 && !(SizeY & (SizeY - 1)));
	// The r2t resource will be needed to read its surface contents
	FRenderTarget* RenderTarget = GameThread_GetRenderTargetResource();

	const EPixelFormat PixelFormat = GetFormat();
	ETextureSourceFormat TextureFormat = TSF_Invalid;
	TextureCompressionSettings CompressionSettings = TC_Default;
	switch (PixelFormat)
	{
		case PF_B8G8R8A8:
			TextureFormat = TSF_BGRA8;
		break;
		case PF_FloatRGBA:
			TextureFormat = TSF_RGBA16F;
			CompressionSettings = TC_HDR;
		break;
	}

	// exit if source is not compatible.
	if (bIsValidSize == false || RenderTarget == NULL || TextureFormat == TSF_Invalid)
	{
		return Result;
	}

	// create the 2d texture
	Result = NewObject<UTexture2D>(Outer, FName(*NewTexName), ObjectFlags);
	// init to the same size as the 2d texture
	Result->Source.Init(SizeX, SizeY, 1, 1, TextureFormat);

	uint32* TextureData = (uint32*)Result->Source.LockMip(0);
	const int32 TextureDataSize = Result->Source.CalcMipSize(0);

	// read the 2d surface
	if (TextureFormat == TSF_BGRA8)
	{
		TArray<FColor> SurfData;
		RenderTarget->ReadPixels(SurfData);
		// override the alpha if desired
		if (AlphaOverride)
		{
			check(SurfData.Num() == AlphaOverride->Num());
			for (int32 Pixel = 0; Pixel < SurfData.Num(); Pixel++)
			{
				SurfData[Pixel].A = (*AlphaOverride)[Pixel];
			}
		}
		else if (Flags & CTF_RemapAlphaAsMasked)
		{
			// if the target was rendered with a masked texture, then the depth will probably have been written instead of 0/255 for the
			// alpha, and the depth when unwritten will be 255, so remap 255 to 0 (masked out area) and anything else as 255 (written to area)
			for (int32 Pixel = 0; Pixel < SurfData.Num(); Pixel++)
			{
				SurfData[Pixel].A = (SurfData[Pixel].A == 255) ? 0 : 255;
			}
		}
		else if (Flags & CTF_ForceOpaque)
		{
			for (int32 Pixel = 0; Pixel < SurfData.Num(); Pixel++)
			{
				SurfData[Pixel].A = 255;
			}
		}
		// copy the 2d surface data to the first mip of the static 2d texture
		check(TextureDataSize == SurfData.Num()*sizeof(FColor));
		FMemory::Memcpy(TextureData, SurfData.GetData(), TextureDataSize);
	}
	else if (TextureFormat == TSF_RGBA16F)
	{
		TArray<FFloat16Color> SurfData;
		RenderTarget->ReadFloat16Pixels(SurfData);
		// override the alpha if desired
		if (AlphaOverride)
		{
			check(SurfData.Num() == AlphaOverride->Num());
			for (int32 Pixel = 0; Pixel < SurfData.Num(); Pixel++)
			{
				SurfData[Pixel].A = ((float)(*AlphaOverride)[Pixel]) / 255.0f;
			}
		}
		else if (Flags & CTF_RemapAlphaAsMasked)
		{
			// if the target was rendered with a masked texture, then the depth will probably have been written instead of 0/255 for the
			// alpha, and the depth when unwritten will be 255, so remap 255 to 0 (masked out area) and anything else as 1 (written to area)
			for (int32 Pixel = 0; Pixel < SurfData.Num(); Pixel++)
			{
				SurfData[Pixel].A = (SurfData[Pixel].A == 255) ? 0.0f : 1.0f;
			}
		}
		else if (Flags & CTF_ForceOpaque)
		{
			for (int32 Pixel = 0; Pixel < SurfData.Num(); Pixel++)
			{
				SurfData[Pixel].A = 1.0f;
			}
		}
		// copy the 2d surface data to the first mip of the static 2d texture
		check(TextureDataSize == SurfData.Num()*sizeof(FFloat16Color));
		FMemory::Memcpy(TextureData, SurfData.GetData(), TextureDataSize);
	}
	Result->Source.UnlockMip(0);
	// if render target gamma used was 1.0 then disable SRGB for the static texture
	if (FMath::Abs(RenderTarget->GetDisplayGamma() - 1.0f) < KINDA_SMALL_NUMBER)
	{
		Flags &= ~CTF_SRGB;
	}

	Result->SRGB = (Flags & CTF_SRGB) != 0;
	Result->MipGenSettings = TMGS_FromTextureGroup;

	if ((Flags & CTF_AllowMips) == 0)
	{
		Result->MipGenSettings = TMGS_NoMipmaps;
	}

	Result->CompressionSettings = CompressionSettings;
	if (Flags & CTF_Compress)
	{
		// Set compression options.
		Result->DeferCompression = (Flags & CTF_DeferCompression) ? true : false;
	}
	else
	{
		// Disable compression
		Result->CompressionNone = true;
		Result->DeferCompression = false;
	}
	Result->PostEditChange();
#endif
	return Result;
}

/*-----------------------------------------------------------------------------
	FTextureRenderTarget2DResource
-----------------------------------------------------------------------------*/

FTextureRenderTarget2DResource::FTextureRenderTarget2DResource(const class UTextureRenderTarget2D* InOwner)
	:	Owner(InOwner)
	,	ClearColor(InOwner->ClearColor)
	,	Format(InOwner->GetFormat())
	,	TargetSizeX(Owner->SizeX)
	,	TargetSizeY(Owner->SizeY)
{
}

/**
 * Clamp size of the render target resource to max values
 *
 * @param MaxSizeX max allowed width
 * @param MaxSizeY max allowed height
 */
void FTextureRenderTarget2DResource::ClampSize(int32 MaxSizeX,int32 MaxSizeY)
{
	// upsize to go back to original or downsize to clamp to max
 	int32 NewSizeX = FMath::Min<int32>(Owner->SizeX,MaxSizeX);
 	int32 NewSizeY = FMath::Min<int32>(Owner->SizeY,MaxSizeY);
	if (NewSizeX != TargetSizeX || NewSizeY != TargetSizeY)
	{
		TargetSizeX = NewSizeX;
		TargetSizeY = NewSizeY;
		// reinit the resource with new TargetSizeX,TargetSizeY
		UpdateRHI();
	}	
}

/**
 * Initializes the RHI render target resources used by this resource.
 * Called when the resource is initialized, or when reseting all RHI resources.
 * This is only called by the rendering thread.
 */
void FTextureRenderTarget2DResource::InitDynamicRHI()
{
	if( TargetSizeX > 0 && TargetSizeY > 0 )
	{
		bool bSRGB=true;
		// if render target gamma used was 1.0 then disable SRGB for the static texture
		if( FMath::Abs(GetDisplayGamma() - 1.0f) < KINDA_SMALL_NUMBER )
		{
			bSRGB = false;
		}

		// Create the RHI texture. Only one mip is used and the texture is targetable for resolve.
		uint32 TexCreateFlags = bSRGB ? TexCreate_SRGB : 0;
		FRHIResourceCreateInfo CreateInfo = FRHIResourceCreateInfo(FClearValueBinding(ClearColor));

		RHICreateTargetableShaderResource2D(
			Owner->SizeX, 
			Owner->SizeY, 
			Format,
			1,
			TexCreateFlags,
			TexCreate_RenderTargetable,
			Owner->bNeedsTwoCopies,
			CreateInfo,
			RenderTargetTextureRHI,
			Texture2DRHI
			);
		TextureRHI = (FTextureRHIRef&)Texture2DRHI;
		RHIUpdateTextureReference(Owner->TextureReference.TextureReferenceRHI,TextureRHI);

		AddToDeferredUpdateList(true);
	}

	// Create the sampler state RHI resource.
	FSamplerStateInitializerRHI SamplerStateInitializer
	(
		(ESamplerFilter)UDeviceProfileManager::Get().GetActiveProfile()->GetTextureLODSettings()->GetSamplerFilter( Owner ),
		Owner->AddressX == TA_Wrap ? AM_Wrap : (Owner->AddressX == TA_Clamp ? AM_Clamp : AM_Mirror),
		Owner->AddressY == TA_Wrap ? AM_Wrap : (Owner->AddressY == TA_Clamp ? AM_Clamp : AM_Mirror),
		AM_Wrap
	);
	SamplerStateRHI = RHICreateSamplerState( SamplerStateInitializer );
}

/**
 * Release the RHI render target resources used by this resource.
 * Called when the resource is released, or when reseting all RHI resources.
 * This is only called by the rendering thread.
 */
void FTextureRenderTarget2DResource::ReleaseDynamicRHI()
{
	// release the FTexture RHI resources here as well
	ReleaseRHI();

	RHIUpdateTextureReference(Owner->TextureReference.TextureReferenceRHI,FTextureRHIParamRef());
	Texture2DRHI.SafeRelease();
	RenderTargetTextureRHI.SafeRelease();	

	// remove grom global list of deferred clears
	RemoveFromDeferredUpdateList();
}

/**
 * Updates (resolves) the render target texture.
 * Optionally clears the contents of the render target to green.
 * This is only called by the rendering thread.
 */
void FTextureRenderTarget2DResource::UpdateDeferredResource( FRHICommandListImmediate& RHICmdList, bool bClearRenderTarget/*=true*/ )
{
	RemoveFromDeferredUpdateList();

 	// clear the target surface to green
	if (bClearRenderTarget)
	{
		RHICmdList.SetViewport(0, 0, 0.0f, TargetSizeX, TargetSizeY, 1.0f);
		ensure(RenderTargetTextureRHI->GetClearColor() == ClearColor);
		SetRenderTarget(RHICmdList, RenderTargetTextureRHI, FTextureRHIRef(), ESimpleRenderTargetMode::EClearColorExistingDepth);
	}
 
 	// copy surface to the texture for use
	RHICmdList.CopyToResolveTarget(RenderTargetTextureRHI, TextureRHI, true, FResolveParams());
}

/** 
 * @return width of target surface
 */

FIntPoint FTextureRenderTarget2DResource::GetSizeXY() const
{ 
	return FIntPoint(TargetSizeX, TargetSizeY); 
}

/** 
* Render target resource should be sampled in linear color space
*
* @return display gamma expected for rendering to this render target 
*/
float FTextureRenderTarget2DResource::GetDisplayGamma() const
{
	if (Owner->TargetGamma > KINDA_SMALL_NUMBER * 10.0f)
	{
		return Owner->TargetGamma;
	}
	if (Format == PF_FloatRGB || Format == PF_FloatRGBA || Owner->bForceLinearGamma )
	{
		return 1.0f;
	}
	return FTextureRenderTargetResource::GetDisplayGamma();
}
