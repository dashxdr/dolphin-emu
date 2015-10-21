// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <fstream>
#include <string>
#include <vector>

#include "Common/FileUtil.h"
#include "Common/MemoryUtil.h"
#include "Common/StringUtil.h"

#include "VideoBackends/OGL/main.h"
#include "VideoBackends/OGL/ProgramShaderCache.h"
#include "VideoBackends/OGL/Render.h"
#include "VideoBackends/OGL/StreamBuffer.h"
#include "VideoBackends/OGL/TextureCache.h"
#include "VideoBackends/OGL/VertexManager.h"

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/ImageWrite.h"
#include "VideoCommon/IndexGenerator.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VertexShaderManager.h"
#include "VideoCommon/VideoConfig.h"

namespace OGL
{
//This are the initially requested size for the buffers expressed in bytes
const u32 MAX_IBUFFER_SIZE =  2*1024*1024;
const u32 MAX_VBUFFER_SIZE = 32*1024*1024;

static StreamBuffer *s_vertexBuffer;
static StreamBuffer *s_indexBuffer;
static size_t s_baseVertex;
static size_t s_index_offset;

VertexManager::VertexManager()
	: m_cpu_v_buffer(MAX_VBUFFER_SIZE), m_cpu_i_buffer(MAX_IBUFFER_SIZE)
{
	CreateDeviceObjects();
}

VertexManager::~VertexManager()
{
	DestroyDeviceObjects();
}

void VertexManager::CreateDeviceObjects()
{
	s_vertexBuffer = StreamBuffer::Create(GL_ARRAY_BUFFER, MAX_VBUFFER_SIZE);
	m_vertex_buffers = s_vertexBuffer->m_buffer;

	s_indexBuffer = StreamBuffer::Create(GL_ELEMENT_ARRAY_BUFFER, MAX_IBUFFER_SIZE);
	m_index_buffers = s_indexBuffer->m_buffer;

	m_last_vao = 0;
}

void VertexManager::DestroyDeviceObjects()
{
	delete s_vertexBuffer;
	delete s_indexBuffer;
}

extern int dumpframestate;
extern FILE *dumpframefile;

static void dumptype(u8 *p8, int n, int type)
{
	switch(type)
	{
	case 0: // U8
		p8 += n;
		fprintf(dumpframefile, "%d", *p8);
		break;
	case 1: // S8
		p8 += n;
		fprintf(dumpframefile, "%d", *(s8 *)p8);
		break;
	case 2: // U16
		p8 += n*2;
		fprintf(dumpframefile, "%d", *(u16 *)p8);
		break;
	case 3: // S16
		p8 += n*2;
		fprintf(dumpframefile, "%d", *(s16 *)p8);
		break;
	case 4: // F32
		p8 += n*4;
		fprintf(dumpframefile, "%f", *(float *)p8);
		break;
	}
}
static void DumpAttributeFormat(u8 *p8, AttributeFormat af[], int count, const char *id)
{
	const char *vartypes[5] = {"U8", "S8", "U16", "S16", "F32"};
	int i;
	for(i=0;i<count;++i)
	{
		if(!af[i].enable) continue;
		char idc[2] = {(char)('0' + i), 0};
		if(count==1) idc[0] = 0;
		const char *type = "unknown";
		if(af[i].type>=0 && af[i].type<5) type=vartypes[af[i].type];
		fprintf(dumpframefile, "%s%s: %s[%d]={", id, idc, type, af[i].components);
		int j;
		for(j=0;j<af[i].components;++j)
		{
			if(j>0) fprintf(dumpframefile, ",");
			dumptype(p8+af[i].offset, j, af[i].type);

		}
		fprintf(dumpframefile, "}\n");
	}
}
static void DumpAttributeFormat(u8 *p8, AttributeFormat af, int count, const char *id)
{
	AttributeFormat tf[1] = {af};
	DumpAttributeFormat(p8, tf, count, id);
}
void VertexManager::PrepareDrawBuffers(u32 stride)
{
	u32 vertex_data_size = IndexGenerator::GetNumVerts() * stride;
	u32 index_data_size = IndexGenerator::GetIndexLen() * sizeof(u16);
	if(dumpframestate==1)
	{
		int numverts = IndexGenerator::GetNumVerts();
//		printf("PrepareDrawBuffers: VAO(%2d) stride=%u, count=%d\n", m_last_vao, stride, numverts);
		fprintf(dumpframefile, "VERTEXLIST: %d[%d]\n", numverts, stride);
		GLVertexFormat *nativeVertexFmt = (GLVertexFormat*)VertexLoaderManager::GetCurrentVertexFormat();
		PortableVertexDeclaration vtx_decl = nativeVertexFmt->GetVertexDeclaration();
		int i;
		u8 *p8 = s_pBaseBufferPointer;// + stride * s_baseVertex;
		for(i=0;i<numverts;++i)
		{
			fprintf(dumpframefile, "Vertex%d:\n", i);
			DumpAttributeFormat(p8, vtx_decl.position, 1, "position");
			DumpAttributeFormat(p8, vtx_decl.normals, 3, "normals");
			DumpAttributeFormat(p8, vtx_decl.colors, 2, "colors");
			DumpAttributeFormat(p8, vtx_decl.texcoords, 8, "texcoords");
			DumpAttributeFormat(p8, vtx_decl.posmtx, 1, "posmtx");
			p8 += stride;
		}
		int index_size = IndexGenerator::GetIndexLen();
		fprintf(dumpframefile, "INDEXLIST: %d\n", index_size);
		for(i=0;i<index_size;++i)
		{
			if(i) fprintf(dumpframefile, ",");
			if((i&15)==15) fprintf(dumpframefile, "\n");
			fprintf(dumpframefile, "%d", s_pIndexBufferPointer[i]);
		}
		fprintf(dumpframefile, "\n");
	}
	s_vertexBuffer->Unmap(vertex_data_size);
	s_indexBuffer->Unmap(index_data_size);

	ADDSTAT(stats.thisFrame.bytesVertexStreamed, vertex_data_size);
	ADDSTAT(stats.thisFrame.bytesIndexStreamed, index_data_size);
}

void VertexManager::ResetBuffer(u32 stride)
{
	if (s_cull_all)
	{
		// This buffer isn't getting sent to the GPU. Just allocate it on the cpu.
		s_pCurBufferPointer = s_pBaseBufferPointer = m_cpu_v_buffer.data();
		s_pEndBufferPointer = s_pBaseBufferPointer + m_cpu_v_buffer.size();

		u16 *tp = m_cpu_i_buffer.data();
		s_pIndexBufferPointer = tp;

		IndexGenerator::Start((u16*)tp);
	}
	else
	{
		auto buffer = s_vertexBuffer->Map(MAXVBUFFERSIZE, stride);
		s_pCurBufferPointer = s_pBaseBufferPointer = buffer.first;
		s_pEndBufferPointer = buffer.first + MAXVBUFFERSIZE;
		s_baseVertex = buffer.second / stride;

		buffer = s_indexBuffer->Map(MAXIBUFFERSIZE * sizeof(u16));
		s_pIndexBufferPointer = (u16*)buffer.first;
		IndexGenerator::Start((u16*)buffer.first);
		s_index_offset = buffer.second;
	}
}

void VertexManager::Draw(u32 stride)
{
	u32 index_size = IndexGenerator::GetIndexLen();
	u32 max_index = IndexGenerator::GetNumVerts();
	GLenum primitive_mode = 0;

	switch (current_primitive_type)
	{
		case PRIMITIVE_POINTS:
			primitive_mode = GL_POINTS;
			glDisable(GL_CULL_FACE);
			break;
		case PRIMITIVE_LINES:
			primitive_mode = GL_LINES;
			glDisable(GL_CULL_FACE);
			break;
		case PRIMITIVE_TRIANGLES:
			primitive_mode = g_ActiveConfig.backend_info.bSupportsPrimitiveRestart ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
			break;
	}

	if (g_ogl_config.bSupportsGLBaseVertex)
	{
		if(dumpframestate==1)
		{
			const char *pname = "unknown";
			switch(current_primitive_type)
			{
			case PRIMITIVE_POINTS:
				pname = "points";
				break;
			case PRIMITIVE_LINES:
				pname = "lines";
				break;
			case PRIMITIVE_TRIANGLES:
				pname = (primitive_mode == GL_TRIANGLES) ? "triangles" : "triangle_strip";
				break;
			}
			fprintf(dumpframefile, "DRAW: primitive=%s\n", pname);
		}
		glDrawRangeElementsBaseVertex(primitive_mode, 0, max_index, index_size, GL_UNSIGNED_SHORT, (u8*)nullptr+s_index_offset, (GLint)s_baseVertex);
	}
	else
	{
		glDrawRangeElements(primitive_mode, 0, max_index, index_size, GL_UNSIGNED_SHORT, (u8*)nullptr+s_index_offset);
	}

	INCSTAT(stats.thisFrame.numDrawCalls);

	if (current_primitive_type != PRIMITIVE_TRIANGLES)
		((OGL::Renderer*)g_renderer)->SetGenerationMode();
}

void VertexManager::vFlush(bool useDstAlpha)
{
	GLVertexFormat *nativeVertexFmt = (GLVertexFormat*)VertexLoaderManager::GetCurrentVertexFormat();
	u32 stride  = nativeVertexFmt->GetVertexStride();

	if (m_last_vao != nativeVertexFmt->VAO)
	{
		glBindVertexArray(nativeVertexFmt->VAO);
		m_last_vao = nativeVertexFmt->VAO;
	}

	PrepareDrawBuffers(stride);

	// Makes sure we can actually do Dual source blending
	bool dualSourcePossible = g_ActiveConfig.backend_info.bSupportsDualSourceBlend;

	// If host supports GL_ARB_blend_func_extended, we can do dst alpha in
	// the same pass as regular rendering.
	if (useDstAlpha && dualSourcePossible)
	{
		ProgramShaderCache::SetShader(DSTALPHA_DUAL_SOURCE_BLEND, nativeVertexFmt->m_components, current_primitive_type);
	}
	else
	{
		ProgramShaderCache::SetShader(DSTALPHA_NONE, nativeVertexFmt->m_components, current_primitive_type);
	}

	// upload global constants
	ProgramShaderCache::UploadConstants();

	// setup the pointers
	nativeVertexFmt->SetupVertexPointers();

	Draw(stride);

	// run through vertex groups again to set alpha
	if (useDstAlpha && !dualSourcePossible)
	{
		ProgramShaderCache::SetShader(DSTALPHA_ALPHA_PASS, nativeVertexFmt->m_components, current_primitive_type);

		// only update alpha
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);

		glDisable(GL_BLEND);

		Draw(stride);

		// restore color mask
		g_renderer->SetColorMask();

		if (bpmem.blendmode.blendenable || bpmem.blendmode.subtract)
			glEnable(GL_BLEND);
	}

#if defined(_DEBUG) || defined(DEBUGFAST)
	if (g_ActiveConfig.iLog & CONF_SAVESHADERS)
	{
		// save the shaders
		ProgramShaderCache::PCacheEntry prog = ProgramShaderCache::GetShaderProgram();
		std::string filename = StringFromFormat("%sps%.3d.txt", File::GetUserPath(D_DUMPFRAMES_IDX).c_str(), g_ActiveConfig.iSaveTargetId);
		std::ofstream fps;
		OpenFStream(fps, filename, std::ios_base::out);
		fps << prog.shader.strpprog.c_str();

		filename = StringFromFormat("%svs%.3d.txt", File::GetUserPath(D_DUMPFRAMES_IDX).c_str(), g_ActiveConfig.iSaveTargetId);
		std::ofstream fvs;
		OpenFStream(fvs, filename, std::ios_base::out);
		fvs << prog.shader.strvprog.c_str();
	}

	if (g_ActiveConfig.iLog & CONF_SAVETARGETS)
	{
		std::string filename = StringFromFormat("%starg%.3d.png", File::GetUserPath(D_DUMPFRAMES_IDX).c_str(), g_ActiveConfig.iSaveTargetId);
		TargetRectangle tr;
		tr.left = 0;
		tr.right = Renderer::GetTargetWidth();
		tr.top = 0;
		tr.bottom = Renderer::GetTargetHeight();
		g_renderer->SaveScreenshot(filename, tr);
	}
#endif
	g_Config.iSaveTargetId++;

	ClearEFBCache();
}


}  // namespace
