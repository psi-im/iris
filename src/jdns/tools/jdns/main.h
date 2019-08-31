/*
 * Copyright (C) 2005  Justin Karneges
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef MAIN_H
#define MAIN_H

#include "qjdns.h"

#include <QObject>
#include <QStringList>

class App : public QObject
{
    Q_OBJECT
public:
    bool opt_debug = false;
    bool opt_ipv6 = false;
    bool opt_quit = false;
    int quit_time = 500;
    QString mode, type, name, ipaddr;
    QStringList nslist;
    QList<QJDns::Record> pubitems;
    QJDns jdns;
    int req_id = 0;

    App();
    ~App();
    
public slots:
    void start();
    
signals:
    void quit();

private slots:
    void jdns_resultsReady(int id, const QJDns::Response &results);
    void jdns_published(int id);
    void jdns_error(int id, QJDns::Error e);
    void jdns_shutdownFinished();
    void jdns_debugLinesReady();
    void doShutdown();
};    

#endif // MAIN_H
