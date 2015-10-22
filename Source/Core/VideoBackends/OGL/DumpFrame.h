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
}
