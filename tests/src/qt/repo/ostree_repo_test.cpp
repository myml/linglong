/*
 * SPDX-FileCopyrightText: 2023 UnionTech Software Technology Co., Ltd.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "../util/mock-network.h"
#include "linglong/repo/ostree_repo.h"

#include <QTest>

using namespace linglong;
using namespace linglong::repo;
using namespace linglong::api::client;

class TestOSTreeRepo : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void push();
};

void TestOSTreeRepo::push()
{
    MockQNetworkAccessManager http;
    int requestNumber = 0;
    connect(&http,
            &MockQNetworkAccessManager::onCreateRequest,
            [&requestNumber](MockReply *rep, auto op, QNetworkRequest req) {
                requestNumber++;
                auto url = req.url().toString();
                qDebug() << "onCreateRequest"
                         << "op" << op << "url" << url;
                // 查询仓库信息
                if (url.endsWith("/api/v1/repos/repo")) {
                    GetRepo_200_response resp;
                    resp.setCode(200);
                    rep->JSON(200, resp.asJsonObject());
                    return;
                }
                auto taskID = QString("test_task_id");
                // 创建上传任务
                if (url.endsWith("/api/v1/upload-tasks")) {
                    NewUploadTaskID_200_response resp;
                    Response_NewUploadTaskResp data;
                    data.setId(taskID);
                    resp.setData(data);
                    resp.setCode(200);
                    rep->JSON(200, resp.asJsonObject());
                    return;
                }
                // 上传tar包
                if (url.endsWith(QString("/api/v1/upload-tasks/%1/tar").arg(taskID))) {
                    UploadTaskFile_200_response resp;
                    Response_UploadTaskResp data;
                    data.setWatchId(0);
                    resp.setData(data);
                    resp.setCode(200);
                    rep->JSON(200, resp.asJsonObject());
                    return;
                }
                // 查询任务状态
                if (url.endsWith(QString("/api/v1/upload-tasks/%1/status").arg(taskID))) {
                    UploadTaskInfo_200_response resp;
                    Response_UploadTaskStatusInfo data;
                    if (requestNumber < 10) {
                        data.setStatus("pending");
                    } else {
                        data.setStatus("complete");
                    }
                    resp.setData(data);
                    resp.setCode(200);
                    rep->JSON(200, resp.asJsonObject());
                    return;
                }
            });

    ClientApi api;
    api.setNewServerForAllOperations(QUrl("https://testmock.deepin.org"));
    api.setNetworkAccessManager(&http);

    // TODO(wurongjie) 不加 new 程序会挂掉
    auto repo =
      new OSTreeRepo("./testdata/ostree_repo_test", "https://testmock.deepin.org", "repo", api);

    auto ref = package::Ref("", "test", "1.0.0", "x86", "runtime");
    // auto importResult = repo->importDirectory(ref, "/tmp/ostree_repo_test/data");
    // Q_ASSERT_X(importResult.has_value(),
    //            "import error",
    //            importResult.error().message().toStdString().c_str());
    qDebug() << "start push" << ref.toOSTreeRefLocalString();
    auto pushResult = repo->push(ref);
    Q_ASSERT_X(pushResult.has_value(),
               "push error",
               pushResult.error().message().toStdString().c_str());
}

QTEST_GUILESS_MAIN(TestOSTreeRepo)
#include "ostree_repo_test.moc"
