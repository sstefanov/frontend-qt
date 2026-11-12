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
#include <QObject>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QSurfaceFormat>

#include <condition_variable>
#include <mutex>
#include <thread>

// Loads an upcoming preset into a second projectM instance on a background thread. Compiling its shaders there
// fills the GPU driver's shader cache, so the real switch does not stall rendering for the compile time.
class QPresetPreloader : public QObject
{
public:
    // Must be created in the GUI thread, which has to own the offscreen surface
    explicit QPresetPreloader(QObject * parent = nullptr) : QObject(parent)
    {
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

    // Only the most recent request is kept
    void preload(const QByteArray & presetFile)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pending = presetFile;
        }
        m_wake.notify_one();
    }

private:
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

        projectm_handle instance = projectm_create();
        if (instance) {
            const int size = 256;
            projectm_set_window_size(instance, size, size);
            QOpenGLFramebufferObject target(size, size);

            for (;;) {
                QByteArray presetFile;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_wake.wait(lock, [this] { return m_quit || !m_pending.isEmpty(); });
                    if (m_quit) {
                        break;
                    }
                    presetFile.swap(m_pending);
                }

                projectm_load_preset_file(instance, presetFile.constData(), false);
                // Drivers may finish compiling a shader only when it is first used
                projectm_opengl_render_frame_fbo(instance, target.handle());
            }

            projectm_destroy(instance);
        }

        context.doneCurrent();
    }

    QOffscreenSurface m_surface;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    QByteArray m_pending;
    bool m_quit = false;
};

#endif
