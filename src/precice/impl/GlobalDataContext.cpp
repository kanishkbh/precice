#include "precice/impl/GlobalDataContext.hpp"
#include <memory>
#include "utils/EigenHelperFunctions.hpp"

namespace precice::impl {

logging::Logger GlobalDataContext::_log{"impl::GlobalDataContext"};

GlobalDataContext::GlobalDataContext(
    mesh::PtrGlobalData data,
    int                 interpolationOrder)
{
  PRECICE_ASSERT(data);
  _providedData = data;
  _waveform     = std::make_shared<time::Waveform>(interpolationOrder);
}

std::string GlobalDataContext::getDataName() const
{
  PRECICE_ASSERT(_providedData);
  return _providedData->getName();
}

DataID GlobalDataContext::getDataDimensions() const
{
  PRECICE_ASSERT(_providedData);
  return _providedData->getDimensions();
}

void GlobalDataContext::resetData()
{
  // See also https://github.com/precice/precice/issues/1156.
  _providedData->toZero();
}

mesh::PtrGlobalData GlobalDataContext::providedData()
{
  PRECICE_ASSERT(_providedData);
  return _providedData;
}

int GlobalDataContext::getInterpolationOrder() const
{
  return _waveform->getInterpolationOrder();
}

Eigen::VectorXd GlobalDataContext::sampleWaveformAt(double normalizedDt)
{
  return _waveform->sample(normalizedDt);
}

} // namespace precice::impl
