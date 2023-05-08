#include "precice/impl/DataContext.hpp"
#include <memory>
#include "utils/EigenHelperFunctions.hpp"

namespace precice::impl {

logging::Logger DataContext::_log{"impl::DataContext"};

DataContext::DataContext(mesh::PtrData data, mesh::PtrMesh mesh)
{
  PRECICE_ASSERT(data);
  _providedData = data;
  _mesh         = mesh;
}

std::string DataContext::getDataName() const
{
  PRECICE_ASSERT(_providedData);
  return _providedData->getName();
}

void DataContext::resetData()
{
  // See also https://github.com/precice/precice/issues/1156.
  _providedData->toZero();
  if (hasMapping()) {
    PRECICE_ASSERT(hasWriteMapping());
    PRECICE_ASSERT(!hasReadMapping());
    std::for_each(_mappingContexts.begin(), _mappingContexts.end(), [](auto &context) { context.toData->toZero(); });
  }
}

int DataContext::getDataDimensions() const
{
  PRECICE_ASSERT(_providedData);
  return _providedData->getDimensions();
}

std::string DataContext::getMeshName() const
{
  PRECICE_ASSERT(_mesh);
  return _mesh->getName();
}

MeshID DataContext::getMeshID() const
{
  PRECICE_ASSERT(_mesh);
  return _mesh->getID();
}

const mesh::Mesh &DataContext::getMesh() const
{
  PRECICE_ASSERT(_mesh);
  return *_mesh;
}

void DataContext::appendMapping(MappingContext mappingContext)
{
  PRECICE_ASSERT(mappingContext.fromData);
  PRECICE_ASSERT(mappingContext.toData);
  // Make sure we don't append a mapping twice
#ifndef NDEBUG
  for (auto &context : _mappingContexts) {
    PRECICE_ASSERT(!((context.mapping == mappingContext.mapping) && (context.fromData == mappingContext.fromData) && (context.fromData == mappingContext.toData)), "The appended mapping already exists.");
  }
#endif
  _mappingContexts.emplace_back(mappingContext);
  PRECICE_ASSERT(mappingContext.fromData == _providedData || mappingContext.toData == _providedData, "Either fromData or toData has to equal _providedData.");
  PRECICE_ASSERT(mappingContext.fromData->getName() == getDataName());
  PRECICE_ASSERT(mappingContext.toData->getName() == getDataName());
}

bool DataContext::hasMapping() const
{
  return hasReadMapping() || hasWriteMapping();
}

void DataContext::mapData()
{
  PRECICE_ASSERT(hasMapping());
  // Execute the mapping
  for (auto &context : _mappingContexts) {
    // Reset the toData before executing the mapping
    context.toData->toZero();
    const DataID fromDataID = context.fromData->getID();
    const DataID toDataID   = context.toData->getID();
    context.mapping->map(fromDataID, toDataID);
    PRECICE_DEBUG("Mapped values = {}", utils::previewRange(3, context.toData->values()));
  }
}

bool DataContext::hasReadMapping() const
{
  return std::any_of(_mappingContexts.begin(), _mappingContexts.end(), [this](auto &context) { return context.toData == _providedData; });
}

bool DataContext::hasWriteMapping() const
{
  return std::any_of(_mappingContexts.begin(), _mappingContexts.end(), [this](auto &context) { return context.fromData == _providedData; });
}

} // namespace precice::impl
