#include "DataConfiguration.hpp"
#include <ostream>
#include "logging/LogMacros.hpp"
#include "utils/assertion.hpp"
#include "xml/ConfigParser.hpp"
#include "xml/XMLAttribute.hpp"

namespace precice::mesh {

DataConfiguration::DataConfiguration(xml::XMLTag &parent)
{
  using namespace xml;

  auto attrName = XMLAttribute<std::string>(ATTR_NAME)
                      .setDocumentation("Unique name for the data set.");

  XMLTag tagScalar(*this, VALUE_SCALAR, XMLTag::OCCUR_ARBITRARY, TAG_MESH_DATA);
  tagScalar.setDocumentation("Defines a scalar data set to be assigned to meshes.");
  tagScalar.addAttribute(attrName);
  parent.addSubtag(tagScalar);

  XMLTag tagVector(*this, VALUE_VECTOR, XMLTag::OCCUR_ARBITRARY, TAG_MESH_DATA);
  tagVector.setDocumentation("Defines a vector data set to be assigned to meshes. The number of "
                             "components of each data entry depends on the spatial dimensions set "
                             "in tag <solver-interface>.");
  tagVector.addAttribute(attrName);
  parent.addSubtag(tagVector);

  XMLTag tagGlobalScalar(*this, VALUE_SCALAR, XMLTag::OCCUR_ARBITRARY, TAG_GLOBAL_DATA);
  tagGlobalScalar.setDocumentation("Defines (global) scalar data not associated to any mesh."
                                   "Typically, it is space-invariant data, e.g., density for incompressible flow.");
  tagGlobalScalar.addAttribute(attrName);
  parent.addSubtag(tagGlobalScalar);

  XMLTag tagGlobalVector(*this, VALUE_VECTOR, XMLTag::OCCUR_ARBITRARY, TAG_GLOBAL_DATA);
  tagGlobalVector.setDocumentation("Defines a (global) vector data not associated to any mesh."
                                   "Typically it is space-invariant data, e.g., angles between coordinate systems."
                                   "The number of components of each data entry depends on the spatial dimensions "
                                   "set in tag <solver-interface>.");
  tagGlobalVector.addAttribute(attrName);
  parent.addSubtag(tagGlobalVector);
}

void DataConfiguration::setDimensions(
    int dimensions)
{
  PRECICE_TRACE(dimensions);
  PRECICE_ASSERT((dimensions == 2) || (dimensions == 3), dimensions);
  _dimensions = dimensions;
}

const std::vector<DataConfiguration::ConfiguredData> &
DataConfiguration::data() const
{
  return _data;
}

bool DataConfiguration::hasGlobalDataName(const std::string &dataName) const
{
  auto iter = std::find_if(_globalData.begin(), _globalData.end(), [&dataName](const auto &dptr) {
    return dptr->getName() == dataName;
  });
  return iter != _globalData.end(); // if name was not found in mesh, iter == _data.end()
}

const PtrData &DataConfiguration::globalData(const std::string &dataName) const
{
  auto iter = std::find_if(_globalData.begin(), _globalData.end(), [&dataName](const auto &dptr) {
    return dptr->getName() == dataName;
  });
  PRECICE_ASSERT(iter != _globalData.end(), "Global Data not found in Data Configuration", dataName);
  return *iter;
}

DataConfiguration::ConfiguredData DataConfiguration::getRecentlyConfiguredData() const
{
  PRECICE_ASSERT(_data.size() > 0);
  PRECICE_ASSERT(_indexLastConfigured >= 0);
  PRECICE_ASSERT(_indexLastConfigured < (int) _data.size());
  return _data[_indexLastConfigured];
}

void DataConfiguration::xmlTagCallback(
    const xml::ConfigurationContext &context,
    xml::XMLTag &                    tag)
{
  if (tag.getNamespace() == TAG_MESH_DATA) {
    const bool isGlobal = false;
    PRECICE_ASSERT(_dimensions != 0);
    const std::string &name           = tag.getStringAttributeValue(ATTR_NAME);
    const std::string &typeName       = tag.getName();
    int                dataDimensions = getDataDimensions(typeName);
    addData(name, dataDimensions, isGlobal);
  } else if (tag.getNamespace() == TAG_GLOBAL_DATA) {

    const bool isGlobal = true;
    PRECICE_ASSERT(_dimensions != 0);
    const std::string &name           = tag.getStringAttributeValue(ATTR_NAME);
    const std::string &typeName       = tag.getName();
    int                dataDimensions = getDataDimensions(typeName);
    if (!_experimental) {
      PRECICE_ERROR("You tried to configure \"{}\" as global data, which is currently still experimental. Please set experimental=\"true\", if you want to use this feature.", name);
    }
    PRECICE_WARN("You configured \"{}\" as global data, which is currently still experimental. Use with care.", name);
    addData(name, dataDimensions, isGlobal);
    createGlobalData(name, dataDimensions, _dataIDManager.getFreeID());
  } else {
    PRECICE_ASSERT(false, "Received callback from an unknown tag.", tag.getName());
  }
}

void DataConfiguration::xmlEndTagCallback(
    const xml::ConfigurationContext &context,
    xml::XMLTag &                    tag)
{
}

void DataConfiguration::addData(
    const std::string &name,
    int                dataDimensions,
    bool               isGlobal)
{
  // Check if data with same name has been added already
  for (auto &elem : _data) {
    PRECICE_CHECK(elem.name != name,
                  "Data \"{0}\" has already been defined. Please rename or remove one of the data tags with name=\"{0}\".",
                  name);
  }

  _data.emplace_back(name, dataDimensions, isGlobal);
}

void DataConfiguration::createGlobalData(const std::string &name,
                                         int                dimension,
                                         DataID             id)
{
  PRECICE_TRACE(name, dimension, id);
  for (const PtrData &globalData : _globalData) {
    PRECICE_CHECK(globalData->getName() != name,
                  "Global data \"{}\" cannot be created twice."
                  "Please rename or remove one of the global-data tags with name \"{}\".",
                  name, name);
  }
  PtrData globalData(new Data(name, id, dimension, _dimensions));
  _globalData.push_back(globalData);
}

int DataConfiguration::getDataDimensions(
    const std::string &typeName) const
{
  if (typeName == VALUE_VECTOR) {
    return _dimensions;
  } else if (typeName == VALUE_SCALAR) {
    return 1;
  }
  // We should never reach this point
  PRECICE_UNREACHABLE("Unknown data type \"{}\".", typeName);
}

void DataConfiguration::setExperimental(
    bool experimental)
{
  _experimental = experimental;
}

} // namespace precice::mesh
