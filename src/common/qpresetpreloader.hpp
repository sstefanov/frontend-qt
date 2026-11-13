/**
 * projectM-qt -- Qt4 based projectM GUI
 * Copyright (C)2003-2004 projectM Team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 * See 'LICENSE.txt' included within this release
 *
 */

#ifndef QPRESETPRELOADER_HPP
#define QPRESETPRELOADER_HPP

#include <projectM-4/projectM.h>
#include <projectM-4/render_opengl.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QObject>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QStandardPaths>
#include <QSurfaceFormat>

#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

// Loads upcoming presets into a second projectM instance on a background thread. Compiling their shaders there
// fills the GPU driver's shader cache, so the real switch does not stall rendering for the compile time.
// While a preset is loaded here, it is also played against a test signal to measure how much its picture moves.
class QPresetPreloader : public QObject
{
public:
    struct Prepared {
        QByteArray file;
        float motion; // mean picture change per frame, as a fraction of full scale
    };

    // Must be created in the GUI thread, which has to own the offscreen surface
    explicit QPresetPreloader(QObject * parent = nullptr) : QObject(parent)
    {
        m_scoreFile = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/projectM/preset-motion.tsv";
        m_surface.setFormat(QSurfaceFormat::defaultFormat());
        m_surface.create();
        m_thread = std::thread(&QPresetPreloader::run, this);
        // The thread must be gone before the GL driver is torn down at process exit
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { stop(); });
    }

    ~QPresetPreloader() override
    {
        stop();
    }

    // Replaces any earlier request; the presets are prepared in the given order
    void prepare(const QList<QByteArray> & presetFiles)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending = presetFiles;
            m_ready.clear();
            ++m_request;
        }
        m_wake.notify_one();
    }

    // The presets of the current request that are loaded and measured so far
    QList<Prepared> ready()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_ready;
    }

private:
    static constexpr int SIZE = 128;
    static constexpr int FPS = 30;
    static constexpr int SAMPLES_PER_FRAME = 44100 / FPS;
    static constexpr int SETTLE_FRAMES = FPS;      // let the preset fade in before measuring
    static constexpr int MEASURE_FRAMES = FPS * 3 / 2;
    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_quit = true;
        }
        m_wake.notify_one();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    void run()
    {
        QOpenGLContext context;
        context.setFormat(m_surface.format());
        if (!context.create() || !context.makeCurrent(&m_surface)) {
            return;
        }

        loadScores();

        projectm_handle instance = projectm_create();
        if (instance) {
            projectm_set_window_size(instance, SIZE, SIZE);
            projectm_set_preset_locked(instance, true);
            projectm_set_preset_switch_failed_event_callback(instance, &QPresetPreloader::loadFailed, this);
            QOpenGLFramebufferObject target(SIZE, SIZE);

            for (;;) {
                QByteArray presetFile;
                quint64 request;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_wake.wait(lock, [this] { return m_quit || !m_pending.isEmpty(); });
                    if (m_quit) {
                        break;
                    }
                    presetFile = m_pending.takeFirst();
                    request = m_request;
                }

                m_loadFailed = false;
                projectm_load_preset_file(instance, presetFile.constData(), false);
                if (m_loadFailed) {
                    continue;
                }

                float motion;
                const auto known = m_scores.constFind(presetFile);
                if (known != m_scores.constEnd()) {
                    motion = known.value();
                    // Drivers may finish compiling a shader only when it is first used
                    renderFrame(instance, target);
                } else {
                    motion = measureMotion(instance, target);
                    m_scores.insert(presetFile, motion);
                    saveScore(presetFile, motion);
                }

                std::lock_guard<std::mutex> lock(m_mutex);
                if (request == m_request) {
                    m_ready.append({presetFile, motion});
                }
            }

            projectm_destroy(instance);
        }

        context.doneCurrent();
    }

    static void loadFailed(const char *, const char *, void * userData)
    {
        static_cast<QPresetPreloader *>(userData)->m_loadFailed = true;
    }

    // A fixed 120 BPM kick, hi-hat and tone, so that scores are comparable between presets and sessions
    void renderFrame(projectm_handle instance, QOpenGLFramebufferObject & target)
    {
        float audio[2 * SAMPLES_PER_FRAME];
        for (int i = 0; i < SAMPLES_PER_FRAME; ++i) {
            const double t = double(m_sample++) / 44100.0;
            const double beat = t * 2.0 - std::floor(t * 2.0);
            const double tick = t * 8.0 - std::floor(t * 8.0);
            m_noise = m_noise * 1664525u + 1013904223u;
            const double noise = double(m_noise >> 8) / 8388608.0 - 1.0;
            audio[2 * i] = audio[2 * i + 1] = float(0.7 * std::exp(-8.0 * beat) * std::sin(6.283185307 * 55.0 * t)
                                                    + 0.15 * std::exp(-30.0 * tick) * noise
                                                    + 0.1 * std::sin(6.283185307 * 440.0 * t));
        }
        projectm_pcm_add_float(instance, audio, SAMPLES_PER_FRAME, PROJECTM_STEREO);
        // Simulated time: the frames are rendered as fast as possible, not in real time
        projectm_set_frame_time(instance, double(m_frame++) / FPS);
        projectm_opengl_render_frame_fbo(instance, target.handle());
    }

    float measureMotion(projectm_handle instance, QOpenGLFramebufferObject & target)
    {
        QOpenGLFunctions * gl = QOpenGLContext::currentContext()->functions();
        std::vector<unsigned char> current(SIZE * SIZE * 4), previous(SIZE * SIZE * 4);
        double change = 0.0;

        for (int frame = 0; frame < SETTLE_FRAMES + MEASURE_FRAMES; ++frame) {
            renderFrame(instance, target);
            gl->glBindFramebuffer(GL_FRAMEBUFFER, target.handle());
            gl->glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, current.data());
            if (frame >= SETTLE_FRAMES) {
                long difference = 0;
                for (size_t i = 0; i < current.size(); i += 4) {
                    difference += std::abs(current[i] - previous[i]) + std::abs(current[i + 1] - previous[i + 1])
                                  + std::abs(current[i + 2] - previous[i + 2]);
                }
                change += double(difference) / (SIZE * SIZE * 3 * 255.0);
            }
            current.swap(previous);
        }

        return float(change / MEASURE_FRAMES);
    }

    void loadScores()
    {
        QFile file(m_scoreFile);
        if (!file.open(QIODevice::ReadOnly)) {
            return;
        }
        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            const int tab = line.indexOf('\t');
            if (tab > 0) {
                m_scores.insert(line.mid(tab + 1), line.left(tab).toFloat());
            }
        }
    }

    void saveScore(const QByteArray & presetFile, float motion)
    {
        QDir().mkpath(QFileInfo(m_scoreFile).path());
        QFile file(m_scoreFile);
        if (file.open(QIODevice::Append)) {
            file.write(QByteArray::number(motion, 'g', 6) + '\t' + presetFile + '\n');
        }
    }

    QOffscreenSurface m_surface;
    QString m_scoreFile;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    QList<QByteArray> m_pending;
    QList<Prepared> m_ready;
    quint64 m_request = 0;
    bool m_quit = false;

    // Used by the background thread only
    QHash<QByteArray, float> m_scores;
    bool m_loadFailed = false;
    quint64 m_sample = 0;
    quint64 m_frame = 0;
    quint32 m_noise = 12345;
};

#endif
