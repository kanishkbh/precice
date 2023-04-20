#pragma once

#include <string>
#include <vector>
#include "logging/Logger.hpp"
#include "mesh/Data.hpp"
#include "utils/ManageUniqueIDs.hpp"
#include "xml/XMLTag.hpp"

namespace precice {
namespace mesh {

/// Performs and provides configuration for Data objects from XML files.
class DataConfiguration : public xml::XMLTag::Listener {
public:
  struct ConfiguredData {
    std::string name;
    int         dimensions;
    bool        isGlobal; // false = mesh data, true = meshless/global data

    ConfiguredData(
        const std::string &name,
        int                dimensions,
        bool               isGlobal)
        : name(name), dimensions(dimensions), isGlobal(isGlobal) {}
  };

  DataConfiguration(xml::XMLTag &parent);

  void setDimensions(int dimensions);

  const std::vector<ConfiguredData> &data() const;

  ConfiguredData getRecentlyConfiguredData() const;

  virtual void xmlTagCallback(
      const xml::ConfigurationContext &context,
      xml::XMLTag &                    callingTag);

  virtual void xmlEndTagCallback(
      const xml::ConfigurationContext &context,
      xml::XMLTag &                    callingTag);

  /**
   * @brief Adds data manually.
   *
   * @param[in] name Unique name of the data.
   * @param[in] dataDimensions Dimensionality (1: scalar, 2,3: vector) of data.
   */
  void addData(const std::string &name, int dataDimensions);

private:
  mutable logging::Logger _log{"mesh::DataConfiguration"};

  const std::string TAG_MESH_DATA   = "data";
  const std::string TAG_GLOBAL_DATA = "global-data";
  const std::string ATTR_NAME       = "name";
  const std::string VALUE_VECTOR    = "vector";
  const std::string VALUE_SCALAR    = "scalar";

  /// Dimension of space.
  int _dimensions = 0;

  std::vector<ConfiguredData> _data;

  int _indexLastConfigured = -1;

  int getDataDimensions(const std::string &typeName) const;
};

} // namespace mesh
} // namespace precice
