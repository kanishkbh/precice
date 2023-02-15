#pragma once

#include <memory>

namespace precice {
namespace cplscheme {

class CouplingScheme;
class CouplingSchemeConfiguration;
class CouplingData;
class GlobalCouplingData;

using PtrCouplingScheme              = std::shared_ptr<CouplingScheme>;
using PtrCouplingSchemeConfiguration = std::shared_ptr<CouplingSchemeConfiguration>;
using PtrCouplingData                = std::shared_ptr<CouplingData>;
using PtrGlobalCouplingData          = std::shared_ptr<GlobalCouplingData>;

} // namespace cplscheme
} // namespace precice
