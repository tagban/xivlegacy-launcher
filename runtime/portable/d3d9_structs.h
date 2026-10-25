/* D3D9 structure offsets and sizes (32-bit), computed from Wine's d3d9types.h / d3d9caps.h. */
#pragma once

#define D3DCAPS9_SIZE 304
#define D3DCAPS9_DeviceType 0
#define D3DCAPS9_AdapterOrdinal 4
#define D3DCAPS9_Caps 8
#define D3DCAPS9_Caps2 12
#define D3DCAPS9_Caps3 16
#define D3DCAPS9_PresentationIntervals 20
#define D3DCAPS9_CursorCaps 24
#define D3DCAPS9_DevCaps 28
#define D3DCAPS9_PrimitiveMiscCaps 32
#define D3DCAPS9_RasterCaps 36
#define D3DCAPS9_ZCmpCaps 40
#define D3DCAPS9_SrcBlendCaps 44
#define D3DCAPS9_DestBlendCaps 48
#define D3DCAPS9_AlphaCmpCaps 52
#define D3DCAPS9_ShadeCaps 56
#define D3DCAPS9_TextureCaps 60
#define D3DCAPS9_TextureFilterCaps 64
#define D3DCAPS9_CubeTextureFilterCaps 68
#define D3DCAPS9_VolumeTextureFilterCaps 72
#define D3DCAPS9_TextureAddressCaps 76
#define D3DCAPS9_VolumeTextureAddressCaps 80
#define D3DCAPS9_LineCaps 84
#define D3DCAPS9_MaxTextureWidth 88
#define D3DCAPS9_MaxTextureHeight 92
#define D3DCAPS9_MaxVolumeExtent 96
#define D3DCAPS9_MaxTextureRepeat 100
#define D3DCAPS9_MaxTextureAspectRatio 104
#define D3DCAPS9_MaxAnisotropy 108
#define D3DCAPS9_MaxVertexW 112
#define D3DCAPS9_GuardBandLeft 116
#define D3DCAPS9_GuardBandTop 120
#define D3DCAPS9_GuardBandRight 124
#define D3DCAPS9_GuardBandBottom 128
#define D3DCAPS9_ExtentsAdjust 132
#define D3DCAPS9_StencilCaps 136
#define D3DCAPS9_FVFCaps 140
#define D3DCAPS9_TextureOpCaps 144
#define D3DCAPS9_MaxTextureBlendStages 148
#define D3DCAPS9_MaxSimultaneousTextures 152
#define D3DCAPS9_VertexProcessingCaps 156
#define D3DCAPS9_MaxActiveLights 160
#define D3DCAPS9_MaxUserClipPlanes 164
#define D3DCAPS9_MaxVertexBlendMatrices 168
#define D3DCAPS9_MaxVertexBlendMatrixIndex 172
#define D3DCAPS9_MaxPointSize 176
#define D3DCAPS9_MaxPrimitiveCount 180
#define D3DCAPS9_MaxVertexIndex 184
#define D3DCAPS9_MaxStreams 188
#define D3DCAPS9_MaxStreamStride 192
#define D3DCAPS9_VertexShaderVersion 196
#define D3DCAPS9_MaxVertexShaderConst 200
#define D3DCAPS9_PixelShaderVersion 204
#define D3DCAPS9_PixelShader1xMaxValue 208
#define D3DCAPS9_DevCaps2 212
#define D3DCAPS9_MaxNpatchTessellationLevel 216
#define D3DCAPS9_Reserved5 220
#define D3DCAPS9_MasterAdapterOrdinal 224
#define D3DCAPS9_AdapterOrdinalInGroup 228
#define D3DCAPS9_NumberOfAdaptersInGroup 232
#define D3DCAPS9_DeclTypes 236
#define D3DCAPS9_NumSimultaneousRTs 240
#define D3DCAPS9_StretchRectFilterCaps 244
#define D3DCAPS9_VS20Caps 248
#define D3DCAPS9_PS20Caps 264
#define D3DCAPS9_VertexTextureFilterCaps 284
#define D3DCAPS9_MaxVShaderInstructionsExecuted 288
#define D3DCAPS9_MaxPShaderInstructionsExecuted 292
#define D3DCAPS9_MaxVertexShader30InstructionSlots 296
#define D3DCAPS9_MaxPixelShader30InstructionSlots 300

#define D3DPRESENT_PARAMETERS_SIZE 56
#define D3DPRESENT_PARAMETERS_BackBufferWidth 0
#define D3DPRESENT_PARAMETERS_BackBufferHeight 4
#define D3DPRESENT_PARAMETERS_BackBufferFormat 8
#define D3DPRESENT_PARAMETERS_BackBufferCount 12
#define D3DPRESENT_PARAMETERS_MultiSampleType 16
#define D3DPRESENT_PARAMETERS_MultiSampleQuality 20
#define D3DPRESENT_PARAMETERS_SwapEffect 24
#define D3DPRESENT_PARAMETERS_hDeviceWindow 28
#define D3DPRESENT_PARAMETERS_Windowed 32
#define D3DPRESENT_PARAMETERS_EnableAutoDepthStencil 36
#define D3DPRESENT_PARAMETERS_AutoDepthStencilFormat 40
#define D3DPRESENT_PARAMETERS_Flags 44
#define D3DPRESENT_PARAMETERS_FullScreen_RefreshRateInHz 48
#define D3DPRESENT_PARAMETERS_PresentationInterval 52

#define D3DADAPTER_IDENTIFIER9_SIZE 1104
#define D3DADAPTER_IDENTIFIER9_Driver 0
#define D3DADAPTER_IDENTIFIER9_Description 512
#define D3DADAPTER_IDENTIFIER9_DeviceName 1024
#define D3DADAPTER_IDENTIFIER9_DriverVersion 1056
#define D3DADAPTER_IDENTIFIER9_VendorId 1064
#define D3DADAPTER_IDENTIFIER9_DeviceId 1068
#define D3DADAPTER_IDENTIFIER9_SubSysId 1072
#define D3DADAPTER_IDENTIFIER9_Revision 1076
#define D3DADAPTER_IDENTIFIER9_DeviceIdentifier 1080
#define D3DADAPTER_IDENTIFIER9_WHQLLevel 1096

#define D3DDISPLAYMODE_SIZE 16
#define D3DDISPLAYMODE_Width 0
#define D3DDISPLAYMODE_Height 4
#define D3DDISPLAYMODE_RefreshRate 8
#define D3DDISPLAYMODE_Format 12

#define D3DSURFACE_DESC_SIZE 32
#define D3DSURFACE_DESC_Format 0
#define D3DSURFACE_DESC_Type 4
#define D3DSURFACE_DESC_Usage 8
#define D3DSURFACE_DESC_Pool 12
#define D3DSURFACE_DESC_MultiSampleType 16
#define D3DSURFACE_DESC_MultiSampleQuality 20
#define D3DSURFACE_DESC_Width 24
#define D3DSURFACE_DESC_Height 28

#define D3DLOCKED_RECT_SIZE 8
#define D3DLOCKED_RECT_Pitch 0
#define D3DLOCKED_RECT_pBits 4

#define D3DVERTEXBUFFER_DESC_SIZE 24
#define D3DVERTEXBUFFER_DESC_Format 0
#define D3DVERTEXBUFFER_DESC_Type 4
#define D3DVERTEXBUFFER_DESC_Usage 8
#define D3DVERTEXBUFFER_DESC_Pool 12
#define D3DVERTEXBUFFER_DESC_Size 16
#define D3DVERTEXBUFFER_DESC_FVF 20

#define D3DINDEXBUFFER_DESC_SIZE 20
#define D3DINDEXBUFFER_DESC_Format 0
#define D3DINDEXBUFFER_DESC_Type 4
#define D3DINDEXBUFFER_DESC_Usage 8
#define D3DINDEXBUFFER_DESC_Pool 12
#define D3DINDEXBUFFER_DESC_Size 16

#define D3DVIEWPORT9_SIZE 24
#define D3DVIEWPORT9_X 0
#define D3DVIEWPORT9_Y 4
#define D3DVIEWPORT9_Width 8
#define D3DVIEWPORT9_Height 12
#define D3DVIEWPORT9_MinZ 16
#define D3DVIEWPORT9_MaxZ 20

#define D3DMATERIAL9_SIZE 68
#define D3DMATERIAL9_Diffuse 0
#define D3DMATERIAL9_Ambient 16
#define D3DMATERIAL9_Specular 32
#define D3DMATERIAL9_Emissive 48
#define D3DMATERIAL9_Power 64

#define D3DLIGHT9_SIZE 104
#define D3DLIGHT9_Type 0
#define D3DLIGHT9_Diffuse 4
#define D3DLIGHT9_Specular 20
#define D3DLIGHT9_Ambient 36
#define D3DLIGHT9_Position 52
#define D3DLIGHT9_Direction 64
#define D3DLIGHT9_Range 76
#define D3DLIGHT9_Falloff 80
#define D3DLIGHT9_Attenuation0 84
#define D3DLIGHT9_Attenuation1 88
#define D3DLIGHT9_Attenuation2 92
#define D3DLIGHT9_Theta 96
#define D3DLIGHT9_Phi 100

#define D3DVERTEXELEMENT9_SIZE 8
#define D3DVERTEXELEMENT9_Stream 0
#define D3DVERTEXELEMENT9_Offset 2
#define D3DVERTEXELEMENT9_Type 4
#define D3DVERTEXELEMENT9_Method 5
#define D3DVERTEXELEMENT9_Usage 6
#define D3DVERTEXELEMENT9_UsageIndex 7

#define D3DRASTER_STATUS_SIZE 8
#define D3DRASTER_STATUS_InVBlank 0
#define D3DRASTER_STATUS_ScanLine 4

#define D3DLOCKED_BOX_SIZE 12
#define D3DLOCKED_BOX_RowPitch 0
#define D3DLOCKED_BOX_SlicePitch 4
#define D3DLOCKED_BOX_pBits 8

#define D3DVOLUME_DESC_SIZE 28
#define D3DVOLUME_DESC_Format 0
#define D3DVOLUME_DESC_Type 4
#define D3DVOLUME_DESC_Usage 8
#define D3DVOLUME_DESC_Pool 12
#define D3DVOLUME_DESC_Width 16
#define D3DVOLUME_DESC_Height 20
#define D3DVOLUME_DESC_Depth 24

#define D3DBOX_SIZE 24
#define D3DBOX_Left 0
#define D3DBOX_Top 4
#define D3DBOX_Right 8
#define D3DBOX_Bottom 12
#define D3DBOX_Front 16
#define D3DBOX_Back 20

#define D3DDEVICE_CREATION_PARAMETERS_SIZE 16
#define D3DDEVICE_CREATION_PARAMETERS_AdapterOrdinal 0
#define D3DDEVICE_CREATION_PARAMETERS_DeviceType 4
#define D3DDEVICE_CREATION_PARAMETERS_hFocusWindow 8
#define D3DDEVICE_CREATION_PARAMETERS_BehaviorFlags 12

#define D3DCLIPSTATUS9_SIZE 8
#define D3DCLIPSTATUS9_ClipUnion 0
#define D3DCLIPSTATUS9_ClipIntersection 4

