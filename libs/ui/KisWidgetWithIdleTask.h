/*
 *  SPDX-FileCopyrightText: 2023 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISWIDGETWITHIDLETASK_H
#define KISWIDGETWITHIDLETASK_H

#include <QWidget>
#include "KisIdleTasksManager.h"

class KisCanvas2;
class KoColorSpace;

template <typename BaseWidget>
class KisWidgetWithIdleTask : public BaseWidget
{
public:
    KisWidgetWithIdleTask(QWidget *parent = 0, Qt::WindowFlags flags = Qt::WindowFlags())
        : BaseWidget (parent, flags)
    {
    }

    ~KisWidgetWithIdleTask() override = default;

    virtual void setCanvas(KisCanvas2 *canvas) {
        if (m_canvas) {
            m_idleTaskGuard = KisIdleTasksManager::TaskGuard();
        }

        m_canvas = canvas;

        if (m_canvas) {
            if (this->isVisible()) {
                m_idleTaskGuard = registerIdleTask(m_canvas);
            }
        }

        clearCachedState();
        this->update();
    }

    void showEvent(QShowEvent *event) override {
        BaseWidget::showEvent(event);

        KIS_SAFE_ASSERT_RECOVER(!m_idleTaskGuard.isValid()) {
            m_idleTaskGuard = KisIdleTasksManager::TaskGuard();
        }

        if (m_canvas) {
            m_idleTaskGuard = registerIdleTask(m_canvas);
        }
        if (m_idleTaskGuard.isValid()) {
            m_idleTaskGuard.trigger();
        }
    }

    void hideEvent(QHideEvent *event) override {
        /// Qt's show/hide events may arrive unbalanced, our
        /// assert should take that into account
        const bool wasVisible = this->isVisible();

        BaseWidget::hideEvent(event);

        KIS_SAFE_ASSERT_RECOVER_NOOP(!m_canvas || wasVisible == m_idleTaskGuard.isValid());
        m_idleTaskGuard = KisIdleTasksManager::TaskGuard();

        clearCachedState();
    }

    void triggerCacheUpdate() {
        if (m_idleTaskGuard.isValid()) {
            m_idleTaskGuard.trigger();
        }
    }

    [[nodiscard]]
    virtual KisIdleTasksManager::TaskGuard registerIdleTask(KisCanvas2 *canvas) = 0;
    virtual void clearCachedState() = 0;

protected:
    KisCanvas2 *m_canvas {0};
    KisIdleTasksManager::TaskGuard m_idleTaskGuard;
};

#endif // KISWIDGETWITHIDLETASK_H
