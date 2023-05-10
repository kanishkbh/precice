#include <algorithm>
#include <map>
#include <memory>
#include <ostream>
#include <type_traits>
#include <utility>

#include "BiCouplingScheme.hpp"
#include "cplscheme/BaseCouplingScheme.hpp"
#include "cplscheme/CouplingData.hpp"
#include "cplscheme/GlobalCouplingData.hpp"
#include "cplscheme/SharedPointer.hpp"
#include "logging/LogMacros.hpp"
#include "m2n/M2N.hpp"
#include "m2n/SharedPointer.hpp"
#include "mesh/Data.hpp"
#include "precice/types.hpp"
#include "utils/Helpers.hpp"

namespace precice::cplscheme {

BiCouplingScheme::BiCouplingScheme(
    double                        maxTime,
    int                           maxTimeWindows,
    double                        timeWindowSize,
    int                           validDigits,
    std::string                   firstParticipant,
    std::string                   secondParticipant,
    const std::string &           localParticipant,
    m2n::PtrM2N                   m2n,
    int                           maxIterations,
    CouplingMode                  cplMode,
    constants::TimesteppingMethod dtMethod,
    int                           extrapolationOrder)
    : BaseCouplingScheme(maxTime, maxTimeWindows, timeWindowSize, validDigits, localParticipant, maxIterations, cplMode, dtMethod, extrapolationOrder),
      _m2n(std::move(m2n)),
      _firstParticipant(std::move(firstParticipant)),
      _secondParticipant(std::move(secondParticipant))
{
  PRECICE_ASSERT(_firstParticipant != _secondParticipant,
                 "First participant and second participant must have different names.");
  if (localParticipant == _firstParticipant) {
    setDoesFirstStep(true);
  } else if (localParticipant == _secondParticipant) {
    setDoesFirstStep(false);
  } else {
    PRECICE_ERROR("Name of local participant \"{}\" does not match any participant specified for the coupling scheme.",
                  localParticipant);
  }
}

void BiCouplingScheme::addDataToSend(
    const mesh::PtrData &data,
    mesh::PtrMesh        mesh,
    bool                 requiresInitialization)
{
  PRECICE_TRACE();
  PtrCouplingData ptrCplData = addCouplingData(data, std::move(mesh), requiresInitialization);

  if (!utils::contained(data->getID(), _sendData)) {
    PRECICE_ASSERT(_sendData.count(data->getID()) == 0, "Key already exists!");
    _sendData.emplace(data->getID(), ptrCplData);
  } else {
    PRECICE_ERROR("Data \"{0}\" cannot be added twice for sending. Please remove any duplicate <exchange data=\"{0}\" .../> tags", data->getName());
  }
}

void BiCouplingScheme::addGlobalDataToSend(
    const mesh::PtrData &data,
    bool                 requiresInitialization)
{
  PRECICE_TRACE();
  PtrGlobalCouplingData ptrGblCplData = addGlobalCouplingData(data, requiresInitialization);
  precice::DataID       id            = data->getID();
  if (!utils::contained(id, _sendGlobalData)) {
    PRECICE_ASSERT(_sendGlobalData.count(id) == 0, "Key already exists!");
    _sendGlobalData.emplace(id, ptrGblCplData);
    PRECICE_DEBUG("Added \"{}\" to _sendGlobalData. Now _sendGlobalData.size is {}.", data->getName(), _sendGlobalData.size());
  } else {
    PRECICE_ERROR("Global Data \"{0}\" cannot be added twice for sending. Please remove any duplicate <exchange data=\"{0}\" .../> tags", data->getName());
  }
}

void BiCouplingScheme::addDataToReceive(
    const mesh::PtrData &data,
    mesh::PtrMesh        mesh,
    bool                 requiresInitialization)
{
  PRECICE_TRACE();
  PtrCouplingData ptrCplData = addCouplingData(data, std::move(mesh), requiresInitialization);

  if (!utils::contained(data->getID(), _receiveData)) {
    PRECICE_ASSERT(_receiveData.count(data->getID()) == 0, "Key already exists!");
    _receiveData.emplace(data->getID(), ptrCplData);
  } else {
    PRECICE_ERROR("Data \"{0}\" cannot be added twice for receiving. Please remove any duplicate <exchange data=\"{0}\" ... /> tags", data->getName());
  }
}

void BiCouplingScheme::addGlobalDataToReceive(
    const mesh::PtrData &data,
    bool                 requiresInitialization)
{
  PRECICE_TRACE();
  PtrGlobalCouplingData PtrGblCplData = addGlobalCouplingData(data, requiresInitialization);
  precice::DataID       id            = data->getID();
  if (!utils::contained(id, _receiveGlobalData)) {
    PRECICE_ASSERT(_receiveGlobalData.count(id) == 0, "Key already exists!");
    // if (isExplicitCouplingScheme()) {
    _receiveGlobalData.emplace(id, std::make_shared<GlobalCouplingData>(data, requiresInitialization));
    PRECICE_DEBUG("Added \"{}\" to _receiveGlobalData.", data->getName());
  } else {
    PRECICE_ERROR("Global Data \"{0}\" cannot be added twice for receiving. Please remove any duplicate <exchange data=\"{0}\" ... /> tags", data->getName());
  }
}

void BiCouplingScheme::determineInitialDataExchange()
{
  determineInitialSend(getSendData());
  determineInitialSend(getSendGlobalData());
  determineInitialReceive(getReceiveData());
  determineInitialReceive(getReceiveGlobalData());
}

std::vector<std::string> BiCouplingScheme::getCouplingPartners() const
{
  std::vector<std::string> partnerNames;
  // Add non-local participant
  if (doesFirstStep()) {
    partnerNames.push_back(_secondParticipant);
  } else {
    partnerNames.push_back(_firstParticipant);
  }
  return partnerNames;
}

DataMap &BiCouplingScheme::getSendData()
{
  return _sendData;
}

DataMap &BiCouplingScheme::getReceiveData()
{
  return _receiveData;
}

GlobalDataMap &BiCouplingScheme::getSendGlobalData()
{
  return _sendGlobalData;
}

GlobalDataMap &BiCouplingScheme::getReceiveGlobalData()
{
  return _receiveGlobalData;
}

CouplingData *BiCouplingScheme::getSendData(
    DataID dataID)
{
  PRECICE_TRACE(dataID);
  DataMap::iterator iter = _sendData.find(dataID);
  if (iter != _sendData.end()) {
    return &(*(iter->second));
  }
  return nullptr;
}

CouplingData *BiCouplingScheme::getReceiveData(
    DataID dataID)
{
  PRECICE_TRACE(dataID);
  DataMap::iterator iter = _receiveData.find(dataID);
  if (iter != _receiveData.end()) {
    return &(*(iter->second));
  }
  return nullptr;
}

GlobalCouplingData *BiCouplingScheme::getSendGlobalData(
    DataID dataID)
{
  PRECICE_TRACE(dataID);
  GlobalDataMap::iterator iter = _sendGlobalData.find(dataID);
  if (iter != _sendGlobalData.end()) {
    return &(*(iter->second));
  }
  return nullptr;
}

GlobalCouplingData *BiCouplingScheme::getReceiveGlobalData(
    DataID dataID)
{
  PRECICE_TRACE(dataID);
  GlobalDataMap::iterator iter = _receiveGlobalData.find(dataID);
  if (iter != _receiveGlobalData.end()) {
    return &(*(iter->second));
  }
  return nullptr;
}

m2n::PtrM2N BiCouplingScheme::getM2N() const
{
  PRECICE_ASSERT(_m2n);
  return _m2n;
}

void BiCouplingScheme::exchangeInitialData()
{
  // F: send, receive, S: receive, send
  if (doesFirstStep()) {
    if (sendsInitializedData()) {
      sendData(getM2N(), getSendData());
      sendGlobalData(getM2N(), getSendGlobalData());
    }
    if (receivesInitializedData()) {
      receiveData(getM2N(), getReceiveData());
      receiveGlobalData(getM2N(), getReceiveGlobalData());
      checkDataHasBeenReceived();
    }
  } else { // second participant
    if (receivesInitializedData()) {
      receiveData(getM2N(), getReceiveData());
      receiveGlobalData(getM2N(), getReceiveGlobalData());
      checkDataHasBeenReceived();
    }
    if (sendsInitializedData()) {
      sendData(getM2N(), getSendData());
      sendGlobalData(getM2N(), getSendGlobalData());
    }
  }
}

bool BiCouplingScheme::hasAnySendData()
{
  return not getSendData().empty();
}

bool BiCouplingScheme::hasAnySendGlobalData()
{
  return not getSendGlobalData().empty();
}

bool BiCouplingScheme::hasSendData(DataID dataID)
{
  return getSendData(dataID) != nullptr;
}

bool BiCouplingScheme::hasSendGlobalData(DataID dataID)
{
  return getSendGlobalData(dataID) != nullptr;
}

} // namespace precice::cplscheme
