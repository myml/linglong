// This file is generated by tools/codegen.sh
// DO NOT EDIT IT.

// clang-format off

//  To parse this JSON data, first install
//
//      json.hpp  https://github.com/nlohmann/json
//
//  Then include this file, and then do
//
//     ApplicationAccessPrivileges.hpp data = nlohmann::json::parse(jsonString);

#pragma once

#include <optional>
#include <nlohmann/json.hpp>
#include "linglong/api/types/v1/helper.hpp"

#include "linglong/api/types/v1/UserDirectories.hpp"

namespace linglong {
namespace api {
namespace types {
namespace v1 {
/**
* Privileges that application access to some directories
* which in user's home directory.
* directory should be relative path of home directory,
* e.g. [ "Desktop", "Downloads/browser" ]
*/

using nlohmann::json;

/**
* Privileges that application access to some directories
* which in user's home directory.
* directory should be relative path of home directory,
* e.g. [ "Desktop", "Downloads/browser" ]
*/
struct ApplicationAccessPrivileges {
std::optional<UserDirectories> userDirectories;
};
}
}
}
}

// clang-format on