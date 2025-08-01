/*
 * SPDX-FileCopyrightText: 2022 UnionTech Software Technology Co., Ltd.
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "package_manager.h"

#include "configure.h"
#include "linglong/api/types/helper.h"
#include "linglong/api/types/v1/Generators.hpp"
#include "linglong/api/types/v1/PackageInfoV2.hpp"
#include "linglong/api/types/v1/PackageManager1JobInfo.hpp"
#include "linglong/api/types/v1/PackageManager1PruneResult.hpp"
#include "linglong/api/types/v1/Repo.hpp"
#include "linglong/api/types/v1/State.hpp"
#include "linglong/extension/extension.h"
#include "linglong/package/layer_file.h"
#include "linglong/package/layer_packager.h"
#include "linglong/package/reference.h"
#include "linglong/package/uab_file.h"
#include "linglong/package_manager/package_task.h"
#include "linglong/repo/config.h"
#include "linglong/repo/ostree_repo.h"
#include "linglong/runtime/run_context.h"
#include "linglong/utils/bash_quote.h"
#include "linglong/utils/command/env.h"
#include "linglong/utils/error/error.h"
#include "linglong/utils/finally/finally.h"
#include "linglong/utils/hooks.h"
#include "linglong/utils/packageinfo_handler.h"
#include "linglong/utils/serialize/json.h"
#include "linglong/utils/transaction.h"
#include "ocppi/runtime/RunOption.hpp"

#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDebug>
#include <QEventLoop>
#include <QJsonArray>
#include <QMetaObject>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cstdint>
#include <utility>

#include <fcntl.h>

namespace linglong::service {

namespace {

constexpr auto repoLockPath = "/run/linglong/lock";

template <typename T>
QVariantMap toDBusReply(const utils::error::Result<T> &x, std::string type = "display") noexcept
{
    Q_ASSERT(!x.has_value());

    return utils::serialize::toQVariantMap(
      api::types::v1::CommonResult{ .code = x.error().code(),                     // NOLINT
                                    .message = x.error().message().toStdString(), // NOLINT
                                    .type = std::move(type) });
}

QVariantMap toDBusReply(int code, const QString &message, std::string type = "display") noexcept
{
    return utils::serialize::toQVariantMap(
      api::types::v1::CommonResult{ .code = code,                     // NOLINT
                                    .message = message.toStdString(), // NOLINT
                                    .type = std::move(type) });
}

QVariantMap toDBusReply(utils::error::ErrorCode code,
                        const QString &message,
                        std::string type = "display") noexcept
{
    return utils::serialize::toQVariantMap(
      api::types::v1::CommonResult{ .code = static_cast<int>(code),   // NOLINT
                                    .message = message.toStdString(), // NOLINT
                                    .type = std::move(type) });
}

bool isTaskDone(linglong::api::types::v1::SubState subState) noexcept
{
    return subState == linglong::api::types::v1::SubState::AllDone
      || subState == linglong::api::types::v1::SubState::PackageManagerDone;
}

utils::error::Result<package::FuzzyReference>
fuzzyReferenceFromPackage(const api::types::v1::PackageManager1Package &pkg) noexcept
{
    std::optional<QString> channel;
    if (pkg.channel) {
        channel = QString::fromStdString(*pkg.channel);
    }

    std::optional<QString> version;
    if (pkg.version) {
        version = QString::fromStdString(*pkg.version);
    }

    auto fuzzyRef = package::FuzzyReference::create(channel,
                                                    QString::fromStdString(pkg.id),
                                                    version,
                                                    std::nullopt);
    return fuzzyRef;
}
} // namespace

PackageManager::PackageManager(linglong::repo::OSTreeRepo &repo,
                               linglong::runtime::ContainerBuilder &containerBuilder,
                               QObject *parent)
    : QObject(parent)
    , repo(repo)
    , tasks(this)
    , containerBuilder(containerBuilder)
{
    using namespace std::chrono_literals;
    auto deferredTimeOut = 3600s;
    auto *deferredTimeOutEnv = ::getenv("LINGLONG_DEFERRED_TIMEOUT");
    if (deferredTimeOutEnv != nullptr) {
        try {
            deferredTimeOut = std::stoi(deferredTimeOutEnv) * 1s;
        } catch (std::invalid_argument &e) {
            qWarning() << "failed to parse LINGLONG_DEFERRED_TIMEOUT[" << deferredTimeOutEnv
                       << "]:" << e.what();
        } catch (std::out_of_range &e) {
            qWarning() << "failed to parse LINGLONG_DEFERRED_TIMEOUT[" << deferredTimeOutEnv
                       << "]:" << e.what();
        }
    }

    qInfo().nospace() << "deferredTimeOut:" << deferredTimeOut.count() << "s";

    auto *timer = new QTimer(this);
    timer->setInterval(deferredTimeOut);
    connect(timer, &QTimer::timeout, [this, timer] {
        this->deferredUninstall();
        timer->start();
    });

    timer->start();
}

PackageManager::~PackageManager()
{
    auto ret = unlockRepo();
    if (!ret) {
        qCritical() << "failed to unlock repo:" << ret.error().message();
    }
}

utils::error::Result<bool> PackageManager::isRefBusy(const package::Reference &ref) noexcept
{
    LINGLONG_TRACE(QString{ "check if ref[%1] is used by some apps" }.arg(ref.toString()));

    auto ret = lockRepo();
    if (!ret) {
        return LINGLONG_ERR(
          QStringLiteral("failed to lock repo, underlying data will not be removed:")
          % ret.error().message());
    }

    auto unlock = utils::finally::finally([this] {
        auto ret = unlockRepo();
        if (!ret) {
            qCritical() << "failed to unlock repo:" << ret.error().message();
        }
    });

    auto running = getAllRunningContainers();
    if (!running) {
        return LINGLONG_ERR(QStringLiteral("failed to get running containers:")
                            % running.error().message());
    }
    auto &runningRef = *running;

    return std::find_if(runningRef.cbegin(),
                        runningRef.cend(),
                        [&ref](const api::types::v1::ContainerProcessStateInfo &info) {
                            return info.app == ref.toString().toStdString();
                        })
      != runningRef.cend();
}

utils::error::Result<std::vector<api::types::v1::ContainerProcessStateInfo>>
PackageManager::getAllRunningContainers() noexcept
{
    LINGLONG_TRACE("get all running containers");

    std::error_code ec;
    auto user_iterator = std::filesystem::directory_iterator{ "/run/linglong", ec };
    if (ec) {
        return LINGLONG_ERR(QStringLiteral("failed to list /run/linglong: ")
                            % ec.message().c_str());
    }

    std::vector<api::types::v1::ContainerProcessStateInfo> result;
    for (const auto &entry : user_iterator) {
        if (!entry.is_directory()) {
            continue;
        }

        auto process_iterator = std::filesystem::directory_iterator{ entry.path(), ec };
        if (ec) {
            return LINGLONG_ERR(QStringLiteral("failed to list ") % entry.path().c_str() % ": "
                                % ec.message().c_str());
        }

        for (const auto &process_entry : process_iterator) {
            if (!process_entry.is_regular_file()) {
                continue;
            }

            auto pid = process_entry.path().filename().string();
            if (auto procDir = "/proc/" + pid; !std::filesystem::exists(procDir, ec)) {
                if (ec) {
                    return LINGLONG_ERR(QStringLiteral("failed to get state of ") % procDir.c_str()
                                        % ": " % ec.message().c_str());
                }

                qInfo() << "ignore" << process_entry.path().c_str()
                        << ",because corrsponding process is not found.";
                continue;
            }

            auto content =
              utils::serialize::LoadJSONFile<api::types::v1::ContainerProcessStateInfo>(
                QString::fromStdString(process_entry.path().string()));
            if (!content) {
                return LINGLONG_ERR(QStringLiteral("failed to load info from ")
                                    % process_entry.path().c_str() % ": "
                                    % content.error().message());
            }

            result.emplace_back(std::move(content).value());
        }
    }

    return result;
}

[[nodiscard]] utils::error::Result<void> PackageManager::lockRepo() noexcept
{
    LINGLONG_TRACE("lock whole repo")
    lockFd = ::open(repoLockPath, O_RDWR | O_CREAT, 0644);
    if (lockFd == -1) {
        return LINGLONG_ERR(QStringLiteral("failed to create lock file ") % repoLockPath % ": "
                            % ::strerror(errno));
    }

    struct flock locker{ .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };

    if (::fcntl(lockFd, F_SETLK, &locker) == -1) {
        return LINGLONG_ERR(QStringLiteral("failed to lock ") % repoLockPath % ": "
                            % ::strerror(errno));
    }

    return LINGLONG_OK;
}

[[nodiscard]] utils::error::Result<void> PackageManager::unlockRepo() noexcept
{
    LINGLONG_TRACE("unlock whole repo")

    if (lockFd == -1) {
        return LINGLONG_OK;
    }

    struct flock unlocker{ .l_type = F_UNLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 0 };

    if (::fcntl(lockFd, F_SETLK, &unlocker)) {
        return LINGLONG_ERR(QStringLiteral("failed to unlock ") % repoLockPath % ": "
                            % ::strerror(errno));
    }

    ::close(lockFd);
    lockFd = -1;

    return LINGLONG_OK;
}

utils::error::Result<void>
PackageManager::removeAfterInstall(const package::Reference &oldRef,
                                   const package::Reference &newRef,
                                   const std::vector<std::string> &modules) noexcept
{
    LINGLONG_TRACE("remove old reference after install")

    auto needDelayRet = isRefBusy(oldRef);
    if (!needDelayRet) {
        return LINGLONG_ERR(needDelayRet);
    }

    utils::Transaction transaction;
    if (*needDelayRet) {
        for (const auto &module : modules) {
            auto ret = this->repo.markDeleted(oldRef, true, module);
            if (!ret) {
                return LINGLONG_ERR("Failed to mark old reference " % oldRef.toString() % "/"
                                      % module.c_str() % " as deleted",
                                    ret);
            }

            transaction.addRollBack([this, &oldRef, module]() noexcept {
                auto ret = this->repo.markDeleted(oldRef, false, module);
                if (!ret) {
                    qWarning() << "Failed to rollback marking old reference " << oldRef.toString()
                               << ":" << ret.error();
                }
            });
        }

        transaction.commit();
        return LINGLONG_OK;
    }

    this->repo.unexportReference(oldRef);
    transaction.addRollBack([this, &oldRef]() noexcept {
        this->repo.exportReference(oldRef);
    });

    for (const auto &module : modules) {
        if (module == "binary" || module == "runtime") {
            auto ret = this->removeCache(oldRef);
            if (!ret) {
                qCritical() << ret.error().message();
            }
        }

        auto ret = this->repo.remove(oldRef, module);
        if (!ret) {
            return LINGLONG_ERR("Failed to remove old reference " % oldRef.toString(), ret);
        }

        transaction.addRollBack([this, &oldRef, module]() noexcept {
            auto tmp = PackageTask::createTemporaryTask();
            this->repo.pull(tmp, oldRef, module);
            if (tmp.state() != linglong::api::types::v1::State::Succeed) {
                qWarning() << "failed to rollback remove old reference" << oldRef.toString() << ":"
                           << tmp.message();
            }
            if (module == "binary" || module == "runtime") {
                auto ret = this->tryGenerateCache(oldRef);
                if (!ret) {
                    qCritical() << ret.error().message();
                }
            }
        });
    }

    auto mergeRet = this->repo.mergeModules();
    if (!mergeRet.has_value()) {
        qCritical() << "merge modules failed: " << mergeRet.error().message();
    }

    this->repo.exportReference(newRef);

    transaction.commit();
    return LINGLONG_OK;
}

void PackageManager::deferredUninstall() noexcept
{
    if (auto ret = lockRepo(); !ret) {
        qCritical() << "failed to lock repo:" << ret.error().message();
        return;
    }
    auto unlock = utils::finally::finally([this] {
        auto ret = unlockRepo();
        if (!ret) {
            qCritical() << "failed to unlock repo:" << ret.error().message();
        }
    });

    // query layers which have been mark 'deleted'
    auto uninstalled = this->repo.listLocalBy(linglong::repo::repoCacheQuery{ .deleted = true });
    if (!uninstalled) {
        qCritical() << "failed to list deleted layers" << uninstalled.error().message();
        return;
    }

    std::unordered_map<std::string, std::vector<api::types::v1::RepositoryCacheLayersItem>>
      uninstalledLayers;
    for (const auto &item : *uninstalled) {
        auto ref = package::Reference::fromPackageInfo(item.info);
        if (!ref) {
            qCritical() << "underlying storage was broken, exit.";
            Q_ASSERT(false);
            return;
        }

        auto [node, isNew] =
          uninstalledLayers.try_emplace(ref->toString().toStdString(),
                                        std::vector<api::types::v1::RepositoryCacheLayersItem>{});
        node->second.push_back(item);
    }

    if (uninstalledLayers.empty()) {
        return;
    }

    // retrieve running info
    auto running = getAllRunningContainers();
    if (!running) {
        qCritical() << "failed to get all running containers:" << running.error().message();
        return;
    }

    for (const auto &container : *running) {
        if (auto it = uninstalledLayers.find(container.app); it != uninstalledLayers.end()) {
            uninstalledLayers.erase(it);
        }
    }

    if (uninstalledLayers.empty()) {
        return;
    }

    // begin to uninstall
    for (const auto &[ref, items] : uninstalledLayers) {
        auto pkgRef = package::Reference::parse(QString::fromStdString(ref));
        if (!pkgRef) {
            qCritical() << "internal error:" << pkgRef.error().message();
            Q_ASSERT(false);
            return;
        }

        this->repo.unexportReference(*pkgRef);
        for (const auto &item : items) {
            if (item.info.packageInfoV2Module == "binary"
                || item.info.packageInfoV2Module == "runtime") {
                auto removeCacheRet = this->removeCache(*pkgRef);
                if (!removeCacheRet) {
                    qCritical() << "remove cache failed: " << removeCacheRet.error().message();
                }
            }
            auto ret = this->repo.remove(*pkgRef, item.info.packageInfoV2Module, item.info.uuid);
            if (!ret) {
                qCritical() << ret.error();
                continue;
            }
        }

        auto mergeRet = this->repo.mergeModules();
        if (!mergeRet) {
            qCritical() << "merge modules failed: " << mergeRet.error().message();
        }

        auto fuzzy =
          package::FuzzyReference::create(pkgRef->channel, pkgRef->id, std::nullopt, pkgRef->arch);
        if (!fuzzy) {
            qCritical() << "internal error:" << fuzzy.error().message();
            Q_ASSERT(false);
            return;
        }

        auto latestRef = this->repo.clearReference(
          *fuzzy,
          linglong::repo::clearReferenceOption{ .fallbackToRemote = false });
        if (!latestRef) {
            qCritical() << "failed to get latest layer item:" << latestRef.error().message();
        }

        this->repo.exportReference(*latestRef);
    }
}

auto PackageManager::getConfiguration() const noexcept -> QVariantMap
{
    return utils::serialize::toQVariantMap(this->repo.getConfig());
}

void PackageManager::setConfiguration(const QVariantMap &parameters) noexcept
{
    auto cfg = utils::serialize::fromQVariantMap<api::types::v1::RepoConfigV2>(parameters);
    if (!cfg) {
        sendErrorReply(QDBusError::InvalidArgs, cfg.error().message());
        return;
    }

    const auto &cfgRef = *cfg;
    const auto &curCfg = repo.getConfig();

    if (cfgRef == curCfg) {
        return;
    }

    if (const auto &defaultRepo = cfg->defaultRepo;
        std::find_if(cfg->repos.begin(),
                     cfg->repos.end(),
                     [&defaultRepo](const auto &repo) {
                         return repo.alias.value_or(repo.name) == defaultRepo;
                     })
        == cfg->repos.end()) {
        sendErrorReply(QDBusError::Failed,
                       "default repository is missing after updating configuration.");
        return;
    }

    auto result = this->repo.setConfig(*cfg);
    if (!result) {
        sendErrorReply(QDBusError::Failed, result.error().message());
    }
}

QVariantMap PackageManager::installFromLayer(const QDBusUnixFileDescriptor &fd,
                                             const api::types::v1::CommonOptions &options) noexcept
{
    auto layerFileRet = package::LayerFile::New(fd.fileDescriptor());
    if (!layerFileRet) {
        return toDBusReply(layerFileRet);
    }
    Q_ASSERT(*layerFileRet != nullptr);

    const auto &layerFile = *layerFileRet;
    auto realFile = layerFile->symLinkTarget();
    auto metaInfoRet = layerFile->metaInfo();
    if (!metaInfoRet) {
        return toDBusReply(metaInfoRet);
    }

    const auto &metaInfo = *metaInfoRet;
    auto packageInfoRet = utils::parsePackageInfo(metaInfo.info);
    if (!packageInfoRet) {
        return toDBusReply(packageInfoRet);
    }

    const auto &packageInfo = *packageInfoRet;

    // FIXME: need to support install develop
    if (packageInfo.packageInfoV2Module != "binary"
        && packageInfo.packageInfoV2Module != "runtime") {
        return toDBusReply(-1,
                           "The current version does not support the develop module installation.");
    }

    auto architectureRet = package::Architecture::parse(packageInfo.arch[0]);
    if (!architectureRet) {
        return toDBusReply(architectureRet);
    }

    auto currentArch = package::Architecture::currentCPUArchitecture();
    if (!currentArch) {
        return toDBusReply(currentArch);
    }

    if (*architectureRet != *currentArch) {
        return toDBusReply(-1,
                           "app arch:" + architectureRet->toString()
                             + " not match host architecture");
    }

    auto versionRet = package::Version::parse(QString::fromStdString(packageInfo.version));
    if (!versionRet) {
        return toDBusReply(versionRet);
    }

    auto packageRefRet = package::Reference::fromPackageInfo(packageInfo);
    if (!packageRefRet) {
        return toDBusReply(packageRefRet);
    }
    const auto &packageRef = *packageRefRet;
    api::types::v1::PackageManager1RequestInteractionAdditionalMessage additionalMessage;
    api::types::v1::InteractionMessageType msgType =
      api::types::v1::InteractionMessageType::Install;

    additionalMessage.remoteRef = packageRef.toString().toStdString();
    // TODO: when install extra module, we should check the same version of main(binary/runtime)
    // module has been installed or not

    // Note: same as InstallRef, we should fuzzy the id instead of version
    auto fuzzyRef = package::FuzzyReference::parse(packageRef.id);
    if (!fuzzyRef) {
        return toDBusReply(fuzzyRef);
    }

    auto localRef = this->repo.clearReference(*fuzzyRef,
                                              {
                                                .fallbackToRemote = false // NOLINT
                                              });
    if (localRef) {
        auto layerDir = this->repo.getLayerDir(*localRef, packageInfo.packageInfoV2Module);
        if (layerDir && layerDir->valid()) {
            additionalMessage.localRef = localRef->toString().toStdString();
        }
    }

    if (!additionalMessage.localRef.empty()) {
        if (packageRef.version == localRef->version) {
            return toDBusReply(-1, localRef->toString() + " is already installed");
        }

        if (packageRef.version > localRef->version) {
            msgType = api::types::v1::InteractionMessageType::Upgrade;
        } else if (!options.force) {
            auto layerName = QString("%1_%2_%3_%4.layer")
                               .arg(packageRef.id,
                                    packageRef.version.toString(),
                                    architectureRet->toString(),
                                    packageInfo.packageInfoV2Module.c_str());
            auto err = QString("The latest version has been installed. If you want to "
                               "replace it, try using 'll-cli install %1 --force'")
                         .arg(layerName);
            return toDBusReply(-1, err);
        }
    }

    auto installer =
      [this,
       fdDup = fd, // keep file descriptor don't close by the destructor of QDBusUnixFileDescriptor
       packageRef,
       layerFile = *layerFileRet,
       module = packageInfo.packageInfoV2Module,
       options,
       msgType,
       additionalMessage,
       localRef = localRef ? std::make_optional(*localRef) : std::nullopt](PackageTask &taskRef) {
          if (msgType == api::types::v1::InteractionMessageType::Upgrade
              && !options.skipInteraction) {
              Q_EMIT RequestInteraction(QDBusObjectPath(taskRef.taskObjectPath()),
                                        static_cast<int>(msgType),
                                        utils::serialize::toQVariantMap(additionalMessage));
              QEventLoop loop;
              auto conn = connect(
                this,
                &PackageManager::ReplyReceived,
                [&taskRef, &loop](const QVariantMap &reply) {
                    // handle reply
                    auto interactionReply =
                      utils::serialize::fromQVariantMap<api::types::v1::InteractionReply>(reply);
                    if (interactionReply->action != "yes") {
                        taskRef.updateState(linglong::api::types::v1::State::Canceled, "canceled");
                    }

                    loop.exit(0);
                });
              loop.exec();

              disconnect(conn);
          }
          if (isTaskDone(taskRef.subState())) {
              return;
          }

          taskRef.updateState(linglong::api::types::v1::State::Processing, "installing layer");
          taskRef.updateSubState(linglong::api::types::v1::SubState::PreAction,
                                 "preparing environment");

          package::LayerPackager layerPackager;
          auto layerDir = layerPackager.unpack(*layerFile);
          if (!layerDir) {
              taskRef.reportError(std::move(layerDir).error());
              return;
          }

          auto unmountLayer = utils::finally::finally([mountPoint = layerDir->absolutePath()] {
              if (QFileInfo::exists(mountPoint)) {
                  auto ret = utils::command::Exec("fusermount", { "-z", "-u", mountPoint });
                  if (!ret) {
                      qCritical() << "failed to umount " << mountPoint
                                  << ", please umount it manually";
                  }
              }
          });

          auto info = (*layerDir).info();
          if (!info) {
              taskRef.reportError(std::move(info).error());
              return;
          }

          pullDependency(taskRef, *info, module);
          if (isTaskDone(taskRef.subState())) {
              return;
          }

          auto result = this->repo.importLayerDir(*layerDir);
          if (!result) {
              taskRef.reportError(std::move(result).error());
              return;
          }

          // develop module only need to import
          if (module != "binary" && module != "runtime") {
              taskRef.updateState(linglong::api::types::v1::State::Succeed,
                                  "install layer successfully");
              return;
          }

          taskRef.updateState(linglong::api::types::v1::State::Succeed,
                              "install layer successfully");

          if (info->kind != "app") {
              return;
          }

          if (!localRef) {
              auto newRef = package::Reference::fromPackageInfo(*info);
              if (!newRef) {
                  taskRef.reportError(std::move(newRef).error());
                  return;
              }

              auto generateCacheRet = this->tryGenerateCache(*newRef);
              if (!generateCacheRet) {
                  taskRef.reportError(std::move(generateCacheRet).error());
                  return;
              }

              this->repo.exportReference(*newRef);
              return;
          }

          auto modules = this->repo.getModuleList(*localRef);
          if (std::find(modules.cbegin(), modules.cend(), module) == modules.cend()) {
              return;
          }

          auto newRef = package::Reference::fromPackageInfo(*info);
          if (!newRef) {
              taskRef.reportError(std::move(newRef).error());
              return;
          }

          auto generateCacheRet = this->tryGenerateCache(*newRef);
          if (!generateCacheRet) {
              taskRef.reportError(std::move(generateCacheRet).error());
              return;
          }

          auto ret = removeAfterInstall(*localRef, *newRef, std::vector{ module });
          if (!ret) {
              qCritical() << "failed to remove old reference" << localRef->toString()
                          << "after install" << packageRef.toString() << ":"
                          << ret.error().message();
          }

          ret = executePostInstallHooks(*newRef);
          if (!ret) {
              taskRef.reportError(std::move(ret).error());
              return;
          }
      };

    auto refSpec =
      QString{ "%1:%2/%3/%4/%5" }.arg("local",
                                      packageRef.channel,
                                      packageRef.id,
                                      packageRef.arch.toString(),
                                      QString::fromStdString(packageInfo.packageInfoV2Module));
    auto taskRet = tasks.addNewTask({ refSpec }, std::move(installer), connection());
    if (!taskRet) {
        return toDBusReply(taskRet);
    }

    auto &taskRef = taskRet->get();
    Q_EMIT TaskAdded(QDBusObjectPath{ taskRef.taskObjectPath() });
    taskRef.updateState(linglong::api::types::v1::State::Queued, "queued to install from layer");
    return utils::serialize::toQVariantMap(api::types::v1::PackageManager1PackageTaskResult{
      .taskObjectPath = taskRef.taskObjectPath().toStdString(),
      .code = 0,
      .message = (realFile + " is now installing").toStdString(),
    });
}

QVariantMap PackageManager::installFromUAB(const QDBusUnixFileDescriptor &fd,
                                           const api::types::v1::CommonOptions &options) noexcept
{
    if (!fd.isValid()) {
        return toDBusReply(-1, "invalid file descriptor");
    }

    std::unique_ptr<utils::InstallHookManager> installHookManager =
      std::make_unique<utils::InstallHookManager>();
    auto ret = installHookManager->parseInstallHooks();
    if (!ret) {
        return toDBusReply(ret);
    }

    ret = installHookManager->executeInstallHooks(fd.fileDescriptor());
    if (!ret) {
        return toDBusReply(-1, "uab package signature verification failed.");
    }

    auto uabRet = package::UABFile::loadFromFile(fd.fileDescriptor());
    if (!uabRet) {
        return toDBusReply(uabRet);
    }

    const auto &uab = *uabRet;
    auto verifyRet = uab->verify();
    if (!verifyRet) {
        return toDBusReply(verifyRet);
    }
    if (!*verifyRet) {
        return toDBusReply(-1, "couldn't pass uab verification");
    }

    auto realFile = uab->symLinkTarget();

    auto metaInfoRet = uab->getMetaInfo();
    if (!metaInfoRet) {
        return toDBusReply(metaInfoRet);
    }

    const auto &metaInfo = *metaInfoRet;
    auto layerInfos = metaInfo.get().layers;
    auto appLayerIt = std::find_if(layerInfos.cbegin(),
                                   layerInfos.cend(),
                                   [](const api::types::v1::UabLayer &layer) {
                                       return layer.info.kind == "app";
                                   });
    if (appLayerIt == layerInfos.cend()) {
        return toDBusReply(-1, "couldn't find application layer in this uab");
    }

    auto appLayer = *appLayerIt;
    layerInfos.erase(appLayerIt);
    auto app =
      layerInfos.insert(layerInfos.begin(),
                        std::move(appLayer)); // app layer should place to the first of vector

    auto architectureRet = package::Architecture::parse(app->info.arch[0]);
    if (!architectureRet) {
        return toDBusReply(architectureRet);
    }

    auto currentArch = package::Architecture::currentCPUArchitecture();
    if (!currentArch) {
        return toDBusReply(currentArch);
    }

    if (*architectureRet != *currentArch) {
        return toDBusReply(-1,
                           "app arch:" + architectureRet->toString()
                             + " not match host architecture");
    }

    auto versionRet = package::Version::parse(QString::fromStdString(app->info.version));
    if (!versionRet) {
        return toDBusReply(versionRet);
    }

    auto appRefRet = package::Reference::fromPackageInfo(app->info);
    if (!appRefRet) {
        return toDBusReply(appRefRet);
    }

    const auto &appRef = *appRefRet;
    api::types::v1::PackageManager1RequestInteractionAdditionalMessage additionalMessage;
    api::types::v1::InteractionMessageType msgType =
      api::types::v1::InteractionMessageType::Install;
    additionalMessage.remoteRef = appRef.toString().toStdString();

    // Note: same as InstallRef, we should fuzzy the id instead of version
    auto fuzzyRef = package::FuzzyReference::parse(appRef.id);
    if (!fuzzyRef) {
        return toDBusReply(fuzzyRef);
    }

    auto localAppRef = this->repo.clearReference(*fuzzyRef,
                                                 {
                                                   .fallbackToRemote = false // NOLINT
                                                 });
    if (localAppRef) {
        auto layerDir = this->repo.getLayerDir(*localAppRef, app->info.packageInfoV2Module);
        if (layerDir && layerDir->valid()) {
            additionalMessage.localRef = localAppRef->toString().toStdString();
        }
    }

    if (!additionalMessage.localRef.empty()) {
        if (appRef.version == localAppRef->version) {
            return toDBusReply(-1, localAppRef->toString() + " is already installed");
        }

        if (appRef.version > localAppRef->version) {
            msgType = api::types::v1::InteractionMessageType::Upgrade;
        } else if (!options.force) {
            auto uabName = QString{ "%1_%2_%3_%4.uab" }.arg(appRef.id,
                                                            architectureRet->toString(),
                                                            appRef.version.toString(),
                                                            app->info.packageInfoV2Module.c_str());
            auto err = QString("The latest version has been installed. If you want to "
                               "replace it, try using 'll-cli install %1 --force'")
                         .arg(uabName);
            return toDBusReply(-1, err);
        }
    }

    auto installer = [this,
                      fdDup = fd, // keep file descriptor don't close by the destructor of
                                  // QDBusUnixFileDescriptor
                      uab = std::move(uabRet).value(),
                      layerInfos = std::move(layerInfos),
                      metaInfo = std::move(metaInfoRet).value(),
                      options,
                      msgType,
                      additionalMessage,
                      newAppRef = std::move(appRefRet).value(),
                      oldAppRef = localAppRef.has_value()
                        ? std::make_optional(std::move(localAppRef).value())
                        : std::nullopt](PackageTask &taskRef) {
        if (msgType == api::types::v1::InteractionMessageType::Upgrade
            && !options.skipInteraction) {
            Q_EMIT RequestInteraction(QDBusObjectPath(taskRef.taskObjectPath()),
                                      static_cast<int>(msgType),
                                      utils::serialize::toQVariantMap(additionalMessage));
            QEventLoop loop;
            auto conn = connect(
              this,
              &PackageManager::ReplyReceived,
              [&taskRef, &loop](const QVariantMap &reply) {
                  // handle reply
                  auto interactionReply =
                    utils::serialize::fromQVariantMap<api::types::v1::InteractionReply>(reply);
                  if (interactionReply->action != "yes") {
                      taskRef.updateState(linglong::api::types::v1::State::Canceled, "canceled");
                  }

                  loop.exit(0);
              });
            loop.exec();
            disconnect(conn);
        }
        if (isTaskDone(taskRef.subState())) {
            return;
        }

        taskRef.updateState(linglong::api::types::v1::State::Processing, "installing uab");
        taskRef.updateSubState(linglong::api::types::v1::SubState::PreAction,
                               "prepare environment");

        auto mountPoint = uab->mountUab();
        if (!mountPoint) {
            taskRef.reportError(std::move(mountPoint).error());
            return;
        }

        if (isTaskDone(taskRef.subState())) {
            return;
        }

        auto uabLayersDir = *mountPoint / "layers";
        const auto &uabLayersDirInfo = QFileInfo{ uabLayersDir.c_str() };
        if (!uabLayersDirInfo.exists() || !uabLayersDirInfo.isDir()) {
            taskRef.updateState(linglong::api::types::v1::State::Failed,
                                "the contents of this uab file are invalid");
            return;
        }

        auto appLayerInfo = std::find_if(layerInfos.begin(),
                                         layerInfos.end(),
                                         [](const linglong::api::types::v1::UabLayer &layer) {
                                             return layer.info.kind == "app";
                                         });
        if (appLayerInfo == layerInfos.end()) {
            taskRef.updateState(linglong::api::types::v1::State::Failed,
                                "the contents of this uab file are invalid");
            return;
        }

        bool onlyApp = metaInfo.get().onlyApp && metaInfo.get().onlyApp.value();
        utils::Transaction transaction;
        for (const auto &layer : layerInfos) {
            if (isTaskDone(taskRef.subState())) {
                return;
            }

            std::error_code ec;
            auto layerDirPath = uabLayersDir / layer.info.id / layer.info.packageInfoV2Module;
            if (!std::filesystem::exists(layerDirPath, ec)) {
                if (ec) {
                    taskRef.updateState(linglong::api::types::v1::State::Failed,
                                        QString{ "get status of" } % layerDirPath.c_str()
                                          % "failed:" % ec.message().c_str());
                    return;
                }

                taskRef.updateState(linglong::api::types::v1::State::Failed,
                                    QString{ "layer directory " } % layerDirPath.c_str()
                                      % " doesn't exist");
                return;
            }

            const auto &layerDir = package::LayerDir{ layerDirPath.c_str() };
            std::optional<std::string> subRef{ std::nullopt };
            if (layer.minified) {
                subRef = metaInfo.get().uuid;
            }

            auto infoRet = layerDir.info();
            if (!infoRet) {
                taskRef.reportError(std::move(infoRet).error());
                return;
            }
            auto &info = *infoRet;

            auto refRet = package::Reference::fromPackageInfo(info);
            if (!refRet) {
                taskRef.reportError(std::move(refRet).error());
                return;
            }
            auto &ref = *refRet;

            std::vector<std::filesystem::path> overlays;
            bool isAppLayer = layer.info.kind == "app";
            if (isAppLayer) { // it's meaningless for app layer that declare minified is true
                subRef = std::nullopt;
                auto ret = uab->extractSignData();
                if (!ret) {
                    taskRef.reportError(std::move(ret).error());
                    return;
                }

                if (!ret->empty()) {
                    overlays.emplace_back(std::move(ret).value());
                }

                if (onlyApp) {
                    pullDependency(taskRef, info, "binary");
                }
            } else {
                if (onlyApp) { // ignore all non-app layers if onlyApp is true
                    continue;
                }

                auto fuzzyString = refRet->id + "/" + refRet->version.toString();
                auto fuzzyRef = package::FuzzyReference::parse(fuzzyString);
                auto localRef = this->repo.clearReference(*fuzzyRef,
                                                          {
                                                            .fallbackToRemote = false // NOLINT
                                                          });
                if (localRef) {
                    auto layerDir = this->repo.getLayerDir(*localRef, info.packageInfoV2Module);
                    if (layerDir && layerDir->valid() && refRet->version == localRef->version) {
                        // if the completed reference of local installed has the same version,
                        // skip it
                        continue;
                    }
                }
            }

            auto ret = this->repo.importLayerDir(layerDir, overlays, subRef);
            if (!ret) {
                taskRef.reportError(std::move(ret).error());
                return;
            }

            std::for_each(overlays.begin(), overlays.end(), [](const std::filesystem::path &dir) {
                std::error_code ec;
                if (std::filesystem::remove_all(dir, ec) == static_cast<std::uintmax_t>(-1) && ec) {
                    qWarning() << "failed to remove temporary directory" << dir.c_str();
                }
            });

            transaction.addRollBack(
              [this, layerInfo = std::move(info), layerRef = ref, subRef]() noexcept {
                  auto ret = this->repo.remove(layerRef, layerInfo.packageInfoV2Module, subRef);
                  if (!ret) {
                      qCritical() << "rollback importLayerDir failed:" << ret.error().message();
                  }
              });
        }

        if (oldAppRef) {
            auto ret =
              removeAfterInstall(*oldAppRef, newAppRef, this->repo.getModuleList(*oldAppRef));
            if (!ret) {
                qCritical() << "remove old reference after install newer version failed:"
                            << ret.error().message();
            }
        } else {
            // export directly
            auto mergeRet = this->repo.mergeModules();
            if (!mergeRet.has_value()) {
                qCritical() << "merge modules failed: " << mergeRet.error().message();
            }

            this->repo.exportReference(newAppRef);
            auto result = this->tryGenerateCache(newAppRef);
            if (!result) {
                taskRef.updateState(linglong::api::types::v1::State::Failed,
                                    "Failed to generate some cache.\n" + result.error().message());
                return;
            }
        }

        auto ret = executePostInstallHooks(newAppRef);
        if (!ret) {
            taskRef.reportError(std::move(ret).error());
            return;
        }

        transaction.commit();
        taskRef.updateState(linglong::api::types::v1::State::Succeed, "install uab successfully");
    };

    auto refSpec =
      QString{ "%1:%2/%3/%4/%5" }.arg("local",
                                      appRef.channel,
                                      appRef.id,
                                      appRef.arch.toString(),
                                      QString::fromStdString(app->info.packageInfoV2Module));
    auto taskRet = tasks.addNewTask({ refSpec }, std::move(installer), connection());
    if (!taskRet) {
        return toDBusReply(taskRet);
    }

    auto &taskRef = taskRet->get();
    Q_EMIT TaskAdded(QDBusObjectPath{ taskRef.taskObjectPath() });
    taskRef.updateState(linglong::api::types::v1::State::Queued, "queued to install from uab");
    return utils::serialize::toQVariantMap(api::types::v1::PackageManager1PackageTaskResult{
      .taskObjectPath = taskRef.taskObjectPath().toStdString(),
      .code = 0,
      .message = (realFile + " is now installing").toStdString(),
    });
}

auto PackageManager::InstallFromFile(const QDBusUnixFileDescriptor &fd,
                                     const QString &fileType,
                                     const QVariantMap &options) noexcept -> QVariantMap
{
    auto opts = utils::serialize::fromQVariantMap<api::types::v1::CommonOptions>(options);
    if (!opts) {
        return toDBusReply(opts);
    }

    const static QHash<QString,
                       QVariantMap (PackageManager::*)(
                         const QDBusUnixFileDescriptor &,
                         const api::types::v1::CommonOptions &) noexcept>
      installers = { { "layer", &PackageManager::installFromLayer },
                     { "uab", &PackageManager::installFromUAB } };

    if (!installers.contains(fileType)) {
        return toDBusReply(QDBusError::NotSupported,
                           QString{ "%1 is unsupported fileType" }.arg(fileType));
    }

    return std::invoke(installers[fileType], this, fd, *opts);
}

auto PackageManager::Install(const QVariantMap &parameters) noexcept -> QVariantMap
{
    auto paras =
      utils::serialize::fromQVariantMap<api::types::v1::PackageManager1InstallParameters>(
        parameters);
    if (!paras) {
        return toDBusReply(utils::error::ErrorCode::AppInstallFailed, paras.error().message());
    }

    api::types::v1::PackageManager1Package package;
    package.id = paras->package.id;
    package.channel = paras->package.channel;
    package.version = paras->package.version;

    // 解析用户输入
    auto fuzzyRef = fuzzyReferenceFromPackage(package);
    if (!fuzzyRef) {
        return toDBusReply(utils::error::ErrorCode::AppInstallFailed, fuzzyRef.error().message());
    }

    std::string curModule = "binary";

    if (paras->package.modules && paras->package.modules->size() == 1) {
        // Manually install single module
        curModule = paras->package.modules->front();
    }

    auto modules = paras->package.modules.value_or(std::vector<std::string>{ curModule });

    // 安装module
    if (curModule != "binary") {
        // 安装module必须是和binary相同的版本，所以不允许指定
        if (fuzzyRef->version) {
            return toDBusReply(utils::error::ErrorCode::AppInstallModuleNoVersion,
                               "cannot specify a version when installing a module");
        }

        auto ret = tasks.addNewTask(
          { fuzzyRef->toString() },
          [this, curModule, fuzzyRef = std::move(*fuzzyRef), repo = paras->repo](
            PackageTask &taskRef) {
              LINGLONG_TRACE("install module")
              auto localRef = this->repo.clearReference(fuzzyRef, { .fallbackToRemote = false });
              if (!localRef.has_value()) {
                  taskRef.reportError(
                    LINGLONG_ERRV("to install the module, one must first install the app",
                                  utils::error::ErrorCode::AppInstallModuleRequireAppFirst));
                  return;
              }
              auto modules = this->repo.getModuleList(*localRef);
              if (std::find(modules.begin(), modules.end(), curModule) != modules.end()) {
                  taskRef.reportError(
                    LINGLONG_ERRV("module is already installed",
                                  utils::error::ErrorCode::AppInstallModuleAlreadyExists));
                  return;
              }
              std::optional<api::types::v1::Repo> remoteRepo;
              if (repo) {
                  auto repoRet = this->repo.getRepoByAlias(*repo);
                  if (!repoRet) {
                      taskRef.reportError(
                        LINGLONG_ERRV("failed to get repo by alias", repoRet.error()));
                      return;
                  }
                  remoteRepo = *repoRet;
              }
              this->Install(taskRef, *localRef, std::nullopt, std::vector{ curModule }, remoteRepo);
          },
          connection());
        if (!ret) {
            return toDBusReply(utils::error::ErrorCode::AppInstallFailed, ret.error().message());
        }

        auto &taskRef = ret->get();
        Q_EMIT TaskAdded(QDBusObjectPath{ taskRef.taskObjectPath() });
        taskRef.updateState(linglong::api::types::v1::State::Queued,
                            "queued to install from remote");
        return utils::serialize::toQVariantMap(api::types::v1::PackageManager1PackageTaskResult{
          .taskObjectPath = taskRef.taskObjectPath().toStdString(),
          .code = 0,
          .message = "installing",
        });
    }

    // 如果用户输入了版本号，检查本地是否已经安装此版本
    if (fuzzyRef->version) {
        auto ref = this->repo.clearReference(*fuzzyRef,
                                             {
                                               .fallbackToRemote = false // NOLINT
                                             });
        if (ref) {
            return toDBusReply(utils::error::ErrorCode::AppInstallAlreadyInstalled,
                               ref->toString() + " is already installed.");
        }
    }

    // we need latest local reference
    std::optional<QString> version = fuzzyRef->version;
    fuzzyRef->version.reset();
    auto localRef = this->repo.clearReference(*fuzzyRef,
                                              {
                                                .fallbackToRemote = false // NOLINT
                                              });
    // set version back

    fuzzyRef->version = version;

    api::types::v1::PackageManager1RequestInteractionAdditionalMessage additionalMessage;
    if (localRef) {
        additionalMessage.localRef = localRef->toString().toStdString();
    }

    auto refRet = [&paras, &fuzzyRef, &curModule, this] {
        if (!paras->repo) {
            return this->repo.getRemoteReferenceByPriority(*fuzzyRef,
                                                           { .onlyClearHighestPriority = false },
                                                           curModule);
        }

        auto originalPriority = this->repo.promotePriority(paras->repo.value());
        auto recover = linglong::utils::finally::finally([&] {
            this->repo.recoverPriority(paras->repo.value(), originalPriority);
        });

        return this->repo.getRemoteReferenceByPriority(*fuzzyRef,
                                                       { .onlyClearHighestPriority = true },
                                                       curModule);
    }();

    if (!refRet) {
        if (refRet.error().code()
            == static_cast<int>(utils::error::ErrorCode::AppNotFoundFromRemote)) {
            return toDBusReply(utils::error::ErrorCode::AppInstallNotFoundFromRemote,
                               refRet.error().message());
        }
        return toDBusReply(refRet.error().code(), refRet.error().message());
    }

    auto remoteRef = refRet->reference;

    additionalMessage.remoteRef = remoteRef.toString().toStdString();

    // 如果远程版本大于本地版本就升级，否则需要加--force降级，如果本地没有则直接安装，如果本地版本和远程版本相等就提示已安装
    auto msgType = api::types::v1::InteractionMessageType::Install;
    if (!additionalMessage.localRef.empty()) {
        if (remoteRef.version == localRef->version) {
            return toDBusReply(utils::error::ErrorCode::AppInstallAlreadyInstalled,
                               localRef->toString() + " is already installed");
        }

        if (remoteRef.version > localRef->version) {
            msgType = api::types::v1::InteractionMessageType::Upgrade;
        } else if (!paras->options.force) {
            auto err = QString("The latest version has been installed. If you want to "
                               "replace it, try using 'll-cli install %1/%2 --force'")
                         .arg(remoteRef.id, remoteRef.version.toString());
            return toDBusReply(utils::error::ErrorCode::AppInstallNeedDowngrade, err);
        }
    }

    auto refSpec = QString{ "%1:%2/%3/%4/%5" }.arg(QString::fromStdString(refRet->repo.name),
                                                   remoteRef.channel,
                                                   remoteRef.id,
                                                   remoteRef.arch.toString(),
                                                   QString::fromStdString(curModule));
    // Note: do not capture any reference of variable which defined in this func.
    // it will be a dangling reference.
    auto installer = [this,
                      remoteRef,
                      localRef = localRef.has_value()
                        ? std::make_optional(std::move(localRef).value())
                        : std::nullopt,
                      curModule,
                      modules,
                      skipInteraction = paras->options.skipInteraction,
                      msgType,
                      additionalMessage,
                      originalRepo = refRet->repo](PackageTask &taskRef) {
        // 升级需要用户交互
        if (msgType == api::types::v1::InteractionMessageType::Upgrade && !skipInteraction) {
            Q_EMIT RequestInteraction(QDBusObjectPath(taskRef.taskObjectPath()),
                                      static_cast<int>(msgType),
                                      utils::serialize::toQVariantMap(additionalMessage));

            QEventLoop loop;
            api::types::v1::InteractionReply interactionReply;
            // Note: if capture the &taskRef into this lambda, be careful with it's life cycle.
            connect(this,
                    &PackageManager::ReplyReceived,
                    [&interactionReply, &loop](const QVariantMap &reply) {
                        interactionReply =
                          *utils::serialize::fromQVariantMap<api::types::v1::InteractionReply>(
                            reply);
                        loop.exit(0);
                    });
            loop.exec();
            if (interactionReply.action != "yes") {
                taskRef.updateState(linglong::api::types::v1::State::Canceled, "canceled");
            }
        }

        if (isTaskDone(taskRef.subState())) {
            return;
        }

        this->Install(taskRef,
                      remoteRef,
                      localRef,
                      localRef.has_value() ? this->repo.getModuleList(*localRef) : modules,
                      originalRepo);
    };

    auto taskRet = tasks.addNewTask({ refSpec }, std::move(installer), connection());
    if (!taskRet) {
        return toDBusReply(utils::error::ErrorCode::Unknown, taskRet.error().message());
    }

    auto &taskRef = taskRet->get();
    Q_EMIT TaskAdded(QDBusObjectPath{ taskRef.taskObjectPath() });
    taskRef.updateState(linglong::api::types::v1::State::Queued, "queued to install from remote");
    return utils::serialize::toQVariantMap(api::types::v1::PackageManager1PackageTaskResult{
      .taskObjectPath = taskRef.taskObjectPath().toStdString(),
      .code = 0,
      .message = (remoteRef.toString() + " is now installing").toStdString(),
    });
}

void PackageManager::Install(PackageTask &taskContext,
                             const package::Reference &newRef,
                             std::optional<package::Reference> oldRef,
                             const std::vector<std::string> &modules,
                             const std::optional<api::types::v1::Repo> &repo) noexcept
{
    LINGLONG_TRACE("install app")
    taskContext.updateState(linglong::api::types::v1::State::Processing,
                            "Installing " + newRef.toString());

    utils::Transaction transaction;

    // 仅安装远程存在的modules
    auto installModules = [&repo, &newRef, &modules, this] {
        if (!repo) {
            return this->repo.getRemoteModuleListByPriority(newRef, modules, repo.has_value());
        }

        auto originalPriority = this->repo.promotePriority(repo->alias.value_or(repo->name));
        auto recover = linglong::utils::finally::finally([&] {
            this->repo.recoverPriority(repo->alias.value_or(repo->name), originalPriority);
        });

        return this->repo.getRemoteModuleListByPriority(newRef, modules, repo.has_value());
    }();

    if (!installModules) {
        taskContext.reportError(LINGLONG_ERRV(std::move(installModules).error().message(),
                                              utils::error::ErrorCode::AppInstallFailed));
        return;
    }
    if (installModules->second.empty()) {
        auto list = std::accumulate(modules.begin(), modules.end(), std::string(","));
        taskContext.reportError(
          LINGLONG_ERRV("These modules do not exist remotely: " + QString::fromStdString(list),
                        utils::error::ErrorCode::AppInstallModuleNotFound));
        return;
    }
    transaction.addRollBack([this, &newRef, installModules = *installModules]() noexcept {
        auto tmp = PackageTask::createTemporaryTask();
        UninstallRef(tmp, newRef, installModules.second);
        if (tmp.state() != linglong::api::types::v1::State::Succeed) {
            qCritical() << "failed to rollback install " << newRef.toString();
        }
    });
    InstallRef(taskContext,
               newRef,
               installModules->second,
               installModules->first); // install modules
    if (isTaskDone(taskContext.subState())) {
        return;
    }

    taskContext.updateSubState(linglong::api::types::v1::SubState::PostAction,
                               "processing after install");

    auto mergeRet = this->repo.mergeModules();
    if (!mergeRet) {
        qCritical() << "merge modules failed: " << mergeRet.error().message();
    }

    auto layer = this->repo.getLayerItem(newRef);
    if (!layer) {
        taskContext.reportError(LINGLONG_ERRV(std::move(layer).error().message(),
                                              utils::error::ErrorCode::AppInstallFailed));
        return;
    }
    // only app should do 'remove' and 'export'
    if (layer->info.kind == "app") {
        // remove all previous modules
        if (oldRef) {
            auto ret = this->removeAfterInstall(*oldRef, newRef, modules);
            if (!ret) {
                taskContext.reportError(LINGLONG_ERRV(
                  "Failed to remove old reference " % oldRef->toString() % " after install "
                    % newRef.toString() % ": " % ret.error().message(),
                  utils::error::ErrorCode::AppInstallFailed));
                return;
            }
        } else {
            this->repo.exportReference(newRef);
        }
        auto result = this->tryGenerateCache(newRef);
        if (!result) {
            taskContext.reportError(
              LINGLONG_ERRV("Failed to generate some cache.\n" + result.error().message(),
                            utils::error::ErrorCode::AppInstallFailed));
            return;
        }
    }

    auto ret = executePostInstallHooks(newRef);
    if (!ret) {
        taskContext.updateState(linglong::api::types::v1::State::Failed,
                                "Failed to execute postInstall hooks.\n" + ret.error().message());
        return;
    }

    transaction.commit();
    taskContext.updateState(linglong::api::types::v1::State::Succeed,
                            "Install " + newRef.toString() + " (from repo: "
                              + installModules->first.name.c_str() + ") " + " success");
}

void PackageManager::InstallRef(PackageTask &taskContext,
                                const package::Reference &ref,
                                std::vector<std::string> modules,
                                const api::types::v1::Repo &repo) noexcept
{
    LINGLONG_TRACE("install " + ref.toString());

    taskContext.updateSubState(linglong::api::types::v1::SubState::PreAction,
                               "Beginning to install");
    auto currentArch = package::Architecture::currentCPUArchitecture();
    if (!currentArch) {
        taskContext.updateState(linglong::api::types::v1::State::Failed,
                                currentArch.error().message());
    }

    if (ref.arch != *currentArch) {
        taskContext.updateState(linglong::api::types::v1::State::Failed,
                                "app arch:" + ref.arch.toString() + " not match host architecture");
    }

    taskContext.updateSubState(linglong::api::types::v1::SubState::InstallApplication,
                               "Installing application " + ref.toString());

    auto deletedList = this->repo.listLocalBy(
      linglong::repo::repoCacheQuery{ .id = ref.id.toStdString(),
                                      .channel = ref.channel.toStdString(),
                                      .version = ref.version.toString().toStdString(),
                                      .deleted = true });
    if (!deletedList) {
        taskContext.updateState(linglong::api::types::v1::State::Failed,
                                deletedList.error().message());
        Q_ASSERT(false);
        return;
    }

    utils::Transaction t;

    for (const auto &deletedItem : *deletedList) {
        if (isTaskDone(taskContext.subState())) {
            return;
        }

        auto it =
          std::find_if(modules.begin(), modules.end(), [&deletedItem](const std::string &module) {
              if (module == "runtime" && deletedItem.info.packageInfoV2Module == "binary") {
                  return true;
              }

              if (module == "binary" && deletedItem.info.packageInfoV2Module == "runtime") {
                  return true;
              }

              return module == deletedItem.info.packageInfoV2Module;
          });
        if (it == modules.end()) {
            continue;
        }

        auto ret = this->repo.markDeleted(ref, false, deletedItem.info.packageInfoV2Module);
        if (!ret) {
            qCritical() << "Failed to mark old package as deleted" << ref.toString() << ":"
                        << ret.error().message();
            taskContext.updateState(linglong::api::types::v1::State::Failed, "install failed");
            Q_ASSERT(false);
        }

        t.addRollBack([this, &ref, module = deletedItem.info.packageInfoV2Module]() noexcept {
            auto ret = this->repo.markDeleted(ref, true, module);
            if (!ret) {
                qWarning() << "failed to rollback marking deleted" << ref.toString() << ":"
                           << ret.error().message();
            }
        });

        modules.erase(it);
    }

    for (const auto &module : modules) {
        if (isTaskDone(taskContext.subState())) {
            return;
        }

        this->repo.pull(taskContext, ref, module, repo);
        if (isTaskDone(taskContext.subState())) {
            return;
        }

        t.addRollBack([this, &ref, &module]() noexcept {
            auto result = this->repo.remove(ref, module);
            if (!result) {
                qCritical() << result.error();
                Q_ASSERT(false);
            }
        });

        if (module != "binary" && module != "runtime") {
            continue;
        }

        auto layerDir = this->repo.getLayerDir(ref);
        if (!layerDir) {
            taskContext.updateState(linglong::api::types::v1::State::Failed,
                                    LINGLONG_ERRV(layerDir).message());
            return;
        }

        auto info = layerDir->info();
        if (!info) {
            taskContext.updateState(linglong::api::types::v1::State::Failed,
                                    LINGLONG_ERRV(info).message());
            return;
        }

        // Note: Do not set module by app's module here
        pullDependency(taskContext, *info, "binary");
    }

    t.commit();
}

auto PackageManager::Uninstall(const QVariantMap &parameters) noexcept -> QVariantMap
{
    auto paras =
      utils::serialize::fromQVariantMap<api::types::v1::PackageManager1UninstallParameters>(
        parameters);
    if (!paras) {
        return toDBusReply(utils::error::ErrorCode::AppUninstallFailed, paras.error().message());
    }

    auto fuzzyRef = fuzzyReferenceFromPackage(paras->package);
    if (!fuzzyRef) {
        return toDBusReply(utils::error::ErrorCode::AppUninstallFailed, fuzzyRef.error().message());
    }

    auto ref = this->repo.clearReference(*fuzzyRef,
                                         {
                                           .fallbackToRemote = false // NOLINT
                                         });
    if (!ref) {
        if (ref.error().code() == static_cast<int>(utils::error::ErrorCode::AppNotFoundFromLocal)) {
            return toDBusReply(utils::error::ErrorCode::AppUninstallNotFoundFromLocal,
                               fuzzyRef->toString() + " not installed.");
        }
        return toDBusReply(ref.error().code(), fuzzyRef->toString() + " not installed.");
    }
    auto reference = *ref;

    auto runningRef = isRefBusy(reference);
    if (!runningRef) {
        return toDBusReply(utils::error::ErrorCode::AppUninstallFailed,
                           "failed to get the state of target ref:" % reference.toString() + ": "
                             + runningRef.error().message());
    }

    if (*runningRef) {
        return toDBusReply(utils::error::ErrorCode::AppUninstallFailed,
                           "The application is currently running and cannot be "
                           "uninstalled. Please turn off the application and try again.",
                           "notification");
    }

    auto curModule = paras->package.packageManager1PackageModule.value_or("binary");
    const auto defaultRepo = linglong::repo::getDefaultRepo(this->repo.getConfig());
    auto refSpec = QString{ "%1:%2/%3/%4/%5" }.arg(QString::fromStdString(defaultRepo.name),
                                                   reference.channel,
                                                   reference.id,
                                                   reference.arch.toString(),
                                                   QString::fromStdString(curModule));

    auto taskRet = tasks.addNewTask(
      { refSpec },
      [this, reference, curModule](PackageTask &taskRef) {
          if (isTaskDone(taskRef.subState())) {
              return;
          }

          this->Uninstall(taskRef, reference, curModule);
      },
      connection());
    if (!taskRet) {
        return toDBusReply(utils::error::ErrorCode::AppUninstallFailed, taskRet.error().message());
    }

    auto &taskRef = taskRet->get();
    Q_EMIT TaskAdded(QDBusObjectPath{ taskRef.taskObjectPath() });
    taskRef.updateState(linglong::api::types::v1::State::Queued, "queued to uninstall");
    return utils::serialize::toQVariantMap(api::types::v1::PackageManager1PackageTaskResult{
      .taskObjectPath = taskRef.taskObjectPath().toStdString(),
      .code = 0,
      .message = (ref->toString() + " is now uninstalling").toStdString(),
    });
}

void PackageManager::UninstallRef(PackageTask &taskContext,
                                  const package::Reference &ref,
                                  const std::vector<std::string> &modules) noexcept
{
    LINGLONG_TRACE("uninstall ref " + ref.toString());
    if (isTaskDone(taskContext.subState())) {
        return;
    }

    taskContext.updateSubState(linglong::api::types::v1::SubState::Uninstall, "Remove layer files");
    utils::Transaction transaction;

    for (const auto &module : modules) {
        if (module == "binary" || module == "runtime") {
            auto ret = this->removeCache(ref);
            if (!ret) {
                qCritical() << ret.error().message();
            }
        }
        auto result = this->repo.remove(ref, module);
        if (!result) {
            taskContext.reportError(
              LINGLONG_ERRV(result.error().message(), utils::error::ErrorCode::AppUninstallFailed));
            return;
        }

        transaction.addRollBack([this, &ref, &module]() noexcept {
            auto tmpTask = PackageTask::createTemporaryTask();
            this->repo.pull(tmpTask, ref, module);
            if (tmpTask.state() != linglong::api::types::v1::State::Succeed) {
                qCritical() << "failed to rollback module" << module.c_str() << "of ref"
                            << ref.toString();
            }
            if (module == "binary" || module == "runtime") {
                auto ret = this->tryGenerateCache(ref);
                if (!ret) {
                    qCritical() << ret.error().message();
                }
            }
        });
    }

    transaction.commit();
}

void PackageManager::Uninstall(PackageTask &taskContext,
                               const package::Reference &ref,
                               const std::string &module) noexcept
{
    if (isTaskDone(taskContext.subState())) {
        return;
    }

    taskContext.updateState(api::types::v1::State::Processing, "start to uninstalling package");
    taskContext.updateSubState(linglong::api::types::v1::SubState::PreAction,
                               "prepare uninstalling package");

    std::vector<std::string> removedModules{ module };
    utils::Transaction transaction;

    if (module == "binary" || module == "runtime") {
        auto modules = this->repo.getModuleList(ref);
        removedModules = std::move(modules);

        this->repo.unexportReference(ref);
        transaction.addRollBack([this, &ref]() noexcept {
            this->repo.exportReference(ref);
        });
    }

    UninstallRef(taskContext, ref, removedModules);
    if (isTaskDone(taskContext.subState())) {
        return;
    }

    auto ret = executePostUninstallHooks(ref);
    if (!ret) {
        qWarning() << "failed to execute postUninstall hooks" << ret.error();
    }

    transaction.commit();

    taskContext.updateState(linglong::api::types::v1::State::Succeed,
                            "Uninstall " + ref.toString() + " success");

    auto mergeRet = this->repo.mergeModules();
    if (!mergeRet.has_value()) {
        qCritical() << "merge modules failed: " << mergeRet.error().message();
    }
}

auto PackageManager::Update(const QVariantMap &parameters) noexcept -> QVariantMap
{
    auto paras = utils::serialize::fromQVariantMap<api::types::v1::PackageManager1UpdateParameters>(
      parameters);
    if (!paras) {
        return toDBusReply(utils::error::ErrorCode::AppUpgradeFailed, paras.error().message());
    }

    std::unordered_map<package::Reference, package::ReferenceWithRepo> upgrades;
    QStringList refSpecs;
    for (const auto &package : paras->packages) {
        auto installedAppFuzzyRef = fuzzyReferenceFromPackage(package);
        if (!installedAppFuzzyRef) {
            return toDBusReply(utils::error::ErrorCode::AppUpgradeFailed,
                               installedAppFuzzyRef.error().message());
        }

        auto ref = this->repo.clearReference(*installedAppFuzzyRef,
                                             {
                                               .fallbackToRemote = false // NOLINT
                                             });
        if (!ref) {
            if (ref.error().code()
                == static_cast<int>(utils::error::ErrorCode::AppNotFoundFromLocal)) {
                return toDBusReply(utils::error::ErrorCode::AppUpgradeNotFound,
                                   installedAppFuzzyRef->toString() + " not installed.");
            }
            return toDBusReply(ref.error().code(),
                               installedAppFuzzyRef->toString() + " not installed.");
        }

        auto layerItem = this->repo.getLayerItem(*ref);
        if (!layerItem) {
            return toDBusReply(utils::error::ErrorCode::AppUpgradeFailed,
                               layerItem.error().message());
        }

        auto newRef = this->repo.latestRemoteReference(*installedAppFuzzyRef);
        if (!newRef) {
            return toDBusReply(newRef.error().code(), newRef.error().message());
        }

        if (newRef->reference.version <= ref->version) {
            return toDBusReply(
              utils::error::ErrorCode::AppUpgradeLatestInstalled,
              QString("remote version is %1, the latest version %2 is already installed")
                .arg(newRef->reference.version.toString(), ref->version.toString()));
        }

        const auto &reference = *ref;
        // FIXME: use sha256 instead of refSpec
        auto refSpec = QString{ "%1:%2/%3/%4/%5" }.arg(QString::fromStdString(newRef->repo.name),
                                                       reference.channel,
                                                       reference.id,
                                                       reference.arch.toString(),
                                                       "binary");
        refSpecs.append(std::move(refSpec));
        upgrades.emplace(std::move(ref).value(), std::move(newRef).value());
    }

    auto ret = tasks.addNewTask(
      refSpecs,
      [this, upgrades = std::move(upgrades)](PackageTask &taskRef) {
          for (const auto &[reference, newReference] : upgrades) {
              if (taskRef.subState() == linglong::api::types::v1::SubState::AllDone) {
                  return;
              }

              qInfo() << "Before upgrade, old Ref: " << reference.toString()
                      << " new Ref: " << newReference.reference.toString();
              this->Update(taskRef, reference, newReference);
          }
      },
      connection());
    if (!ret) {
        return toDBusReply(utils::error::ErrorCode::AppUpgradeFailed, ret.error().message());
    }

    auto &taskRef = ret->get();
    Q_EMIT TaskAdded(QDBusObjectPath{ taskRef.taskObjectPath() });
    taskRef.updateState(linglong::api::types::v1::State::Queued, "queued to update");
    return utils::serialize::toQVariantMap(api::types::v1::PackageManager1PackageTaskResult{
      .taskObjectPath = taskRef.taskObjectPath().toStdString(),
      .code = 0,
      .message = "updating",
    });
}

void PackageManager::Update(PackageTask &taskContext,
                            const package::Reference &oldRef,
                            const package::ReferenceWithRepo &refWithRepo) noexcept
{
    LINGLONG_TRACE("update " + oldRef.toString());

    const auto &newRef = refWithRepo.reference;
    taskContext.updateState(api::types::v1::State::Processing, "start to uninstalling package");
    auto modules = this->repo.getModuleList(oldRef);
    // 仅安装远程存在的modules
    auto installModules =
      this->repo.getRemoteModuleListByPriority(newRef, modules, false, refWithRepo.repo);
    if (!installModules.has_value()) {
        taskContext.reportError(LINGLONG_ERRV(std::move(installModules).error().message(),
                                              utils::error::ErrorCode::AppUpgradeFailed));
        return;
    }
    if (installModules->second.empty()) {
        auto list = std::accumulate(modules.begin(), modules.end(), std::string(","));
        taskContext.reportError(
          LINGLONG_ERRV("These modules do not exist remotely: " + QString::fromStdString(list),
                        utils::error::ErrorCode::AppUpgradeFailed));
        return;
    }
    this->InstallRef(taskContext, newRef, installModules->second, installModules->first);
    if (isTaskDone(taskContext.subState())) {
        return;
    }

    auto oldRefLayerItem = this->repo.getLayerItem(oldRef);

    taskContext.updateState(
      linglong::api::types::v1::State::PartCompleted,
      QString{ "Upgrade %1 (from repo: %2) to %3 (from repo: %4) success" }.arg(
        oldRef.toString(),
        oldRefLayerItem ? oldRefLayerItem->repo.c_str() : "local",
        newRef.toString(),
        refWithRepo.repo.name.c_str()));

    auto ret = this->isRefBusy(oldRef);
    if (ret.has_value() && *ret == true) {
        // use setMessage and setSubState directly will not trigger signal
        taskContext.setSubState(linglong::api::types::v1::SubState::PackageManagerDone),
          taskContext.setMessage(
            "Please restart the application after saving the data to experience the new version.");
    }

    // we don't need to set task state to failed after install newer version successfully
    auto newItem = this->repo.getLayerItem(newRef);
    if (!newItem) {
        qCritical() << "get layer item of ref" << newRef.toString()
                    << "failed:" << newItem.error().message();
        return;
    }

    if (newItem->info.kind == "app") {
        auto ret = removeAfterInstall(oldRef, newRef, modules);
        if (!ret) {
            qCritical() << "remove after install of ref" << oldRef.toString()
                        << "failed:" << ret.error().message();
            return;
        }

        ret = executePostInstallHooks(newRef);
        if (!ret) {
            qCritical() << "failed to execute post install hooks" << ret.error().message();
            return;
        }

        auto result = this->tryGenerateCache(newRef);
        if (!result) {
            taskContext.reportError(
              LINGLONG_ERRV("Failed to generate some cache.\n" + result.error().message(),
                            utils::error::ErrorCode::AppUpgradeFailed));
            return;
        }
    }

    auto mergeRet = this->repo.mergeModules();
    if (!mergeRet.has_value()) {
        qCritical() << "merge modules failed: " << mergeRet.error().message();
    }
}

auto PackageManager::Search(const QVariantMap &parameters) noexcept -> QVariantMap
{
    auto paras = utils::serialize::fromQVariantMap<api::types::v1::PackageManager1SearchParameters>(
      parameters);
    if (!paras) {
        return toDBusReply(paras);
    }

    auto fuzzyRef =
      package::FuzzyReference::create(std::nullopt, paras->id.c_str(), std::nullopt, std::nullopt);
    if (!fuzzyRef) {
        return toDBusReply(fuzzyRef);
    }
    auto jobID = QUuid::createUuid().toString();
    auto repoConfig = this->repo.getConfig();

    m_search_queue.runTask([this,
                            jobID,
                            params = std::move(paras).value(),
                            ref = std::move(*fuzzyRef),
                            repo = std::move(repoConfig)]() {
        std::map<std::string, std::vector<api::types::v1::PackageInfoV2>> pkgs;
        for (const auto &repoAlias : params.repos) {
            auto repoRet = this->repo.getRepoByAlias(repoAlias);
            if (!repoRet) {
                qWarning() << "repo" << repoAlias.c_str() << "not found";
                continue;
            }

            auto pkgInfosRet = this->repo.listRemote(ref, *repoRet);
            if (!pkgInfosRet) {
                qWarning() << "list remote failed: " << pkgInfosRet.error().message();
                Q_EMIT this->SearchFinished(
                  jobID,
                  toDBusReply(pkgInfosRet.error().code(), pkgInfosRet.error().message()));
                return;
            }

            if (pkgInfosRet->empty()) {
                continue;
            }

            pkgs.emplace(repoRet->alias.value_or(repoRet->name), std::move(*pkgInfosRet));
        }

        Q_EMIT this->SearchFinished(
          jobID,
          utils::serialize::toQVariantMap(api::types::v1::PackageManager1SearchResult{
            .packages = std::move(pkgs),
            .code = 0,
            .message = "",
            .type = "",
          }));
    });
    auto result = utils::serialize::toQVariantMap(api::types::v1::PackageManager1JobInfo{
      .id = jobID.toStdString(),
      .code = 0,
      .message = "",
      .type = "",
    });
    return result;
}

void PackageManager::pullDependency(PackageTask &taskContext,
                                    const api::types::v1::PackageInfoV2 &info,
                                    const std::string &module) noexcept
{
    if (info.kind != "app") {
        return;
    }

    if (module != "binary" && module != "runtime") {
        return;
    }

    LINGLONG_TRACE("pull dependencies of " + QString::fromStdString(info.id));

    utils::Transaction transaction;
    if (info.runtime) {
        auto fuzzyRuntime = package::FuzzyReference::parse(QString::fromStdString(*info.runtime));
        if (!fuzzyRuntime) {
            taskContext.updateState(linglong::api::types::v1::State::Failed,
                                    LINGLONG_ERRV(fuzzyRuntime).message());
            return;
        }

        auto runtime = this->repo.getRemoteReferenceByPriority(*fuzzyRuntime,
                                                               {
                                                                 .semanticMatching = true,
                                                               });
        if (!runtime) {
            taskContext.updateState(linglong::api::types::v1::State::Failed,
                                    runtime.error().message());
            return;
        }

        // 如果runtime已存在，则直接使用, 否则从远程拉取
        auto runtimeLayerDir = repo.getLayerDir(runtime->reference);
        if (!runtimeLayerDir) {
            if (isTaskDone(taskContext.subState())) {
                return;
            }

            taskContext.updateSubState(linglong::api::types::v1::SubState::InstallRuntime,
                                       "Installing runtime " + runtime->reference.toString());

            this->repo.pull(taskContext, runtime->reference, module, runtime->repo);
            if (isTaskDone(taskContext.subState())) {
                return;
            }

            auto ret = executePostInstallHooks(runtime->reference);
            if (!ret) {
                taskContext.updateState(linglong::api::types::v1::State::Failed,
                                        LINGLONG_ERRV(ret).message());
                return;
            }

            transaction.addRollBack([this, runtimeRef = *runtime, module]() noexcept {
                auto result = this->repo.remove(runtimeRef.reference, module);
                if (!result) {
                    qCritical() << result.error();
                    Q_ASSERT(false);
                }

                result = executePostUninstallHooks(runtimeRef.reference);
                if (!result) {
                    qCritical() << result.error();
                    Q_ASSERT(false);
                }
            });
        }
    }

    auto fuzzyBase = package::FuzzyReference::parse(QString::fromStdString(info.base));
    if (!fuzzyBase) {
        taskContext.updateState(linglong::api::types::v1::State::Failed,
                                LINGLONG_ERRV(fuzzyBase).message());
        return;
    }

    auto base = this->repo.getRemoteReferenceByPriority(*fuzzyBase,
                                                        {
                                                          .semanticMatching = true,
                                                        });
    if (!base) {
        taskContext.updateState(linglong::api::types::v1::State::Failed,
                                LINGLONG_ERRV(base).message());
        return;
    }

    // 如果base已存在，则直接使用, 否则从远程拉取
    auto baseLayerDir = repo.getLayerDir(base->reference, module);
    if (!baseLayerDir) {
        if (isTaskDone(taskContext.subState())) {
            return;
        }

        taskContext.updateSubState(linglong::api::types::v1::SubState::InstallBase,
                                   "Installing base " + base->reference.toString());
        this->repo.pull(taskContext, base->reference, module, base->repo);
        if (isTaskDone(taskContext.subState())) {
            return;
        }

        auto ret = executePostInstallHooks(base->reference);
        if (!ret) {
            taskContext.updateState(linglong::api::types::v1::State::Failed,
                                    LINGLONG_ERRV(ret).message());
            return;
        }

        transaction.addRollBack([this, baseRef = *base, module]() noexcept {
            auto result = this->repo.remove(baseRef.reference, module);
            if (!result) {
                qCritical() << result.error();
                Q_ASSERT(false);
            }

            result = executePostUninstallHooks(baseRef.reference);
            if (!result) {
                qCritical() << result.error();
                Q_ASSERT(false);
            }
        });
    }

    transaction.commit();
}

auto PackageManager::Prune() noexcept -> QVariantMap
{
    auto jobID = QUuid::createUuid().toString();
    m_prune_queue.runTask([this, jobID]() {
        std::vector<api::types::v1::PackageInfoV2> pkgs;
        auto ret = Prune(pkgs);
        if (!ret.has_value()) {
            Q_EMIT this->PruneFinished(jobID, toDBusReply(ret));
            return;
        }
        auto result = api::types::v1::PackageManager1PruneResult{
            .packages = pkgs,
            .code = 0,
            .message = "",
        };
        Q_EMIT this->PruneFinished(jobID, utils::serialize::toQVariantMap(result));
    });
    auto result = utils::serialize::toQVariantMap(api::types::v1::PackageManager1JobInfo{
      .id = jobID.toStdString(),
      .code = 0,
      .message = "",
    });
    return result;
}

utils::error::Result<void>
PackageManager::Prune(std::vector<api::types::v1::PackageInfoV2> &removed) noexcept
{
    LINGLONG_TRACE("prune");
    auto pkgsInfo = this->repo.listLocal();
    if (!pkgsInfo) {
        return LINGLONG_ERR(pkgsInfo);
    }

    std::unordered_map<package::Reference, int> target;

    auto scanExtensionsByInfo = [&target, this](const api::types::v1::PackageInfoV2 &info) {
        if (info.extensions) {
            for (const auto &extension : *info.extensions) {
                std::string name = extension.name;
                auto ext = extension::ExtensionFactory::makeExtension(name);
                if (!ext->shouldEnable(name)) {
                    continue;
                }

                auto fuzzyRef =
                  package::FuzzyReference::create(QString::fromStdString(info.channel),
                                                  QString::fromStdString(name),
                                                  QString::fromStdString(extension.version),
                                                  std::nullopt);
                auto ref = repo.clearReference(
                  *fuzzyRef,
                  { .forceRemote = false, .fallbackToRemote = false, .semanticMatching = true });
                if (ref) {
                    target[*ref] += 1;
                }
            }
        }
    };
    auto scanExtensionsByRef = [scanExtensionsByInfo, this](package::Reference &ref) {
        auto item = this->repo.getLayerItem(ref);
        if (!item) {
            qWarning() << item.error().message();
            return;
        }
        scanExtensionsByInfo(item->info);
    };

    for (const auto &info : *pkgsInfo) {
        if (info.packageInfoV2Module != "binary" && info.packageInfoV2Module != "runtime") {
            continue;
        }

        if (info.kind != "app") {
            auto ref = package::Reference::fromPackageInfo(info);
            if (!ref) {
                qWarning() << ref.error().message();
                continue;
            }
            // Note: if the ref already exists, it's ok, somebody depends it.
            target.try_emplace(std::move(*ref), 0);
            continue;
        }

        if (info.runtime) {
            auto runtimeFuzzyRef =
              package::FuzzyReference::parse(QString::fromStdString(info.runtime.value()));
            if (!runtimeFuzzyRef) {
                qWarning() << runtimeFuzzyRef.error().message();
                continue;
            }

            auto runtimeRef = this->repo.clearReference(*runtimeFuzzyRef,
                                                        {
                                                          .forceRemote = false,
                                                          .fallbackToRemote = false,
                                                          .semanticMatching = true,
                                                        });
            if (!runtimeRef) {
                qWarning() << runtimeRef.error().message();
                continue;
            }
            target[*runtimeRef] += 1;
            scanExtensionsByRef(*runtimeRef);
        }

        auto baseFuzzyRef = package::FuzzyReference::parse(QString::fromStdString(info.base));
        if (!baseFuzzyRef) {
            qWarning() << baseFuzzyRef.error().message();
            continue;
        }

        auto baseRef = this->repo.clearReference(*baseFuzzyRef,
                                                 {
                                                   .forceRemote = false,
                                                   .fallbackToRemote = false,
                                                   .semanticMatching = true,
                                                 });
        if (!baseRef) {
            qWarning() << baseRef.error().message();
            continue;
        }
        target[*baseRef] += 1;
        scanExtensionsByRef(*baseRef);
        scanExtensionsByInfo(info);
    }

    for (const auto &it : target) {
        if (it.second != 0) {
            continue;
        }

        // NOTE: if the binary module is removed, other modules should be removed too.
        for (const auto &module : this->repo.getModuleList(it.first)) {
            auto layer = this->repo.getLayerDir(it.first, module);
            if (!layer) {
                qWarning() << layer.error().message();
                continue;
            }

            auto info = layer->info();

            if (!info) {
                qWarning() << info.error().message();
                continue;
            }

            removed.emplace_back(std::move(*info));

            auto result = this->repo.remove(it.first, module);
            if (!result) {
                return LINGLONG_ERR(result);
            }
        }
    }

    if (!target.empty()) {
        auto mergeRet = this->repo.mergeModules();
        if (!mergeRet.has_value()) {
            qCritical() << "merge modules failed: " << mergeRet.error().message();
        }
    }
    auto pruneRet = this->repo.prune();
    if (!pruneRet) {
        return LINGLONG_ERR(pruneRet);
    }
    return LINGLONG_OK;
}

void PackageManager::ReplyInteraction([[maybe_unused]] QDBusObjectPath object_path,
                                      const QVariantMap &replies)
{
    Q_EMIT this->ReplyReceived(replies);
}

utils::error::Result<void> PackageManager::generateCache(const package::Reference &ref) noexcept
{
    LINGLONG_TRACE("generate cache for " + ref.toString());

    auto layerItem = this->repo.getLayerItem(ref);
    if (!layerItem) {
        return LINGLONG_ERR(layerItem);
    }

    const auto appCache = std::filesystem::path(LINGLONG_ROOT) / "cache" / layerItem->commit;
    const std::string appCacheDest = "/run/linglong/cache";
    const std::string generatorDest = "/run/linglong/generator";

    utils::Transaction transaction;

#ifdef LINGLONG_FONT_CACHE_GENERATOR
    const auto appFontCache = appCache / "fontconfig";
    const std::string fontGenerator = generatorDest + "/font-cache-generator";
#endif
    std::error_code ec;
    std::filesystem::create_directories(appCache, ec);
    if (ec) {
        return LINGLONG_ERR(QString::fromStdString(ec.message()));
    }

    transaction.addRollBack([&appCache]() noexcept {
        std::error_code ec;
        std::filesystem::remove_all(appCache, ec);
        if (ec) {
            qCritical() << QString::fromStdString(ec.message());
        }
    });

    runtime::RunContext runContext(this->repo);
    auto res = runContext.resolve(ref);
    if (!res) {
        return LINGLONG_ERR(res);
    }

    int64_t uid = getuid();
    int64_t gid = getgid();

    std::filesystem::path ldConfPath{ appCache / "ld.so.conf" };

    linglong::generator::ContainerCfgBuilder cfgBuilder;
    res = runContext.fillContextCfg(cfgBuilder);
    if (!res) {
        return LINGLONG_ERR(res);
    }
    cfgBuilder.setAppId(ref.id.toStdString())
      .setAppCache(appCache, false)
      .addUIdMapping(uid, uid, 1)
      .addGIdMapping(gid, gid, 1)
      .bindDefault()
      .bindCgroup()
      .bindRun()
      .bindUserGroup()
      .forwardDefaultEnv()
      .addExtraMounts(std::vector<ocppi::runtime::config::types::Mount>{
        ocppi::runtime::config::types::Mount{ .destination = generatorDest,
                                              .options = { { "rbind", "ro" } },
                                              .source = LINGLONG_LIBEXEC_DIR,
                                              .type = "bind" },
        ocppi::runtime::config::types::Mount{
          .destination = "/etc/ld.so.conf.d/zz_deepin-linglong-app.conf",
          .options = { { "rbind", "ro" } },
          .source = ldConfPath,
          .type = "bind",
        } })
      .enableSelfAdjustingMount();
#ifdef LINGLONG_FONT_CACHE_GENERATOR
    cfgBuilder.enableFontCache();
#endif

    // generate ld config
    {
        std::ofstream ofs(ldConfPath, std::ios::binary | std::ios::out | std::ios::trunc);
        Q_ASSERT(ofs.is_open());
        if (!ofs.is_open()) {
            return LINGLONG_ERR("create ld config in bundle directory");
        }
        ofs << cfgBuilder.ldConf(ref.arch.getTriplet().toStdString());
    }

    if (!cfgBuilder.build()) {
        auto err = cfgBuilder.getError();
        return LINGLONG_ERR("build cfg error: " + QString::fromStdString(err.reason));
    }

    auto container =
      this->containerBuilder.create(cfgBuilder,
                                    QString::fromStdString(runContext.getContainerId()));
    if (!container) {
        return LINGLONG_ERR(container);
    }

    ocppi::runtime::config::types::Process process{};
    process.cwd = "/";
    process.noNewPrivileges = true;
    process.terminal = true;

    auto currentArch = package::Architecture::currentCPUArchitecture();
    if (!currentArch) {
        return LINGLONG_ERR(currentArch);
    }
    auto ldGenerateCmd =
      std::vector<std::string>{ "/sbin/ldconfig", "-X", "-C", appCacheDest + "/ld.so.cache" };
#ifdef LINGLONG_FONT_CACHE_GENERATOR
    // Usage: font-cache-generator [cacheRoot] [id]
    const std::string fontGenerateCmd = utils::quoteBashArg(fontGenerator) + " "
      + utils::quoteBashArg(appCacheDest) + " " + utils::quoteBashArg(ref.id.toStdString());
    auto ldGenerateCmdstr;
    for (const auto &c : ldGenerateCmd) {
        ldGenerateCmdstr.append(utils::quoteBashArg(c));
        ldGenerateCmdstr.append(" ");
    }
    process.args =
      std::vector<std::string>{ "bash", "-c", ldGenerateCmdstr + ";" + fontGenerateCmd };
#endif

    process.args = std::move(ldGenerateCmd);

    // Note: XDG_RUNTIME_DIR is not set in PM, the ll-box will finally fallback to /run/ll-box.
    //       But PM has no write permission in that place, so we should specific the root path.
    //       Qt will fackback to /tmp/runtime-{USER}, we use this fallback.
    auto XDGRuntimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    auto containerStateRoot = std::filesystem::path(XDGRuntimeDir.toStdString()) / "ll-box";

    ocppi::runtime::RunOption opt;
    opt.GlobalOption::root = containerStateRoot;
    auto result = (*container)->run(process, opt);
    if (!result) {
        return LINGLONG_ERR(result);
    }

    transaction.commit();
    return LINGLONG_OK;
}

// it's safe to skip cache generation here, if the cache directory already exists.
// when application begins to run, it will regenerate the cache if necessary
utils::error::Result<void> PackageManager::tryGenerateCache(const package::Reference &ref) noexcept
{
    LINGLONG_TRACE("try to generate cache for " + ref.toString());

    auto layerItem = this->repo.getLayerItem(ref);
    if (!layerItem) {
        return LINGLONG_ERR(layerItem);
    }
    auto appCache = std::filesystem::path(LINGLONG_ROOT) / "cache" / layerItem->commit;
    std::error_code ec;
    if (std::filesystem::exists(appCache, ec)) {
        return LINGLONG_OK;
    }
    if (ec) {
        return LINGLONG_ERR(QString::fromStdString(ec.message()));
    }

    auto ret = generateCache(ref);
    if (!ret) {
        qWarning() << "failed to generate cache" << ret.error();
    }

    return LINGLONG_OK;
}

utils::error::Result<void> PackageManager::removeCache(const package::Reference &ref) noexcept
{
    LINGLONG_TRACE("remove the cache of " + ref.toString());

    auto layerItem = this->repo.getLayerItem(ref);
    if (!layerItem) {
        return LINGLONG_ERR(layerItem);
    }

    const auto appCache = std::filesystem::path(LINGLONG_ROOT) / "cache" / layerItem->commit;
    std::error_code ec;
    std::filesystem::remove_all(appCache, ec);
    if (ec) {
        return LINGLONG_ERR("failed to remove cache directory", ec);
    }

    return LINGLONG_OK;
}

auto PackageManager::GenerateCache(const QString &reference) noexcept -> QVariantMap
{
    auto refRet = package::Reference::parse(reference);
    if (!refRet) {
        return toDBusReply(refRet);
    }
    auto ref = *refRet;
    auto jobID = QUuid::createUuid().toString();
    m_generator_queue.runTask([this, jobID, ref]() {
        qInfo() << "Generate cache for:" << ref.toString();

        auto ret = this->generateCache(ref);
        if (!ret) {
            qCritical() << "failed to generate cache for:" << ref.toString();
            qCritical() << ret.error().message();
            Q_EMIT this->GenerateCacheFinished(jobID, false);
            return;
        }

        qInfo() << "Generate cache finished";
        Q_EMIT this->GenerateCacheFinished(jobID, true);
    });
    auto result = utils::serialize::toQVariantMap(api::types::v1::PackageManager1JobInfo{
      .id = jobID.toStdString(),
      .code = 0,
      .message = "",
    });
    return result;
}

utils::error::Result<void>
PackageManager::executePostInstallHooks(const package::Reference &ref) noexcept
{
    LINGLONG_TRACE("execute post install hooks for: " + ref.toString());

    std::unique_ptr<utils::InstallHookManager> installHookManager =
      std::make_unique<utils::InstallHookManager>();
    auto ret = installHookManager->parseInstallHooks();
    if (!ret) {
        return LINGLONG_ERR(ret);
    }

    auto layerDir = this->repo.getLayerDir(ref);
    if (!layerDir) {
        return LINGLONG_ERR(layerDir);
    }

    auto appPath = layerDir->absolutePath();

    ret = installHookManager->executePostInstallHooks(ref.id.toStdString(), appPath.toStdString());
    if (!ret) {
        return LINGLONG_ERR(ret);
    }

    return LINGLONG_OK;
}

utils::error::Result<void>
PackageManager::executePostUninstallHooks(const package::Reference &ref) noexcept
{
    LINGLONG_TRACE("execute post uninstall hooks for: " + ref.toString());

    std::unique_ptr<utils::InstallHookManager> installHookManager =
      std::make_unique<utils::InstallHookManager>();
    auto ret = installHookManager->parseInstallHooks();
    if (!ret) {
        return LINGLONG_ERR(ret);
    }

    ret = installHookManager->executePostUninstallHooks(ref.id.toStdString());
    if (!ret) {
        return LINGLONG_ERR(ret);
    }

    return LINGLONG_OK;
}

} // namespace linglong::service
