/* This file is part of the KDE project
   SPDX-FileCopyrightText: 2000 Simon Hausmann <hausmann@kde.org>
   SPDX-FileCopyrightText: 2000 David Faure <faure@kde.org>

   SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "kxmlguibuilder.h"

#include "kxmlguiclient.h"
#include "ktoolbar.h"
#include "kmainwindow.h"
#include "kxmlguiwindow.h"

#include <klocalizedstring.h>

#include <QDomElement>
#include <QObject>
#include <QStringList>
#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>
#include <QDebug>

#include <kis_icon_utils.h>

#if defined(KCONFIG_BEFORE_5_24)
# define authorizeAction authorizeKAction
#endif

using namespace KDEPrivate;

class KisKXMLGUIBuilderPrivate
{
public:
    KisKXMLGUIBuilderPrivate() { }
    ~KisKXMLGUIBuilderPrivate() { }

    QWidget *m_widget {0};

    QString tagMainWindow;
    QString tagMenuBar;
    QString tagMenu;
    QString tagToolBar;
    QString tagStatusBar;

    QString tagSeparator;
    QString tagTearOffHandle;
    QString tagMenuTitle;

    QString attrName;
    QString attrLineSeparator;

    QString attrDomain;
    QString attrText1;
    QString attrText2;
    QString attrContext;

    QString attrIcon;

    KisKXMLGUIClient *m_client {0};
};

KisKXMLGUIBuilder::KisKXMLGUIBuilder(QWidget *widget)
    : d(new KisKXMLGUIBuilderPrivate)
{
    d->m_widget = widget;

    d->tagMainWindow = QStringLiteral("mainwindow");
    d->tagMenuBar = QStringLiteral("menubar");
    d->tagMenu = QStringLiteral("menu");
    d->tagToolBar = QStringLiteral("toolbar");
    d->tagStatusBar = QStringLiteral("statusbar");

    d->tagSeparator = QStringLiteral("separator");
    d->tagTearOffHandle = QStringLiteral("tearoffhandle");
    d->tagMenuTitle = QStringLiteral("title");

    d->attrName = QStringLiteral("name");
    d->attrLineSeparator = QStringLiteral("lineseparator");

    d->attrDomain = QStringLiteral("translationDomain");
    d->attrText1 = QStringLiteral("text");
    d->attrText2 = QStringLiteral("Text");
    d->attrContext = QStringLiteral("context");

    d->attrIcon = QStringLiteral("icon");
}

KisKXMLGUIBuilder::~KisKXMLGUIBuilder()
{
    delete d;
}

QWidget *KisKXMLGUIBuilder::widget()
{
    return d->m_widget;
}

QStringList KisKXMLGUIBuilder::containerTags() const
{
    QStringList res;
    res << d->tagMenu << d->tagToolBar << d->tagMainWindow << d->tagMenuBar << d->tagStatusBar;

    return res;
}

QWidget *KisKXMLGUIBuilder::createContainer(QWidget *parent, int index, const QDomElement &element, QAction *&containerAction)
{
    containerAction = 0;

    if (element.attribute(QStringLiteral("deleted")).toLower() == QLatin1String("true")) {
        return 0;
    }

    const QString tagName = element.tagName().toLower();
    if (tagName == d->tagMainWindow) {
        KisKMainWindow *mainwindow = qobject_cast<KisKMainWindow *>(d->m_widget);  // could be 0
        return mainwindow;
    }

    if (tagName == d->tagMenuBar) {
        KisKMainWindow *mainWin = qobject_cast<KisKMainWindow *>(d->m_widget);
        QMenuBar *bar = 0;
        if (mainWin) {
            bar = mainWin->menuBar();
        }
        if (!bar) {
            bar = new QMenuBar(d->m_widget);
        }
        bar->show();
        return bar;
    }

    if (tagName == d->tagMenu) {
        // Look up to see if we are inside a mainwindow. If yes, then
        // use it as parent widget (to get kaction to plug itself into the
        // mainwindow). Don't use a popupmenu as parent widget, otherwise
        // the popup won't be hidden if it is used as a standalone menu as well.
        //
        // Note: menus with a parent of 0, coming from child clients, can be
        // leaked if the child client is deleted without a proper removeClient call, though.

        QWidget *p = parent;

        if (!p && qobject_cast<QMainWindow *>(d->m_widget)) {
                    p = d->m_widget;
        }

        while (p && !qobject_cast<QMainWindow *>(p)) {
            p = p->parentWidget();
        }

        QString name = element.attribute(d->attrName);

        QMenu *popup = new QMenu(p);
        popup->setObjectName(name);

        QString i18nText;
        QDomElement textElem = element.namedItem(d->attrText1).toElement();
        if (textElem.isNull()) { // try with capital T
            textElem = element.namedItem(d->attrText2).toElement();
        }
        const QString text = textElem.text();
        const QString context = textElem.attribute(d->attrContext);

        //qDebug(260) << "DOMAIN" << KLocalizedString::applicationDomain();
        //qDebug(260) << "ELEMENT TEXT:" << text;

        if (text.isEmpty()) { // still no luck
            i18nText = i18n("No text");
        } else {
            QByteArray domain = textElem.attribute(d->attrDomain).toUtf8();
            if (domain.isEmpty()) {
                domain = element.ownerDocument().documentElement().attribute(d->attrDomain).toUtf8();
                if (domain.isEmpty()) {
                    domain = KLocalizedString::applicationDomain();
                }
            }
            if (context.isEmpty()) {
                i18nText = i18nd(domain.constData(), text.toUtf8().constData());
            } else {
                i18nText = i18ndc(domain.constData(), context.toUtf8().constData(), text.toUtf8().constData());
            }
        }

        //qDebug(260) << "ELEMENT i18n TEXT:" << i18nText;

        const QString icon = element.attribute(d->attrIcon);
        QIcon pix;
        if (!icon.isEmpty()) {
            pix = KisIconUtils::loadIcon(icon);
        }

        if (parent) {
            QAction *act = popup->menuAction();
            if (!icon.isEmpty()) {
                act->setIcon(pix);
            }
            act->setText(i18nText);
            if (index == -1 || index >= parent->actions().count()) {
                parent->addAction(act);
            } else {
                parent->insertAction(parent->actions().value(index), act);
            }
            containerAction = act;
            containerAction->setObjectName(name);
        }

        return popup;
    }

    if (tagName == d->tagToolBar) {
        QString name = element.attribute(d->attrName);

        KisToolBar *bar = static_cast<KisToolBar *>(d->m_widget->findChild<KisToolBar *>(name));
        if (!bar) {
            bar = new KisToolBar(name, d->m_widget, false);
        }

        if (qobject_cast<KisKMainWindow *>(d->m_widget)) {
            if (d->m_client && !d->m_client->xmlFile().isEmpty()) {
                bar->addXMLGUIClient(d->m_client);
            }
        }

        bar->loadState(element);

        return bar;
    }

    if (tagName == d->tagStatusBar) {
        KisKMainWindow *mainWin = qobject_cast<KisKMainWindow *>(d->m_widget);
        if (mainWin) {
            mainWin->statusBar()->show();
            return mainWin->statusBar();
        }
        QStatusBar *bar = new QStatusBar(d->m_widget);
        return bar;
    }

    return 0L;
}

void KisKXMLGUIBuilder::removeContainer(QWidget *container, QWidget *parent, QDomElement &element, QAction *containerAction)
{
    // Warning parent can be 0L

    if (qobject_cast<QMenu *>(container)) {
        if (parent) {
            parent->removeAction(containerAction);
        }

        delete container;
    } else if (qobject_cast<KisToolBar *>(container)) {
        KisToolBar *tb = static_cast<KisToolBar *>(container);

        tb->saveState(element);
        delete tb;
    } else if (qobject_cast<QMenuBar *>(container)) {
        QMenuBar *mb = static_cast<QMenuBar *>(container);
        mb->hide();
        // Don't delete menubar - it can be reused by createContainer.
        // If you decide that you do need to delete the menubar, make
        // sure that QMainWindow::d->mb does not point to a deleted
        // menubar object.
    } else if (qobject_cast<QStatusBar *>(container)) {
        if (qobject_cast<KisKMainWindow *>(d->m_widget)) {
            container->hide();
        } else {
            delete static_cast<QStatusBar *>(container);
        }
    } else {
        qWarning() << "Unhandled container to remove : " << container->metaObject()->className();
    }
}

QStringList KisKXMLGUIBuilder::customTags() const
{
    QStringList res;
    res << d->tagSeparator << d->tagTearOffHandle << d->tagMenuTitle;
    return res;
}

QAction *KisKXMLGUIBuilder::createCustomElement(QWidget *parent, int index, const QDomElement &element)
{
    QAction *before = 0L;
    if (index > 0 && index < parent->actions().count()) {
        before = parent->actions().at(index);
    }

    const QString tagName = element.tagName().toLower();
    if (tagName == d->tagSeparator) {
        if (QMenu *menu = qobject_cast<QMenu *>(parent)) {
            // QMenu already cares for leading/trailing/repeated separators
            // no need to check anything
            return menu->insertSeparator(before);
        } else if (QMenuBar *bar = qobject_cast<QMenuBar *>(parent)) {
            QAction *separatorAction = new QAction(bar);
            separatorAction->setSeparator(true);
            bar->insertAction(before, separatorAction);
            return separatorAction;
        } else if (KisToolBar *bar = qobject_cast<KisToolBar *>(parent)) {
            /* FIXME KAction port - any need to provide a replacement for lineSeparator/normal separator?
            bool isLineSep = true;

            QDomNamedNodeMap attributes = element.attributes();
            unsigned int i = 0;
            for (; i < attributes.length(); i++ )
            {
              QDomAttr attr = attributes.item( i ).toAttr();

              if ( attr.name().toLower() == d->attrLineSeparator &&
                   attr.value().toLower() == QStringLiteral("false") )
              {
                isLineSep = false;
                break;
              }
            }

            if ( isLineSep )
                return bar->insertSeparator( index ? bar->actions()[index - 1] : 0L );
            else*/

            return bar->insertSeparator(before);
        }
    } else if (tagName == d->tagTearOffHandle) {
        static_cast<QMenu *>(parent)->setTearOffEnabled(true);
    } else if (tagName == d->tagMenuTitle) {
        if (QMenu *m = qobject_cast<QMenu *>(parent)) {
            QString i18nText;
            const QString text = element.text();

            if (text.isEmpty()) {
                i18nText = i18n("No text");
            } else {
                QByteArray domain = element.attribute(d->attrDomain).toUtf8();
                if (domain.isEmpty()) {
                    domain = element.ownerDocument().documentElement().attribute(d->attrDomain).toUtf8();
                    if (domain.isEmpty()) {
                        domain = KLocalizedString::applicationDomain();
                    }
                }
                i18nText = i18nd(domain.constData(), qPrintable(text));
            }

            QString icon = element.attribute(d->attrIcon);
            QIcon pix;

            if (!icon.isEmpty()) {
                pix = KisIconUtils::loadIcon(icon);
            }

            if (!icon.isEmpty()) {
                return m->insertSection(before, pix, i18nText);
            } else {
                return m->insertSection(before, i18nText);
            }
        }
    }

    QAction *blank = new QAction(parent);
    blank->setVisible(false);
    parent->insertAction(before, blank);
    return blank;
}

void KisKXMLGUIBuilder::removeCustomElement(QWidget *parent, QAction *action)
{
    parent->removeAction(action);
}

KisKXMLGUIClient *KisKXMLGUIBuilder::builderClient() const
{
    return d->m_client;
}

void KisKXMLGUIBuilder::setBuilderClient(KisKXMLGUIClient *client)
{
    d->m_client = client;
}

void KisKXMLGUIBuilder::finalizeGUI(KisKXMLGUIClient *)
{
    KXmlGuiWindow *window = qobject_cast<KXmlGuiWindow *>(d->m_widget);
    if (!window) {
        return;
    }
#if 0
    KisToolBar *toolbar = 0;
    QListIterator<KisToolBar> it(((KisKMainWindow *)d->m_widget)->toolBarIterator());
    while ((toolbar = it.current())) {
        //qDebug(260) << "KisKXMLGUIBuilder::finalizeGUI toolbar=" << (void*)toolbar;
        ++it;
        toolbar->positionYourself();
    }
#else
    window->finalizeGUI(false);
#endif
}

void KisKXMLGUIBuilder::virtual_hook(int, void *)
{
    /*BASE::virtual_hook( id, data );*/
}

