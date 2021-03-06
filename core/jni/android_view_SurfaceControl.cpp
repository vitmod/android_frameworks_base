/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "SurfaceControl"

#include <stdio.h>

#include "jni.h"
#include "JNIHelp.h"

#include "android_os_Parcel.h"
#include "android_util_Binder.h"
#include "android/graphics/Bitmap.h"
#include "android/graphics/GraphicsJNI.h"
#include "android/graphics/Region.h"

#include "core_jni_helpers.h"
#include <android_runtime/android_view_Surface.h>
#include <android_runtime/android_view_SurfaceSession.h>

#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>

#include <ui/DisplayInfo.h>
#include <ui/FrameStats.h>
#include <ui/Rect.h>
#include <ui/Region.h>

#include <utils/Log.h>

#include <ScopedUtfChars.h>

#include "SkTemplates.h"

#include <linux/videodev2.h>
#include <hardware/hardware.h>
#include <hardware/aml_screen.h>

#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <cutils/properties.h>

// ----------------------------------------------------------------------------

namespace android {

static const char* const OutOfResourcesException =
    "android/view/Surface$OutOfResourcesException";

static struct {
    jclass clazz;
    jmethodID ctor;
    jfieldID width;
    jfieldID height;
    jfieldID refreshRate;
    jfieldID density;
    jfieldID xDpi;
    jfieldID yDpi;
    jfieldID secure;
    jfieldID appVsyncOffsetNanos;
    jfieldID presentationDeadlineNanos;
    jfieldID colorTransform;
} gPhysicalDisplayInfoClassInfo;

static struct {
    jfieldID bottom;
    jfieldID left;
    jfieldID right;
    jfieldID top;
} gRectClassInfo;

// Implements SkMallocPixelRef::ReleaseProc, to delete the screenshot on unref.
void DeleteScreenshot(void* addr, void* context) {
    SkASSERT(addr == ((ScreenshotClient*) context)->getPixels());
    delete ((ScreenshotClient*) context);
}

static struct {
    nsecs_t UNDEFINED_TIME_NANO;
    jmethodID init;
} gWindowContentFrameStatsClassInfo;

static struct {
    nsecs_t UNDEFINED_TIME_NANO;
    jmethodID init;
} gWindowAnimationFrameStatsClassInfo;

// ----------------------------------------------------------------------------

static jlong nativeCreate(JNIEnv* env, jclass clazz, jobject sessionObj,
        jstring nameStr, jint w, jint h, jint format, jint flags) {
    ScopedUtfChars name(env, nameStr);
    sp<SurfaceComposerClient> client(android_view_SurfaceSession_getClient(env, sessionObj));
    sp<SurfaceControl> surface = client->createSurface(
            String8(name.c_str()), w, h, format, flags);
    if (surface == NULL) {
        jniThrowException(env, OutOfResourcesException, NULL);
        return 0;
    }
    surface->incStrong((void *)nativeCreate);
    return reinterpret_cast<jlong>(surface.get());
}

static void nativeRelease(JNIEnv* env, jclass clazz, jlong nativeObject) {
    sp<SurfaceControl> ctrl(reinterpret_cast<SurfaceControl *>(nativeObject));
    ctrl->decStrong((void *)nativeCreate);
}

static void nativeDestroy(JNIEnv* env, jclass clazz, jlong nativeObject) {
    sp<SurfaceControl> ctrl(reinterpret_cast<SurfaceControl *>(nativeObject));
    ctrl->clear();
    ctrl->decStrong((void *)nativeCreate);
}

static jobject nativeScreenshotBitmap_t(JNIEnv* env, jclass clazz,
        jobject displayTokenObj, jobject sourceCropObj, jint width, jint height,
        jint minLayer, jint maxLayer, bool allLayers, bool useIdentityTransform,
        int rotation) {
    sp<IBinder> displayToken = ibinderForJavaObject(env, displayTokenObj);
    if (displayToken == NULL) {
        return NULL;
    }

    int left = env->GetIntField(sourceCropObj, gRectClassInfo.left);
    int top = env->GetIntField(sourceCropObj, gRectClassInfo.top);
    int right = env->GetIntField(sourceCropObj, gRectClassInfo.right);
    int bottom = env->GetIntField(sourceCropObj, gRectClassInfo.bottom);
    ALOGD("[%s %d]", __FUNCTION__, __LINE__);
    int CAPTURE_MAX_BITMAP_W = 1920;
    int CAPTURE_MAX_BITMAP_H = 1080;
    ALOGD("rect size: %d, %d, %d, %d", left, top, right, bottom);

    if (left < 0 || top < 0 || right < 0 || bottom <0 || left > right || top > bottom) {
        ALOGE("wrong rect size!!!");
        return NULL ;
    }
    if (width <= 0 || height <= 0) {
        ALOGE("wrong bitmap size!!!");
        return NULL ;
    }

    jobject ret1 = NULL;
    int customW = 0;
    int customH = 0;

    //-----------------getVideoBuffer--------------//
    aml_screen_module_t* screenModule = NULL;
    aml_screen_device_t* screenDev = NULL;
    if (!screenModule)
        hw_get_module(AML_SCREEN_HARDWARE_MODULE_ID, (const hw_module_t **)&screenModule);

    if (screenModule)
        screenModule->common.methods->open((const hw_module_t *)screenModule, "1",
                (struct hw_device_t **)&screenDev);

    if (screenDev) {
        customW = width > 0 ? width : CAPTURE_MAX_BITMAP_W;
        customH = height > 0 ? height : CAPTURE_MAX_BITMAP_H;
        screenDev->ops.set_format(screenDev, customW, customH, V4L2_PIX_FMT_RGB565X); // V4L2_PIX_FMT_NV21 ,V4L2_PIX_FMT_RGB24
        screenDev->ops.set_port_type(screenDev, (int)0x1000C000); //TVIN_PORT_HDMI0 = 0x4000
        if (left < right && top < bottom)
            screenDev->ops.set_amlvideo2_crop(screenDev, left, top,(right-left), (bottom-top));
        screenDev->ops.start_v4l2_device(screenDev);

        aml_screen_buffer_info_t buff_info;
        int ret = 0;

        int framecount = 0;
        long *src = NULL;
#if 0
        char propBuf[PROPERTY_VALUE_MAX];
        int dumpfd ;
#endif
        while (framecount < 10) {
            ret = screenDev->ops.aquire_buffer(screenDev, &buff_info);
            ALOGD("ret = %d",ret);
            if (ret != 0 || (buff_info.buffer_mem == 0)) {
                framecount++;
                ALOGD("Get V4l2 buffer failed,retry,sleep 10ms");
                usleep(10000);
                continue;
            } else {
                ALOGD("get buffer finish!");
                src = (long *)buff_info.buffer_mem;
#if 0
            property_get("debug.loadcapturedatatofile", propBuf, "");
            if (strcmp(propBuf, "true") == 0) {
                ALOGD("--create /data/data/capture \n");
                dumpfd = open("/data/data/capture", O_CREAT | O_RDWR | O_TRUNC, 0644);
                write(dumpfd, src , customW*customH*2);// V4L2_PIX_FMT_NV21:*3/2  ; V4L2_PIX_FMT_RGB24: *3
                ALOGD("--write finish\n");
                close(dumpfd);
            }
#endif
                break;
            }
        }

        //------------------create bitmap-------------------//
        if (src) {
            SkBitmap result;
            SkBitmap *createdBitmap = new SkBitmap();//createFrameBitmap();

            if (createdBitmap != NULL) {
                //-----------------setPinxels for bitmap--------------//
                ALOGD("final bitmap size: %dX%d",customW,customH);
                SkImageInfo info = SkImageInfo::Make(customW, customH,kRGB_565_SkColorType,kPremul_SkAlphaType); //  kRGBA_8888_SkColorType

                createdBitmap->setInfo(info);
                createdBitmap->setPixels(src);

                JavaPixelAllocator  allocator(env);
                if (createdBitmap->copyTo(&result, &allocator)) {
                    Bitmap* bitmap = allocator.getStorageObjAndReset();
                    if (bitmap != NULL)
                        ret1 = GraphicsJNI::createBitmap(env, bitmap, false);
                }
            }
        }

        if (framecount < 10 && src != NULL )
            screenDev->ops.release_buffer(screenDev,src);
        screenDev->ops.stop_v4l2_device(screenDev);
        screenDev->common.close((hw_device_t*)screenDev);
    }
    ALOGD("nativeScreenshotBitmap finish");
    return ret1;
}

static jobject nativeScreenshotBitmap(JNIEnv* env, jclass clazz,
        jobject displayTokenObj, jobject sourceCropObj, jint width, jint height,
        jint minLayer, jint maxLayer, bool allLayers, bool useIdentityTransform,
        int rotation) {
    sp<IBinder> displayToken = ibinderForJavaObject(env, displayTokenObj);
    if (displayToken == NULL) {
        return NULL;
    }

    int left = env->GetIntField(sourceCropObj, gRectClassInfo.left);
    int top = env->GetIntField(sourceCropObj, gRectClassInfo.top);
    int right = env->GetIntField(sourceCropObj, gRectClassInfo.right);
    int bottom = env->GetIntField(sourceCropObj, gRectClassInfo.bottom);
    Rect sourceCrop(left, top, right, bottom);

    SkAutoTDelete<ScreenshotClient> screenshot(new ScreenshotClient());
    status_t res;
    if (allLayers) {
        minLayer = 0;
        maxLayer = -1;
    }

    res = screenshot->update(displayToken, sourceCrop, width, height,
        minLayer, maxLayer, useIdentityTransform, static_cast<uint32_t>(rotation));
    if (res != NO_ERROR) {
        return NULL;
    }

    SkImageInfo screenshotInfo;
    screenshotInfo.fWidth = screenshot->getWidth();
    screenshotInfo.fHeight = screenshot->getHeight();

    switch (screenshot->getFormat()) {
        case PIXEL_FORMAT_RGBX_8888: {
            screenshotInfo.fColorType = kRGBA_8888_SkColorType;
            screenshotInfo.fAlphaType = kOpaque_SkAlphaType;
            break;
        }
        case PIXEL_FORMAT_RGBA_8888: {
            screenshotInfo.fColorType = kRGBA_8888_SkColorType;
            screenshotInfo.fAlphaType = kPremul_SkAlphaType;
            break;
        }
        case PIXEL_FORMAT_RGB_565: {
            screenshotInfo.fColorType = kRGB_565_SkColorType;
            screenshotInfo.fAlphaType = kOpaque_SkAlphaType;
            break;
        }
        default: {
            return NULL;
        }
    }

    const size_t rowBytes =
            screenshot->getStride() * android::bytesPerPixel(screenshot->getFormat());

    if (!screenshotInfo.fWidth || !screenshotInfo.fHeight) {
        return NULL;
    }

    Bitmap* bitmap = new Bitmap(
            (void*) screenshot->getPixels(), (void*) screenshot.get(), DeleteScreenshot,
            screenshotInfo, rowBytes, nullptr);
    screenshot.detach();
    bitmap->peekAtPixelRef()->setImmutable();

    return GraphicsJNI::createBitmap(env, bitmap,
            GraphicsJNI::kBitmapCreateFlag_Premultiplied, NULL);
}


static void nativeScreenshot(JNIEnv* env, jclass clazz, jobject displayTokenObj,
        jobject surfaceObj, jobject sourceCropObj, jint width, jint height,
        jint minLayer, jint maxLayer, bool allLayers, bool useIdentityTransform) {
    sp<IBinder> displayToken = ibinderForJavaObject(env, displayTokenObj);
    if (displayToken != NULL) {
        sp<Surface> consumer = android_view_Surface_getSurface(env, surfaceObj);
        if (consumer != NULL) {
            int left = env->GetIntField(sourceCropObj, gRectClassInfo.left);
            int top = env->GetIntField(sourceCropObj, gRectClassInfo.top);
            int right = env->GetIntField(sourceCropObj, gRectClassInfo.right);
            int bottom = env->GetIntField(sourceCropObj, gRectClassInfo.bottom);
            Rect sourceCrop(left, top, right, bottom);

            if (allLayers) {
                minLayer = 0;
                maxLayer = -1;
            }
            ScreenshotClient::capture(displayToken,
                    consumer->getIGraphicBufferProducer(), sourceCrop,
                    width, height, uint32_t(minLayer), uint32_t(maxLayer),
                    useIdentityTransform);
        }
    }
}

static void nativeOpenTransaction(JNIEnv* env, jclass clazz) {
    SurfaceComposerClient::openGlobalTransaction();
}

static void nativeCloseTransaction(JNIEnv* env, jclass clazz) {
    SurfaceComposerClient::closeGlobalTransaction();
}

static void nativeSetAnimationTransaction(JNIEnv* env, jclass clazz) {
    SurfaceComposerClient::setAnimationTransaction();
}

static void nativeSetLayer(JNIEnv* env, jclass clazz, jlong nativeObject, jint zorder) {
    SurfaceControl* const ctrl = reinterpret_cast<SurfaceControl *>(nativeObject);
    status_t err = ctrl->setLayer(zorder);
    if (err < 0 && err != NO_INIT) {
        doThrowIAE(env);
    }
}

static void nativeSetPosition(JNIEnv* env, jclass clazz, jlong nativeObject, jfloat x, jfloat y) {
    SurfaceControl* const ctrl = reinterpret_cast<SurfaceControl *>(nativeObject);
    status_t err = ctrl->setPosition(x, y);
    if (err < 0 && err != NO_INIT) {
        doThrowIAE(env);
    }
}

static void nativeSetSize(JNIEnv* env, jclass clazz, jlong nativeObject, jint w, jint h) {
    SurfaceControl* const ctrl = reinterpret_cast<SurfaceControl *>(nativeObject);
    status_t err = ctrl->setSize(w, h);
    if (err < 0 && err != NO_INIT) {
        doThrowIAE(env);
    }
}

static void nativeSetFlags(JNIEnv* env, jclass clazz, jlong nativeObject, jint flags, jint mask) {
    SurfaceControl* const ctrl = reinterpret_cast<SurfaceControl *>(nativeObject);
    status_t err = ctrl->setFlags(flags, mask);
    if (err < 0 && err != NO_INIT) {
        doThrowIAE(env);
    }
}

static void nativeSetTransparentRegionHint(JNIEnv* env, jclass clazz, jlong nativeObject, jobject regionObj) {
    SurfaceControl* const ctrl = reinterpret_cast<SurfaceControl *>(nativeObject);
    SkRegion* region = android_graphics_Region_getSkRegion(env, regionObj);
    if (!region) {
        doThrowIAE(env);
        return;
    }

    const SkIRect& b(region->getBounds());
    Region reg(Rect(b.fLeft, b.fTop, b.fRight, b.fBottom));
    if (region->isComplex()) {
        SkRegion::Iterator it(*region);
        while (!it.done()) {
            const SkIRect& r(it.rect());
            reg.addRectUnchecked(r.fLeft, r.fTop, r.fRight, r.fBottom);
            it.next();
        }
    }

    status_t err = ctrl->setTransparentRegionHint(reg);
    if (err < 0 && err != NO_INIT) {
        doThrowIAE(env);
    }
}

static void nativeSetAlpha(JNIEnv* env, jclass clazz, jlong nativeObject, jfloat alpha) {
    SurfaceControl* const ctrl = reinterpret_cast<SurfaceControl *>(nativeObject);
    status_t err = ctrl->setAlpha(alpha);
    if (err < 0 && err != NO_INIT) {
        doThrowIAE(env);
    }
}

static void nativeSetMatrix(JNIEnv* env, jclass clazz, jlong nativeObject,
        jfloat dsdx, jfloat dtdx, jfloat dsdy, jfloat dtdy) {
    SurfaceControl* const ctrl = reinterpret_cast<SurfaceControl *>(nativeObject);
    status_t err = ctrl->setMatrix(dsdx, dtdx, dsdy, dtdy);
    if (err < 0 && err != NO_INIT) {
        doThrowIAE(env);
    }
}

static void nativeSetWindowCrop(JNIEnv* env, jclass clazz, jlong nativeObject,
        jint l, jint t, jint r, jint b) {
    SurfaceControl* const ctrl = reinterpret_cast<SurfaceControl *>(nativeObject);
    Rect crop(l, t, r, b);
    status_t err = ctrl->setCrop(crop);
    if (err < 0 && err != NO_INIT) {
        doThrowIAE(env);
    }
}

static void nativeSetLayerStack(JNIEnv* env, jclass clazz, jlong nativeObject, jint layerStack) {
    SurfaceControl* const ctrl = reinterpret_cast<SurfaceControl *>(nativeObject);
    status_t err = ctrl->setLayerStack(layerStack);
    if (err < 0 && err != NO_INIT) {
        doThrowIAE(env);
    }
}

static jobject nativeGetBuiltInDisplay(JNIEnv* env, jclass clazz, jint id) {
    sp<IBinder> token(SurfaceComposerClient::getBuiltInDisplay(id));
    return javaObjectForIBinder(env, token);
}

static jobject nativeCreateDisplay(JNIEnv* env, jclass clazz, jstring nameObj,
        jboolean secure) {
    ScopedUtfChars name(env, nameObj);
    sp<IBinder> token(SurfaceComposerClient::createDisplay(
            String8(name.c_str()), bool(secure)));
    return javaObjectForIBinder(env, token);
}

static void nativeDestroyDisplay(JNIEnv* env, jclass clazz, jobject tokenObj) {
    sp<IBinder> token(ibinderForJavaObject(env, tokenObj));
    if (token == NULL) return;
    SurfaceComposerClient::destroyDisplay(token);
}

static void nativeSetDisplaySurface(JNIEnv* env, jclass clazz,
        jobject tokenObj, jlong nativeSurfaceObject) {
    sp<IBinder> token(ibinderForJavaObject(env, tokenObj));
    if (token == NULL) return;
    sp<IGraphicBufferProducer> bufferProducer;
    sp<Surface> sur(reinterpret_cast<Surface *>(nativeSurfaceObject));
    if (sur != NULL) {
        bufferProducer = sur->getIGraphicBufferProducer();
    }
    SurfaceComposerClient::setDisplaySurface(token, bufferProducer);
}

static void nativeSetDisplayLayerStack(JNIEnv* env, jclass clazz,
        jobject tokenObj, jint layerStack) {
    sp<IBinder> token(ibinderForJavaObject(env, tokenObj));
    if (token == NULL) return;

    SurfaceComposerClient::setDisplayLayerStack(token, layerStack);
}

static void nativeSetDisplayProjection(JNIEnv* env, jclass clazz,
        jobject tokenObj, jint orientation,
        jint layerStackRect_left, jint layerStackRect_top, jint layerStackRect_right, jint layerStackRect_bottom,
        jint displayRect_left, jint displayRect_top, jint displayRect_right, jint displayRect_bottom) {
    sp<IBinder> token(ibinderForJavaObject(env, tokenObj));
    if (token == NULL) return;
    Rect layerStackRect(layerStackRect_left, layerStackRect_top, layerStackRect_right, layerStackRect_bottom);
    Rect displayRect(displayRect_left, displayRect_top, displayRect_right, displayRect_bottom);
    SurfaceComposerClient::setDisplayProjection(token, orientation, layerStackRect, displayRect);
}

static void nativeSetDisplaySize(JNIEnv* env, jclass clazz,
        jobject tokenObj, jint width, jint height) {
    sp<IBinder> token(ibinderForJavaObject(env, tokenObj));
    if (token == NULL) return;
    SurfaceComposerClient::setDisplaySize(token, width, height);
}

static jobjectArray nativeGetDisplayConfigs(JNIEnv* env, jclass clazz,
        jobject tokenObj) {
    sp<IBinder> token(ibinderForJavaObject(env, tokenObj));
    if (token == NULL) return NULL;

    Vector<DisplayInfo> configs;
    if (SurfaceComposerClient::getDisplayConfigs(token, &configs) != NO_ERROR ||
            configs.size() == 0) {
        return NULL;
    }

    jobjectArray configArray = env->NewObjectArray(configs.size(),
            gPhysicalDisplayInfoClassInfo.clazz, NULL);

    for (size_t c = 0; c < configs.size(); ++c) {
        const DisplayInfo& info = configs[c];
        jobject infoObj = env->NewObject(gPhysicalDisplayInfoClassInfo.clazz,
                gPhysicalDisplayInfoClassInfo.ctor);
        env->SetIntField(infoObj, gPhysicalDisplayInfoClassInfo.width, info.w);
        env->SetIntField(infoObj, gPhysicalDisplayInfoClassInfo.height, info.h);
        env->SetFloatField(infoObj, gPhysicalDisplayInfoClassInfo.refreshRate, info.fps);
        env->SetFloatField(infoObj, gPhysicalDisplayInfoClassInfo.density, info.density);
        env->SetFloatField(infoObj, gPhysicalDisplayInfoClassInfo.xDpi, info.xdpi);
        env->SetFloatField(infoObj, gPhysicalDisplayInfoClassInfo.yDpi, info.ydpi);
        env->SetBooleanField(infoObj, gPhysicalDisplayInfoClassInfo.secure, info.secure);
        env->SetLongField(infoObj, gPhysicalDisplayInfoClassInfo.appVsyncOffsetNanos,
                info.appVsyncOffset);
        env->SetLongField(infoObj, gPhysicalDisplayInfoClassInfo.presentationDeadlineNanos,
                info.presentationDeadline);
        env->SetIntField(infoObj, gPhysicalDisplayInfoClassInfo.colorTransform,
                info.colorTransform);
        env->SetObjectArrayElement(configArray, static_cast<jsize>(c), infoObj);
        env->DeleteLocalRef(infoObj);
    }

    return configArray;
}

static jint nativeGetActiveConfig(JNIEnv* env, jclass clazz, jobject tokenObj) {
    sp<IBinder> token(ibinderForJavaObject(env, tokenObj));
    if (token == NULL) return -1;
    return static_cast<jint>(SurfaceComposerClient::getActiveConfig(token));
}

static jboolean nativeSetActiveConfig(JNIEnv* env, jclass clazz, jobject tokenObj, jint id) {
    sp<IBinder> token(ibinderForJavaObject(env, tokenObj));
    if (token == NULL) return JNI_FALSE;
    status_t err = SurfaceComposerClient::setActiveConfig(token, static_cast<int>(id));
    return err == NO_ERROR ? JNI_TRUE : JNI_FALSE;
}

static void nativeSetDisplayPowerMode(JNIEnv* env, jclass clazz, jobject tokenObj, jint mode) {
    sp<IBinder> token(ibinderForJavaObject(env, tokenObj));
    if (token == NULL) return;

    ALOGD_IF_SLOW(100, "Excessive delay in setPowerMode()");
    SurfaceComposerClient::setDisplayPowerMode(token, mode);
}

static jboolean nativeClearContentFrameStats(JNIEnv* env, jclass clazz, jlong nativeObject) {
    SurfaceControl* const ctrl = reinterpret_cast<SurfaceControl *>(nativeObject);
    status_t err = ctrl->clearLayerFrameStats();

    if (err < 0 && err != NO_INIT) {
        doThrowIAE(env);
    }

    // The other end is not ready, just report we failed.
    if (err == NO_INIT) {
        return JNI_FALSE;
    }

    return JNI_TRUE;
}

static jboolean nativeGetContentFrameStats(JNIEnv* env, jclass clazz, jlong nativeObject,
    jobject outStats) {
    FrameStats stats;

    SurfaceControl* const ctrl = reinterpret_cast<SurfaceControl *>(nativeObject);
    status_t err = ctrl->getLayerFrameStats(&stats);
    if (err < 0 && err != NO_INIT) {
        doThrowIAE(env);
    }

    // The other end is not ready, fine just return empty stats.
    if (err == NO_INIT) {
        return JNI_FALSE;
    }

    jlong refreshPeriodNano = static_cast<jlong>(stats.refreshPeriodNano);
    size_t frameCount = stats.desiredPresentTimesNano.size();

    jlongArray postedTimesNanoDst = env->NewLongArray(frameCount);
    if (postedTimesNanoDst == NULL) {
        return JNI_FALSE;
    }

    jlongArray presentedTimesNanoDst = env->NewLongArray(frameCount);
    if (presentedTimesNanoDst == NULL) {
        return JNI_FALSE;
    }

    jlongArray readyTimesNanoDst = env->NewLongArray(frameCount);
    if (readyTimesNanoDst == NULL) {
        return JNI_FALSE;
    }

    nsecs_t postedTimesNanoSrc[frameCount];
    nsecs_t presentedTimesNanoSrc[frameCount];
    nsecs_t readyTimesNanoSrc[frameCount];

    for (size_t i = 0; i < frameCount; i++) {
        nsecs_t postedTimeNano = stats.desiredPresentTimesNano[i];
        if (postedTimeNano == INT64_MAX) {
            postedTimeNano = gWindowContentFrameStatsClassInfo.UNDEFINED_TIME_NANO;
        }
        postedTimesNanoSrc[i] = postedTimeNano;

        nsecs_t presentedTimeNano = stats.actualPresentTimesNano[i];
        if (presentedTimeNano == INT64_MAX) {
            presentedTimeNano = gWindowContentFrameStatsClassInfo.UNDEFINED_TIME_NANO;
        }
        presentedTimesNanoSrc[i] = presentedTimeNano;

        nsecs_t readyTimeNano = stats.frameReadyTimesNano[i];
        if (readyTimeNano == INT64_MAX) {
            readyTimeNano = gWindowContentFrameStatsClassInfo.UNDEFINED_TIME_NANO;
        }
        readyTimesNanoSrc[i] = readyTimeNano;
    }

    env->SetLongArrayRegion(postedTimesNanoDst, 0, frameCount, postedTimesNanoSrc);
    env->SetLongArrayRegion(presentedTimesNanoDst, 0, frameCount, presentedTimesNanoSrc);
    env->SetLongArrayRegion(readyTimesNanoDst, 0, frameCount, readyTimesNanoSrc);

    env->CallVoidMethod(outStats, gWindowContentFrameStatsClassInfo.init, refreshPeriodNano,
            postedTimesNanoDst, presentedTimesNanoDst, readyTimesNanoDst);

    if (env->ExceptionCheck()) {
        return JNI_FALSE;
    }

    return JNI_TRUE;
}

static jboolean nativeClearAnimationFrameStats(JNIEnv* env, jclass clazz) {
    status_t err = SurfaceComposerClient::clearAnimationFrameStats();

    if (err < 0 && err != NO_INIT) {
        doThrowIAE(env);
    }

    // The other end is not ready, just report we failed.
    if (err == NO_INIT) {
        return JNI_FALSE;
    }

    return JNI_TRUE;
}

static jboolean nativeGetAnimationFrameStats(JNIEnv* env, jclass clazz, jobject outStats) {
    FrameStats stats;

    status_t err = SurfaceComposerClient::getAnimationFrameStats(&stats);
    if (err < 0 && err != NO_INIT) {
        doThrowIAE(env);
    }

    // The other end is not ready, fine just return empty stats.
    if (err == NO_INIT) {
        return JNI_FALSE;
    }

    jlong refreshPeriodNano = static_cast<jlong>(stats.refreshPeriodNano);
    size_t frameCount = stats.desiredPresentTimesNano.size();

    jlongArray presentedTimesNanoDst = env->NewLongArray(frameCount);
    if (presentedTimesNanoDst == NULL) {
        return JNI_FALSE;
    }

    nsecs_t presentedTimesNanoSrc[frameCount];

    for (size_t i = 0; i < frameCount; i++) {
        nsecs_t presentedTimeNano = stats.actualPresentTimesNano[i];
        if (presentedTimeNano == INT64_MAX) {
            presentedTimeNano = gWindowContentFrameStatsClassInfo.UNDEFINED_TIME_NANO;
        }
        presentedTimesNanoSrc[i] = presentedTimeNano;
    }

    env->SetLongArrayRegion(presentedTimesNanoDst, 0, frameCount, presentedTimesNanoSrc);

    env->CallVoidMethod(outStats, gWindowAnimationFrameStatsClassInfo.init, refreshPeriodNano,
            presentedTimesNanoDst);

    if (env->ExceptionCheck()) {
        return JNI_FALSE;
    }

    return JNI_TRUE;
}

// ----------------------------------------------------------------------------

static JNINativeMethod sSurfaceControlMethods[] = {
    {"nativeCreate", "(Landroid/view/SurfaceSession;Ljava/lang/String;IIII)J",
            (void*)nativeCreate },
    {"nativeRelease", "(J)V",
            (void*)nativeRelease },
    {"nativeDestroy", "(J)V",
            (void*)nativeDestroy },
    {"nativeScreenshot", "(Landroid/os/IBinder;Landroid/graphics/Rect;IIIIZZI)Landroid/graphics/Bitmap;",
            (void*)nativeScreenshotBitmap },
    {"nativeScreenshot_t", "(Landroid/os/IBinder;Landroid/graphics/Rect;IIIIZZI)Landroid/graphics/Bitmap;",
            (void*)nativeScreenshotBitmap_t },
    {"nativeScreenshot", "(Landroid/os/IBinder;Landroid/view/Surface;Landroid/graphics/Rect;IIIIZZ)V",
            (void*)nativeScreenshot },
    {"nativeOpenTransaction", "()V",
            (void*)nativeOpenTransaction },
    {"nativeCloseTransaction", "()V",
            (void*)nativeCloseTransaction },
    {"nativeSetAnimationTransaction", "()V",
            (void*)nativeSetAnimationTransaction },
    {"nativeSetLayer", "(JI)V",
            (void*)nativeSetLayer },
    {"nativeSetPosition", "(JFF)V",
            (void*)nativeSetPosition },
    {"nativeSetSize", "(JII)V",
            (void*)nativeSetSize },
    {"nativeSetTransparentRegionHint", "(JLandroid/graphics/Region;)V",
            (void*)nativeSetTransparentRegionHint },
    {"nativeSetAlpha", "(JF)V",
            (void*)nativeSetAlpha },
    {"nativeSetMatrix", "(JFFFF)V",
            (void*)nativeSetMatrix },
    {"nativeSetFlags", "(JII)V",
            (void*)nativeSetFlags },
    {"nativeSetWindowCrop", "(JIIII)V",
            (void*)nativeSetWindowCrop },
    {"nativeSetLayerStack", "(JI)V",
            (void*)nativeSetLayerStack },
    {"nativeGetBuiltInDisplay", "(I)Landroid/os/IBinder;",
            (void*)nativeGetBuiltInDisplay },
    {"nativeCreateDisplay", "(Ljava/lang/String;Z)Landroid/os/IBinder;",
            (void*)nativeCreateDisplay },
    {"nativeDestroyDisplay", "(Landroid/os/IBinder;)V",
            (void*)nativeDestroyDisplay },
    {"nativeSetDisplaySurface", "(Landroid/os/IBinder;J)V",
            (void*)nativeSetDisplaySurface },
    {"nativeSetDisplayLayerStack", "(Landroid/os/IBinder;I)V",
            (void*)nativeSetDisplayLayerStack },
    {"nativeSetDisplayProjection", "(Landroid/os/IBinder;IIIIIIIII)V",
            (void*)nativeSetDisplayProjection },
    {"nativeSetDisplaySize", "(Landroid/os/IBinder;II)V",
            (void*)nativeSetDisplaySize },
    {"nativeGetDisplayConfigs", "(Landroid/os/IBinder;)[Landroid/view/SurfaceControl$PhysicalDisplayInfo;",
            (void*)nativeGetDisplayConfigs },
    {"nativeGetActiveConfig", "(Landroid/os/IBinder;)I",
            (void*)nativeGetActiveConfig },
    {"nativeSetActiveConfig", "(Landroid/os/IBinder;I)Z",
            (void*)nativeSetActiveConfig },
    {"nativeClearContentFrameStats", "(J)Z",
            (void*)nativeClearContentFrameStats },
    {"nativeGetContentFrameStats", "(JLandroid/view/WindowContentFrameStats;)Z",
            (void*)nativeGetContentFrameStats },
    {"nativeClearAnimationFrameStats", "()Z",
            (void*)nativeClearAnimationFrameStats },
    {"nativeGetAnimationFrameStats", "(Landroid/view/WindowAnimationFrameStats;)Z",
            (void*)nativeGetAnimationFrameStats },
    {"nativeSetDisplayPowerMode", "(Landroid/os/IBinder;I)V",
            (void*)nativeSetDisplayPowerMode },
};

int register_android_view_SurfaceControl(JNIEnv* env)
{
    int err = RegisterMethodsOrDie(env, "android/view/SurfaceControl",
            sSurfaceControlMethods, NELEM(sSurfaceControlMethods));

    jclass clazz = FindClassOrDie(env, "android/view/SurfaceControl$PhysicalDisplayInfo");
    gPhysicalDisplayInfoClassInfo.clazz = MakeGlobalRefOrDie(env, clazz);
    gPhysicalDisplayInfoClassInfo.ctor = GetMethodIDOrDie(env,
            gPhysicalDisplayInfoClassInfo.clazz, "<init>", "()V");
    gPhysicalDisplayInfoClassInfo.width =       GetFieldIDOrDie(env, clazz, "width", "I");
    gPhysicalDisplayInfoClassInfo.height =      GetFieldIDOrDie(env, clazz, "height", "I");
    gPhysicalDisplayInfoClassInfo.refreshRate = GetFieldIDOrDie(env, clazz, "refreshRate", "F");
    gPhysicalDisplayInfoClassInfo.density =     GetFieldIDOrDie(env, clazz, "density", "F");
    gPhysicalDisplayInfoClassInfo.xDpi =        GetFieldIDOrDie(env, clazz, "xDpi", "F");
    gPhysicalDisplayInfoClassInfo.yDpi =        GetFieldIDOrDie(env, clazz, "yDpi", "F");
    gPhysicalDisplayInfoClassInfo.secure =      GetFieldIDOrDie(env, clazz, "secure", "Z");
    gPhysicalDisplayInfoClassInfo.appVsyncOffsetNanos = GetFieldIDOrDie(env,
            clazz, "appVsyncOffsetNanos", "J");
    gPhysicalDisplayInfoClassInfo.presentationDeadlineNanos = GetFieldIDOrDie(env,
            clazz, "presentationDeadlineNanos", "J");
    gPhysicalDisplayInfoClassInfo.colorTransform = GetFieldIDOrDie(env, clazz,
            "colorTransform", "I");

    jclass rectClazz = FindClassOrDie(env, "android/graphics/Rect");
    gRectClassInfo.bottom = GetFieldIDOrDie(env, rectClazz, "bottom", "I");
    gRectClassInfo.left =   GetFieldIDOrDie(env, rectClazz, "left", "I");
    gRectClassInfo.right =  GetFieldIDOrDie(env, rectClazz, "right", "I");
    gRectClassInfo.top =    GetFieldIDOrDie(env, rectClazz, "top", "I");

    jclass frameStatsClazz = FindClassOrDie(env, "android/view/FrameStats");
    jfieldID undefined_time_nano_field = GetStaticFieldIDOrDie(env,
            frameStatsClazz, "UNDEFINED_TIME_NANO", "J");
    nsecs_t undefined_time_nano = env->GetStaticLongField(frameStatsClazz, undefined_time_nano_field);

    jclass contFrameStatsClazz = FindClassOrDie(env, "android/view/WindowContentFrameStats");
    gWindowContentFrameStatsClassInfo.init = GetMethodIDOrDie(env,
            contFrameStatsClazz, "init", "(J[J[J[J)V");
    gWindowContentFrameStatsClassInfo.UNDEFINED_TIME_NANO = undefined_time_nano;

    jclass animFrameStatsClazz = FindClassOrDie(env, "android/view/WindowAnimationFrameStats");
    gWindowAnimationFrameStatsClassInfo.init =  GetMethodIDOrDie(env,
            animFrameStatsClazz, "init", "(J[J)V");
    gWindowAnimationFrameStatsClassInfo.UNDEFINED_TIME_NANO = undefined_time_nano;

    return err;
}

};
