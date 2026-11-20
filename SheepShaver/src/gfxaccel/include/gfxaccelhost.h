#ifndef GFXACCELHOST_H
#define GFXACCELHOST_H

#include <stdint.h>

#define GFXACCELHOSTUNSIGNEDINT8888 0x8035u
#define GFXACCELHOSTREADFRAMEBUFFER 0x8ca8u
#define GFXACCELHOSTRGBA16F 0x881au
#define GFXACCELHOSTHALFFLOAT 0x140bu
#define GFXACCELHOSTVERTEXPROGRAM 0x8620u
#define GFXACCELHOSTFRAGMENTPROGRAM 0x8804u
#define GFXACCELHOSTTEXTURERECTANGLE 0x84f5u
#define GFXACCELHOSTPROGRAMERRORPOSITION 0x864bu
#define GFXACCELHOSTPROGRAMLENGTH 0x8627u
#define GFXACCELHOSTATTRIBUTEENTRY 19
#define GFXACCELHOSTVOLUMEENTRY 20
#define GFXACCELHOSTCUBEMAP 0x8513u
#define GFXACCELHOSTCUBEMAPFIRSTFACE 0x8515u
#define GFXACCELHOSTCUBEMAPLASTFACE 0x851au
#define GFXACCELHOSTTEXTURE3D 0x806fu
#define GFXACCELHOSTTEXTUREWRAPR 0x8072u
#define GFXACCELHOSTCLAMP 0x2900u
#define GFXACCELHOSTDEPTHOUTSIDE -1.0f
#define GFXACCELHOSTTEXTURE2D 0x0de1u
#define GFXACCELHOSTPROGRAMUNITS 8
#define GFXACCELHOSTPROGRAMLOCALS 96
#define GFXACCELHOSTPROGRAMATTRIBUTES 7
#define GFXACCELHOSTPROGRAMTEXCOORDS 8
#define GFXACCELHOSTATTRIBUTEPOSITION 0
#define GFXACCELHOSTATTRIBUTECOLOR 1
#define GFXACCELHOSTATTRIBUTESECONDARY 2
#define GFXACCELHOSTATTRIBUTETEXCOORD 3
#define GFXACCELHOSTATTRIBUTETEXCOORDS 2
#define GFXACCELHOSTATTRIBUTEFOG 5
#define GFXACCELHOSTSTATEMATRICES 0
#define GFXACCELHOSTSTATETEXTUREMATRICES 4
#define GFXACCELHOSTSTATEINVERSE 28
#define GFXACCELHOSTSTATEVERTEXFOG 56
#define GFXACCELHOSTSTATEVERTEXZERO 58
#define GFXACCELHOSTSTATEVERTEXCOUNT 60
#define GFXACCELHOSTSTATEFRAGMENTFOG 0
#define GFXACCELHOSTSTATEALPHA 2
#define GFXACCELHOSTSTATETEXENV 3
#define GFXACCELHOSTSTATEFRAGMENTZERO 11
#define GFXACCELHOSTSTATEFRAGMENTCOUNT 12
#define GFXACCELHOSTFOGLINEAR 1
#define GFXACCELHOSTFOGEXP 2
#define GFXACCELHOSTFOGEXP2 3

struct GLContext;
struct GLTextureObject;

uint32_t GuestTickCount(void);
char *GfxAccelHostTranslateProgram(const char *programtext, uint32_t length, int fragmentprogram, int metalshading);
int GfxAccelHostProgramTextureTargets(const char *programtext, uint32_t length, uint32_t *targets);

void GfxAccelHostBeginFrame(GLContext *ctx);
void GfxAccelHostBitmap(GLContext *ctx, int width, int height, const uint8_t *bgra_data, int data_len);
void GfxAccelHostClear(GLContext *ctx, uint32_t mask);
void GfxAccelHostDestroyTexture(GLTextureObject *texObj);
void GfxAccelHostDrawPixels(GLContext *ctx, int width, int height, const uint8_t *bgra_data, int data_len);
void GfxAccelHostEndFrame(GLContext *ctx);
void GfxAccelHostFlushImmediateMode(GLContext *ctx);
void GfxAccelHostInit(GLContext *ctx);
uint8_t * GfxAccelHostReadFramebufferRect(GLContext *ctx, int x, int y, int width, int height, int *out_len);
void GfxAccelHostRelease(GLContext *ctx);
void GfxAccelHostUpload3DTexture(GLContext *ctx, GLTextureObject *texObj, int level, int width, int height, int depth, const uint8_t *data, int dataLen);
void GfxAccelHostUploadSubTexture(GLContext *ctx, GLTextureObject *texObj, int level, int xoff, int yoff, int w, int h, const uint8_t *data, int bytesPerRow);
void GfxAccelHostUploadSubTexture3D(GLContext *ctx, GLTextureObject *texObj, int level, int xoff, int yoff, int zoff, int w, int h, int d, const uint8_t *data, int bytesPerRow, int bytesPerImage);
void GfxAccelHostUploadTexture(GLContext *ctx, GLTextureObject *texObj, int level, int width, int height, const uint8_t *data, int dataLen);

void *GfxAccelHostTargetCreate(void);
void GfxAccelHostTargetDestroy(void *target);
void GfxAccelHostTargetLeave(void *target);
void GfxAccelHostTargetEnter(void *target);
void GfxAccelHostTargetResume(GLContext *context, void *target);
int GfxAccelHostTargetFrameActive(void);
void GfxAccelHostTargetFlush(void);
void GfxAccelHostTargetReadPixels(GLContext *context, uint32_t width, uint32_t height, void *pixels);
int GfxAccelHostTargetAdopt(GLContext *context, uint32_t width, uint32_t height);
void GfxAccelHostTargetReleaseOverlays(void *target);
void GfxAccelHostTargetAttachTexture(void *target, unsigned texture, uint32_t width, uint32_t height);
void *GfxAccelHostOverlayVend(void *target, int index, uint32_t width, uint32_t height);
void GfxAccelHostOverlayRelease(void *texture);
void GfxAccelHostOverlayBind(int32_t width, int32_t height);
int GfxAccelHostDeviceReady(void);
void GfxAccelHostInvalidateState(GLContext *context);
int GfxAccelHostTextureUnitCount(GLContext *context);
void GfxAccelHostHostString(uint32_t name, char *text, uint32_t size);
unsigned GfxAccelHostTextureCreate(uint32_t width, uint32_t height, int floatpixels);
void GfxAccelHostTextureDelete(unsigned texture);
int GfxAccelHostFramebufferAvailable(void);
void GfxAccelHostReadFramebufferAttach(void *target, unsigned texture);
void GfxAccelHostReadFramebufferRelease(void);
void GfxAccelHostTextureCopyFromRead(GLTextureObject *object, uint32_t internalformat, int width, int height);
void GfxAccelHostShadowTextureCreate(GLContext *context, GLTextureObject *object);
void GfxAccelHostShadowTextureDestroy(GLTextureObject *object);
void GfxAccelHostTextureApply(GLContext *context, int unit, uint32_t target, uint32_t guestname, int enabled);
void GfxAccelHostTextureParameter(GLContext *context, int unit, uint32_t target, GLTextureObject *object, uint32_t pname, float value);
void GfxAccelHostTextureImage(int unit, uint32_t target, GLTextureObject *object, int level, int xoffset, int yoffset, int width, int height, const uint8_t *pixels, int replacewholeimage);
void GfxAccelHostTextureVolume(void *target, int unit, GLTextureObject *object, int level, int xoffset, int yoffset, int zoffset, int width, int height, int depth, const uint8_t *pixels, int replacewholeimage);
void GfxAccelHostTextureCopyFramebuffer(GLContext *context, int unit, uint32_t target, GLTextureObject *object, int level, uint32_t internalformat, int sourcex, int sourcey, int width, int height, int xoffset, int yoffset, int replacewholeimage);
void GfxAccelHostBlendFunc(uint32_t sourcefactor, uint32_t destinationfactor, int separate, uint32_t sourcealpha, uint32_t destinationalpha);
void GfxAccelHostRasterPosition(GLContext *context);
void GfxAccelHostMatrixMode(void *target, uint32_t mode);
void GfxAccelHostLoadMatrix(GLContext *context, void *target, const float *matrix);
void GfxAccelHostViewport(GLContext *context, void *target, int x, int y, int width, int height);
void GfxAccelHostSamples(GLContext *context, int samples);
float *GfxAccelHostReadDepthRect(GLContext *context, int x, int y, int width, int height);
int GfxAccelHostProgramEntryAvailable(void *target, int entry);
void GfxAccelHostProgramEnable(void *target, uint32_t capability, int enable);
void GfxAccelHostProgramBind(void *target, uint32_t programtarget, uint32_t name);
void GfxAccelHostProgramsDelete(void *target, uint32_t count, const uint32_t *names);
void GfxAccelHostProgramsGenerate(void *target, uint32_t count, uint32_t *names);
uint32_t GfxAccelHostProgramIs(void *target, uint32_t name);
void GfxAccelHostProgramParameterDoubles(void *target, int local, uint32_t programtarget, uint32_t slot, double *values);
void GfxAccelHostProgramParameterFloats(void *target, int local, uint32_t programtarget, uint32_t slot, float *values);
void GfxAccelHostProgramGetDoubles(void *target, int local, uint32_t programtarget, uint32_t slot, double *values);
void GfxAccelHostProgramGetFloats(void *target, int local, uint32_t programtarget, uint32_t slot, float *values);
int GfxAccelHostProgramString(void *target, uint32_t programtarget, uint32_t format, uint32_t length, const void *text);
void GfxAccelHostProgramGetString(void *target, uint32_t programtarget, uint32_t name, void *text);
int GfxAccelHostProgramQuery(void *target, uint32_t programtarget, uint32_t name);
int GfxAccelHostProgramEnabled(void *target, uint32_t capability);
void GfxAccelHostDrawAttributes(GLContext *context, void *target, uint32_t mode, uint32_t count, const uint32_t *attributes, uint32_t attributecount, const float *values);

#endif
