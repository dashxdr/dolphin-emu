namespace OGL
{
extern int dumpframestate;
extern int dumpframecount;
extern FILE *dumpframefile;
extern void write32(u32 v);
extern void write4c(const char *s);
extern void writepad(void);
extern int dumpframeconstants;
extern int dumpedshadercount;
extern int currentshaderid;
extern void dumpframestart(void);
extern int dumpedshaderid(DSTALPHA_MODE dstAlphaMode, u32 components, u32 primitive_type);
extern void dumpframe_bindtexture(int ndx, std::string basename);
extern void dumpframe_textures(void);
extern void dumpframe_texturepath(std::string pathname);

extern struct vpt {
	float xorig, yorig;
	float width, height;
	int scissorxoff, scissoryoff;
	float near, far;
	int depthEnable, depthMask;
	GLint depthfunc;
// blend settings
	int blendenable;
	GLenum blendequation, blendequationAlpha;
	GLenum srcFactor, dstFactor, srcFactorAlpha, dstFactorAlpha;
	int logicop;
// clear screen settings
	int colorEnable, alphaEnable, zEnable;
	uint32_t clearcolor;
	float cleardepth;
// culling
	int cullmode;
// colormasks
	int ColorMask, AlphaMask;
} new_vpt, old_vpt;

}
