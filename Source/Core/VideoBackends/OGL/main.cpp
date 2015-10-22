// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.



// OpenGL Backend Documentation
/*

1.1 Display settings

Internal and fullscreen resolution: Since the only internal resolutions allowed
are also fullscreen resolution allowed by the system there is only need for one
resolution setting that applies to both the internal resolution and the
fullscreen resolution.  - Apparently no, someone else doesn't agree

Todo: Make the internal resolution option apply instantly, currently only the
native and 2x option applies instantly. To do this we need to be able to change
the reinitialize FramebufferManager:Init() while a game is running.

1.2 Screenshots


The screenshots should be taken from the internal representation of the picture
regardless of what the current window size is. Since AA and wireframe is
applied together with the picture resizing this rule is not currently applied
to AA or wireframe pictures, they are instead taken from whatever the window
size is.

Todo: Render AA and wireframe to a separate picture used for the screenshot in
addition to the one for display.

1.3 AA

Make AA apply instantly during gameplay if possible

*/

#include <algorithm>
#include <cstdarg>
#include <regex>

#include "Common/Atomic.h"
#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/Thread.h"
#include "Common/Logging/LogManager.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Host.h"

#include "VideoBackends/OGL/BoundingBox.h"
#include "VideoBackends/OGL/FramebufferManager.h"
#include "VideoBackends/OGL/GLInterfaceBase.h"
#include "VideoBackends/OGL/GLUtil.h"
#include "VideoBackends/OGL/PerfQuery.h"
#include "VideoBackends/OGL/PostProcessing.h"
#include "VideoBackends/OGL/ProgramShaderCache.h"
#include "VideoBackends/OGL/Render.h"
#include "VideoBackends/OGL/SamplerCache.h"
#include "VideoBackends/OGL/TextureCache.h"
#include "VideoBackends/OGL/TextureConverter.h"
#include "VideoBackends/OGL/VertexManager.h"
#include "VideoBackends/OGL/VideoBackend.h"

#include "VideoCommon/BPStructs.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/ImageWrite.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/LookUpTables.h"
#include "VideoCommon/MainBase.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoState.h"

#include "DumpFrame.h"

namespace OGL
{

std::string VideoBackend::GetName() const
{
	return "OGL";
}

std::string VideoBackend::GetDisplayName() const
{
	if (GLInterface != nullptr && GLInterface->GetMode() == GLInterfaceMode::MODE_OPENGLES3)
		return "OpenGLES";
	else
		return "OpenGL";
}

static std::vector<std::string> GetShaders(const std::string &sub_dir = "")
{
	std::vector<std::string> paths = DoFileSearch({"*.glsl"}, {
		File::GetUserPath(D_SHADERS_IDX) + sub_dir,
		File::GetSysDirectory() + SHADERS_DIR DIR_SEP + sub_dir
	});
	std::vector<std::string> result;
	for (std::string path : paths)
		result.push_back(std::regex_replace(path, std::regex("^.*/(.*)\\.glsl$"), "$1"));
	return result;
}

static void InitBackendInfo()
{
	g_Config.backend_info.APIType = API_OPENGL;
	g_Config.backend_info.bSupportsExclusiveFullscreen = false;
	g_Config.backend_info.bSupportsOversizedViewports = true;
	g_Config.backend_info.bSupportsGeometryShaders = true;
	g_Config.backend_info.bSupports3DVision = false;
	g_Config.backend_info.bSupportsPostProcessing = true;

	g_Config.backend_info.Adapters.clear();

	// aamodes
	const char* caamodes[] = {_trans("None"), "2x", "4x", "8x", "4x SSAA"};
	g_Config.backend_info.AAModes.assign(caamodes, caamodes + sizeof(caamodes)/sizeof(*caamodes));

	// pp shaders
	g_Config.backend_info.PPShaders = GetShaders("");
	g_Config.backend_info.AnaglyphShaders = GetShaders(ANAGLYPH_DIR DIR_SEP);
}

void VideoBackend::ShowConfig(void *_hParent)
{
	if (!s_BackendInitialized)
		InitBackendInfo();
	Host_ShowVideoConfig(_hParent, GetDisplayName(), "gfx_opengl");
}

bool VideoBackend::Initialize(void *window_handle)
{
	InitializeShared();
	InitBackendInfo();

	frameCount = 0;

	g_Config.Load(File::GetUserPath(D_CONFIG_IDX) + "gfx_opengl.ini");
	g_Config.GameIniLoad();
	g_Config.UpdateProjectionHack();
	g_Config.VerifyValidity();
	UpdateActiveConfig();

	InitInterface();
	GLInterface->SetMode(GLInterfaceMode::MODE_DETECT);
	if (!GLInterface->Create(window_handle))
		return false;

	// Do our OSD callbacks
	OSD::DoCallbacks(OSD::OSD_INIT);

	s_BackendInitialized = true;

	return true;
}

int dumpframestate = 0;
int dumpframeconstants = 0;
FILE *dumpframefile = 0;
int dumpframecount = 0;
int dumpedshadercount = 0;
int currentshaderid = 0;
void writepad(void)
{
	if(!dumpframefile) return;
	int v = ftell(dumpframefile)&3;
	if(v)
	{
		while(v++<4) fputc(0, dumpframefile);
	}
}
void write32(u32 v)
{
	if(dumpframefile)
		fwrite(&v, sizeof(v), 1, dumpframefile);
}
void write4c(const char *s)
{
	if(strlen(s)<4) return;
	u32 v = (s[0]<<24) | (s[1]<<16) | (s[2]<<8) | s[3];
	write32(v);
}
void dumpframestart(void)
{
	if(dumpframestate > 0)
	{
		--dumpframestate;
		if(dumpframestate == 1)
		{
			dumpframeconstants=1;
			dumpedshadercount=0;
			char tempname[64];
			snprintf(tempname, sizeof(tempname), "/tmp/dumpframe%04d.bin",
				dumpframecount++);
			dumpframefile = fopen(tempname, "wb");
			printf("Dumping frame to file %s\n", tempname);
			write4c("Ddv0");
		} else if(dumpframestate == 0)
		{
			fclose(dumpframefile);
			dumpframefile = 0;
		}
	}
}

#define MAXDUMPEDSHADERS 1024
struct dumpedshaderinfo {
	DSTALPHA_MODE dstAlphaMode;
	u32 components;
	u32 primitive_type;
} dumpedshaders[MAXDUMPEDSHADERS];

// returns 0 or 1 depending on whether it's a new entry.
int dumpedshaderid(DSTALPHA_MODE dstAlphaMode, u32 components, u32 primitive_type)
{
	int i;
	struct dumpedshaderinfo *d;
	for(i=0;i<dumpedshadercount;++i)
	{
		d = dumpedshaders+i;
		if(d->dstAlphaMode == dstAlphaMode && d->components == components && d->primitive_type==primitive_type)
		{
			currentshaderid = i;
			return 0;
		}
	}
	if(i==MAXDUMPEDSHADERS) return -1;
	i = dumpedshadercount++;
	currentshaderid = i;
	d = dumpedshaders + i;
	d->dstAlphaMode = dstAlphaMode;
	d->components = components;
	d->primitive_type = primitive_type;
	return i;
}


// Dumpframe format. All multi-byte values are in native endian order
// String ID's are 4 bytes output as a value where the first character
// is shifted left 24 bits and the last character is in the LSB position.
// FORMAT OF DUMP FILE:
// String ID "Ddv0"
// Any number of 4 byte String ID followed by 4 bytes of bytecount
//    that many bytes of data, format depends on String ID
//    padding null bytes in order that we start on an even 4-byte location 
// vdcl #### vtx_decl AttributeFormat structures position*1, normals*3, colors*2, texcoords*8,posmtx*1
// vrtx #### u32 stride then_all_the_vertex_data       (The vertex count is (####-4) / stride)
// indx #### u16[]_index_values    (The index count is #### / 2)
// cnst #### the uniform constant blocks for pixel, vertex, geometry shaders
// shad #### Two null terminated strings, the pixel + vertex shaders
// draw #### u32_primitive u32_shaderid        (The primitive is GL_POINTS, GL_LINES, GL_TRIANGLE_STRIP, etc.)

void VideoBackend::Video_DumpFrame()
{
	printf("Video_DumpFrame!\n");
	if(dumpframestate==0) dumpframestate = 2;
}

// This is called after Initialize() from the Core
// Run from the graphics thread
void VideoBackend::Video_Prepare()
{
	GLInterface->MakeCurrent();

	g_renderer = new Renderer;

	CommandProcessor::Init();
	PixelEngine::Init();

	BPInit();
	g_vertex_manager = new VertexManager;
	g_perf_query = GetPerfQuery();
	Fifo_Init(); // must be done before OpcodeDecoder_Init()
	OpcodeDecoder_Init();
	IndexGenerator::Init();
	VertexShaderManager::Init();
	PixelShaderManager::Init();
	GeometryShaderManager::Init();
	ProgramShaderCache::Init();
	g_texture_cache = new TextureCache();
	g_sampler_cache = new SamplerCache();
	Renderer::Init();
	VertexLoaderManager::Init();
	TextureConverter::Init();
	BoundingBox::Init();

	// Notify the core that the video backend is ready
	Host_Message(WM_USER_CREATE);
}

void VideoBackend::Shutdown()
{
	s_BackendInitialized = false;

	// Do our OSD callbacks
	OSD::DoCallbacks(OSD::OSD_SHUTDOWN);

	GLInterface->Shutdown();
	delete GLInterface;
	GLInterface = nullptr;
}

void VideoBackend::Video_Cleanup()
{
	if (g_renderer)
	{
		Fifo_Shutdown();

		// The following calls are NOT Thread Safe
		// And need to be called from the video thread
		Renderer::Shutdown();
		BoundingBox::Shutdown();
		TextureConverter::Shutdown();
		VertexLoaderManager::Shutdown();
		delete g_sampler_cache;
		g_sampler_cache = nullptr;
		delete g_texture_cache;
		g_texture_cache = nullptr;
		ProgramShaderCache::Shutdown();
		VertexShaderManager::Shutdown();
		PixelShaderManager::Shutdown();
		GeometryShaderManager::Shutdown();
		delete g_perf_query;
		g_perf_query = nullptr;
		delete g_vertex_manager;
		g_vertex_manager = nullptr;
		OpcodeDecoder_Shutdown();
		delete g_renderer;
		g_renderer = nullptr;
		GLInterface->ClearCurrent();
	}
}

}
