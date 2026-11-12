// SPDX-FileCopyrightText: 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "linglong/cli/error_presenter.h"

#include "linglong/utils/error/error.h"
#include "linglong/utils/gettext.h"

#include <unordered_map>
#include <vector>

namespace linglong::cli {

namespace {

using utils::error::ErrorCode;

// 表在第一次查表时才构造：gettext 的 domain 在 main 里绑定，静态初始化阶段
// 取到的会是未翻译的原文。
//
// ⚠️ 提示文本必须和原来散在 handle*Error 里的 `_()` 串逐字一致，否则 po/ 里
//    已有的翻译会全部失效（xgettext 按 msgid 匹配）。改文案要单独走翻译流程。

const std::unordered_map<ErrorCode, ErrorPresentation> &installMessages()
{
    static const std::unordered_map<ErrorCode, ErrorPresentation> table = {
        { ErrorCode::AppInstallModuleRequireAppFirst,
          { _("To install the module, one must first install the app.") } },
        { ErrorCode::AppInstallModuleAlreadyExists, { _("Module is already installed.") } },
        { ErrorCode::AppInstallModuleNotFound, { _("The module could not be found remotely.") } },
        { ErrorCode::AppInstallAlreadyInstalled, { _("Application already installed") } },
        { ErrorCode::AppInstallNotFoundFromRemote,
          { _("Application {} is not found in remote repo."), DetailSource::PackageId } },
        { ErrorCode::AppInstallModuleNoVersion,
          { _("Cannot specify a version when installing a module.") } },
        { ErrorCode::AppInstallNeedDowngrade,
          { _("The latest version has been installed. If you want to "
              "replace it, try using 'll-cli install {} --force'"),
            DetailSource::PackageId } },
        { ErrorCode::AppInstallFailed, { _("Install failed") } },
        { ErrorCode::Unknown, { _("Install failed") } },
    };
    return table;
}

const std::unordered_map<ErrorCode, ErrorPresentation> &installFromFileMessages()
{
    static const std::unordered_map<ErrorCode, ErrorPresentation> table = {
        { ErrorCode::AppInstallAlreadyInstalled, { _("Application already installed") } },
    };
    return table;
}

const std::unordered_map<ErrorCode, ErrorPresentation> &uninstallMessages()
{
    static const std::unordered_map<ErrorCode, ErrorPresentation> table = {
        { ErrorCode::AppUninstallAppIsRunning,
          { _("The application is currently running and cannot be "
              "uninstalled. Please turn off the application and try again."),
            DetailSource::None,
            true } },
        { ErrorCode::AppUninstallNotFoundFromLocal, { _("Application is not installed.") } },
        { ErrorCode::AppUninstallMultipleVersions,
          { _("Multiple versions of the package are installed. Please specify a single "
              "version to uninstall:\n{}"),
            DetailSource::ErrorMessage } },
        { ErrorCode::AppUninstallBaseOrRuntime,
          { _("Base or runtime cannot be uninstalled, please use 'll-cli prune'.") } },
        { ErrorCode::AppUninstallFailed, { _("Uninstall failed") } },
        { ErrorCode::Unknown, { _("Uninstall failed") } },
    };
    return table;
}

const std::unordered_map<ErrorCode, ErrorPresentation> &upgradeMessages()
{
    static const std::unordered_map<ErrorCode, ErrorPresentation> table = {
        { ErrorCode::AppUpgradeLocalNotFound, { _("Application is not installed.") } },
        { ErrorCode::AppUpgradeFailed, { _("Upgrade failed") } },
        { ErrorCode::Unknown, { _("Upgrade failed") } },
    };
    return table;
}

const std::unordered_map<ErrorCode, ErrorPresentation> &commonMessages()
{
    static const std::unordered_map<ErrorCode, ErrorPresentation> table = {
        { ErrorCode::NetworkError,
          { _("Network connection failed. Please:"
              "\n1. Check your internet connection"
              "\n2. Verify network proxy settings if used") } },
        { ErrorCode::LayerCompatibilityError, { _("Package not found") } },
        { ErrorCode::Canceled, { _("Operation canceled") } },
        { ErrorCode::PermissionDenied, { _("Permission denied, authentication is required") } },
    };
    return table;
}

std::optional<ErrorPresentation>
lookup(const std::unordered_map<ErrorCode, ErrorPresentation> &table, ErrorCode code)
{
    auto it = table.find(code);
    if (it == table.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace

std::optional<ErrorPresentation> operationPresentation(ErrorOperation operation, ErrorCode code)
{
    switch (operation) {
    case ErrorOperation::Install:
        return lookup(installMessages(), code);
    case ErrorOperation::InstallFromFile:
        return lookup(installFromFileMessages(), code);
    case ErrorOperation::Uninstall:
        return lookup(uninstallMessages(), code);
    case ErrorOperation::Upgrade:
        return lookup(upgradeMessages(), code);
    }

    return std::nullopt;
}

std::optional<ErrorPresentation> commonPresentation(ErrorCode code)
{
    return lookup(commonMessages(), code);
}

const std::vector<ErrorCode> &allErrorCodes()
{
    static const std::vector<ErrorCode> codes = {
        ErrorCode::Failed,
        ErrorCode::Success,
        ErrorCode::Canceled,
        ErrorCode::PermissionDenied,
        ErrorCode::Unknown,
        ErrorCode::AppNotFoundFromRemote,
        ErrorCode::AppNotFoundFromLocal,
        ErrorCode::AppInstallFailed,
        ErrorCode::AppInstallNotFoundFromRemote,
        ErrorCode::AppInstallAlreadyInstalled,
        ErrorCode::AppInstallNeedDowngrade,
        ErrorCode::AppInstallModuleNoVersion,
        ErrorCode::AppInstallModuleRequireAppFirst,
        ErrorCode::AppInstallModuleAlreadyExists,
        ErrorCode::AppInstallArchNotMatch,
        ErrorCode::AppInstallModuleNotFound,
        ErrorCode::AppInstallErofsNotFound,
        ErrorCode::AppInstallUnsupportedFileFormat,
        ErrorCode::AppUninstallFailed,
        ErrorCode::AppUninstallNotFoundFromLocal,
        ErrorCode::AppUninstallAppIsRunning,
        ErrorCode::LayerCompatibilityError,
        ErrorCode::AppUninstallMultipleVersions,
        ErrorCode::AppUninstallBaseOrRuntime,
        ErrorCode::AppUpgradeFailed,
        ErrorCode::AppUpgradeLocalNotFound,
        ErrorCode::NetworkError,
        ErrorCode::InvalidFuzzyReference,
        ErrorCode::UnknownArchitecture,
    };
    return codes;
}

} // namespace linglong::cli
