/*
 * Copyright (C) 2007 The Android Open Source Project
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

//#define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "Layer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <math.h>

#include <cutils/compiler.h>
#include <cutils/native_handle.h>
#include <cutils/properties.h>

#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/NativeHandle.h>
#include <utils/StopWatch.h>
#include <utils/Trace.h>

#include <ui/DebugUtils.h>
#include <ui/GraphicBuffer.h>
#include <ui/PixelFormat.h>

#include <gui/BufferItem.h>
#include <gui/BufferQueue.h>
#include <gui/LayerDebugInfo.h>
#include <gui/Surface.h>

#include "clz.h"
#include "Colorizer.h"
#include "DisplayDevice.h"
#include "Layer.h"
#include "LayerRejecter.h"
#include "MonitoredProducer.h"
#include "SurfaceFlinger.h"

#include "DisplayHardware/HWComposer.h"

#include "RenderEngine/RenderEngine.h"

#include <mutex>
#include "LayerProtoHelper.h"

#define DEBUG_RESIZE    0

namespace android {

// ---------------------------------------------------------------------------

int32_t Layer::sSequence = 1;

Layer::Layer(SurfaceFlinger* flinger, const sp<Client>& client,
        const String8& name, uint32_t w, uint32_t h, uint32_t flags)
    :   contentDirty(false),
        sequence(uint32_t(android_atomic_inc(&sSequence))),
        mFlinger(flinger),
        mTextureName(UINT32_MAX),
        mPremultipliedAlpha(true),
        mName("unnamed"),
        mFormat(PIXEL_FORMAT_NONE),
        mTransactionFlags(0),
        mPendingStateMutex(),
        mPendingStates(),
        mQueuedFrames(0),
        mSidebandStreamChanged(false),
        mActiveBufferSlot(BufferQueue::INVALID_BUFFER_SLOT),
        mCurrentTransform(0),
        mCurrentScalingMode(NATIVE_WINDOW_SCALING_MODE_FREEZE),
        mOverrideScalingMode(-1),
        mCurrentOpacity(true),
        mBufferLatched(false),
        mCurrentFrameNumber(0),
        mPreviousFrameNumber(0),
        mRefreshPending(false),
        mFrameLatencyNeeded(false),
        mFiltering(false),
        mNeedsFiltering(false),
        mMesh(Mesh::TRIANGLE_FAN, 4, 2, 2),
#ifndef USE_HWC2
        mIsGlesComposition(false),
#endif
        mProtectedByApp(false),
        mHasSurface(false),
        mClientRef(client),
        mPotentialCursor(false),
        mQueueItemLock(),
        mQueueItemCondition(),
        mQueueItems(),
        mLastFrameNumberReceived(0),
        mUpdateTexImageFailed(false),
        mAutoRefresh(false),
        mFreezeGeometryUpdates(false)
{
#ifdef USE_HWC2
    ALOGV("Creating Layer %s", name.string());
#endif

    mCurrentCrop.makeInvalid();
    mFlinger->getRenderEngine().genTextures(1, &mTextureName);
    mTexture.init(Texture::TEXTURE_EXTERNAL, mTextureName);

    uint32_t layerFlags = 0;
    if (flags & ISurfaceComposerClient::eHidden)
        layerFlags |= layer_state_t::eLayerHidden;
    if (flags & ISurfaceComposerClient::eOpaque)
        layerFlags |= layer_state_t::eLayerOpaque;
    if (flags & ISurfaceComposerClient::eSecure)
        layerFlags |= layer_state_t::eLayerSecure;

    if (flags & ISurfaceComposerClient::eNonPremultiplied)
        mPremultipliedAlpha = false;

    mName = name;
    mTransactionName = String8("TX - ") + mName;

    mCurrentState.active.w = w;
    mCurrentState.active.h = h;
    mCurrentState.active.transform.set(0, 0);
    mCurrentState.crop.makeInvalid();
    mCurrentState.finalCrop.makeInvalid();
    mCurrentState.requestedFinalCrop = mCurrentState.finalCrop;
    mCurrentState.requestedCrop = mCurrentState.crop;
    mCurrentState.z = 0;
    mCurrentState.color.a = 1.0f;
    mCurrentState.layerStack = 0;
    mCurrentState.flags = layerFlags;
    mCurrentState.sequence = 0;
    mCurrentState.requested = mCurrentState.active;
    mCurrentState.dataSpace = HAL_DATASPACE_UNKNOWN;
    mCurrentState.appId = 0;
    mCurrentState.type = 0;

    // drawing state & current state are identical
    mDrawingState = mCurrentState;

#ifdef USE_HWC2
    const auto& hwc = flinger->getHwComposer();
    const auto& activeConfig = hwc.getActiveConfig(HWC_DISPLAY_PRIMARY);
    nsecs_t displayPeriod = activeConfig->getVsyncPeriod();
#else
    nsecs_t displayPeriod =
            flinger->getHwComposer().getRefreshPeriod(HWC_DISPLAY_PRIMARY);
#endif
    mFrameTracker.setDisplayRefreshPeriod(displayPeriod);

    CompositorTiming compositorTiming;
    flinger->getCompositorTiming(&compositorTiming);
    mFrameEventHistory.initializeCompositorTiming(compositorTiming);
}

void Layer::onFirstRef() {
    // Creates a custom BufferQueue for SurfaceFlingerConsumer to use
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer, true);
    mProducer = new MonitoredProducer(producer, mFlinger, this);
    mSurfaceFlingerConsumer = new SurfaceFlingerConsumer(consumer, mTextureName, this);
    mSurfaceFlingerConsumer->setConsumerUsageBits(getEffectiveUsage(0));
    mSurfaceFlingerConsumer->setContentsChangedListener(this);
    mSurfaceFlingerConsumer->setName(mName);

    if (mFlinger->isLayerTripleBufferingDisabled()) {
        mProducer->setMaxDequeuedBufferCount(2);
    }

    const sp<const DisplayDevice> hw(mFlinger->getDefaultDisplayDevice());
    updateTransformHint(hw);
}

Layer::~Layer() {
  sp<Client> c(mClientRef.promote());
    if (c != 0) {
        c->detachLayer(this);
    }

    for (auto& point : mRemoteSyncPoints) {
        point->setTransactionApplied();
    }
    for (auto& point : mLocalSyncPoints) {
        point->setFrameAvailable();
    }
    mFlinger->deleteTextureAsync(mTextureName);
    mFrameTracker.logAndResetStats(mName);

#ifdef USE_HWC2
    if (!mHwcLayers.empty()) {
        ALOGE("Found stale hardware composer layers when destroying "
                "surface flinger layer %s", mName.string());
        destroyAllHwcLayers();
    }
#endif
}

// ---------------------------------------------------------------------------
// callbacks
// ---------------------------------------------------------------------------

#ifdef USE_HWC2
void Layer::onLayerDisplayed(const sp<Fence>& releaseFence) {
    if (mHwcLayers.empty()) {
        return;
    }
    mSurfaceFlingerConsumer->setReleaseFence(releaseFence);
}
#else
void Layer::onLayerDisplayed(const sp<const DisplayDevice>& /* hw */,
        HWComposer::HWCLayerInterface* layer) {
    if (layer) {
        layer->onDisplayed();
        mSurfaceFlingerConsumer->setReleaseFence(layer->getAndResetReleaseFence());
    }
}
#endif

void Layer::onFrameAvailable(const BufferItem& item) {
    // Add this buffer from our internal queue tracker
    { // Autolock scope
        Mutex::Autolock lock(mQueueItemLock);
        mFlinger->mInterceptor.saveBufferUpdate(this, item.mGraphicBuffer->getWidth(),
                item.mGraphicBuffer->getHeight(), item.mFrameNumber);
        // Reset the frame number tracker when we receive the first buffer after
        // a frame number reset
        if (item.mFrameNumber == 1) {
            mLastFrameNumberReceived = 0;
        }

        // Ensure that callbacks are handled in order
        while (item.mFrameNumber != mLastFrameNumberReceived + 1) {
            status_t result = mQueueItemCondition.waitRelative(mQueueItemLock,
                    ms2ns(500));
            if (result != NO_ERROR) {
                ALOGE("[%s] Timed out waiting on callback", mName.string());
            }
        }

        mQueueItems.push_back(item);
        android_atomic_inc(&mQueuedFrames);

        // Wake up any pending callbacks
        mLastFrameNumberReceived = item.mFrameNumber;
        mQueueItemCondition.broadcast();
    }

    mFlinger->signalLayerUpdate();
}

void Layer::onFrameReplaced(const BufferItem& item) {
    { // Autolock scope
        Mutex::Autolock lock(mQueueItemLock);

        // Ensure that callbacks are handled in order
        while (item.mFrameNumber != mLastFrameNumberReceived + 1) {
            status_t result = mQueueItemCondition.waitRelative(mQueueItemLock,
                    ms2ns(500));
            if (result != NO_ERROR) {
                ALOGE("[%s] Timed out waiting on callback", mName.string());
            }
        }

        if (mQueueItems.empty()) {
            ALOGE("Can't replace a frame on an empty queue");
            return;
        }
        mQueueItems.editItemAt(mQueueItems.size() - 1) = item;

        // Wake up any pending callbacks
        mLastFrameNumberReceived = item.mFrameNumber;
        mQueueItemCondition.broadcast();
    }
}

void Layer::onSidebandStreamChanged() {
    if (android_atomic_release_cas(false, true, &mSidebandStreamChanged) == 0) {
        // mSidebandStreamChanged was false
        mFlinger->signalLayerUpdate();
    }
}

void Layer::onRemovedFromCurrentState() {
    // the layer is removed from SF mCurrentState to mLayersPendingRemoval

    if (mCurrentState.zOrderRelativeOf != nullptr) {
        sp<Layer> strongRelative = mCurrentState.zOrderRelativeOf.promote();
        if (strongRelative != nullptr) {
            strongRelative->removeZOrderRelative(this);
            mFlinger->setTransactionFlags(eTraversalNeeded);
        }
        mCurrentState.zOrderRelativeOf = nullptr;
    }

    for (const auto& child : mCurrentChildren) {
        child->onRemovedFromCurrentState();
    }
}

void Layer::onRemoved() {
    // the layer is removed from SF mLayersPendingRemoval

    mSurfaceFlingerConsumer->abandon();
#ifdef USE_HWC2
    destroyAllHwcLayers();
#endif

    for (const auto& child : mCurrentChildren) {
        child->onRemoved();
    }
}

// ---------------------------------------------------------------------------
// set-up
// ---------------------------------------------------------------------------

const String8& Layer::getName() const {
    return mName;
}

bool Layer::getPremultipledAlpha() const {
    return mPremultipliedAlpha;
}

status_t Layer::setBuffers( uint32_t w, uint32_t h,
                            PixelFormat format, uint32_t flags)
{
    uint32_t const maxSurfaceDims = min(
            mFlinger->getMaxTextureSize(), mFlinger->getMaxViewportDims());

    // never allow a surface larger than what our underlying GL implementation
    // can handle.
    if ((uint32_t(w)>maxSurfaceDims) || (uint32_t(h)>maxSurfaceDims)) {
        ALOGE("dimensions too large %u x %u", uint32_t(w), uint32_t(h));
        return BAD_VALUE;
    }

    mFormat = format;

    mPotentialCursor = (flags & ISurfaceComposerClient::eCursorWindow) ? true : false;
    mProtectedByApp = (flags & ISurfaceComposerClient::eProtectedByApp) ? true : false;
    mCurrentOpacity = getOpacityForFormat(format);

    mSurfaceFlingerConsumer->setDefaultBufferSize(w, h);
    mSurfaceFlingerConsumer->setDefaultBufferFormat(format);
    mSurfaceFlingerConsumer->setConsumerUsageBits(getEffectiveUsage(0));

    return NO_ERROR;
}

sp<IBinder> Layer::getHandle() {
    Mutex::Autolock _l(mLock);

    LOG_ALWAYS_FATAL_IF(mHasSurface,
            "Layer::getHandle() has already been called");

    mHasSurface = true;

    return new Handle(mFlinger, this);
}

sp<IGraphicBufferProducer> Layer::getProducer() const {
    return mProducer;
}

// ---------------------------------------------------------------------------
// h/w composer set-up
// ---------------------------------------------------------------------------

#ifdef USE_HWC2
bool Layer::createHwcLayer(HWComposer* hwc, int32_t hwcId) {
    LOG_ALWAYS_FATAL_IF(mHwcLayers.count(hwcId) != 0,
                "Already have a layer for hwcId %d", hwcId);
    HWC2::Layer* layer = hwc->createLayer(hwcId);
    if (!layer) {
        return false;
    }
    HWCInfo& hwcInfo = mHwcLayers[hwcId];
    hwcInfo.hwc = hwc;
    hwcInfo.layer = layer;
    layer->setLayerDestroyedListener(
            [this, hwcId] (HWC2::Layer* /*layer*/){mHwcLayers.erase(hwcId);});
    return true;
}

void Layer::destroyHwcLayer(int32_t hwcId) {
    if (mHwcLayers.count(hwcId) == 0) {
        return;
    }
    auto& hwcInfo = mHwcLayers[hwcId];
    LOG_ALWAYS_FATAL_IF(hwcInfo.layer == nullptr,
            "Attempt to destroy null layer");
    LOG_ALWAYS_FATAL_IF(hwcInfo.hwc == nullptr, "Missing HWComposer");
    hwcInfo.hwc->destroyLayer(hwcId, hwcInfo.layer);
    // The layer destroyed listener should have cleared the entry from
    // mHwcLayers. Verify that.
    LOG_ALWAYS_FATAL_IF(mHwcLayers.count(hwcId) != 0,
            "Stale layer entry in mHwcLayers");
}

void Layer::destroyAllHwcLayers() {
    size_t numLayers = mHwcLayers.size();
    for (size_t i = 0; i < numLayers; ++i) {
        LOG_ALWAYS_FATAL_IF(mHwcLayers.empty(), "destroyAllHwcLayers failed");
        destroyHwcLayer(mHwcLayers.begin()->first);
    }
    LOG_ALWAYS_FATAL_IF(!mHwcLayers.empty(),
            "All hardware composer layers should have been destroyed");
}
#endif

Rect Layer::getContentCrop() const {
    // this is the crop rectangle that applies to the buffer
    // itself (as opposed to the window)
    Rect crop;
    if (!mCurrentCrop.isEmpty()) {
        // if the buffer crop is defined, we use that
        crop = mCurrentCrop;
    } else if (mActiveBuffer != NULL) {
        // otherwise we use the whole buffer
        crop = mActiveBuffer->getBounds();
    } else {
        // if we don't have a buffer yet, we use an empty/invalid crop
        crop.makeInvalid();
    }
    return crop;
}

static Rect reduce(const Rect& win, const Region& exclude) {
    if (CC_LIKELY(exclude.isEmpty())) {
        return win;
    }
    if (exclude.isRect()) {
        return win.reduce(exclude.getBounds());
    }
    return Region(win).subtract(exclude).getBounds();
}

Rect Layer::computeScreenBounds(bool reduceTransparentRegion) const {
    const Layer::State& s(getDrawingState());
    Rect win(s.active.w, s.active.h);

    if (!s.crop.isEmpty()) {
        win.intersect(s.crop, &win);
    }

    Transform t = getTransform();
    win = t.transform(win);

    if (!s.finalCrop.isEmpty()) {
        win.intersect(s.finalCrop, &win);
    }

    const sp<Layer>& p = mDrawingParent.promote();
    // Now we need to calculate the parent bounds, so we can clip ourselves to those.
    // When calculating the parent bounds for purposes of clipping,
    // we don't need to constrain the parent to its transparent region.
    // The transparent region is an optimization based on the
    // buffer contents of the layer, but does not affect the space allocated to
    // it by policy, and thus children should be allowed to extend into the
    // parent's transparent region. In fact one of the main uses, is to reduce
    // buffer allocation size in cases where a child window sits behind a main window
    // (by marking the hole in the parent window as a transparent region)
    if (p != nullptr) {
        Rect bounds = p->computeScreenBounds(false);
        bounds.intersect(win, &win);
    }

    if (reduceTransparentRegion) {
        auto const screenTransparentRegion = t.transform(s.activeTransparentRegion);
        win = reduce(win, screenTransparentRegion);
    }

    return win;
}

Rect Layer::computeBounds() const {
    const Layer::State& s(getDrawingState());
    return computeBounds(s.activeTransparentRegion);
}

Rect Layer::computeBounds(const Region& activeTransparentRegion) const {
    const Layer::State& s(getDrawingState());
    Rect win(s.active.w, s.active.h);

    if (!s.crop.isEmpty()) {
        win.intersect(s.crop, &win);
    }

    Rect bounds = win;
    const auto& p = mDrawingParent.promote();
    if (p != nullptr) {
        // Look in computeScreenBounds recursive call for explanation of
        // why we pass false here.
        bounds = p->computeScreenBounds(false /* reduceTransparentRegion */);
    }

    Transform t = getTransform();
    if (p != nullptr) {
        win = t.transform(win);
        win.intersect(bounds, &win);
        win = t.inverse().transform(win);
    }

    // subtract the transparent region and snap to the bounds
    return reduce(win, activeTransparentRegion);
}

Rect Layer::computeInitialCrop(const sp<const DisplayDevice>& hw) const {
    // the crop is the area of the window that gets cropped, but not
    // scaled in any ways.
    const State& s(getDrawingState());

    // apply the projection's clipping to the window crop in
    // layerstack space, and convert-back to layer space.
    // if there are no window scaling involved, this operation will map to full
    // pixels in the buffer.
    // FIXME: the 3 lines below can produce slightly incorrect clipping when we have
    // a viewport clipping and a window transform. we should use floating point to fix this.

    Rect activeCrop(s.active.w, s.active.h);
    if (!s.crop.isEmpty()) {
        activeCrop.intersect(s.crop, &activeCrop);
    }

    Transform t = getTransform();
    activeCrop = t.transform(activeCrop);
    if (!activeCrop.intersect(hw->getViewport(), &activeCrop)) {
        activeCrop.clear();
    }
    if (!s.finalCrop.isEmpty()) {
        if(!activeCrop.intersect(s.finalCrop, &activeCrop)) {
            activeCrop.clear();
        }
    }

    const auto& p = mDrawingParent.promote();
    if (p != nullptr) {
        auto parentCrop = p->computeInitialCrop(hw);
        activeCrop.intersect(parentCrop, &activeCrop);
    }

    return activeCrop;
}

FloatRect Layer::computeCrop(const sp<const DisplayDevice>& hw) const {
    // the content crop is the area of the content that gets scaled to the
    // layer's size. This is in buffer space.
    FloatRect crop = getContentCrop().toFloatRect();

    // In addition there is a WM-specified crop we pull from our drawing state.
    const State& s(getDrawingState());

    // Screen space to make reduction to parent crop clearer.
    Rect activeCrop = computeInitialCrop(hw);
    Transform t = getTransform();
    // Back to layer space to work with the content crop.
    activeCrop = t.inverse().transform(activeCrop);

    // This needs to be here as transform.transform(Rect) computes the
    // transformed rect and then takes the bounding box of the result before
    // returning. This means
    // transform.inverse().transform(transform.transform(Rect)) != Rect
    // in which case we need to make sure the final rect is clipped to the
    // display bounds.
    if (!activeCrop.intersect(Rect(s.active.w, s.active.h), &activeCrop)) {
        activeCrop.clear();
    }

    // subtract the transparent region and snap to the bounds
    activeCrop = reduce(activeCrop, s.activeTransparentRegion);

    // Transform the window crop to match the buffer coordinate system,
    // which means using the inverse of the current transform set on the
    // SurfaceFlingerConsumer.
    uint32_t invTransform = mCurrentTransform;
    if (getTransformToDisplayInverse()) {
        /*
         * the code below applies the primary display's inverse transform to the
         * buffer
         */
        uint32_t invTransformOrient =
                DisplayDevice::getPrimaryDisplayOrientationTransform();
        // calculate the inverse transform
        if (invTransformOrient & NATIVE_WINDOW_TRANSFORM_ROT_90) {
            invTransformOrient ^= NATIVE_WINDOW_TRANSFORM_FLIP_V |
                    NATIVE_WINDOW_TRANSFORM_FLIP_H;
        }
        // and apply to the current transform
        invTransform = (Transform(invTransformOrient) * Transform(invTransform))
                .getOrientation();
    }

    int winWidth = s.active.w;
    int winHeight = s.active.h;
    if (invTransform & NATIVE_WINDOW_TRANSFORM_ROT_90) {
        // If the activeCrop has been rotate the ends are rotated but not
        // the space itself so when transforming ends back we can't rely on
        // a modification of the axes of rotation. To account for this we
        // need to reorient the inverse rotation in terms of the current
        // axes of rotation.
        bool is_h_flipped = (invTransform & NATIVE_WINDOW_TRANSFORM_FLIP_H) != 0;
        bool is_v_flipped = (invTransform & NATIVE_WINDOW_TRANSFORM_FLIP_V) != 0;
        if (is_h_flipped == is_v_flipped) {
            invTransform ^= NATIVE_WINDOW_TRANSFORM_FLIP_V |
                    NATIVE_WINDOW_TRANSFORM_FLIP_H;
        }
        winWidth = s.active.h;
        winHeight = s.active.w;
    }
    const Rect winCrop = activeCrop.transform(
            invTransform, s.active.w, s.active.h);

    // below, crop is intersected with winCrop expressed in crop's coordinate space
    float xScale = crop.getWidth()  / float(winWidth);
    float yScale = crop.getHeight() / float(winHeight);

    float insetL = winCrop.left                 * xScale;
    float insetT = winCrop.top                  * yScale;
    float insetR = (winWidth - winCrop.right )  * xScale;
    float insetB = (winHeight - winCrop.bottom) * yScale;

    crop.left   += insetL;
    crop.top    += insetT;
    crop.right  -= insetR;
    crop.bottom -= insetB;

    return crop;
}

#ifdef USE_HWC2
void Layer::setGeometry(const sp<const DisplayDevice>& displayDevice, uint32_t z)
#else
void Layer::setGeometry(
    const sp<const DisplayDevice>& hw,
        HWComposer::HWCLayerInterface& layer)
#endif
{
#ifdef USE_HWC2
    const auto hwcId = displayDevice->getHwcDisplayId();
    auto& hwcInfo = mHwcLayers[hwcId];
#else
    layer.setDefaultState();
#endif

    // enable this layer
#ifdef USE_HWC2
    hwcInfo.forceClientComposition = false;

    if (isSecure() && !displayDevice->isSecure()) {
        hwcInfo.forceClientComposition = true;
    }

    auto& hwcLayer = hwcInfo.layer;
#else
    layer.setSkip(false);

    if (isSecure() && !hw->isSecure()) {
        layer.setSkip(true);
    }
#endif

    // this gives us only the "orientation" component of the transform
    const State& s(getDrawingState());
#ifdef USE_HWC2
    auto blendMode = HWC2::BlendMode::None;
    if (!isOpaque(s) || getAlpha() != 1.0f) {
        blendMode = mPremultipliedAlpha ?
                HWC2::BlendMode::Premultiplied : HWC2::BlendMode::Coverage;
    }
    auto error = hwcLayer->setBlendMode(blendMode);
    ALOGE_IF(error != HWC2::Error::None, "[%s] Failed to set blend mode %s:"
             " %s (%d)", mName.string(), to_string(blendMode).c_str(),
             to_string(error).c_str(), static_cast<int32_t>(error));
#else
    if (!isOpaque(s) || getAlpha() != 1.0f) {
        layer.setBlending(mPremultipliedAlpha ?
                HWC_BLENDING_PREMULT :
                HWC_BLENDING_COVERAGE);
    }
#endif

    // apply the layer's transform, followed by the display's global transform
    // here we're guaranteed that the layer's transform preserves rects
    Region activeTransparentRegion(s.activeTransparentRegion);
    Transform t = getTransform();
    if (!s.crop.isEmpty()) {
        Rect activeCrop(s.crop);
        activeCrop = t.transform(activeCrop);
#ifdef USE_HWC2
        if(!activeCrop.intersect(displayDevice->getViewport(), &activeCrop)) {
#else
        if(!activeCrop.intersect(hw->getViewport(), &activeCrop)) {
#endif
            activeCrop.clear();
        }
        activeCrop = t.inverse().transform(activeCrop, true);
        // This needs to be here as transform.transform(Rect) computes the
        // transformed rect and then takes the bounding box of the result before
        // returning. This means
        // transform.inverse().transform(transform.transform(Rect)) != Rect
        // in which case we need to make sure the final rect is clipped to the
        // display bounds.
        if(!activeCrop.intersect(Rect(s.active.w, s.active.h), &activeCrop)) {
            activeCrop.clear();
        }
        // mark regions outside the crop as transparent
        activeTransparentRegion.orSelf(Rect(0, 0, s.active.w, activeCrop.top));
        activeTransparentRegion.orSelf(Rect(0, activeCrop.bottom,
                s.active.w, s.active.h));
        activeTransparentRegion.orSelf(Rect(0, activeCrop.top,
                activeCrop.left, activeCrop.bottom));
        activeTransparentRegion.orSelf(Rect(activeCrop.right, activeCrop.top,
                s.active.w, activeCrop.bottom));
    }

    Rect frame(t.transform(computeBounds(activeTransparentRegion)));
    if (!s.finalCrop.isEmpty()) {
        if(!frame.intersect(s.finalCrop, &frame)) {
            frame.clear();
        }
    }
#ifdef USE_HWC2
    if (!frame.intersect(displayDevice->getViewport(), &frame)) {
        frame.clear();
    }
    const Transform& tr(displayDevice->getTransform());
    Rect transformedFrame = tr.transform(frame);
    error = hwcLayer->setDisplayFrame(transformedFrame);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set display frame [%d, %d, %d, %d]: %s (%d)",
                mName.string(), transformedFrame.left, transformedFrame.top,
                transformedFrame.right, transformedFrame.bottom,
                to_string(error).c_str(), static_cast<int32_t>(error));
    } else {
        hwcInfo.displayFrame = transformedFrame;
    }

    FloatRect sourceCrop = computeCrop(displayDevice);
    error = hwcLayer->setSourceCrop(sourceCrop);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set source crop [%.3f, %.3f, %.3f, %.3f]: "
                "%s (%d)", mName.string(), sourceCrop.left, sourceCrop.top,
                sourceCrop.right, sourceCrop.bottom, to_string(error).c_str(),
                static_cast<int32_t>(error));
    } else {
        hwcInfo.sourceCrop = sourceCrop;
    }

    float alpha = static_cast<float>(getAlpha());
    error = hwcLayer->setPlaneAlpha(alpha);
    ALOGE_IF(error != HWC2::Error::None, "[%s] Failed to set plane alpha %.3f: "
            "%s (%d)", mName.string(), alpha, to_string(error).c_str(),
            static_cast<int32_t>(error));

    error = hwcLayer->setZOrder(z);
    ALOGE_IF(error != HWC2::Error::None, "[%s] Failed to set Z %u: %s (%d)",
            mName.string(), z, to_string(error).c_str(),
            static_cast<int32_t>(error));

    int type = s.type;
    int appId = s.appId;
    sp<Layer> parent = mDrawingParent.promote();
    if (parent.get()) {
        auto& parentState = parent->getDrawingState();
        type = parentState.type;
        appId = parentState.appId;
    }

    error = hwcLayer->setInfo(type, appId);
    ALOGE_IF(error != HWC2::Error::None, "[%s] Failed to set info (%d)",
             mName.string(), static_cast<int32_t>(error));
#else
    if (!frame.intersect(hw->getViewport(), &frame)) {
        frame.clear();
    }
    const Transform& tr(hw->getTransform());
    layer.setFrame(tr.transform(frame));
    layer.setCrop(computeCrop(hw));
    layer.setPlaneAlpha(static_cast<uint8_t>(std::round(255.0f*getAlpha())));
#endif

    /*
     * Transformations are applied in this order:
     * 1) buffer orientation/flip/mirror
     * 2) state transformation (window manager)
     * 3) layer orientation (screen orientation)
     * (NOTE: the matrices are multiplied in reverse order)
     */

    const Transform bufferOrientation(mCurrentTransform);
    Transform transform(tr * t * bufferOrientation);

    if (getTransformToDisplayInverse()) {
        /*
         * the code below applies the primary display's inverse transform to the
         * buffer
         */
        uint32_t invTransform =
                DisplayDevice::getPrimaryDisplayOrientationTransform();
        // calculate the inverse transform
        if (invTransform & NATIVE_WINDOW_TRANSFORM_ROT_90) {
            invTransform ^= NATIVE_WINDOW_TRANSFORM_FLIP_V |
                    NATIVE_WINDOW_TRANSFORM_FLIP_H;
        }

        /*
         * Here we cancel out the orientation component of the WM transform.
         * The scaling and translate components are already included in our bounds
         * computation so it's enough to just omit it in the composition.
         * See comment in onDraw with ref to b/36727915 for why.
         */
        transform = Transform(invTransform) * tr * bufferOrientation;
    }

    // this gives us only the "orientation" component of the transform
    const uint32_t orientation = transform.getOrientation();
#ifdef USE_HWC2
    if (orientation & Transform::ROT_INVALID) {
        // we can only handle simple transformation
        hwcInfo.forceClientComposition = true;
    } else {
        auto transform = static_cast<HWC2::Transform>(orientation);
        auto error = hwcLayer->setTransform(transform);
        ALOGE_IF(error != HWC2::Error::None, "[%s] Failed to set transform %s: "
                "%s (%d)", mName.string(), to_string(transform).c_str(),
                to_string(error).c_str(), static_cast<int32_t>(error));
    }
#else
    if (orientation & Transform::ROT_INVALID) {
        // we can only handle simple transformation
        layer.setSkip(true);
    } else {
        layer.setTransform(orientation);
    }
#endif
}

#ifdef USE_HWC2
void Layer::forceClientComposition(int32_t hwcId) {
    if (mHwcLayers.count(hwcId) == 0) {
        ALOGE("forceClientComposition: no HWC layer found (%d)", hwcId);
        return;
    }

    mHwcLayers[hwcId].forceClientComposition = true;
}

void Layer::setPerFrameData(const sp<const DisplayDevice>& displayDevice) {
    // Apply this display's projection's viewport to the visible region
    // before giving it to the HWC HAL.
    const Transform& tr = displayDevice->getTransform();
    const auto& viewport = displayDevice->getViewport();
    Region visible = tr.transform(visibleRegion.intersect(viewport));
    auto hwcId = displayDevice->getHwcDisplayId();
    auto& hwcInfo = mHwcLayers[hwcId];
    auto& hwcLayer = hwcInfo.layer;
    auto error = hwcLayer->setVisibleRegion(visible);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set visible region: %s (%d)", mName.string(),
                to_string(error).c_str(), static_cast<int32_t>(error));
        visible.dump(LOG_TAG);
    }

    error = hwcLayer->setSurfaceDamage(surfaceDamageRegion);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set surface damage: %s (%d)", mName.string(),
                to_string(error).c_str(), static_cast<int32_t>(error));
        surfaceDamageRegion.dump(LOG_TAG);
    }

    // Sideband layers
    if (mSidebandStream.get()) {
        setCompositionType(hwcId, HWC2::Composition::Sideband);
        ALOGV("[%s] Requesting Sideband composition", mName.string());
        error = hwcLayer->setSidebandStream(mSidebandStream->handle());
        if (error != HWC2::Error::None) {
            ALOGE("[%s] Failed to set sideband stream %p: %s (%d)",
                    mName.string(), mSidebandStream->handle(),
                    to_string(error).c_str(), static_cast<int32_t>(error));
        }
        return;
    }

    // Client layers
    if (hwcInfo.forceClientComposition ||
            (mActiveBuffer != nullptr && mActiveBuffer->handle == nullptr)) {
        ALOGV("[%s] Requesting Client composition", mName.string());
        setCompositionType(hwcId, HWC2::Composition::Client);
        return;
    }

    // SolidColor layers
    if (mActiveBuffer == nullptr) {
        setCompositionType(hwcId, HWC2::Composition::SolidColor);

        half4 color = getColor();
        error = hwcLayer->setColor({static_cast<uint8_t>(std::round(255.0f*color.r)),
                    static_cast<uint8_t>(std::round(255.0f * color.g)),
                    static_cast<uint8_t>(std::round(255.0f * color.b)),
                    255});
        if (error != HWC2::Error::None) {
            ALOGE("[%s] Failed to set color: %s (%d)", mName.string(),
                    to_string(error).c_str(), static_cast<int32_t>(error));
        }

        // Clear out the transform, because it doesn't make sense absent a
        // source buffer
        error = hwcLayer->setTransform(HWC2::Transform::None);
        if (error != HWC2::Error::None) {
            ALOGE("[%s] Failed to clear transform: %s (%d)", mName.string(),
                    to_string(error).c_str(), static_cast<int32_t>(error));
        }

        return;
    }

    // Device or Cursor layers
    if (mPotentialCursor) {
        ALOGV("[%s] Requesting Cursor composition", mName.string());
        setCompositionType(hwcId, HWC2::Composition::Cursor);
    } else {
        ALOGV("[%s] Requesting Device composition", mName.string());
        setCompositionType(hwcId, HWC2::Composition::Device);
    }

    ALOGV("setPerFrameData: dataspace = %d", mCurrentState.dataSpace);
    error = hwcLayer->setDataspace(mCurrentState.dataSpace);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set dataspace %d: %s (%d)", mName.string(),
              mCurrentState.dataSpace, to_string(error).c_str(),
              static_cast<int32_t>(error));
    }

    uint32_t hwcSlot = 0;
    sp<GraphicBuffer> hwcBuffer;
    hwcInfo.bufferCache.getHwcBuffer(mActiveBufferSlot, mActiveBuffer,
            &hwcSlot, &hwcBuffer);

    auto acquireFence = mSurfaceFlingerConsumer->getCurrentFence();
    error = hwcLayer->setBuffer(hwcSlot, hwcBuffer, acquireFence);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set buffer %p: %s (%d)", mName.string(),
                mActiveBuffer->handle, to_string(error).c_str(),
                static_cast<int32_t>(error));
    }
}

#else
void Layer::setPerFrameData(const sp<const DisplayDevice>& hw,
        HWComposer::HWCLayerInterface& layer) {
    // we have to set the visible region on every frame because
    // we currently free it during onLayerDisplayed(), which is called
    // after HWComposer::commit() -- every frame.
    // Apply this display's projection's viewport to the visible region
    // before giving it to the HWC HAL.
    const Transform& tr = hw->getTransform();
    Region visible = tr.transform(visibleRegion.intersect(hw->getViewport()));
    layer.setVisibleRegionScreen(visible);
    layer.setSurfaceDamage(surfaceDamageRegion);
    mIsGlesComposition = (layer.getCompositionType() == HWC_FRAMEBUFFER);

    if (mSidebandStream.get()) {
        layer.setSidebandStream(mSidebandStream);
    } else {
        // NOTE: buffer can be NULL if the client never drew into this
        // layer yet, or if we ran out of memory
        layer.setBuffer(mActiveBuffer);
    }
}
#endif

#ifdef USE_HWC2
void Layer::updateCursorPosition(const sp<const DisplayDevice>& displayDevice) {
    auto hwcId = displayDevice->getHwcDisplayId();
    if (mHwcLayers.count(hwcId) == 0 ||
            getCompositionType(hwcId) != HWC2::Composition::Cursor) {
        return;
    }

    // This gives us only the "orientation" component of the transform
    const State& s(getCurrentState());

    // Apply the layer's transform, followed by the display's global transform
    // Here we're guaranteed that the layer's transform preserves rects
    Rect win(s.active.w, s.active.h);
    if (!s.crop.isEmpty()) {
        win.intersect(s.crop, &win);
    }
    // Subtract the transparent region and snap to the bounds
    Rect bounds = reduce(win, s.activeTransparentRegion);
    Rect frame(getTransform().transform(bounds));
    frame.intersect(displayDevice->getViewport(), &frame);
    if (!s.finalCrop.isEmpty()) {
        frame.intersect(s.finalCrop, &frame);
    }
    auto& displayTransform(displayDevice->getTransform());
    auto position = displayTransform.transform(frame);

    auto error = mHwcLayers[hwcId].layer->setCursorPosition(position.left,
            position.top);
    ALOGE_IF(error != HWC2::Error::None, "[%s] Failed to set cursor position "
            "to (%d, %d): %s (%d)", mName.string(), position.left,
            position.top, to_string(error).c_str(),
            static_cast<int32_t>(error));
}
#else
void Layer::setAcquireFence(const sp<const DisplayDevice>& /* hw */,
        HWComposer::HWCLayerInterface& layer) {
    int fenceFd = -1;

    // TODO: there is a possible optimization here: we only need to set the
    // acquire fence the first time a new buffer is acquired on EACH display.

    if (layer.getCompositionType() == HWC_OVERLAY || layer.getCompositionType() == HWC_CURSOR_OVERLAY) {
        sp<Fence> fence = mSurfaceFlingerConsumer->getCurrentFence();
        if (fence->isValid()) {
            fenceFd = fence->dup();
            if (fenceFd == -1) {
                ALOGW("failed to dup layer fence, skipping sync: %d", errno);
            }
        }
    }
    layer.setAcquireFenceFd(fenceFd);
}

Rect Layer::getPosition(
    const sp<const DisplayDevice>& hw)
{
    // this gives us only the "orientation" component of the transform
    const State& s(getCurrentState());

    // apply the layer's transform, followed by the display's global transform
    // here we're guaranteed that the layer's transform preserves rects
    Rect win(s.active.w, s.active.h);
    if (!s.crop.isEmpty()) {
        win.intersect(s.crop, &win);
    }
    // subtract the transparent region and snap to the bounds
    Rect bounds = reduce(win, s.activeTransparentRegion);
    Rect frame(getTransform().transform(bounds));
    frame.intersect(hw->getViewport(), &frame);
    if (!s.finalCrop.isEmpty()) {
        frame.intersect(s.finalCrop, &frame);
    }
    const Transform& tr(hw->getTransform());
    return Rect(tr.transform(frame));
}
#endif

// ---------------------------------------------------------------------------
// drawing...
// ---------------------------------------------------------------------------

void Layer::draw(const RenderArea& renderArea, const Region& clip) const {
    onDraw(renderArea, clip, false);
}

void Layer::draw(const RenderArea& renderArea, bool useIdentityTransform) const {
    onDraw(renderArea, Region(renderArea.getBounds()), useIdentityTransform);
}

void Layer::draw(const RenderArea& renderArea) const {
    onDraw(renderArea, Region(renderArea.getBounds()), false);
}

static constexpr mat4 inverseOrientation(uint32_t transform) {
    const mat4 flipH(-1,0,0,0,  0,1,0,0, 0,0,1,0, 1,0,0,1);
    const mat4 flipV( 1,0,0,0, 0,-1,0,0, 0,0,1,0, 0,1,0,1);
    const mat4 rot90( 0,1,0,0, -1,0,0,0, 0,0,1,0, 1,0,0,1);
    mat4 tr;

    if (transform & NATIVE_WINDOW_TRANSFORM_ROT_90) {
        tr = tr * rot90;
    }
    if (transform & NATIVE_WINDOW_TRANSFORM_FLIP_H) {
        tr = tr * flipH;
    }
    if (transform & NATIVE_WINDOW_TRANSFORM_FLIP_V) {
        tr = tr * flipV;
    }
    return inverse(tr);
}

/*
 * onDraw will draw the current layer onto the presentable buffer
 */
void Layer::onDraw(const RenderArea& renderArea, const Region& clip,
        bool useIdentityTransform) const
{
    ATRACE_CALL();

    if (CC_UNLIKELY(mActiveBuffer == 0)) {
        // the texture has not been created yet, this Layer has
        // in fact never been drawn into. This happens frequently with
        // SurfaceView because the WindowManager can't know when the client
        // has drawn the first time.

        // If there is nothing under us, we paint the screen in black, otherwise
        // we just skip this update.

        // figure out if there is something below us
        Region under;
        bool finished = false;
        mFlinger->mDrawingState.traverseInZOrder([&](Layer* layer) {
            if (finished || layer == static_cast<Layer const*>(this)) {
                finished = true;
                return;
            }
            under.orSelf(renderArea.getTransform().transform(layer->visibleRegion));
        });
        // if not everything below us is covered, we plug the holes!
        Region holes(clip.subtract(under));
        if (!holes.isEmpty()) {
            clearWithOpenGL(renderArea, 0, 0, 0, 1);
        }
        return;
    }

    // Bind the current buffer to the GL texture, and wait for it to be
    // ready for us to draw into.
    status_t err = mSurfaceFlingerConsumer->bindTextureImage();
    if (err != NO_ERROR) {
        ALOGW("onDraw: bindTextureImage failed (err=%d)", err);
        // Go ahead and draw the buffer anyway; no matter what we do the screen
        // is probably going to have something visibly wrong.
    }

    bool blackOutLayer = isProtected() || (isSecure() && !renderArea.isSecure());

    RenderEngine& engine(mFlinger->getRenderEngine());

    if (!blackOutLayer) {
        // TODO: we could be more subtle with isFixedSize()
        const bool useFiltering = getFiltering() || needsFiltering(renderArea) || isFixedSize();

        // Query the texture matrix given our current filtering mode.
        float textureMatrix[16];
        mSurfaceFlingerConsumer->setFilteringEnabled(useFiltering);
        mSurfaceFlingerConsumer->getTransformMatrix(textureMatrix);

        if (getTransformToDisplayInverse()) {

            /*
             * the code below applies the primary display's inverse transform to
             * the texture transform
             */
            uint32_t transform =
                    DisplayDevice::getPrimaryDisplayOrientationTransform();
            mat4 tr = inverseOrientation(transform);

            /**
             * TODO(b/36727915): This is basically a hack.
             *
             * Ensure that regardless of the parent transformation,
             * this buffer is always transformed from native display
             * orientation to display orientation. For example, in the case
             * of a camera where the buffer remains in native orientation,
             * we want the pixels to always be upright.
             */
            sp<Layer> p = mDrawingParent.promote();
            if (p != nullptr) {
                const auto parentTransform = p->getTransform();
                tr = tr * inverseOrientation(parentTransform.getOrientation());
            }

            // and finally apply it to the original texture matrix
            const mat4 texTransform(mat4(static_cast<const float*>(textureMatrix)) * tr);
            memcpy(textureMatrix, texTransform.asArray(), sizeof(textureMatrix));
        }

        // Set things up for texturing.
        mTexture.setDimensions(mActiveBuffer->getWidth(), mActiveBuffer->getHeight());
        mTexture.setFiltering(useFiltering);
        mTexture.setMatrix(textureMatrix);

        engine.setupLayerTexturing(mTexture);
    } else {
        engine.setupLayerBlackedOut();
    }
    drawWithOpenGL(renderArea, useIdentityTransform);
    engine.disableTexturing();
}


void Layer::clearWithOpenGL(const RenderArea& renderArea,
        float red, float green, float blue,
        float alpha) const
{
    RenderEngine& engine(mFlinger->getRenderEngine());
    computeGeometry(renderArea, mMesh, false);
    engine.setupFillWithColor(red, green, blue, alpha);
    engine.drawMesh(mMesh);
}

void Layer::clearWithOpenGL(const RenderArea& renderArea) const {
    clearWithOpenGL(renderArea, 0,0,0,0);
}

void Layer::drawWithOpenGL(const RenderArea& renderArea, bool useIdentityTransform) const {
    const State& s(getDrawingState());

    computeGeometry(renderArea, mMesh, useIdentityTransform);

    /*
     * NOTE: the way we compute the texture coordinates here produces
     * different results than when we take the HWC path -- in the later case
     * the "source crop" is rounded to texel boundaries.
     * This can produce significantly different results when the texture
     * is scaled by a large amount.
     *
     * The GL code below is more logical (imho), and the difference with
     * HWC is due to a limitation of the HWC API to integers -- a question
     * is suspend is whether we should ignore this problem or revert to
     * GL composition when a buffer scaling is applied (maybe with some
     * minimal value)? Or, we could make GL behave like HWC -- but this feel
     * like more of a hack.
     */
    Rect win(computeBounds());

    Transform t = getTransform();
    if (!s.finalCrop.isEmpty()) {
        win = t.transform(win);
        if (!win.intersect(s.finalCrop, &win)) {
            win.clear();
        }
        win = t.inverse().transform(win);
        if (!win.intersect(computeBounds(), &win)) {
            win.clear();
        }
    }

    float left   = float(win.left)   / float(s.active.w);
    float top    = float(win.top)    / float(s.active.h);
    float right  = float(win.right)  / float(s.active.w);
    float bottom = float(win.bottom) / float(s.active.h);

    // TODO: we probably want to generate the texture coords with the mesh
    // here we assume that we only have 4 vertices
    Mesh::VertexArray<vec2> texCoords(mMesh.getTexCoordArray<vec2>());
    texCoords[0] = vec2(left, 1.0f - top);
    texCoords[1] = vec2(left, 1.0f - bottom);
    texCoords[2] = vec2(right, 1.0f - bottom);
    texCoords[3] = vec2(right, 1.0f - top);

    RenderEngine& engine(mFlinger->getRenderEngine());
    engine.setupLayerBlending(mPremultipliedAlpha, isOpaque(s),
        false /* disableTexture */, getColor());
#ifdef USE_HWC2
    engine.setSourceDataSpace(mCurrentState.dataSpace);
#endif
    engine.drawMesh(mMesh);
    engine.disableBlending();
}

#ifdef USE_HWC2
void Layer::setCompositionType(int32_t hwcId, HWC2::Composition type,
        bool callIntoHwc) {
    if (mHwcLayers.count(hwcId) == 0) {
        ALOGE("setCompositionType called without a valid HWC layer");
        return;
    }
    auto& hwcInfo = mHwcLayers[hwcId];
    auto& hwcLayer = hwcInfo.layer;
    ALOGV("setCompositionType(%" PRIx64 ", %s, %d)", hwcLayer->getId(),
            to_string(type).c_str(), static_cast<int>(callIntoHwc));
    if (hwcInfo.compositionType != type) {
        ALOGV("    actually setting");
        hwcInfo.compositionType = type;
        if (callIntoHwc) {
            auto error = hwcLayer->setCompositionType(type);
            ALOGE_IF(error != HWC2::Error::None, "[%s] Failed to set "
                    "composition type %s: %s (%d)", mName.string(),
                    to_string(type).c_str(), to_string(error).c_str(),
                    static_cast<int32_t>(error));
        }
    }
}

HWC2::Composition Layer::getCompositionType(int32_t hwcId) const {
    if (hwcId == DisplayDevice::DISPLAY_ID_INVALID) {
        // If we're querying the composition type for a display that does not
        // have a HWC counterpart, then it will always be Client
        return HWC2::Composition::Client;
    }
    if (mHwcLayers.count(hwcId) == 0) {
        ALOGE("getCompositionType called with an invalid HWC layer");
        return HWC2::Composition::Invalid;
    }
    return mHwcLayers.at(hwcId).compositionType;
}

void Layer::setClearClientTarget(int32_t hwcId, bool clear) {
    if (mHwcLayers.count(hwcId) == 0) {
        ALOGE("setClearClientTarget called without a valid HWC layer");
        return;
    }
    mHwcLayers[hwcId].clearClientTarget = clear;
}

bool Layer::getClearClientTarget(int32_t hwcId) const {
    if (mHwcLayers.count(hwcId) == 0) {
        ALOGE("getClearClientTarget called without a valid HWC layer");
        return false;
    }
    return mHwcLayers.at(hwcId).clearClientTarget;
}
#endif

uint32_t Layer::getProducerStickyTransform() const {
    int producerStickyTransform = 0;
    int ret = mProducer->query(NATIVE_WINDOW_STICKY_TRANSFORM, &producerStickyTransform);
    if (ret != OK) {
        ALOGW("%s: Error %s (%d) while querying window sticky transform.", __FUNCTION__,
                strerror(-ret), ret);
        return 0;
    }
    return static_cast<uint32_t>(producerStickyTransform);
}

bool Layer::latchUnsignaledBuffers() {
    static bool propertyLoaded = false;
    static bool latch = false;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (!propertyLoaded) {
        char value[PROPERTY_VALUE_MAX] = {};
        property_get("debug.sf.latch_unsignaled", value, "0");
        latch = atoi(value);
        propertyLoaded = true;
    }
    return latch;
}

uint64_t Layer::getHeadFrameNumber() const {
    Mutex::Autolock lock(mQueueItemLock);
    if (!mQueueItems.empty()) {
        return mQueueItems[0].mFrameNumber;
    } else {
        return mCurrentFrameNumber;
    }
}

bool Layer::headFenceHasSignaled() const {
#ifdef USE_HWC2
    if (latchUnsignaledBuffers()) {
        return true;
    }

    Mutex::Autolock lock(mQueueItemLock);
    if (mQueueItems.empty()) {
        return true;
    }
    if (mQueueItems[0].mIsDroppable) {
        // Even though this buffer's fence may not have signaled yet, it could
        // be replaced by another buffer before it has a chance to, which means
        // that it's possible to get into a situation where a buffer is never
        // able to be latched. To avoid this, grab this buffer anyway.
        return true;
    }
    return mQueueItems[0].mFenceTime->getSignalTime() !=
            Fence::SIGNAL_TIME_PENDING;
#else
    return true;
#endif
}

bool Layer::addSyncPoint(const std::shared_ptr<SyncPoint>& point) {
    if (point->getFrameNumber() <= mCurrentFrameNumber) {
        // Don't bother with a SyncPoint, since we've already latched the
        // relevant frame
        return false;
    }

    Mutex::Autolock lock(mLocalSyncPointMutex);
    mLocalSyncPoints.push_back(point);
    return true;
}

void Layer::setFiltering(bool filtering) {
    mFiltering = filtering;
}

bool Layer::getFiltering() const {
    return mFiltering;
}

// As documented in libhardware header, formats in the range
// 0x100 - 0x1FF are specific to the HAL implementation, and
// are known to have no alpha channel
// TODO: move definition for device-specific range into
// hardware.h, instead of using hard-coded values here.
#define HARDWARE_IS_DEVICE_FORMAT(f) ((f) >= 0x100 && (f) <= 0x1FF)

bool Layer::getOpacityForFormat(uint32_t format) {
    if (HARDWARE_IS_DEVICE_FORMAT(format)) {
        return true;
    }
    switch (format) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
        case HAL_PIXEL_FORMAT_RGBA_FP16:
        case HAL_PIXEL_FORMAT_RGBA_1010102:
            return false;
    }
    // in all other case, we have no blending (also for unknown formats)
    return true;
}

// ----------------------------------------------------------------------------
// local state
// ----------------------------------------------------------------------------

static void boundPoint(vec2* point, const Rect& crop) {
    if (point->x < crop.left) {
        point->x = crop.left;
    }
    if (point->x > crop.right) {
        point->x = crop.right;
    }
    if (point->y < crop.top) {
        point->y = crop.top;
    }
    if (point->y > crop.bottom) {
        point->y = crop.bottom;
    }
}

void Layer::computeGeometry(const RenderArea& renderArea, Mesh& mesh,
                            bool useIdentityTransform) const {
    const Layer::State& s(getDrawingState());
    const Transform renderAreaTransform(renderArea.getTransform());
    const uint32_t height = renderArea.getHeight();
    Rect win = computeBounds();

    vec2 lt = vec2(win.left, win.top);
    vec2 lb = vec2(win.left, win.bottom);
    vec2 rb = vec2(win.right, win.bottom);
    vec2 rt = vec2(win.right, win.top);

    Transform layerTransform = getTransform();
    if (!useIdentityTransform) {
        lt = layerTransform.transform(lt);
        lb = layerTransform.transform(lb);
        rb = layerTransform.transform(rb);
        rt = layerTransform.transform(rt);
    }

    if (!s.finalCrop.isEmpty()) {
        boundPoint(&lt, s.finalCrop);
        boundPoint(&lb, s.finalCrop);
        boundPoint(&rb, s.finalCrop);
        boundPoint(&rt, s.finalCrop);
    }

    Mesh::VertexArray<vec2> position(mesh.getPositionArray<vec2>());
    position[0] = renderAreaTransform.transform(lt);
    position[1] = renderAreaTransform.transform(lb);
    position[2] = renderAreaTransform.transform(rb);
    position[3] = renderAreaTransform.transform(rt);
    for (size_t i=0 ; i<4 ; i++) {
        position[i].y = height - position[i].y;
    }
}

bool Layer::isOpaque(const Layer::State& s) const
{
    // if we don't have a buffer or sidebandStream yet, we're translucent regardless of the
    // layer's opaque flag.
    if ((mSidebandStream == nullptr) && (mActiveBuffer == nullptr)) {
        return false;
    }

    // if the layer has the opaque flag, then we're always opaque,
    // otherwise we use the current buffer's format.
    return ((s.flags & layer_state_t::eLayerOpaque) != 0) || mCurrentOpacity;
}

bool Layer::isSecure() const
{
    const Layer::State& s(mDrawingState);
    return (s.flags & layer_state_t::eLayerSecure);
}

bool Layer::isProtected() const
{
    const sp<GraphicBuffer>& activeBuffer(mActiveBuffer);
    return (activeBuffer != 0) &&
            (activeBuffer->getUsage() & GRALLOC_USAGE_PROTECTED);
}

bool Layer::isFixedSize() const {
    return getEffectiveScalingMode() != NATIVE_WINDOW_SCALING_MODE_FREEZE;
}

bool Layer::isCropped() const {
    return !mCurrentCrop.isEmpty();
}

bool Layer::needsFiltering(const RenderArea& renderArea) const {
    return mNeedsFiltering || renderArea.needsFiltering();
}

void Layer::setVisibleRegion(const Region& visibleRegion) {
    // always called from main thread
    this->visibleRegion = visibleRegion;
}

void Layer::setCoveredRegion(const Region& coveredRegion) {
    // always called from main thread
    this->coveredRegion = coveredRegion;
}

void Layer::setVisibleNonTransparentRegion(const Region&
        setVisibleNonTransparentRegion) {
    // always called from main thread
    this->visibleNonTransparentRegion = setVisibleNonTransparentRegion;
}

// ----------------------------------------------------------------------------
// transaction
// ----------------------------------------------------------------------------

void Layer::pushPendingState() {
    if (!mCurrentState.modified) {
        return;
    }

    // If this transaction is waiting on the receipt of a frame, generate a sync
    // point and send it to the remote layer.
    if (mCurrentState.barrierLayer != nullptr) {
        sp<Layer> barrierLayer = mCurrentState.barrierLayer.promote();
        if (barrierLayer == nullptr) {
            ALOGE("[%s] Unable to promote barrier Layer.", mName.string());
            // If we can't promote the layer we are intended to wait on,
            // then it is expired or otherwise invalid. Allow this transaction
            // to be applied as per normal (no synchronization).
            mCurrentState.barrierLayer = nullptr;
        } else {
            auto syncPoint = std::make_shared<SyncPoint>(
                    mCurrentState.frameNumber);
            if (barrierLayer->addSyncPoint(syncPoint)) {
                mRemoteSyncPoints.push_back(std::move(syncPoint));
            } else {
                // We already missed the frame we're supposed to synchronize
                // on, so go ahead and apply the state update
                mCurrentState.barrierLayer = nullptr;
            }
        }

        // Wake us up to check if the frame has been received
        setTransactionFlags(eTransactionNeeded);
        mFlinger->setTransactionFlags(eTraversalNeeded);
    }
    mPendingStates.push_back(mCurrentState);
    ATRACE_INT(mTransactionName.string(), mPendingStates.size());
}

void Layer::popPendingState(State* stateToCommit) {
    auto oldFlags = stateToCommit->flags;
    *stateToCommit = mPendingStates[0];
    stateToCommit->flags = (oldFlags & ~stateToCommit->mask) |
            (stateToCommit->flags & stateToCommit->mask);

    mPendingStates.removeAt(0);
    ATRACE_INT(mTransactionName.string(), mPendingStates.size());
}

bool Layer::applyPendingStates(State* stateToCommit) {
    bool stateUpdateAvailable = false;
    while (!mPendingStates.empty()) {
        if (mPendingStates[0].barrierLayer != nullptr) {
            if (mRemoteSyncPoints.empty()) {
                // If we don't have a sync point for this, apply it anyway. It
                // will be visually wrong, but it should keep us from getting
                // into too much trouble.
                ALOGE("[%s] No local sync point found", mName.string());
                popPendingState(stateToCommit);
                stateUpdateAvailable = true;
                continue;
            }

            if (mRemoteSyncPoints.front()->getFrameNumber() !=
                    mPendingStates[0].frameNumber) {
                ALOGE("[%s] Unexpected sync point frame number found",
                        mName.string());

                // Signal our end of the sync point and then dispose of it
                mRemoteSyncPoints.front()->setTransactionApplied();
                mRemoteSyncPoints.pop_front();
                continue;
            }

            if (mRemoteSyncPoints.front()->frameIsAvailable()) {
                // Apply the state update
                popPendingState(stateToCommit);
                stateUpdateAvailable = true;

                // Signal our end of the sync point and then dispose of it
                mRemoteSyncPoints.front()->setTransactionApplied();
                mRemoteSyncPoints.pop_front();
            } else {
                break;
            }
        } else {
            popPendingState(stateToCommit);
            stateUpdateAvailable = true;
        }
    }

    // If we still have pending updates, wake SurfaceFlinger back up and point
    // it at this layer so we can process them
    if (!mPendingStates.empty()) {
        setTransactionFlags(eTransactionNeeded);
        mFlinger->setTransactionFlags(eTraversalNeeded);
    }

    mCurrentState.modified = false;
    return stateUpdateAvailable;
}

void Layer::notifyAvailableFrames() {
    auto headFrameNumber = getHeadFrameNumber();
    bool headFenceSignaled = headFenceHasSignaled();
    Mutex::Autolock lock(mLocalSyncPointMutex);
    for (auto& point : mLocalSyncPoints) {
        if (headFrameNumber >= point->getFrameNumber() && headFenceSignaled) {
            point->setFrameAvailable();
        }
    }
}

uint32_t Layer::doTransaction(uint32_t flags) {
    ATRACE_CALL();

    pushPendingState();
    Layer::State c = getCurrentState();
    if (!applyPendingStates(&c)) {
        return 0;
    }

    const Layer::State& s(getDrawingState());

    const bool sizeChanged = (c.requested.w != s.requested.w) ||
                             (c.requested.h != s.requested.h);

    if (sizeChanged) {
        // the size changed, we need to ask our client to request a new buffer
        ALOGD_IF(DEBUG_RESIZE,
                "doTransaction: geometry (layer=%p '%s'), tr=%02x, scalingMode=%d\n"
                "  current={ active   ={ wh={%4u,%4u} crop={%4d,%4d,%4d,%4d} (%4d,%4d) }\n"
                "            requested={ wh={%4u,%4u} }}\n"
                "  drawing={ active   ={ wh={%4u,%4u} crop={%4d,%4d,%4d,%4d} (%4d,%4d) }\n"
                "            requested={ wh={%4u,%4u} }}\n",
                this, getName().string(), mCurrentTransform,
                getEffectiveScalingMode(),
                c.active.w, c.active.h,
                c.crop.left,
                c.crop.top,
                c.crop.right,
                c.crop.bottom,
                c.crop.getWidth(),
                c.crop.getHeight(),
                c.requested.w, c.requested.h,
                s.active.w, s.active.h,
                s.crop.left,
                s.crop.top,
                s.crop.right,
                s.crop.bottom,
                s.crop.getWidth(),
                s.crop.getHeight(),
                s.requested.w, s.requested.h);

        // record the new size, form this point on, when the client request
        // a buffer, it'll get the new size.
        mSurfaceFlingerConsumer->setDefaultBufferSize(
                c.requested.w, c.requested.h);
    }

    // Don't let Layer::doTransaction update the drawing state
    // if we have a pending resize, unless we are in fixed-size mode.
    // the drawing state will be updated only once we receive a buffer
    // with the correct size.
    //
    // In particular, we want to make sure the clip (which is part
    // of the geometry state) is latched together with the size but is
    // latched immediately when no resizing is involved.
    //
    // If a sideband stream is attached, however, we want to skip this
    // optimization so that transactions aren't missed when a buffer
    // never arrives
    //
    // In the case that we don't have a buffer we ignore other factors
    // and avoid entering the resizePending state. At a high level the
    // resizePending state is to avoid applying the state of the new buffer
    // to the old buffer. However in the state where we don't have an old buffer
    // there is no such concern but we may still be being used as a parent layer.
    const bool resizePending = ((c.requested.w != c.active.w) ||
            (c.requested.h != c.active.h)) && (mActiveBuffer != nullptr);
    if (!isFixedSize()) {
        if (resizePending && mSidebandStream == NULL) {
            flags |= eDontUpdateGeometryState;
        }
    }

    // Here we apply various requested geometry states, depending on our
    // latching configuration. See Layer.h for a detailed discussion of
    // how geometry latching is controlled.
    if (!(flags & eDontUpdateGeometryState)) {
        Layer::State& editCurrentState(getCurrentState());

        // If mFreezeGeometryUpdates is true we are in the setGeometryAppliesWithResize
        // mode, which causes attributes which normally latch regardless of scaling mode,
        // to be delayed. We copy the requested state to the active state making sure
        // to respect these rules (again see Layer.h for a detailed discussion).
        //
        // There is an awkward asymmetry in the handling of the crop states in the position
        // states, as can be seen below. Largely this arises from position and transform
        // being stored in the same data structure while having different latching rules.
        // b/38182305
        //
        // Careful that "c" and editCurrentState may not begin as equivalent due to
        // applyPendingStates in the presence of deferred transactions.
        if (mFreezeGeometryUpdates) {
            float tx = c.active.transform.tx();
            float ty = c.active.transform.ty();
            c.active = c.requested;
            c.active.transform.set(tx, ty);
            editCurrentState.active = c.active;
        } else {
            editCurrentState.active = editCurrentState.requested;
            c.active = c.requested;
        }
    }

    if (s.active != c.active) {
        // invalidate and recompute the visible regions if needed
        flags |= Layer::eVisibleRegion;
    }

    if (c.sequence != s.sequence) {
        // invalidate and recompute the visible regions if needed
        flags |= eVisibleRegion;
        this->contentDirty = true;

        // we may use linear filtering, if the matrix scales us
        const uint8_t type = c.active.transform.getType();
        mNeedsFiltering = (!c.active.transform.preserveRects() ||
                (type >= Transform::SCALE));
    }

    // If the layer is hidden, signal and clear out all local sync points so
    // that transactions for layers depending on this layer's frames becoming
    // visible are not blocked
    if (c.flags & layer_state_t::eLayerHidden) {
        clearSyncPoints();
    }

    // Commit the transaction
    commitTransaction(c);
    return flags;
}

void Layer::commitTransaction(const State& stateToCommit) {
    mDrawingState = stateToCommit;
}

uint32_t Layer::getTransactionFlags(uint32_t flags) {
    return android_atomic_and(~flags, &mTransactionFlags) & flags;
}

uint32_t Layer::setTransactionFlags(uint32_t flags) {
    return android_atomic_or(flags, &mTransactionFlags);
}

bool Layer::setPosition(float x, float y, bool immediate) {
    if (mCurrentState.requested.transform.tx() == x && mCurrentState.requested.transform.ty() == y)
        return false;
    mCurrentState.sequence++;

    // We update the requested and active position simultaneously because
    // we want to apply the position portion of the transform matrix immediately,
    // but still delay scaling when resizing a SCALING_MODE_FREEZE layer.
    mCurrentState.requested.transform.set(x, y);
    if (immediate && !mFreezeGeometryUpdates) {
        // Here we directly update the active state
        // unlike other setters, because we store it within
        // the transform, but use different latching rules.
        // b/38182305
        mCurrentState.active.transform.set(x, y);
    }
    mFreezeGeometryUpdates = mFreezeGeometryUpdates || !immediate;

    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setChildLayer(const sp<Layer>& childLayer, int32_t z) {
    ssize_t idx = mCurrentChildren.indexOf(childLayer);
    if (idx < 0) {
        return false;
    }
    if (childLayer->setLayer(z)) {
        mCurrentChildren.removeAt(idx);
        mCurrentChildren.add(childLayer);
    }
    return true;
}

bool Layer::setLayer(int32_t z) {
    if (mCurrentState.z == z)
        return false;
    mCurrentState.sequence++;
    mCurrentState.z = z;
    mCurrentState.modified = true;

    // Discard all relative layering.
    if (mCurrentState.zOrderRelativeOf != nullptr) {
        sp<Layer> strongRelative = mCurrentState.zOrderRelativeOf.promote();
        if (strongRelative != nullptr) {
            strongRelative->removeZOrderRelative(this);
        }
        mCurrentState.zOrderRelativeOf = nullptr;
    }
    setTransactionFlags(eTransactionNeeded);
    return true;
}

void Layer::removeZOrderRelative(const wp<Layer>& relative) {
    mCurrentState.zOrderRelatives.remove(relative);
    mCurrentState.sequence++;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
}

void Layer::addZOrderRelative(const wp<Layer>& relative) {
    mCurrentState.zOrderRelatives.add(relative);
    mCurrentState.modified = true;
    mCurrentState.sequence++;
    setTransactionFlags(eTransactionNeeded);
}

bool Layer::setRelativeLayer(const sp<IBinder>& relativeToHandle, int32_t z) {
    sp<Handle> handle = static_cast<Handle*>(relativeToHandle.get());
    if (handle == nullptr) {
        return false;
    }
    sp<Layer> relative = handle->owner.promote();
    if (relative == nullptr) {
        return false;
    }

    mCurrentState.sequence++;
    mCurrentState.modified = true;
    mCurrentState.z = z;

    auto oldZOrderRelativeOf = mCurrentState.zOrderRelativeOf.promote();
    if (oldZOrderRelativeOf != nullptr) {
        oldZOrderRelativeOf->removeZOrderRelative(this);
    }
    mCurrentState.zOrderRelativeOf = relative;
    relative->addZOrderRelative(this);

    setTransactionFlags(eTransactionNeeded);

    return true;
}

bool Layer::setSize(uint32_t w, uint32_t h) {
    if (mCurrentState.requested.w == w && mCurrentState.requested.h == h)
        return false;
    mCurrentState.requested.w = w;
    mCurrentState.requested.h = h;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setAlpha(float alpha) {
    if (mCurrentState.color.a == alpha)
        return false;
    mCurrentState.sequence++;
    mCurrentState.color.a = alpha;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setColor(const half3& color) {
    if (color.r == mCurrentState.color.r && color.g == mCurrentState.color.g
        && color.b == mCurrentState.color.b)
        return false;

    mCurrentState.sequence++;
    mCurrentState.color.r = color.r;
    mCurrentState.color.g = color.g;
    mCurrentState.color.b = color.b;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setMatrix(const layer_state_t::matrix22_t& matrix) {
    mCurrentState.sequence++;
    mCurrentState.requested.transform.set(
            matrix.dsdx, matrix.dtdy, matrix.dtdx, matrix.dsdy);
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setTransparentRegionHint(const Region& transparent) {
    mCurrentState.requestedTransparentRegion = transparent;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setFlags(uint8_t flags, uint8_t mask) {
    const uint32_t newFlags = (mCurrentState.flags & ~mask) | (flags & mask);
    if (mCurrentState.flags == newFlags)
        return false;
    mCurrentState.sequence++;
    mCurrentState.flags = newFlags;
    mCurrentState.mask = mask;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setCrop(const Rect& crop, bool immediate) {
    if (mCurrentState.requestedCrop == crop)
        return false;
    mCurrentState.sequence++;
    mCurrentState.requestedCrop = crop;
    if (immediate && !mFreezeGeometryUpdates) {
        mCurrentState.crop = crop;
    }
    mFreezeGeometryUpdates = mFreezeGeometryUpdates || !immediate;

    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setFinalCrop(const Rect& crop, bool immediate) {
    if (mCurrentState.requestedFinalCrop == crop)
        return false;
    mCurrentState.sequence++;
    mCurrentState.requestedFinalCrop = crop;
    if (immediate && !mFreezeGeometryUpdates) {
        mCurrentState.finalCrop = crop;
    }
    mFreezeGeometryUpdates = mFreezeGeometryUpdates || !immediate;

    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setOverrideScalingMode(int32_t scalingMode) {
    if (scalingMode == mOverrideScalingMode)
        return false;
    mOverrideScalingMode = scalingMode;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

void Layer::setInfo(uint32_t type, uint32_t appId) {
  mCurrentState.appId = appId;
  mCurrentState.type = type;
  mCurrentState.modified = true;
  setTransactionFlags(eTransactionNeeded);
}

uint32_t Layer::getEffectiveScalingMode() const {
    if (mOverrideScalingMode >= 0) {
      return mOverrideScalingMode;
    }
    return mCurrentScalingMode;
}

bool Layer::setLayerStack(uint32_t layerStack) {
    if (mCurrentState.layerStack == layerStack)
        return false;
    mCurrentState.sequence++;
    mCurrentState.layerStack = layerStack;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setDataSpace(android_dataspace dataSpace) {
    if (mCurrentState.dataSpace == dataSpace)
        return false;
    mCurrentState.sequence++;
    mCurrentState.dataSpace = dataSpace;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

android_dataspace Layer::getDataSpace() const {
    return mCurrentState.dataSpace;
}

uint32_t Layer::getLayerStack() const {
    auto p = mDrawingParent.promote();
    if (p == nullptr) {
        return getDrawingState().layerStack;
    }
    return p->getLayerStack();
}

void Layer::deferTransactionUntil(const sp<Layer>& barrierLayer,
        uint64_t frameNumber) {
    mCurrentState.barrierLayer = barrierLayer;
    mCurrentState.frameNumber = frameNumber;
    // We don't set eTransactionNeeded, because just receiving a deferral
    // request without any other state updates shouldn't actually induce a delay
    mCurrentState.modified = true;
    pushPendingState();
    mCurrentState.barrierLayer = nullptr;
    mCurrentState.frameNumber = 0;
    mCurrentState.modified = false;
}

void Layer::deferTransactionUntil(const sp<IBinder>& barrierHandle,
        uint64_t frameNumber) {
    sp<Handle> handle = static_cast<Handle*>(barrierHandle.get());
    deferTransactionUntil(handle->owner.promote(), frameNumber);
}

void Layer::useSurfaceDamage() {
    if (mFlinger->mForceFullDamage) {
        surfaceDamageRegion = Region::INVALID_REGION;
    } else {
        surfaceDamageRegion = mSurfaceFlingerConsumer->getSurfaceDamage();
    }
}

void Layer::useEmptyDamage() {
    surfaceDamageRegion.clear();
}

// ----------------------------------------------------------------------------
// pageflip handling...
// ----------------------------------------------------------------------------

bool Layer::shouldPresentNow(const DispSync& dispSync) const {
    if (mSidebandStreamChanged || mAutoRefresh) {
        return true;
    }

    Mutex::Autolock lock(mQueueItemLock);
    if (mQueueItems.empty()) {
        return false;
    }
    auto timestamp = mQueueItems[0].mTimestamp;
    nsecs_t expectedPresent =
            mSurfaceFlingerConsumer->computeExpectedPresent(dispSync);

    // Ignore timestamps more than a second in the future
    bool isPlausible = timestamp < (expectedPresent + s2ns(1));
    ALOGW_IF(!isPlausible, "[%s] Timestamp %" PRId64 " seems implausible "
            "relative to expectedPresent %" PRId64, mName.string(), timestamp,
            expectedPresent);

    bool isDue = timestamp < expectedPresent;
    return isDue || !isPlausible;
}

bool Layer::onPreComposition(nsecs_t refreshStartTime) {
    if (mBufferLatched) {
        Mutex::Autolock lock(mFrameEventHistoryMutex);
        mFrameEventHistory.addPreComposition(mCurrentFrameNumber, refreshStartTime);
    }
    mRefreshPending = false;
    return mQueuedFrames > 0 || mSidebandStreamChanged || mAutoRefresh;
}

bool Layer::onPostComposition(const std::shared_ptr<FenceTime>& glDoneFence,
        const std::shared_ptr<FenceTime>& presentFence,
        const CompositorTiming& compositorTiming) {
    // mFrameLatencyNeeded is true when a new frame was latched for the
    // composition.
    if (!mFrameLatencyNeeded)
        return false;

    // Update mFrameEventHistory.
    {
        Mutex::Autolock lock(mFrameEventHistoryMutex);
        mFrameEventHistory.addPostComposition(mCurrentFrameNumber,
                glDoneFence, presentFence, compositorTiming);
    }

    // Update mFrameTracker.
    nsecs_t desiredPresentTime = mSurfaceFlingerConsumer->getTimestamp();
    mFrameTracker.setDesiredPresentTime(desiredPresentTime);

    std::shared_ptr<FenceTime> frameReadyFence =
            mSurfaceFlingerConsumer->getCurrentFenceTime();
    if (frameReadyFence->isValid()) {
        mFrameTracker.setFrameReadyFence(std::move(frameReadyFence));
    } else {
        // There was no fence for this frame, so assume that it was ready
        // to be presented at the desired present time.
        mFrameTracker.setFrameReadyTime(desiredPresentTime);
    }

    if (presentFence->isValid()) {
        mFrameTracker.setActualPresentFence(
                std::shared_ptr<FenceTime>(presentFence));
    } else {
        // The HWC doesn't support present fences, so use the refresh
        // timestamp instead.
        mFrameTracker.setActualPresentTime(
            mFlinger->getHwComposer().getRefreshTimestamp(
                HWC_DISPLAY_PRIMARY));
    }

    mFrameTracker.advanceFrame();
    mFrameLatencyNeeded = false;
    return true;
}

#ifdef USE_HWC2
void Layer::releasePendingBuffer(nsecs_t dequeueReadyTime) {
    if (!mSurfaceFlingerConsumer->releasePendingBuffer()) {
        return;
    }

    auto releaseFenceTime = std::make_shared<FenceTime>(
            mSurfaceFlingerConsumer->getPrevFinalReleaseFence());
    mReleaseTimeline.updateSignalTimes();
    mReleaseTimeline.push(releaseFenceTime);

    Mutex::Autolock lock(mFrameEventHistoryMutex);
    if (mPreviousFrameNumber != 0) {
        mFrameEventHistory.addRelease(mPreviousFrameNumber,
                dequeueReadyTime, std::move(releaseFenceTime));
    }
}
#endif

bool Layer::isHiddenByPolicy() const {
    const Layer::State& s(mDrawingState);
    const auto& parent = mDrawingParent.promote();
    if (parent != nullptr && parent->isHiddenByPolicy()) {
        return true;
    }
    return s.flags & layer_state_t::eLayerHidden;
}

bool Layer::isVisible() const {
    return !(isHiddenByPolicy()) && getAlpha() > 0.0f
            && (mActiveBuffer != NULL || mSidebandStream != NULL);
}

bool Layer::allTransactionsSignaled() {
    auto headFrameNumber = getHeadFrameNumber();
    bool matchingFramesFound = false;
    bool allTransactionsApplied = true;
    Mutex::Autolock lock(mLocalSyncPointMutex);

    for (auto& point : mLocalSyncPoints) {
        if (point->getFrameNumber() > headFrameNumber) {
            break;
        }
        matchingFramesFound = true;

        if (!point->frameIsAvailable()) {
           // We haven't notified the remote layer that the frame for
           // this point is available yet. Notify it now, and then
           // abort this attempt to latch.
           point->setFrameAvailable();
           allTransactionsApplied = false;
           break;
        }

        allTransactionsApplied = allTransactionsApplied && point->transactionIsApplied();
    }
    return !matchingFramesFound || allTransactionsApplied;
}

Region Layer::latchBuffer(bool& recomputeVisibleRegions, nsecs_t latchTime)
{
    ATRACE_CALL();

    if (android_atomic_acquire_cas(true, false, &mSidebandStreamChanged) == 0) {
        // mSidebandStreamChanged was true
        mSidebandStream = mSurfaceFlingerConsumer->getSidebandStream();
        if (mSidebandStream != NULL) {
            setTransactionFlags(eTransactionNeeded);
            mFlinger->setTransactionFlags(eTraversalNeeded);
        }
        recomputeVisibleRegions = true;

        const State& s(getDrawingState());
        return getTransform().transform(Region(Rect(s.active.w, s.active.h)));
    }

    Region outDirtyRegion;
    if (mQueuedFrames <= 0 && !mAutoRefresh) {
        return outDirtyRegion;
    }

    // if we've already called updateTexImage() without going through
    // a composition step, we have to skip this layer at this point
    // because we cannot call updateTeximage() without a corresponding
    // compositionComplete() call.
    // we'll trigger an update in onPreComposition().
    if (mRefreshPending) {
        return outDirtyRegion;
    }

    // If the head buffer's acquire fence hasn't signaled yet, return and
    // try again later
    if (!headFenceHasSignaled()) {
        mFlinger->signalLayerUpdate();
        return outDirtyRegion;
    }

    // Capture the old state of the layer for comparisons later
    const State& s(getDrawingState());
    const bool oldOpacity = isOpaque(s);
    sp<GraphicBuffer> oldActiveBuffer = mActiveBuffer;

    if (!allTransactionsSignaled()) {
        mFlinger->signalLayerUpdate();
        return outDirtyRegion;
    }

    // This boolean is used to make sure that SurfaceFlinger's shadow copy
    // of the buffer queue isn't modified when the buffer queue is returning
    // BufferItem's that weren't actually queued. This can happen in shared
    // buffer mode.
    bool queuedBuffer = false;
    LayerRejecter r(mDrawingState, getCurrentState(), recomputeVisibleRegions,
                    getProducerStickyTransform() != 0, mName.string(),
                    mOverrideScalingMode, mFreezeGeometryUpdates);
    status_t updateResult = mSurfaceFlingerConsumer->updateTexImage(&r,
            mFlinger->mPrimaryDispSync, &mAutoRefresh, &queuedBuffer,
            mLastFrameNumberReceived);
    if (updateResult == BufferQueue::PRESENT_LATER) {
        // Producer doesn't want buffer to be displayed yet.  Signal a
        // layer update so we check again at the next opportunity.
        mFlinger->signalLayerUpdate();
        return outDirtyRegion;
    } else if (updateResult == SurfaceFlingerConsumer::BUFFER_REJECTED) {
        // If the buffer has been rejected, remove it from the shadow queue
        // and return early
        if (queuedBuffer) {
            Mutex::Autolock lock(mQueueItemLock);
            mQueueItems.removeAt(0);
            android_atomic_dec(&mQueuedFrames);
        }
        return outDirtyRegion;
    } else if (updateResult != NO_ERROR || mUpdateTexImageFailed) {
        // This can occur if something goes wrong when trying to create the
        // EGLImage for this buffer. If this happens, the buffer has already
        // been released, so we need to clean up the queue and bug out
        // early.
        if (queuedBuffer) {
            Mutex::Autolock lock(mQueueItemLock);
            mQueueItems.clear();
            android_atomic_and(0, &mQueuedFrames);
        }

        // Once we have hit this state, the shadow queue may no longer
        // correctly reflect the incoming BufferQueue's contents, so even if
        // updateTexImage starts working, the only safe course of action is
        // to continue to ignore updates.
        mUpdateTexImageFailed = true;

        return outDirtyRegion;
    }

    if (queuedBuffer) {
        // Autolock scope
        auto currentFrameNumber = mSurfaceFlingerConsumer->getFrameNumber();

        Mutex::Autolock lock(mQueueItemLock);

        // Remove any stale buffers that have been dropped during
        // updateTexImage
        while (mQueueItems[0].mFrameNumber != currentFrameNumber) {
            mQueueItems.removeAt(0);
            android_atomic_dec(&mQueuedFrames);
        }

        mQueueItems.removeAt(0);
    }


    // Decrement the queued-frames count.  Signal another event if we
    // have more frames pending.
    if ((queuedBuffer && android_atomic_dec(&mQueuedFrames) > 1)
            || mAutoRefresh) {
        mFlinger->signalLayerUpdate();
    }

    // update the active buffer
    mActiveBuffer = mSurfaceFlingerConsumer->getCurrentBuffer(
            &mActiveBufferSlot);
    if (mActiveBuffer == NULL) {
        // this can only happen if the very first buffer was rejected.
        return outDirtyRegion;
    }

    mBufferLatched = true;
    mPreviousFrameNumber = mCurrentFrameNumber;
    mCurrentFrameNumber = mSurfaceFlingerConsumer->getFrameNumber();

    {
        Mutex::Autolock lock(mFrameEventHistoryMutex);
        mFrameEventHistory.addLatch(mCurrentFrameNumber, latchTime);
#ifndef USE_HWC2
        auto releaseFenceTime = std::make_shared<FenceTime>(
                mSurfaceFlingerConsumer->getPrevFinalReleaseFence());
        mReleaseTimeline.updateSignalTimes();
        mReleaseTimeline.push(releaseFenceTime);
        if (mPreviousFrameNumber != 0) {
            mFrameEventHistory.addRelease(mPreviousFrameNumber,
                    latchTime, std::move(releaseFenceTime));
        }
#endif
    }

    mRefreshPending = true;
    mFrameLatencyNeeded = true;
    if (oldActiveBuffer == NULL) {
         // the first time we receive a buffer, we need to trigger a
         // geometry invalidation.
        recomputeVisibleRegions = true;
     }

    setDataSpace(mSurfaceFlingerConsumer->getCurrentDataSpace());

    Rect crop(mSurfaceFlingerConsumer->getCurrentCrop());
    const uint32_t transform(mSurfaceFlingerConsumer->getCurrentTransform());
    const uint32_t scalingMode(mSurfaceFlingerConsumer->getCurrentScalingMode());
    if ((crop != mCurrentCrop) ||
        (transform != mCurrentTransform) ||
        (scalingMode != mCurrentScalingMode))
    {
        mCurrentCrop = crop;
        mCurrentTransform = transform;
        mCurrentScalingMode = scalingMode;
        recomputeVisibleRegions = true;
    }

    if (oldActiveBuffer != NULL) {
        uint32_t bufWidth  = mActiveBuffer->getWidth();
        uint32_t bufHeight = mActiveBuffer->getHeight();
        if (bufWidth != uint32_t(oldActiveBuffer->width) ||
            bufHeight != uint32_t(oldActiveBuffer->height)) {
            recomputeVisibleRegions = true;
        }
    }

    mCurrentOpacity = getOpacityForFormat(mActiveBuffer->format);
    if (oldOpacity != isOpaque(s)) {
        recomputeVisibleRegions = true;
    }

    // Remove any sync points corresponding to the buffer which was just
    // latched
    {
        Mutex::Autolock lock(mLocalSyncPointMutex);
        auto point = mLocalSyncPoints.begin();
        while (point != mLocalSyncPoints.end()) {
            if (!(*point)->frameIsAvailable() ||
                    !(*point)->transactionIsApplied()) {
                // This sync point must have been added since we started
                // latching. Don't drop it yet.
                ++point;
                continue;
            }

            if ((*point)->getFrameNumber() <= mCurrentFrameNumber) {
                point = mLocalSyncPoints.erase(point);
            } else {
                ++point;
            }
        }
    }

    // FIXME: postedRegion should be dirty & bounds
    Region dirtyRegion(Rect(s.active.w, s.active.h));

    // transform the dirty region to window-manager space
    outDirtyRegion = (getTransform().transform(dirtyRegion));

    return outDirtyRegion;
}

uint32_t Layer::getEffectiveUsage(uint32_t usage) const
{
    // TODO: should we do something special if mSecure is set?
    if (mProtectedByApp) {
        // need a hardware-protected path to external video sink
        usage |= GraphicBuffer::USAGE_PROTECTED;
    }
    if (mPotentialCursor) {
        usage |= GraphicBuffer::USAGE_CURSOR;
    }
    usage |= GraphicBuffer::USAGE_HW_COMPOSER;
    return usage;
}

void Layer::updateTransformHint(const sp<const DisplayDevice>& hw) const {
    uint32_t orientation = 0;
    if (!mFlinger->mDebugDisableTransformHint) {
        // The transform hint is used to improve performance, but we can
        // only have a single transform hint, it cannot
        // apply to all displays.
        const Transform& planeTransform(hw->getTransform());
        orientation = planeTransform.getOrientation();
        if (orientation & Transform::ROT_INVALID) {
            orientation = 0;
        }
    }
    mSurfaceFlingerConsumer->setTransformHint(orientation);
}

// ----------------------------------------------------------------------------
// debugging
// ----------------------------------------------------------------------------

LayerDebugInfo Layer::getLayerDebugInfo() const {
    LayerDebugInfo info;
    const Layer::State& ds = getDrawingState();
    info.mName = getName();
    sp<Layer> parent = getParent();
    info.mParentName = (parent == nullptr ? std::string("none") : parent->getName().string());
    info.mType = String8(getTypeId());
    info.mTransparentRegion = ds.activeTransparentRegion;
    info.mVisibleRegion = visibleRegion;
    info.mSurfaceDamageRegion = surfaceDamageRegion;
    info.mLayerStack = getLayerStack();
    info.mX = ds.active.transform.tx();
    info.mY = ds.active.transform.ty();
    info.mZ = ds.z;
    info.mWidth = ds.active.w;
    info.mHeight = ds.active.h;
    info.mCrop = ds.crop;
    info.mFinalCrop = ds.finalCrop;
    info.mColor = ds.color;
    info.mFlags = ds.flags;
    info.mPixelFormat = getPixelFormat();
    info.mDataSpace = getDataSpace();
    info.mMatrix[0][0] = ds.active.transform[0][0];
    info.mMatrix[0][1] = ds.active.transform[0][1];
    info.mMatrix[1][0] = ds.active.transform[1][0];
    info.mMatrix[1][1] = ds.active.transform[1][1];
    {
        sp<const GraphicBuffer> activeBuffer = getActiveBuffer();
        if (activeBuffer != 0) {
            info.mActiveBufferWidth = activeBuffer->getWidth();
            info.mActiveBufferHeight = activeBuffer->getHeight();
            info.mActiveBufferStride = activeBuffer->getStride();
            info.mActiveBufferFormat = activeBuffer->format;
        } else {
            info.mActiveBufferWidth = 0;
            info.mActiveBufferHeight = 0;
            info.mActiveBufferStride = 0;
            info.mActiveBufferFormat = 0;
        }
    }
    info.mNumQueuedFrames = getQueuedFrameCount();
    info.mRefreshPending = isBufferLatched();
    info.mIsOpaque = isOpaque(ds);
    info.mContentDirty = contentDirty;
    return info;
}
#ifdef USE_HWC2
void Layer::miniDumpHeader(String8& result) {
    result.append("----------------------------------------");
    result.append("---------------------------------------\n");
    result.append(" Layer name\n");
    result.append("           Z | ");
    result.append(" Comp Type | ");
    result.append("  Disp Frame (LTRB) | ");
    result.append("         Source Crop (LTRB)\n");
    result.append("----------------------------------------");
    result.append("---------------------------------------\n");
}

void Layer::miniDump(String8& result, int32_t hwcId) const {
    if (mHwcLayers.count(hwcId) == 0) {
        return;
    }

    String8 name;
    if (mName.length() > 77) {
        std::string shortened;
        shortened.append(mName.string(), 36);
        shortened.append("[...]");
        shortened.append(mName.string() + (mName.length() - 36), 36);
        name = shortened.c_str();
    } else {
        name = mName;
    }

    result.appendFormat(" %s\n", name.string());

    const Layer::State& layerState(getDrawingState());
    const HWCInfo& hwcInfo = mHwcLayers.at(hwcId);
    result.appendFormat("  %10d | ", layerState.z);
    result.appendFormat("%10s | ",
            to_string(getCompositionType(hwcId)).c_str());
    const Rect& frame = hwcInfo.displayFrame;
    result.appendFormat("%4d %4d %4d %4d | ", frame.left, frame.top,
            frame.right, frame.bottom);
    const FloatRect& crop = hwcInfo.sourceCrop;
    result.appendFormat("%6.1f %6.1f %6.1f %6.1f\n", crop.left, crop.top,
            crop.right, crop.bottom);

    result.append("- - - - - - - - - - - - - - - - - - - - ");
    result.append("- - - - - - - - - - - - - - - - - - - -\n");
}
#endif

void Layer::dumpFrameStats(String8& result) const {
    mFrameTracker.dumpStats(result);
}

void Layer::clearFrameStats() {
    mFrameTracker.clearStats();
}

void Layer::logFrameStats() {
    mFrameTracker.logAndResetStats(mName);
}

void Layer::getFrameStats(FrameStats* outStats) const {
    mFrameTracker.getStats(outStats);
}

void Layer::dumpFrameEvents(String8& result) {
    result.appendFormat("- Layer %s (%s, %p)\n",
            getName().string(), getTypeId(), this);
    Mutex::Autolock lock(mFrameEventHistoryMutex);
    mFrameEventHistory.checkFencesForCompletion();
    mFrameEventHistory.dump(result);
}

void Layer::onDisconnect() {
    Mutex::Autolock lock(mFrameEventHistoryMutex);
    mFrameEventHistory.onDisconnect();
}

void Layer::addAndGetFrameTimestamps(const NewFrameEventsEntry* newTimestamps,
        FrameEventHistoryDelta *outDelta) {
    Mutex::Autolock lock(mFrameEventHistoryMutex);
    if (newTimestamps) {
        // If there are any unsignaled fences in the aquire timeline at this
        // point, the previously queued frame hasn't been latched yet. Go ahead
        // and try to get the signal time here so the syscall is taken out of
        // the main thread's critical path.
        mAcquireTimeline.updateSignalTimes();
        // Push the new fence after updating since it's likely still pending.
        mAcquireTimeline.push(newTimestamps->acquireFence);
        mFrameEventHistory.addQueue(*newTimestamps);
    }

    if (outDelta) {
        mFrameEventHistory.getAndResetDelta(outDelta);
    }
}

std::vector<OccupancyTracker::Segment> Layer::getOccupancyHistory(
        bool forceFlush) {
    std::vector<OccupancyTracker::Segment> history;
    status_t result = mSurfaceFlingerConsumer->getOccupancyHistory(forceFlush,
            &history);
    if (result != NO_ERROR) {
        ALOGW("[%s] Failed to obtain occupancy history (%d)", mName.string(),
                result);
        return {};
    }
    return history;
}

bool Layer::getTransformToDisplayInverse() const {
    return mSurfaceFlingerConsumer->getTransformToDisplayInverse();
}

size_t Layer::getChildrenCount() const {
    size_t count = 0;
    for (const sp<Layer>& child : mCurrentChildren) {
        count += 1 + child->getChildrenCount();
    }
    return count;
}

void Layer::addChild(const sp<Layer>& layer) {
    mCurrentChildren.add(layer);
    layer->setParent(this);
}

ssize_t Layer::removeChild(const sp<Layer>& layer) {
    layer->setParent(nullptr);
    return mCurrentChildren.remove(layer);
}

bool Layer::reparentChildren(const sp<IBinder>& newParentHandle) {
    sp<Handle> handle = nullptr;
    sp<Layer> newParent = nullptr;
    if (newParentHandle == nullptr) {
        return false;
    }
    handle = static_cast<Handle*>(newParentHandle.get());
    newParent = handle->owner.promote();
    if (newParent == nullptr) {
        ALOGE("Unable to promote Layer handle");
        return false;
    }

    for (const sp<Layer>& child : mCurrentChildren) {
        newParent->addChild(child);

        sp<Client> client(child->mClientRef.promote());
        if (client != nullptr) {
            client->setParentLayer(newParent);
        }
    }
    mCurrentChildren.clear();

    return true;
}

bool Layer::reparent(const sp<IBinder>& newParentHandle) {
    if (newParentHandle == nullptr) {
        return false;
    }

    auto handle = static_cast<Handle*>(newParentHandle.get());
    sp<Layer> newParent = handle->owner.promote();
    if (newParent == nullptr) {
        ALOGE("Unable to promote Layer handle");
        return false;
    }

    sp<Layer> parent = getParent();
    if (parent != nullptr) {
        parent->removeChild(this);
    }
    newParent->addChild(this);

    sp<Client> client(mClientRef.promote());
    sp<Client> newParentClient(newParent->mClientRef.promote());

    if (client != newParentClient) {
        client->setParentLayer(newParent);
    }

    return true;
}

bool Layer::detachChildren() {
    traverseInZOrder(LayerVector::StateSet::Drawing, [this](Layer* child) {
        if (child == this) {
            return;
        }

        sp<Client> parentClient = mClientRef.promote();
        sp<Client> client(child->mClientRef.promote());
        if (client != nullptr && parentClient != client) {
            client->detachLayer(child);
        }
    });

    return true;
}

void Layer::setParent(const sp<Layer>& layer) {
    mCurrentParent = layer;
}

void Layer::clearSyncPoints() {
    for (const auto& child : mCurrentChildren) {
        child->clearSyncPoints();
    }

    Mutex::Autolock lock(mLocalSyncPointMutex);
    for (auto& point : mLocalSyncPoints) {
        point->setFrameAvailable();
    }
    mLocalSyncPoints.clear();
}

int32_t Layer::getZ() const {
    return mDrawingState.z;
}

__attribute__((no_sanitize("unsigned-integer-overflow")))
LayerVector Layer::makeTraversalList(LayerVector::StateSet stateSet) {
    LOG_ALWAYS_FATAL_IF(stateSet == LayerVector::StateSet::Invalid,
                        "makeTraversalList received invalid stateSet");
    const bool useDrawing = stateSet == LayerVector::StateSet::Drawing;
    const LayerVector& children = useDrawing ? mDrawingChildren : mCurrentChildren;
    const State& state = useDrawing ? mDrawingState : mCurrentState;

    if (state.zOrderRelatives.size() == 0) {
        return children;
    }
    LayerVector traverse;

    for (const wp<Layer>& weakRelative : state.zOrderRelatives) {
        sp<Layer> strongRelative = weakRelative.promote();
        if (strongRelative != nullptr) {
            traverse.add(strongRelative);
        }
    }

    for (const sp<Layer>& child : children) {
        traverse.add(child);
    }

    return traverse;
}

/**
 * Negatively signed relatives are before 'this' in Z-order.
 */
void Layer::traverseInZOrder(LayerVector::StateSet stateSet, const LayerVector::Visitor& visitor) {
    LayerVector list = makeTraversalList(stateSet);

    size_t i = 0;
    for (; i < list.size(); i++) {
        const auto& relative = list[i];
        if (relative->getZ() >= 0) {
            break;
        }
        relative->traverseInZOrder(stateSet, visitor);
    }
    visitor(this);
    for (; i < list.size(); i++) {
        const auto& relative = list[i];
        relative->traverseInZOrder(stateSet, visitor);
    }
}

/**
 * Positively signed relatives are before 'this' in reverse Z-order.
 */
void Layer::traverseInReverseZOrder(LayerVector::StateSet stateSet,
                                    const LayerVector::Visitor& visitor) {
    LayerVector list = makeTraversalList(stateSet);

    int32_t i = 0;
    for (i = list.size()-1; i>=0; i--) {
        const auto& relative = list[i];
        if (relative->getZ() < 0) {
            break;
        }
        relative->traverseInReverseZOrder(stateSet, visitor);
    }
    visitor(this);
    for (; i>=0; i--) {
        const auto& relative = list[i];
        relative->traverseInReverseZOrder(stateSet, visitor);
    }
}

/**
 * Traverse only children in z order, ignoring relative layers.
 */
void Layer::traverseChildrenInZOrder(LayerVector::StateSet stateSet,
                                     const LayerVector::Visitor& visitor) {
    const bool useDrawing = stateSet == LayerVector::StateSet::Drawing;
    const LayerVector& children = useDrawing ? mDrawingChildren : mCurrentChildren;

    size_t i = 0;
    for (; i < children.size(); i++) {
        const auto& relative = children[i];
        if (relative->getZ() >= 0) {
            break;
        }
        relative->traverseChildrenInZOrder(stateSet, visitor);
    }
    visitor(this);
    for (; i < children.size(); i++) {
        const auto& relative = children[i];
        relative->traverseChildrenInZOrder(stateSet, visitor);
    }
}

Transform Layer::getTransform() const {
    Transform t;
    const auto& p = mDrawingParent.promote();
    if (p != nullptr) {
        t = p->getTransform();

        // If the parent is not using NATIVE_WINDOW_SCALING_MODE_FREEZE (e.g.
        // it isFixedSize) then there may be additional scaling not accounted
        // for in the transform. We need to mirror this scaling in child surfaces
        // or we will break the contract where WM can treat child surfaces as
        // pixels in the parent surface.
        if (p->isFixedSize() && p->mActiveBuffer != nullptr) {
            int bufferWidth;
            int bufferHeight;
            if ((p->mCurrentTransform & NATIVE_WINDOW_TRANSFORM_ROT_90) == 0) {
                bufferWidth = p->mActiveBuffer->getWidth();
                bufferHeight = p->mActiveBuffer->getHeight();
            } else {
                bufferHeight = p->mActiveBuffer->getWidth();
                bufferWidth = p->mActiveBuffer->getHeight();
            }
            float sx = p->getDrawingState().active.w /
                    static_cast<float>(bufferWidth);
            float sy = p->getDrawingState().active.h /
                    static_cast<float>(bufferHeight);
            Transform extraParentScaling;
            extraParentScaling.set(sx, 0, 0, sy);
            t = t * extraParentScaling;
        }
    }
    return t * getDrawingState().active.transform;
}

half Layer::getAlpha() const {
    const auto& p = mDrawingParent.promote();

    half parentAlpha = (p != nullptr) ? p->getAlpha() : 1.0_hf;
    return parentAlpha * getDrawingState().color.a;
}

half4 Layer::getColor() const {
    const half4 color(getDrawingState().color);
    return half4(color.r, color.g, color.b, getAlpha());
}

void Layer::commitChildList() {
    for (size_t i = 0; i < mCurrentChildren.size(); i++) {
        const auto& child = mCurrentChildren[i];
        child->commitChildList();
    }
    mDrawingChildren = mCurrentChildren;
    mDrawingParent = mCurrentParent;
}

void Layer::writeToProto(LayerProto* layerInfo, LayerVector::StateSet stateSet) {
    const bool useDrawing = stateSet == LayerVector::StateSet::Drawing;
    const LayerVector& children = useDrawing ? mDrawingChildren : mCurrentChildren;
    const State& state = useDrawing ? mDrawingState : mCurrentState;

    Transform requestedTransform = state.active.transform;
    Transform transform = getTransform();

    layerInfo->set_id(sequence);
    layerInfo->set_name(getName().c_str());
    layerInfo->set_type(String8(getTypeId()));

    for (const auto& child : children) {
        layerInfo->add_children(child->sequence);
    }

    for (const wp<Layer>& weakRelative : state.zOrderRelatives) {
        sp<Layer> strongRelative = weakRelative.promote();
        if (strongRelative != nullptr) {
            layerInfo->add_relatives(strongRelative->sequence);
        }
    }

    LayerProtoHelper::writeToProto(state.activeTransparentRegion,
                                   layerInfo->mutable_transparent_region());
    LayerProtoHelper::writeToProto(visibleRegion, layerInfo->mutable_visible_region());
    LayerProtoHelper::writeToProto(surfaceDamageRegion, layerInfo->mutable_damage_region());

    layerInfo->set_layer_stack(getLayerStack());
    layerInfo->set_z(state.z);

    PositionProto* position = layerInfo->mutable_position();
    position->set_x(transform.tx());
    position->set_y(transform.ty());

    PositionProto* requestedPosition = layerInfo->mutable_requested_position();
    requestedPosition->set_x(requestedTransform.tx());
    requestedPosition->set_y(requestedTransform.ty());

    SizeProto* size = layerInfo->mutable_size();
    size->set_w(state.active.w);
    size->set_h(state.active.h);

    LayerProtoHelper::writeToProto(state.crop, layerInfo->mutable_crop());
    LayerProtoHelper::writeToProto(state.finalCrop, layerInfo->mutable_final_crop());

    layerInfo->set_is_opaque(isOpaque(state));
    layerInfo->set_invalidate(contentDirty);
    layerInfo->set_dataspace(dataspaceDetails(getDataSpace()));
    layerInfo->set_pixel_format(decodePixelFormat(getPixelFormat()));
    LayerProtoHelper::writeToProto(getColor(), layerInfo->mutable_color());
    LayerProtoHelper::writeToProto(state.color, layerInfo->mutable_requested_color());
    layerInfo->set_flags(state.flags);

    LayerProtoHelper::writeToProto(transform, layerInfo->mutable_transform());
    LayerProtoHelper::writeToProto(requestedTransform, layerInfo->mutable_requested_transform());

    auto parent = getParent();
    if (parent != nullptr) {
        layerInfo->set_parent(parent->sequence);
    }

    auto zOrderRelativeOf = state.zOrderRelativeOf.promote();
    if (zOrderRelativeOf != nullptr) {
        layerInfo->set_z_order_relative_of(zOrderRelativeOf->sequence);
    }

    auto activeBuffer = getActiveBuffer();
    if (activeBuffer != nullptr) {
        LayerProtoHelper::writeToProto(activeBuffer, layerInfo->mutable_active_buffer());
    }

    layerInfo->set_queued_frames(getQueuedFrameCount());
    layerInfo->set_refresh_pending(isBufferLatched());
}

// ---------------------------------------------------------------------------

}; // namespace android

#if defined(__gl_h_)
#error "don't include gl/gl.h in this file"
#endif

#if defined(__gl2_h_)
#error "don't include gl2/gl2.h in this file"
#endif
