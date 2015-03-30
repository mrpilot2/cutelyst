/*
 * Copyright (C) 2013-2015 Daniel Nicoletti <dantti12@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB. If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "dispatcher_p.h"

#include "common.h"
#include "context.h"
#include "controller.h"
#include "controller_p.h"
#include "action.h"
#include "request_p.h"
#include "dispatchtypepath.h"
#include "dispatchtypechained.h"

#include <QUrl>
#include <QMetaMethod>
#include <QStringBuilder>
#include <QDebug>

using namespace Cutelyst;

Dispatcher::Dispatcher(QObject *parent) :
    QObject(parent),
    d_ptr(new DispatcherPrivate(this))
{
    registerDispatchType(new DispatchTypePath(this));
    registerDispatchType(new DispatchTypeChained(this));
}

Dispatcher::~Dispatcher()
{
    delete d_ptr;
}

void Dispatcher::setupActions(const QList<Controller*> &controllers)
{
    Q_D(Dispatcher);

    ActionList registeredActions;
    Q_FOREACH (Controller *controller, controllers) {
        bool instanceUsed = false;
        Q_FOREACH (Action *action, controller->actions()) {
            bool registered = false;
            if (!d->actionHash.contains(action->reverse())) {
                if (!action->attributes().contains(QStringLiteral("Private"))) {
                    // Register the action with each dispatcher
                    Q_FOREACH (DispatchType *dispatch, d->dispatchers) {
                        if (dispatch->registerAction(action)) {
                            registered = true;
                        }
                    }
                } else {
                    // We register private actions
                    registered = true;
                }
            }

            // The Begin, Auto, End actions are not
            // registered by Dispatchers but we need them
            // as private actions anyway
            if (registered) {
                d->actionHash.insert(action->ns() % QLatin1Char('/') % action->name(), action);
                d->containerHash[action->ns()] << action;
                registeredActions.append(action);
                instanceUsed = true;
            } else if (action->name() != QLatin1String("_DISPATCH") &&
                       action->name() != QLatin1String("_BEGIN") &&
                       action->name() != QLatin1String("_AUTO") &&
                       action->name() != QLatin1String("_ACTION") &&
                       action->name() != QLatin1String("_END")) {
                qCDebug(CUTELYST_DISPATCHER) << "The action" << action->name() << "of"
                                             << action->controller()->objectName()
                                             << "controller was not registered in any dispatcher."
                                                " If you still want to access it internally (via actionFor())"
                                                " you may make it's method private.";
            } else if (d->showInternalActions) {
                qCCritical(CUTELYST_DISPATCHER) << "The action" << action->name() << "of"
                                                << action->controller()->objectName()
                                                << "controller was alread registered by the"
                                                << d->actionHash.value(action->reverse())->controller()->objectName()
                                                << "controller.";
                exit(1);
            }
        }

        if (instanceUsed) {
            d->constrollerHash.insert(controller->objectName(), controller);
        }
    }

    // Cache root actions, BEFORE the controllers set them
    d->rootActions = d->containerHash.value(QStringLiteral(""));

    Q_FOREACH (Controller *controller, controllers) {
        controller->d_ptr->setupFinished();
    }

    // Unregister any dispatcher that is not in use
    int i = 0;
    while (i < d->dispatchers.size()) {
        DispatchType *type = d->dispatchers.at(i);
        if (!type->inUse()) {
            d->dispatchers.removeAt(i);
            continue;
        }
        ++i;
    }

    d->printActions();
}

bool Dispatcher::dispatch(Context *ctx)
{
    Action *action = ctx->action();
    if (action) {
        return forward(ctx, QLatin1Char('/') % action->ns() % QLatin1String("/_DISPATCH"));
    } else {
        const QString &path = ctx->req()->path();
        if (path.isEmpty()) {
            ctx->error(tr("No default action defined"));
        } else {
            ctx->error(tr("Unknown resource \"%1\".").arg(path));
        };
    }
    return false;
}

bool Dispatcher::forward(Context *ctx, Code *component)
{
    Q_ASSERT(component);
    // If the component was an Action
    // the dispatch() would call ctx->execute
    return ctx->execute(component);
}

bool Dispatcher::forward(Context *ctx, const QString &opname)
{
    Q_D(const Dispatcher);

    Action *action = d->command2Action(ctx, opname);
    if (action) {
        return action->dispatch(ctx);
    }

    qCCritical(CUTELYST_DISPATCHER) << "Action not found" << action;
    return false;
}

void Dispatcher::prepareAction(Context *ctx)
{
    Q_D(Dispatcher);

    Request *request = ctx->request();
    const QString &path = request->path();
    QStringList pathParts = path.split(QLatin1Char('/'));
    QStringList args;

    // Root action
    pathParts.prepend(QStringLiteral(""));

    int pos = path.size();

    //  "foo/bar"
    //  "foo/" skip
    //  "foo"
    //  ""
    Q_FOREVER {
        // Check out the dispatch types to see if any
        // will handle the path at this level
        const QString &actionPath = path.mid(0, pos);
        Q_FOREACH (DispatchType *type, d->dispatchers) {
            if (type->match(ctx, actionPath, args) == DispatchType::ExactMatch) {
                goto out;
            }
        }

        // leave the loop if we are at the root "/"
        if (pos <= 0) {
            break;
        }

        pos = path.lastIndexOf(QLatin1Char('/'), --pos);
        if (pos == -1) {
            pos = 0;
        }

        // If not, move the last part path to args
        args.prepend(QUrl::fromPercentEncoding(pathParts.takeLast().toLatin1()));
    }

out:
    if (!request->match().isEmpty()) {
        qCDebug(CUTELYST_DISPATCHER) << "Path is" << request->match();
    }

    if (!request->args().isEmpty()) {
        qCDebug(CUTELYST_DISPATCHER) << "Arguments are" << request->args().join(QLatin1Char('/'));
    }
}

Action *Dispatcher::getAction(const QString &name, const QString &nameSpace) const
{
    Q_D(const Dispatcher);

    if (name.isEmpty()) {
        return 0;
    }

    const QString &ns = DispatcherPrivate::cleanNamespace(nameSpace);

    return d->actionHash.value(ns % QLatin1Char('/') % name);
}

Action *Dispatcher::getActionByPath(const QString &path) const
{
    Q_D(const Dispatcher);

    QString _path = path;
    if (_path.startsWith('/')) {
        _path.remove(0, 1);
    }
    return d->actionHash.value(_path);
}

ActionList Dispatcher::getActions(const QString &name, const QString &nameSpace) const
{
    Q_D(const Dispatcher);

    if (name.isEmpty()) {
        return ActionList();
    }

    const QString &ns = DispatcherPrivate::cleanNamespace(nameSpace);

    ActionList ret;
    const ActionList &containers = d->getContainers(ns);
    Q_FOREACH (Action *action, containers) {
        if (action->name() == name) {
            ret.prepend(action);
        }
    }
    return ret;
}

QHash<QString, Controller *> Dispatcher::controllers() const
{
    Q_D(const Dispatcher);
    return d->constrollerHash;
}

QString Dispatcher::uriForAction(Action *action, const QStringList &captures) const
{
    Q_D(const Dispatcher);
    Q_FOREACH (DispatchType *dispatch, d->dispatchers) {
        QString uri = dispatch->uriForAction(action, captures);
        if (!uri.isNull()) {
            return uri.isEmpty() ? QStringLiteral("/") : uri;
        }
    }
    return QString();
}

QList<DispatchType *> Dispatcher::dispatchers() const
{
    Q_D(const Dispatcher);
    return d->dispatchers;
}

void Dispatcher::registerDispatchType(DispatchType *dispatchType)
{
    Q_D(Dispatcher);
    d->dispatchers.append(dispatchType);
}

QString DispatcherPrivate::cleanNamespace(const QString &ns)
{
    QString ret = ns;
    bool lastWasSlash = true; // remove initial slash
    int nsSize = ns.size();
    for (int i = 0; i < nsSize; ++i) {
        // Mark if the last char was a slash
        // so that two or more consecutive slashes
        // could be converted to just one
        // "a///b" -> "a/b"
        if (ret.at(i) == QLatin1Char('/')) {
            if (lastWasSlash) {
                ret.remove(i, 1);
                --nsSize;
            } else {
                lastWasSlash = true;
            }
        } else {
            lastWasSlash = false;
        }
    }
    return ret;
}

DispatcherPrivate::DispatcherPrivate(Dispatcher *q) : q_ptr(q)
{
}

void DispatcherPrivate::printActions() const
{
    QList<QStringList> table;

    QList<QString> keys = actionHash.keys();
    qSort(keys.begin(), keys.end());
    Q_FOREACH (const QString &key, keys) {
        Action *action = actionHash.value(key);
        if (showInternalActions || !action->name().startsWith(QLatin1Char('_'))) {
            QString path = key;
            if (!path.startsWith(QLatin1Char('/'))) {
                path.prepend(QLatin1Char('/'));
            }

            QStringList row;
            row.append(path);
            row.append(action->className());
            row.append(action->name());
            table.append(row);
        }
    }

    qCDebug(CUTELYST_DISPATCHER) <<  DispatchType::buildTable(table, {
                                                                  QStringLiteral("Private"),
                                                                  QStringLiteral("Class"),
                                                                  QStringLiteral("Method")
                                                              },
                                                              QStringLiteral("Loaded Private actions:")).data();

    // List all public actions
    Q_FOREACH (DispatchType *dispatch, dispatchers) {
        qCDebug(CUTELYST_DISPATCHER) << dispatch->list().data();
    }
}

ActionList DispatcherPrivate::getContainers(const QString &ns) const
{
    ActionList ret;

    if (ns != QLatin1String("/")) {
        int pos = ns.size();
//        qDebug() << pos << ns.mid(0, pos);
        while (pos > 0) {
//            qDebug() << pos << ns.mid(0, pos);
            ret.append(containerHash.value(ns.mid(0, pos)));
            pos = ns.lastIndexOf('/', pos - 1);
        }
    }
//    qDebug() << containerHash.size() << rootActions;
    ret.append(rootActions);

    return ret;
}

Action *DispatcherPrivate::command2Action(Context *ctx, const QString &command, const QStringList &extraParams) const
{
    Action *ret = actionHash.value(command);
    if (!ret) {
        ret = invokeAsPath(ctx, command, ctx->request()->args());
    }

    return ret;
}

Action *DispatcherPrivate::invokeAsPath(Context *ctx, const QString &relativePath, const QStringList &args) const
{
    Q_Q(const Dispatcher);

    Action *ret;
    QString path = DispatcherPrivate::actionRel2Abs(ctx, relativePath);

    int pos = path.lastIndexOf('/');
    int lastPos = path.size();
    do {
        if (pos == -1) {
            ret = q->getAction(path, QString());
            if (ret) {
                return ret;
            }
        } else {
            QString name = path.mid(pos + 1, lastPos);
            path = path.mid(0, pos);
            ret = q->getAction(name, path);
            if (ret) {
                return ret;
            }
        }

        lastPos = pos;
        pos = path.indexOf('/', pos - 1);
    } while (pos != -1);

    return 0;
}

QString DispatcherPrivate::actionRel2Abs(Context *ctx, const QString &path)
{
    QString ret;
    if (!path.startsWith(QLatin1Char('/'))) {
        const QString &ns = qobject_cast<Action *>(ctx->stack().last())->ns();
        ret = ns % QLatin1Char('/') % path;
    } else {
        ret = path;
    }

    if (ret.startsWith(QLatin1Char('/'))) {
        ret.remove(0, 1);
    }
    return ret;
}
