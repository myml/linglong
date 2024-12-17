// SPDX-FileCopyrightText: 2024 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#pragma once

#include "linglong/oci-cfg-generators/generator.h"

namespace linglong::generator {

struct Initialize : public Generator
{
    [[nodiscard]] std::string_view name() const override { return "05-initialize"; }

    bool generate(ocppi::runtime::config::types::Config &config) const noexcept override;
};

} // namespace linglong::generator