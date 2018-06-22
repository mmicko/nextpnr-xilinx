/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Miodrag Milanovic <miodrag@symbioticeda.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "worker.h"
#include <fstream>
#include "bitstream.h"
#include "design_utils.h"
#include "jsonparse.h"
#include "log.h"
#include "pack.h"
#include "pcf.h"
#include "place_sa.h"
#include "route.h"
#include "timing.h"

NEXTPNR_NAMESPACE_BEGIN

struct WorkerInterruptionRequested
{
};

Worker::Worker(Context *_ctx, TaskManager *parent) : ctx(_ctx)
{
    log_write_function = [this, parent](std::string text) {
        Q_EMIT log(text);
        if (parent->shouldTerminate()) {
            parent->clearTerminate();
            throw WorkerInterruptionRequested();
        }
        if (parent->isPaused())
        {
            Q_EMIT taskPaused();
        }
        while (parent->isPaused()) {
            if (parent->shouldTerminate()) {
                parent->clearTerminate();
                throw WorkerInterruptionRequested();
            }
            QThread::sleep(1);
        }
    };
}

void Worker::loadfile(const std::string &filename)
{
    Q_EMIT taskStarted();
    std::string fn = filename;
    std::ifstream f(fn);
    try {
        Q_EMIT loadfile_finished(parse_json_file(f, fn, ctx));
    } catch (WorkerInterruptionRequested) {
        Q_EMIT taskCanceled();
    }
}

void Worker::pack()
{
    Q_EMIT taskStarted();
    try {
        Q_EMIT pack_finished(pack_design(ctx));
    } catch (WorkerInterruptionRequested) {
        Q_EMIT taskCanceled();
    }
}

void Worker::place()
{
    Q_EMIT taskStarted();
    try {
        double freq = 50e6;
        assign_budget(ctx, freq);
        print_utilisation(ctx);
        Q_EMIT place_finished(place_design_sa(ctx));
    } catch (WorkerInterruptionRequested) {
        Q_EMIT taskCanceled();
    }
}

void Worker::route()
{
    Q_EMIT taskStarted();
    try {
        Q_EMIT route_finished(route_design(ctx));
    } catch (WorkerInterruptionRequested) {
        Q_EMIT taskCanceled();
    }
}


TaskManager::TaskManager(Context *ctx) : toTerminate(false), toPause(false)
{
    Worker *worker = new Worker(ctx, this);
    worker->moveToThread(&workerThread);
    
    connect(&workerThread, &QThread::finished, worker, &QObject::deleteLater);
    
    connect(this, &TaskManager::loadfile, worker, &Worker::loadfile);
    connect(this, &TaskManager::pack, worker, &Worker::pack);
    connect(this, &TaskManager::place, worker, &Worker::place);
    connect(this, &TaskManager::route, worker, &Worker::route);

    connect(worker, &Worker::log, this, &TaskManager::info);
    connect(worker, &Worker::loadfile_finished, this, &TaskManager::loadfile_finished);
    connect(worker, &Worker::pack_finished, this, &TaskManager::pack_finished);
    connect(worker, &Worker::place_finished, this, &TaskManager::place_finished);
    connect(worker, &Worker::route_finished, this, &TaskManager::route_finished);
    
    connect(worker, &Worker::taskCanceled, this, &TaskManager::taskCanceled);
    connect(worker, &Worker::taskStarted, this, &TaskManager::taskStarted);
    connect(worker, &Worker::taskPaused, this, &TaskManager::taskPaused);

    workerThread.start();
}

TaskManager::~TaskManager()
{
    if (workerThread.isRunning())
        terminate_thread();
    workerThread.quit();
    workerThread.wait();
}

void TaskManager::info(const std::string &result) { Q_EMIT log(result); }

void TaskManager::terminate_thread()
{
    QMutexLocker locker(&mutex);
    toPause = false;
    toTerminate = true;
}

bool TaskManager::shouldTerminate()
{
    QMutexLocker locker(&mutex);
    return toTerminate;
}

void TaskManager::clearTerminate()
{
    QMutexLocker locker(&mutex);
    toTerminate = false;
}

void TaskManager::pause_thread()
{
    QMutexLocker locker(&mutex);
    toPause = true;
}

void TaskManager::continue_thread()
{
    QMutexLocker locker(&mutex);
    toPause = false;
    Q_EMIT taskStarted();
}

bool TaskManager::isPaused()
{
    QMutexLocker locker(&mutex);
    return toPause;
}

NEXTPNR_NAMESPACE_END