// TextureDiffusion3DSettings.cpp

#include "TextureDiffusion3DSettings.h"

UTextureDiffusion3DSettings::UTextureDiffusion3DSettings()
{
    // ComfyUI Settings
    ComfyUIServerAddress = TEXT("http://127.0.0.1:8188/");

    // Workflow Defaults
    InitialProjectionWorkflow.FilePath = TEXT("Juggernaut_ControlNet.json");
    InpaintProjectionWorkflow.FilePath = TEXT("Multiview_Inpaint_IpAdapter_Depth_Canny_Appearance.json");
    
}