/* This file is part of the KDE project
   SPDX-FileCopyrightText: 2022 Emmet O'Neill <emmetoneill.pdx@gmail.com>
   SPDX-FileCopyrightText: 2022 Eoin O'Neill <eoinoneill1991@gmail.com>

   SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "KisPlaybackEngineQT.h"

#include "kis_debug.h"
#include "kis_canvas2.h"
#include "KisCanvasAnimationState.h"
#include "kis_image.h"
#include "kis_image_animation_interface.h"

#include <QTimer>
#include "animation/KisFrameDisplayProxy.h"
#include "KisRollingMeanAccumulatorWrapper.h"
#include "KisRollingSumAccumulatorWrapper.h"

#include "KisPlaybackEngineQT.h"

#include <QFileInfo>

// =====

/** @brief Base class for different types of playback.
 * 
 * When `KisPlaybackEngineQT` supported audio (through QtMultimedia) it was
 * useful to have separate playback methods for non-audio and audio situations.
 */
class PlaybackDriver : public QObject {
    Q_OBJECT
public:
    PlaybackDriver( class KisPlaybackEngineQT* engine, QObject* parent = nullptr );
    ~PlaybackDriver();

    virtual void setPlaybackState(PlaybackState state) = 0;

    virtual void setFrame(int) {}

    /** @brief Optionally return which frame the playback driver thinks we should render.
     * 
     * This is mostly useful when the driver itself dictates the frame to be shown.
     * However, in other cases (for example, when `drop frames` is off and we must wait)
     * we don't rely on this method.
    */
    virtual boost::optional<int> desiredFrame() { return boost::none; }

    virtual void setVolume(qreal) {}

    virtual void setSpeed(qreal) {}
    virtual double speed() = 0;

    virtual void setFramerate(int rate) {}

    virtual void setDropFrames(bool) {}
    virtual bool dropFrames() { return true; }

    virtual void setTimerDuration(int) {}
    virtual int timerDuration() { return 1000 / 24; }

    KisPlaybackEngineQT* engine() { return m_engine; }

Q_SIGNALS:
    void throttledShowFrame();

private:
    KisPlaybackEngineQT* m_engine;
};

PlaybackDriver::PlaybackDriver( KisPlaybackEngineQT* engine, QObject* parent )
    : QObject(parent)
    , m_engine(engine)
{
    KIS_ASSERT(engine);
}

PlaybackDriver::~PlaybackDriver()
{
}

// =====

/** @brief A simple QTimer-based playback method for situations when audio is not 
 * used (and thus audio-video playback synchronization is not a concern).
 */
class LoopDrivenPlayback : public PlaybackDriver {
    Q_OBJECT
public:
    LoopDrivenPlayback(KisPlaybackEngineQT* engine, QObject* parent = nullptr);
    ~LoopDrivenPlayback();

    virtual void setPlaybackState( PlaybackState newState ) override;

    virtual void setFramerate(int rate) override;
    virtual void setSpeed(qreal speed) override;
    virtual double speed() override;
    virtual void setDropFrames(bool drop) override;
    virtual bool dropFrames() override;
    virtual void setTimerDuration(int timeMS) override;
    virtual int timerDuration() override;

private:
    void updatePlaybackLoopInterval(const int& in_fps, const qreal& in_speed);

private:
    QTimer m_playbackLoop;
    double m_speed;
    int m_fps;
    bool m_dropFrames;
};

LoopDrivenPlayback::LoopDrivenPlayback(KisPlaybackEngineQT *engine, QObject *parent)
    : PlaybackDriver(engine, parent)
    , m_speed(1.0)
    , m_fps(24)
    , m_dropFrames(true)
{
    connect( &m_playbackLoop, SIGNAL(timeout()), this, SIGNAL(throttledShowFrame()) );
}

LoopDrivenPlayback::~LoopDrivenPlayback()
{
}

void LoopDrivenPlayback::setPlaybackState(PlaybackState newState) {
    switch (newState) {
    case PlaybackState::PLAYING:
        m_playbackLoop.start();
        break;
    case PlaybackState::PAUSED:
    case PlaybackState::STOPPED:
    default:
        m_playbackLoop.stop();
        break;
    }
}

void LoopDrivenPlayback::setFramerate(int rate) {
    KIS_SAFE_ASSERT_RECOVER_RETURN(rate > 0);
    m_fps = rate;
    updatePlaybackLoopInterval(m_fps, m_speed);
}

void LoopDrivenPlayback::setSpeed(qreal speed) {
    KIS_SAFE_ASSERT_RECOVER_RETURN(speed > 0.f);
    m_speed = speed;
    updatePlaybackLoopInterval(m_fps, m_speed);
}

double LoopDrivenPlayback::speed()
{
    return m_speed;
}

void LoopDrivenPlayback::setDropFrames(bool drop) {
    m_dropFrames = drop;
}

bool LoopDrivenPlayback::dropFrames() {
    return m_dropFrames;
}

void LoopDrivenPlayback::setTimerDuration(int timeMS)
{
    KIS_ASSERT(timeMS > 0);
    m_playbackLoop.setInterval(timeMS);
}

int LoopDrivenPlayback::timerDuration()
{
    return m_playbackLoop.interval();
}

void LoopDrivenPlayback::updatePlaybackLoopInterval(const int &in_fps, const qreal &in_speed) {
    int loopMS = qRound( 1000.f / (qreal(in_fps) * in_speed));
    m_playbackLoop.setInterval(loopMS);
}

// ======

/** @brief Struct used to keep track of all frame time variance
 * and acommodate for skipped frames. Also tracks whether a frame
 * is still being loaded by the display proxy.
 *
 * Only allocated when playback begins.
 */
struct FrameMeasure {
    static constexpr int frameStatsWindow = 50;

    FrameMeasure()
        : averageTimePerFrame(frameStatsWindow)
        , waitingForFrame(false)
        , droppedFramesStat(frameStatsWindow)

    {
        timeSinceLastFrame.start();
    }

    QElapsedTimer timeSinceLastFrame;
    KisRollingMeanAccumulatorWrapper averageTimePerFrame;
    bool waitingForFrame;

    KisRollingSumAccumulatorWrapper droppedFramesStat;
};

// ====== KisPlaybackEngineQT ======

struct KisPlaybackEngineQT::Private {
public:
    Private(KisPlaybackEngineQT* p_self)
        : driver(nullptr)
    {
    }

    ~Private() {
        driver.reset();
    }

    QScopedPointer<PlaybackDriver> driver;
    QScopedPointer<FrameMeasure> measure;

private:
    KisPlaybackEngineQT* self;
};

KisPlaybackEngineQT::KisPlaybackEngineQT(QObject *parent)
    : KisPlaybackEngine(parent)
    , m_d(new Private(this))
{
}

KisPlaybackEngineQT::~KisPlaybackEngineQT()
{
}

void KisPlaybackEngineQT::seek(int frameIndex, SeekOptionFlags flags)
{
    if (!activeCanvas())
        return;

    KIS_SAFE_ASSERT_RECOVER_RETURN(activeCanvas()->animationState());
    KisFrameDisplayProxy* displayProxy = activeCanvas()->animationState()->displayProxy();
    KIS_SAFE_ASSERT_RECOVER_RETURN(displayProxy);

    KIS_SAFE_ASSERT_RECOVER_RETURN(frameIndex >= 0);

    m_d->driver->setFrame(frameIndex);
    if (displayProxy->activeFrame() != frameIndex) {
        displayProxy->displayFrame(frameIndex, flags & SEEK_FINALIZE);
    }
}

void KisPlaybackEngineQT::setDropFramesMode(bool value)
{
    KisPlaybackEngine::setDropFramesMode(value);
    KIS_SAFE_ASSERT_RECOVER_RETURN(m_d->driver);
    m_d->driver->setDropFrames(value);
}

boost::optional<int64_t> KisPlaybackEngineQT::activeFramesPerSecond() const
{
    if (activeCanvas()) {
        return activeCanvas()->image()->animationInterface()->framerate();
    } else {
        return boost::none;
    }
}

KisPlaybackEngine::PlaybackStats KisPlaybackEngineQT::playbackStatistics() const
{
    KisPlaybackEngine::PlaybackStats stats;

    if (m_d->measure && activeCanvas()->animationState()->playbackState() == PLAYING) {
        const int droppedFrames = m_d->measure->droppedFramesStat.rollingSum();
        const int totalFrames =
            m_d->measure->droppedFramesStat.rollingCount() +
            droppedFrames;

        stats.droppedFramesPortion = qreal(droppedFrames) / totalFrames;
        stats.expectedFps = qreal(activeFramesPerSecond().get_value_or(24)) * m_d->driver->speed();

        const qreal avgTimePerFrame = m_d->measure->averageTimePerFrame.rollingMeanSafe();
        stats.realFps = !qFuzzyIsNull(avgTimePerFrame) ? 1000.0 / avgTimePerFrame : 0.0;
    }

    return stats;
}

void KisPlaybackEngineQT::throttledDriverCallback()
{
    if (!m_d->driver)
        return;

    KIS_SAFE_ASSERT_RECOVER_RETURN(activeCanvas()->animationState());
    KisFrameDisplayProxy* displayProxy = activeCanvas()->animationState()->displayProxy();
    KIS_SAFE_ASSERT_RECOVER_RETURN(displayProxy);

    KIS_SAFE_ASSERT_RECOVER_RETURN(activeCanvas()->image());
    KisImageAnimationInterface *animInterface = activeCanvas()->image()->animationInterface();
    KIS_SAFE_ASSERT_RECOVER_RETURN(animInterface);

    KIS_ASSERT(m_d->measure);

    // If we're waiting for each frame, then we delay our callback.
    if (m_d->measure && m_d->measure->waitingForFrame) {
        // Without drop frames on, we need to factor out time that we're waiting
        // for a frame from our time
        return;
    }

    const int currentFrame = displayProxy->activeFrame();
    const int startFrame = animInterface->activePlaybackRange().start();
    const int endFrame = animInterface->activePlaybackRange().end();

    const int timeSinceLastFrame =  m_d->measure->timeSinceLastFrame.restart();
    const int timePerFrame = qRound(1000.0 / qreal(activeFramesPerSecond().get_value_or(24)) / m_d->driver->speed());
    m_d->measure->averageTimePerFrame(timeSinceLastFrame);


    // Drop frames logic...
    int extraFrames = 0;
    if (m_d->driver->dropFrames()) {
        KIS_ASSERT(m_d->measure);
        const int offset = timeSinceLastFrame - timePerFrame;
        extraFrames = qMax(0, offset) / timePerFrame;
    }

    m_d->measure->droppedFramesStat(extraFrames);

    // If we have an audio-driver or otherwise external playback driver: we will only go to what the driver determines to be the desired frame...
    if (m_d->driver->desiredFrame()) {
        const int desiredFrame = m_d->driver->desiredFrame().get();
        const int targetFrame = frameWrap(desiredFrame, startFrame, endFrame );

        if (currentFrame != targetFrame) {
            displayProxy->displayFrame(targetFrame, false);
        }

        // We've wrapped, let's do whatever correction we can...
        if (targetFrame != desiredFrame) {
            m_d->driver->setFrame(targetFrame);
        }
    } else { // Otherwise, we just advance the frame ourselves based on the displayProxy's active frame.
        int targetFrame = currentFrame + 1 + extraFrames;

        targetFrame = frameWrap(targetFrame, startFrame, endFrame);

        if (currentFrame != targetFrame) {
            // We only wait when drop frames is enabled.
            m_d->measure->waitingForFrame = !m_d->driver->dropFrames();

            bool neededRefresh = displayProxy->displayFrame(targetFrame, false);

            // If we didn't need to refresh, we just continue as usual.
            m_d->measure->waitingForFrame = m_d->measure->waitingForFrame && neededRefresh;
        }
    }
}

void KisPlaybackEngineQT::setCanvas(KoCanvasBase *p_canvas)
{
    KisCanvas2* canvas = dynamic_cast<KisCanvas2*>(p_canvas);

    struct StopAndResume {
        StopAndResume(KisPlaybackEngineQT* p_self)
            : m_self(p_self) {
            if (m_self->m_d->driver) {
                m_self->m_d->driver->setPlaybackState(PlaybackState::STOPPED);
            }
        }

        ~StopAndResume() {
            if (m_self->activeCanvas() &&  m_self->m_d->driver) {
                m_self->m_d->driver->setPlaybackState(m_self->activeCanvas()->animationState()->playbackState());
            }
        }

    private:
        KisPlaybackEngineQT* m_self;
    };

    if (activeCanvas() == canvas) {
        return;
    }

    if (activeCanvas()) {
        KisCanvasAnimationState* animationState = activeCanvas()->animationState();

        KIS_SAFE_ASSERT_RECOVER_RETURN(m_d->driver);

        // Disconnect internal..
        m_d->driver.data()->disconnect(this);

        { // Disconnect old Image Anim Interface, prepare for new one..
            auto image = activeCanvas()->image();
            KisImageAnimationInterface* aniInterface = image ? image->animationInterface() : nullptr;
            if (aniInterface) {
                this->disconnect(image->animationInterface());
                image->animationInterface()->disconnect(this);
            }
        }

        { // Disconnect old display proxy, prepare for new one.
            KisFrameDisplayProxy* displayProxy = animationState->displayProxy();

            if (displayProxy) {
                displayProxy->disconnect(this);
            }
        }

        { // Disconnect old animation state, prepare for new one..
            if (animationState) {
                this->disconnect(animationState);
                animationState->disconnect(this);
            }
        }
    }

    StopAndResume stopResume(this);

    KisPlaybackEngine::setCanvas(canvas);

    if (activeCanvas()) {
        KisCanvasAnimationState* animationState = activeCanvas()->animationState();
        KIS_ASSERT(animationState);

        recreateDriver(animationState->mediaInfo());

        KIS_SAFE_ASSERT_RECOVER_RETURN(m_d->driver);

        { // Animation State Connections
            connect(animationState, &KisCanvasAnimationState::sigPlaybackMediaChanged, this, [this]() {
                KisCanvasAnimationState* animationState2 = activeCanvas()->animationState();
                if (animationState2) {
                    recreateDriver(animationState2->mediaInfo());
                }
            });

            connect(animationState, &KisCanvasAnimationState::sigPlaybackStateChanged, this, [this](PlaybackState state){
                if (!m_d->driver)
                    return;

                if (state == PLAYING) {
                    m_d->measure.reset(new FrameMeasure);
                } else {
                    m_d->measure.reset();
                }

                m_d->driver->setPlaybackState(state);
            });

            connect(animationState, &KisCanvasAnimationState::sigPlaybackSpeedChanged, this, [this](qreal value){
                if (!m_d->driver)
                    return;

                m_d->driver->setSpeed(value);
            });
            m_d->driver->setSpeed(animationState->playbackSpeed());
        }

        { // Display proxy connections
            KisFrameDisplayProxy* displayProxy = animationState->displayProxy();
            KIS_ASSERT(displayProxy);
            connect(displayProxy, &KisFrameDisplayProxy::sigFrameDisplayRefreshed, this, [this](){
                if (m_d->measure && m_d->measure->waitingForFrame) {
                    m_d->measure->waitingForFrame = false;
                }
            });

            connect(displayProxy, &KisFrameDisplayProxy::sigFrameRefreshSkipped, this, [this](){
                if (m_d->measure && m_d->measure->waitingForFrame) {
                    m_d->measure->waitingForFrame = false;
                }
            });
        }


        {   // Animation Interface Connections
            auto image = activeCanvas()->image();
            KIS_ASSERT(image);
            KisImageAnimationInterface* aniInterface = image->animationInterface();
            KIS_ASSERT(aniInterface);

            connect(aniInterface, &KisImageAnimationInterface::sigFramerateChanged, this, [this](){
                if (!activeCanvas())
                    return;

                KisImageWSP img = activeCanvas()->image();
                KIS_SAFE_ASSERT_RECOVER_RETURN(img);
                KisImageAnimationInterface* aniInterface = img->animationInterface();
                KIS_SAFE_ASSERT_RECOVER_RETURN(aniInterface);

                m_d->driver->setFramerate(aniInterface->framerate());
            });

            m_d->driver->setFramerate(aniInterface->framerate());
        }

        // Internal connections
        connect(m_d->driver.data(), SIGNAL(throttledShowFrame()), this, SLOT(throttledDriverCallback()));

    }
    else {
        recreateDriver(boost::none);
    }
}

void KisPlaybackEngineQT::unsetCanvas()
{
    setCanvas(nullptr);
}

void KisPlaybackEngineQT::recreateDriver(boost::optional<QFileInfo> file)
{
    m_d->driver.reset();

    if (!activeCanvas())
        return;

    m_d->driver.reset(new LoopDrivenPlayback(this));
}


#include "KisPlaybackEngineQT.moc"
