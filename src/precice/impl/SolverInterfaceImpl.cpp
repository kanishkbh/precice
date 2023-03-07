#include <Eigen/Core>
#include <Eigen/src/Core/util/Meta.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <ostream>
#include <sstream>
#include <tuple>
#include <utility>

#include "SolverInterfaceImpl.hpp"
#include "action/SharedPointer.hpp"
#include "com/Communication.hpp"
#include "com/SharedPointer.hpp"
#include "cplscheme/Constants.hpp"
#include "cplscheme/CouplingScheme.hpp"
#include "cplscheme/config/CouplingSchemeConfiguration.hpp"
#include "io/Export.hpp"
#include "io/ExportContext.hpp"
#include "io/SharedPointer.hpp"
#include "logging/LogConfiguration.hpp"
#include "logging/LogMacros.hpp"
#include "m2n/BoundM2N.hpp"
#include "m2n/M2N.hpp"
#include "m2n/SharedPointer.hpp"
#include "m2n/config/M2NConfiguration.hpp"
#include "mapping/Mapping.hpp"
#include "mapping/SharedPointer.hpp"
#include "mapping/config/MappingConfiguration.hpp"
#include "math/differences.hpp"
#include "math/geometry.hpp"
#include "mesh/Data.hpp"
#include "mesh/Edge.hpp"
#include "mesh/GlobalData.hpp"
#include "mesh/Mesh.hpp"
#include "mesh/SharedPointer.hpp"
#include "mesh/Utils.hpp"
#include "mesh/Vertex.hpp"
#include "mesh/config/MeshConfiguration.hpp"
#include "partition/Partition.hpp"
#include "partition/ProvidedPartition.hpp"
#include "partition/ReceivedPartition.hpp"
#include "partition/SharedPointer.hpp"
#include "precice/config/Configuration.hpp"
#include "precice/config/ParticipantConfiguration.hpp"
#include "precice/config/SharedPointer.hpp"
#include "precice/config/SolverInterfaceConfiguration.hpp"
#include "precice/impl/CommonErrorMessages.hpp"
#include "precice/impl/MappingContext.hpp"
#include "precice/impl/MeshContext.hpp"
#include "precice/impl/Participant.hpp"
#include "precice/impl/ReadDataContext.hpp"
#include "precice/impl/ValidationMacros.hpp"
#include "precice/impl/WatchIntegral.hpp"
#include "precice/impl/WatchPoint.hpp"
#include "precice/impl/WriteDataContext.hpp"
#include "precice/impl/versions.hpp"
#include "precice/types.hpp"
#include "utils/EigenHelperFunctions.hpp"
#include "utils/EigenIO.hpp"
#include "utils/Event.hpp"
#include "utils/EventUtils.hpp"
#include "utils/Helpers.hpp"
#include "utils/IntraComm.hpp"
#include "utils/Parallel.hpp"
#include "utils/Petsc.hpp"
#include "utils/algorithm.hpp"
#include "utils/assertion.hpp"
#include "xml/XMLTag.hpp"

using precice::utils::Event;
using precice::utils::EventRegistry;

namespace precice {

/// Enabled further inter- and intra-solver synchronisation
bool syncMode = false;

namespace impl {

SolverInterfaceImpl::SolverInterfaceImpl(
    std::string        participantName,
    const std::string &configurationFileName,
    int                solverProcessIndex,
    int                solverProcessSize,
    void *             communicator,
    bool               allowNullptr)
    : _accessorName(std::move(participantName)),
      _accessorProcessRank(solverProcessIndex),
      _accessorCommunicatorSize(solverProcessSize)
{
  if (!allowNullptr) {
    PRECICE_CHECK(communicator != nullptr,
                  "Passing \"nullptr\" as \"communicator\" to SolverInterface constructor is not allowed. Please use the SolverInterface constructor without the \"communicator\" argument, if you don't want to pass an MPI communicator.");
  }
  PRECICE_CHECK(!_accessorName.empty(),
                "This participant's name is an empty string. "
                "When constructing a preCICE interface you need to pass the name of the "
                "participant as first argument to the constructor.");
  PRECICE_CHECK(_accessorProcessRank >= 0,
                "The solver process index needs to be a non-negative number, not: {}. "
                "Please check the value given when constructing a preCICE interface.",
                _accessorProcessRank);
  PRECICE_CHECK(_accessorCommunicatorSize >= 1,
                "The solver process size needs to be a positive number, not: {}. "
                "Please check the value given when constructing a preCICE interface.",
                _accessorCommunicatorSize);
  PRECICE_CHECK(_accessorProcessRank < _accessorCommunicatorSize,
                "The solver process index, currently: {}  needs to be smaller than the solver process size, currently: {}. "
                "Please check the values given when constructing a preCICE interface.",
                _accessorProcessRank, _accessorCommunicatorSize);

// Set the global communicator to the passed communicator.
// This is a noop if preCICE is not configured with MPI.
// nullpointer signals to use MPI_COMM_WORLD
#ifndef PRECICE_NO_MPI
  if (communicator != nullptr) {
    auto commptr = static_cast<utils::Parallel::Communicator *>(communicator);
    utils::Parallel::registerUserProvidedComm(*commptr);
  }
#endif

  logging::setParticipant(_accessorName);

  configure(configurationFileName);

// This block cannot be merge with the one above as only configure calls
// utils::Parallel::initializeMPI, which is needed for getProcessRank.
#ifndef PRECICE_NO_MPI
  if (communicator != nullptr) {
    const auto currentRank = utils::Parallel::current()->rank();
    PRECICE_CHECK(_accessorProcessRank == currentRank,
                  "The solver process index given in the preCICE interface constructor({}) does not match the rank of the passed MPI communicator ({}).",
                  _accessorProcessRank, currentRank);
    const auto currentSize = utils::Parallel::current()->size();
    PRECICE_CHECK(_accessorCommunicatorSize == currentSize,
                  "The solver process size given in the preCICE interface constructor({}) does not match the size of the passed MPI communicator ({}).",
                  _accessorCommunicatorSize, currentSize);
  }
#endif
}

SolverInterfaceImpl::SolverInterfaceImpl(
    std::string        participantName,
    const std::string &configurationFileName,
    int                solverProcessIndex,
    int                solverProcessSize)
    : SolverInterfaceImpl::SolverInterfaceImpl(std::move(participantName), configurationFileName, solverProcessIndex, solverProcessSize, nullptr, true)
{
}

SolverInterfaceImpl::SolverInterfaceImpl(
    std::string        participantName,
    const std::string &configurationFileName,
    int                solverProcessIndex,
    int                solverProcessSize,
    void *             communicator)
    : SolverInterfaceImpl::SolverInterfaceImpl(std::move(participantName), configurationFileName, solverProcessIndex, solverProcessSize, communicator, false)
{
}

SolverInterfaceImpl::~SolverInterfaceImpl()
{
  if (_state != State::Finalized) {
    PRECICE_INFO("Implicitly finalizing in destructor");
    finalize();
  }
}

void SolverInterfaceImpl::configure(
    const std::string &configurationFileName)
{
  config::Configuration config;
  utils::Parallel::initializeManagedMPI(nullptr, nullptr);
  logging::setMPIRank(utils::Parallel::current()->rank());
  xml::ConfigurationContext context{
      _accessorName,
      _accessorProcessRank,
      _accessorCommunicatorSize};
  xml::configure(config.getXMLTag(), context, configurationFileName);
  if (_accessorProcessRank == 0) {
    PRECICE_INFO("This is preCICE version {}", PRECICE_VERSION);
    PRECICE_INFO("Revision info: {}", precice::preciceRevision);
    PRECICE_INFO("Build type: "
#ifndef NDEBUG
                 "Debug"
#else // NDEBUG
                 "Release"
#ifndef PRECICE_NO_DEBUG_LOG
                 " + debug log"
#else
                 " (without debug log)"
#endif
#ifndef PRECICE_NO_TRACE_LOG
                 " + trace log"
#endif
#ifndef PRECICE_NO_ASSERTIONS
                 " + assertions"
#endif
#endif // NDEBUG
    );
    PRECICE_INFO("Configuring preCICE with configuration \"{}\"", configurationFileName);
    PRECICE_INFO("I am participant \"{}\"", _accessorName);
  }
  configure(config.getSolverInterfaceConfiguration());
}

void SolverInterfaceImpl::configure(
    const config::SolverInterfaceConfiguration &config)
{
  PRECICE_TRACE();

  Event                    e("configure"); // no precice::syncMode as this is not yet configured here
  utils::ScopedEventPrefix sep("configure/");

  _meshLock.clear();

  _dimensions         = config.getDimensions();
  _allowsExperimental = config.allowsExperimental();
  _accessor           = determineAccessingParticipant(config);
  _accessor->setMeshIdManager(config.getMeshConfiguration()->extractMeshIdManager());

  PRECICE_ASSERT(_accessorCommunicatorSize == 1 || _accessor->useIntraComm(),
                 "A parallel participant needs an intra-participant communication");
  PRECICE_CHECK(not(_accessorCommunicatorSize == 1 && _accessor->useIntraComm()),
                "You cannot use an intra-participant communication with a serial participant. "
                "If you do not know exactly what an intra-participant communication is and why you want to use it "
                "you probably just want to remove the intraComm tag from the preCICE configuration.");

  utils::IntraComm::configure(_accessorProcessRank, _accessorCommunicatorSize);

  _participants = config.getParticipantConfiguration()->getParticipants();
  configureM2Ns(config.getM2NConfiguration());

  PRECICE_CHECK(_participants.size() > 1,
                "In the preCICE configuration, only one participant is defined. "
                "One participant makes no coupled simulation. "
                "Please add at least another one.");
  configurePartitions(config.getM2NConfiguration());

  cplscheme::PtrCouplingSchemeConfiguration cplSchemeConfig =
      config.getCouplingSchemeConfiguration();
  _couplingScheme = cplSchemeConfig->getCouplingScheme(_accessorName);

  // Register all MeshIds to the lock, but unlock them straight away as
  // writing is allowed after configuration.
  for (const MeshContext *meshContext : _accessor->usedMeshContexts()) {
    _meshLock.add(meshContext->mesh->getID(), false);
  }

  utils::EventRegistry::instance().initialize("precice-" + _accessorName, "", utils::Parallel::current()->comm);

  PRECICE_DEBUG("Initialize intra-participant communication");
  if (utils::IntraComm::isParallel()) {
    initializeIntraCommunication();
  }

  auto &solverInitEvent = EventRegistry::instance().getStoredEvent("solver.initialize");
  solverInitEvent.start(precice::syncMode);
}

double SolverInterfaceImpl::initialize()
{
  PRECICE_TRACE();
  PRECICE_CHECK(_state != State::Finalized, "initialize() cannot be called after finalize().")
  PRECICE_CHECK(_state != State::Initialized, "initialize() may only be called once.");
  PRECICE_ASSERT(not _couplingScheme->isInitialized());

  bool failedToInitialize = _couplingScheme->isActionRequired(cplscheme::CouplingScheme::Action::InitializeData) && not _couplingScheme->isActionFulfilled(cplscheme::CouplingScheme::Action::InitializeData);
  PRECICE_CHECK(not failedToInitialize,
                "Initial data has to be written to preCICE before calling initialize(). "
                "After defining your mesh, call requiresInitialData() to check if the participant is required to write initial data using an appropriate write...Data() function.");

  auto &solverInitEvent = EventRegistry::instance().getStoredEvent("solver.initialize");
  solverInitEvent.pause(precice::syncMode);
  Event                    e("initialize", precice::syncMode);
  utils::ScopedEventPrefix sep("initialize/");

  PRECICE_DEBUG("Preprocessing provided meshes");
  for (MeshContext *meshContext : _accessor->usedMeshContexts()) {
    if (meshContext->provideMesh) {
      auto &mesh = *(meshContext->mesh);
      Event e("preprocess." + mesh.getName(), precice::syncMode);
      meshContext->mesh->preprocess();
    }
  }

  // Setup communication

  PRECICE_INFO("Setting up primary communication to coupling partner/s");
  for (auto &m2nPair : _m2ns) {
    auto &bm2n       = m2nPair.second;
    bool  requesting = bm2n.isRequesting;
    if (bm2n.m2n->isConnected()) {
      PRECICE_DEBUG("Primary connection {} {} already connected.", (requesting ? "from" : "to"), bm2n.remoteName);
    } else {
      PRECICE_DEBUG((requesting ? "Awaiting primary connection from {}" : "Establishing primary connection to {}"), bm2n.remoteName);
      bm2n.prepareEstablishment();
      bm2n.connectPrimaryRanks();
      PRECICE_DEBUG("Established primary connection {} {}", (requesting ? "from " : "to "), bm2n.remoteName);
    }
  }

  PRECICE_INFO("Primary ranks are connected");

  compareBoundingBoxes();

  PRECICE_INFO("Setting up preliminary secondary communication to coupling partner/s");
  for (auto &m2nPair : _m2ns) {
    auto &bm2n = m2nPair.second;
    bm2n.preConnectSecondaryRanks();
  }

  computePartitions();

  PRECICE_INFO("Setting up secondary communication to coupling partner/s");
  for (auto &m2nPair : _m2ns) {
    auto &bm2n = m2nPair.second;
    bm2n.connectSecondaryRanks();
    PRECICE_DEBUG("Established secondary connection {} {}", (bm2n.isRequesting ? "from " : "to "), bm2n.remoteName);
  }
  PRECICE_INFO("Secondary ranks are connected");

  for (auto &m2nPair : _m2ns) {
    m2nPair.second.cleanupEstablishment();
  }

  PRECICE_DEBUG("Initialize watchpoints");
  for (PtrWatchPoint &watchPoint : _accessor->watchPoints()) {
    watchPoint->initialize();
  }
  for (PtrWatchIntegral &watchIntegral : _accessor->watchIntegrals()) {
    watchIntegral->initialize();
  }

  // Initialize coupling state, overwrite these values for restart
  double time       = 0.0;
  int    timeWindow = 1;

  for (auto &context : _accessor->readDataContexts()) {
    context.initializeWaveform();
  }
  for (auto &context : _accessor->globalDataContexts()) {
    context.initializeWaveform();
  }

  _meshLock.lockAll();

  if (_couplingScheme->sendsInitializedData()) {
    performDataActions({action::Action::WRITE_MAPPING_PRIOR}, 0.0);
    mapWrittenData();
    performDataActions({action::Action::WRITE_MAPPING_POST}, 0.0);
  }

  PRECICE_DEBUG("Initialize coupling schemes");
  // result of _couplingScheme->getNextTimestepMaxLength() can change when calling _couplingScheme->initialize(...) and first participant method is used for setting the time window size.
  _couplingScheme->initialize(time, timeWindow);

  if (_couplingScheme->hasDataBeenReceived()) {
    performDataActions({action::Action::READ_MAPPING_PRIOR}, 0.0);
    mapReadData();
    performDataActions({action::Action::READ_MAPPING_POST}, 0.0);
  }

  for (auto &context : _accessor->readDataContexts()) {
    context.moveToNextWindow();
  }

  for (auto &context : _accessor->globalDataContexts()) {
    context.moveToNextWindow();
  }

  _couplingScheme->receiveResultOfFirstAdvance();

  if (_couplingScheme->hasDataBeenReceived()) {
    performDataActions({action::Action::READ_MAPPING_PRIOR}, 0.0);
    mapReadData();
    performDataActions({action::Action::READ_MAPPING_POST}, 0.0);
  }

  resetWrittenData();
  PRECICE_DEBUG("Plot output");
  _accessor->exportFinal();
  solverInitEvent.start(precice::syncMode);

  _state = State::Initialized;
  PRECICE_INFO(_couplingScheme->printCouplingState());

  // determine dt at the very end of the method to get the final value, even if first participant method is used (see above).
  double dt = _couplingScheme->getNextTimestepMaxLength();
  return dt;
}

double SolverInterfaceImpl::advance(
    double computedTimestepLength)
{

  PRECICE_TRACE(computedTimestepLength);

  // Events for the solver time, stopped when we enter, restarted when we leave advance
  auto &solverEvent = EventRegistry::instance().getStoredEvent("solver.advance");
  solverEvent.stop(precice::syncMode);
  auto &solverInitEvent = EventRegistry::instance().getStoredEvent("solver.initialize");
  solverInitEvent.stop(precice::syncMode);

  Event                    e("advance", precice::syncMode);
  utils::ScopedEventPrefix sep("advance/");

  PRECICE_CHECK(_state != State::Constructed, "initialize() has to be called before advance().");
  PRECICE_CHECK(_state != State::Finalized, "advance() cannot be called after finalize().")
  PRECICE_CHECK(_state == State::Initialized, "initialize() has to be called before advance().")
  PRECICE_ASSERT(_couplingScheme->isInitialized());
  PRECICE_CHECK(isCouplingOngoing(), "advance() cannot be called when isCouplingOngoing() returns false.");
  PRECICE_CHECK(!math::equals(computedTimestepLength, 0.0), "advance() cannot be called with a timestep size of 0.");
  PRECICE_CHECK(computedTimestepLength > 0.0, "advance() cannot be called with a negative timestep size {}.", computedTimestepLength);
  _numberAdvanceCalls++;

#ifndef NDEBUG
  PRECICE_DEBUG("Synchronize timestep length");
  if (utils::IntraComm::isParallel()) {
    syncTimestep(computedTimestepLength);
  }
#endif

  // Update the coupling scheme time state. Necessary to get correct remainder.
  _couplingScheme->addComputedTime(computedTimestepLength);
  // Current time
  double time = _couplingScheme->getTime();

  if (_couplingScheme->willDataBeExchanged(0.0)) {
    performDataActions({action::Action::WRITE_MAPPING_PRIOR}, time);
    mapWrittenData();
    performDataActions({action::Action::WRITE_MAPPING_POST}, time);
  }

  advanceCouplingScheme();

  if (_couplingScheme->isTimeWindowComplete()) {
    for (auto &context : _accessor->readDataContexts()) {
      context.moveToNextWindow();
    }
  }

  if (_couplingScheme->hasDataBeenReceived()) {
    performDataActions({action::Action::READ_MAPPING_PRIOR}, time);
    mapReadData();
    performDataActions({action::Action::READ_MAPPING_POST}, time);
  }

  if (_couplingScheme->isTimeWindowComplete()) {
    performDataActions({action::Action::ON_TIME_WINDOW_COMPLETE_POST}, time);
  }

  PRECICE_INFO(_couplingScheme->printCouplingState());

  PRECICE_DEBUG("Handle exports");
  handleExports();

  resetWrittenData();

  _meshLock.lockAll();
  solverEvent.start(precice::syncMode);
  return _couplingScheme->getNextTimestepMaxLength();
}

void SolverInterfaceImpl::finalize()
{
  PRECICE_TRACE();
  PRECICE_CHECK(_state != State::Finalized, "finalize() may only be called once.");

  // Events for the solver time, finally stopped here
  auto &solverEvent = EventRegistry::instance().getStoredEvent("solver.advance");
  solverEvent.stop(precice::syncMode);

  Event                    e("finalize"); // no precice::syncMode here as MPI is already finalized at destruction of this event
  utils::ScopedEventPrefix sep("finalize/");

  if (_state == State::Initialized) {

    PRECICE_ASSERT(_couplingScheme->isInitialized());
    PRECICE_DEBUG("Finalize coupling scheme");
    _couplingScheme->finalize();

    PRECICE_DEBUG("Handle exports");
    _accessor->exportFinal();
    closeCommunicationChannels(CloseChannels::All);
  }

  // Release ownership
  _couplingScheme.reset();
  _participants.clear();
  _accessor.reset();

  // Close Connections
  PRECICE_DEBUG("Close intra-participant communication");
  if (utils::IntraComm::isParallel()) {
    utils::IntraComm::getCommunication()->closeConnection();
    utils::IntraComm::getCommunication() = nullptr;
  }
  _m2ns.clear();

  // Stop and print Event logging
  e.stop();

  // Finalize PETSc and Events first
  utils::Petsc::finalize();
  utils::EventRegistry::instance().finalize();

  // Printing requires finalization
  if (not precice::utils::IntraComm::isSecondary()) {
    utils::EventRegistry::instance().printAll();
  }

  // Finally clear events and finalize MPI
  utils::EventRegistry::instance().clear();
  utils::Parallel::finalizeManagedMPI();
  _state = State::Finalized;
}

int SolverInterfaceImpl::getDimensions() const
{
  PRECICE_TRACE(_dimensions);
  return _dimensions;
}

bool SolverInterfaceImpl::isCouplingOngoing() const
{
  PRECICE_TRACE();
  PRECICE_CHECK(_state != State::Finalized, "isCouplingOngoing() cannot be called after finalize().");
  PRECICE_CHECK(_state == State::Initialized, "initialize() has to be called before isCouplingOngoing() can be evaluated.");
  return _couplingScheme->isCouplingOngoing();
}

bool SolverInterfaceImpl::isTimeWindowComplete() const
{
  PRECICE_TRACE();
  PRECICE_CHECK(_state != State::Constructed, "initialize() has to be called before isTimeWindowComplete().");
  PRECICE_CHECK(_state != State::Finalized, "isTimeWindowComplete() cannot be called after finalize().");
  return _couplingScheme->isTimeWindowComplete();
}

bool SolverInterfaceImpl::requiresInitialData()
{
  PRECICE_TRACE();
  PRECICE_CHECK(_state == State::Constructed, "requiresInitialData() has to be called before initialize().");
  bool required = _couplingScheme->isActionRequired(cplscheme::CouplingScheme::Action::InitializeData);
  if (required) {
    _couplingScheme->markActionFulfilled(cplscheme::CouplingScheme::Action::InitializeData);
  }
  return required;
}

bool SolverInterfaceImpl::requiresWritingCheckpoint()
{
  PRECICE_TRACE();
  PRECICE_CHECK(_state == State::Initialized, "initialize() has to be called before requiresWritingCheckpoint().");
  bool required = _couplingScheme->isActionRequired(cplscheme::CouplingScheme::Action::WriteCheckpoint);
  if (required) {
    _couplingScheme->markActionFulfilled(cplscheme::CouplingScheme::Action::WriteCheckpoint);
  }
  return required;
}

bool SolverInterfaceImpl::requiresReadingCheckpoint()
{
  PRECICE_TRACE();
  PRECICE_CHECK(_state == State::Initialized, "initialize() has to be called before requiresReadingCheckpoint().");
  bool required = _couplingScheme->isActionRequired(cplscheme::CouplingScheme::Action::ReadCheckpoint);
  if (required) {
    _couplingScheme->markActionFulfilled(cplscheme::CouplingScheme::Action::ReadCheckpoint);
  }
  return required;
}

bool SolverInterfaceImpl::hasMesh(
    const std::string &meshName) const
{
  PRECICE_TRACE(meshName);
  return _accessor->hasMesh(meshName);
}

int SolverInterfaceImpl::getMeshID(
    const std::string &meshName) const
{
  PRECICE_TRACE(meshName);
  PRECICE_CHECK(_accessor->hasMesh(meshName),
                "The given mesh name \"{}\" is unknown to preCICE. "
                "Please check the mesh definitions in the configuration.",
                meshName);
  PRECICE_CHECK(_accessor->isMeshUsed(meshName),
                "The given mesh name \"{0}\" is not used by the participant \"{1}\". "
                "Please define a <provide-mesh name=\"{0}\"/> or a <receive-mesh name=\"{0}\" from=\"...\" /> node for the particpant \"{1}\".",
                meshName, _accessorName);
  return _accessor->getUsedMeshID(meshName);
}

std::set<int> SolverInterfaceImpl::getMeshIDs() const
{
  PRECICE_TRACE();
  std::set<int> ids;
  for (const impl::MeshContext *context : _accessor->usedMeshContexts()) {
    ids.insert(context->mesh->getID());
  }
  return ids;
}

bool SolverInterfaceImpl::hasData(
    const std::string &dataName, MeshID meshID) const
{
  PRECICE_TRACE(dataName, meshID);
  PRECICE_VALIDATE_MESH_ID(meshID);
  return _accessor->isDataUsed(dataName, meshID);
}

// bool SolverInterfaceImpl::hasGlobalData(
//     const std::string &dataName) const
// {
//   PRECICE_TRACE(dataName);
//   // PRECICE_VALIDATE_MESH_ID(meshID);
//   return _accessor->isGlobalDataUsed(dataName);
// }

int SolverInterfaceImpl::getDataID(
    const std::string &dataName, MeshID meshID) const
{
  PRECICE_TRACE(dataName, meshID);
  PRECICE_VALIDATE_MESH_ID(meshID);
  PRECICE_CHECK(_accessor->isDataUsed(dataName, meshID),
                "Data with name \"{0}\" is not defined on mesh \"{1}\". "
                "Please add <use-data name=\"{0}\"/> under <mesh name=\"{1}\"/>.",
                dataName, _accessor->getMeshName(meshID));
  return _accessor->getUsedDataID(dataName, meshID);
}

int SolverInterfaceImpl::getGlobalDataID(
    const std::string &dataName) const
{
  // PRECICE_TRACE(dataName);
  return _accessor->getUsedGlobalDataID(dataName);
}

bool SolverInterfaceImpl::requiresMeshConnectivityFor(int meshID) const
{
  PRECICE_VALIDATE_MESH_ID(meshID);
  MeshContext &context = _accessor->usedMeshContext(meshID);
  return context.meshRequirement == mapping::Mapping::MeshRequirement::FULL;
}

bool SolverInterfaceImpl::requiresGradientDataFor(int dataID) const
{
  PRECICE_VALIDATE_DATA_ID(dataID);
  // Read data never requires gradients
  if (!_accessor->isDataWrite(dataID))
    return false;

  WriteDataContext &context = _accessor->writeDataContext(dataID);
  return context.providedData()->hasGradient();
}

int SolverInterfaceImpl::getMeshVertexSize(
    MeshID meshID) const
{
  PRECICE_TRACE(meshID);
  PRECICE_REQUIRE_MESH_USE(meshID);
  // In case we access received mesh data: check, if the requested mesh data has already been received.
  // Otherwise, the function call doesn't make any sense
  PRECICE_CHECK((_state == State::Initialized) || _accessor->isMeshProvided(meshID), "initialize() has to be called before accessing"
                                                                                     " data of the received mesh \"{}\" on participant \"{}\".",
                _accessor->getMeshName(meshID), _accessor->getName());
  MeshContext &context = _accessor->usedMeshContext(meshID);
  PRECICE_ASSERT(context.mesh.get() != nullptr);
  return context.mesh->vertices().size();
}

/// @todo Currently not supported as we would need to re-compute the re-partition
void SolverInterfaceImpl::resetMesh(
    MeshID meshID)
{
  PRECICE_EXPERIMENTAL_API();
  PRECICE_TRACE(meshID);
  PRECICE_VALIDATE_MESH_ID(meshID);
  impl::MeshContext &context = _accessor->usedMeshContext(meshID);

  PRECICE_DEBUG("Clear mesh positions for mesh \"{}\"", context.mesh->getName());
  _meshLock.unlock(meshID);
  context.mesh->clear();
}

int SolverInterfaceImpl::setMeshVertex(
    int           meshID,
    const double *position)
{
  PRECICE_TRACE(meshID);
  PRECICE_REQUIRE_MESH_MODIFY(meshID);
  Eigen::VectorXd internalPosition{
      Eigen::Map<const Eigen::VectorXd>{position, _dimensions}};
  PRECICE_DEBUG("Position = {}", internalPosition.format(utils::eigenio::debug()));
  int           index   = -1;
  MeshContext & context = _accessor->usedMeshContext(meshID);
  mesh::PtrMesh mesh(context.mesh);
  PRECICE_DEBUG("MeshRequirement: {}", fmt::streamed(context.meshRequirement));
  index = mesh->createVertex(internalPosition).getID();
  mesh->allocateDataValues();
  return index;
}

void SolverInterfaceImpl::setMeshVertices(
    int           meshID,
    int           size,
    const double *positions,
    int *         ids)
{
  PRECICE_TRACE(meshID, size);
  PRECICE_REQUIRE_MESH_MODIFY(meshID);
  MeshContext & context = _accessor->usedMeshContext(meshID);
  mesh::PtrMesh mesh(context.mesh);
  PRECICE_DEBUG("Set positions");
  const Eigen::Map<const Eigen::MatrixXd> posMatrix{
      positions, _dimensions, static_cast<EIGEN_DEFAULT_DENSE_INDEX_TYPE>(size)};
  for (int i = 0; i < size; ++i) {
    Eigen::VectorXd current(posMatrix.col(i));
    ids[i] = mesh->createVertex(current).getID();
  }
  mesh->allocateDataValues();
}

void SolverInterfaceImpl::setMeshEdge(
    MeshID meshID,
    int    firstVertexID,
    int    secondVertexID)
{
  PRECICE_TRACE(meshID, firstVertexID, secondVertexID);
  PRECICE_REQUIRE_MESH_MODIFY(meshID);
  MeshContext &context = _accessor->usedMeshContext(meshID);
  if (context.meshRequirement == mapping::Mapping::MeshRequirement::FULL) {
    mesh::PtrMesh &mesh = context.mesh;
    using impl::errorInvalidVertexID;
    PRECICE_CHECK(mesh->isValidVertexID(firstVertexID), errorInvalidVertexID(firstVertexID));
    PRECICE_CHECK(mesh->isValidVertexID(secondVertexID), errorInvalidVertexID(secondVertexID));
    mesh::Vertex &v0 = mesh->vertices()[firstVertexID];
    mesh::Vertex &v1 = mesh->vertices()[secondVertexID];
    mesh->createEdge(v0, v1);
  }
}

void SolverInterfaceImpl::setMeshEdges(
    int        meshID,
    int        size,
    const int *vertices)
{
  PRECICE_TRACE(meshID, size);
  PRECICE_REQUIRE_MESH_MODIFY(meshID);
  MeshContext &context = _accessor->usedMeshContext(meshID);
  if (context.meshRequirement != mapping::Mapping::MeshRequirement::FULL) {
    return;
  }

  mesh::PtrMesh &mesh = context.mesh;
  {
    auto end           = std::next(vertices, size * 2);
    auto [first, last] = utils::find_first_range(vertices, end, [&mesh](VertexID vid) {
      return !mesh->isValidVertexID(vid);
    });
    PRECICE_CHECK(first == end,
                  impl::errorInvalidVertexIDRange,
                  std::distance(vertices, first),
                  std::distance(vertices, last));
  }

  for (int i = 0; i < size; ++i) {
    auto aid = vertices[2 * i];
    auto bid = vertices[2 * i + 1];
    mesh->createEdge(mesh->vertices()[aid], mesh->vertices()[bid]);
  }
}

void SolverInterfaceImpl::setMeshTriangle(
    MeshID meshID,
    int    firstVertexID,
    int    secondVertexID,
    int    thirdVertexID)
{
  PRECICE_TRACE(meshID, firstVertexID,
                secondVertexID, thirdVertexID);

  PRECICE_REQUIRE_MESH_MODIFY(meshID);
  MeshContext &context = _accessor->usedMeshContext(meshID);
  if (context.meshRequirement == mapping::Mapping::MeshRequirement::FULL) {
    mesh::PtrMesh &mesh = context.mesh;
    using impl::errorInvalidVertexID;
    PRECICE_CHECK(mesh->isValidVertexID(firstVertexID), errorInvalidVertexID(firstVertexID));
    PRECICE_CHECK(mesh->isValidVertexID(secondVertexID), errorInvalidVertexID(secondVertexID));
    PRECICE_CHECK(mesh->isValidVertexID(thirdVertexID), errorInvalidVertexID(thirdVertexID));
    PRECICE_CHECK(utils::unique_elements(utils::make_array(firstVertexID, secondVertexID, thirdVertexID)),
                  "setMeshTriangle() was called with repeated Vertex IDs ({}, {}, {}).",
                  firstVertexID, secondVertexID, thirdVertexID);
    mesh::Vertex *vertices[3];
    vertices[0] = &mesh->vertices()[firstVertexID];
    vertices[1] = &mesh->vertices()[secondVertexID];
    vertices[2] = &mesh->vertices()[thirdVertexID];
    PRECICE_CHECK(utils::unique_elements(utils::make_array(vertices[0]->getCoords(),
                                                           vertices[1]->getCoords(), vertices[2]->getCoords())),
                  "setMeshTriangle() was called with vertices located at identical coordinates (IDs: {}, {}, {}).",
                  firstVertexID, secondVertexID, thirdVertexID);
    mesh::Edge *edges[3];
    edges[0] = &mesh->createEdge(*vertices[0], *vertices[1]);
    edges[1] = &mesh->createEdge(*vertices[1], *vertices[2]);
    edges[2] = &mesh->createEdge(*vertices[2], *vertices[0]);

    mesh->createTriangle(*edges[0], *edges[1], *edges[2]);
  }
}

void SolverInterfaceImpl::setMeshTriangles(
    int        meshID,
    int        size,
    const int *vertices)
{
  PRECICE_TRACE(meshID, size);
  PRECICE_REQUIRE_MESH_MODIFY(meshID);
  MeshContext &context = _accessor->usedMeshContext(meshID);
  if (context.meshRequirement != mapping::Mapping::MeshRequirement::FULL) {
    return;
  }

  mesh::PtrMesh &mesh = context.mesh;
  {
    auto end           = std::next(vertices, size * 3);
    auto [first, last] = utils::find_first_range(vertices, end, [&mesh](VertexID vid) {
      return !mesh->isValidVertexID(vid);
    });
    PRECICE_CHECK(first == end,
                  impl::errorInvalidVertexIDRange,
                  std::distance(vertices, first),
                  std::distance(vertices, last));
  }

  for (int i = 0; i < size; ++i) {
    auto aid = vertices[3 * i];
    auto bid = vertices[3 * i + 1];
    auto cid = vertices[3 * i + 2];
    mesh->createTriangle(mesh->vertices()[aid],
                         mesh->vertices()[bid],
                         mesh->vertices()[cid]);
  }
}

void SolverInterfaceImpl::setMeshQuad(
    MeshID meshID,
    int    firstVertexID,
    int    secondVertexID,
    int    thirdVertexID,
    int    fourthVertexID)
{
  PRECICE_TRACE(meshID, firstVertexID,
                secondVertexID, thirdVertexID, fourthVertexID);
  PRECICE_CHECK(_dimensions == 3, "setMeshQuad is only possible for 3D cases."
                                  " Please set the dimension to 3 in the preCICE configuration file.");
  PRECICE_REQUIRE_MESH_MODIFY(meshID);
  MeshContext &context = _accessor->usedMeshContext(meshID);
  if (context.meshRequirement == mapping::Mapping::MeshRequirement::FULL) {
    PRECICE_ASSERT(context.mesh);
    mesh::Mesh &mesh = *(context.mesh);
    using impl::errorInvalidVertexID;
    PRECICE_CHECK(mesh.isValidVertexID(firstVertexID), errorInvalidVertexID(firstVertexID));
    PRECICE_CHECK(mesh.isValidVertexID(secondVertexID), errorInvalidVertexID(secondVertexID));
    PRECICE_CHECK(mesh.isValidVertexID(thirdVertexID), errorInvalidVertexID(thirdVertexID));
    PRECICE_CHECK(mesh.isValidVertexID(fourthVertexID), errorInvalidVertexID(fourthVertexID));

    auto vertexIDs = utils::make_array(firstVertexID, secondVertexID, thirdVertexID, fourthVertexID);
    PRECICE_CHECK(utils::unique_elements(vertexIDs), "The four vertex ID's are not unique. Please check that the vertices that form the quad are correct.");

    auto coords = mesh::coordsFor(mesh, vertexIDs);
    PRECICE_CHECK(utils::unique_elements(coords),
                  "The four vertices that form the quad are not unique. The resulting shape may be a point, line or triangle."
                  "Please check that the adapter sends the four unique vertices that form the quad, or that the mesh on the interface is composed of quads.");

    auto convexity = math::geometry::isConvexQuad(coords);
    PRECICE_CHECK(convexity.convex, "The given quad is not convex. "
                                    "Please check that the adapter send the four correct vertices or that the interface is composed of quads.");
    auto reordered = utils::reorder_array(convexity.vertexOrder, mesh::vertexPtrsFor(mesh, vertexIDs));

    // Vertices are now in the order: V0-V1-V2-V3-V0.
    // Use the shortest diagonal to split the quad into 2 triangles.
    // Vertices are now in V0-V1-V2-V3-V0 order. The new edge, e[4] is either 0-2 or 1-3
    double distance02 = (reordered[0]->getCoords() - reordered[2]->getCoords()).norm();
    double distance13 = (reordered[1]->getCoords() - reordered[3]->getCoords()).norm();

    // The new edge, e[4], is the shortest diagonal of the quad
    if (distance02 <= distance13) {
      mesh.createTriangle(*reordered[0], *reordered[2], *reordered[1]);
      mesh.createTriangle(*reordered[0], *reordered[2], *reordered[3]);
    } else {
      mesh.createTriangle(*reordered[1], *reordered[3], *reordered[0]);
      mesh.createTriangle(*reordered[1], *reordered[3], *reordered[2]);
    }
  }
}

void SolverInterfaceImpl::setMeshQuads(
    int        meshID,
    int        size,
    const int *vertices)
{
  PRECICE_TRACE(meshID, size);
  PRECICE_REQUIRE_MESH_MODIFY(meshID);
  MeshContext &context = _accessor->usedMeshContext(meshID);
  if (context.meshRequirement != mapping::Mapping::MeshRequirement::FULL) {
    return;
  }

  mesh::Mesh &mesh = *(context.mesh);
  {
    auto end           = std::next(vertices, size * 4);
    auto [first, last] = utils::find_first_range(vertices, end, [&mesh](VertexID vid) {
      return !mesh.isValidVertexID(vid);
    });
    PRECICE_CHECK(first == end,
                  impl::errorInvalidVertexIDRange,
                  std::distance(vertices, first),
                  std::distance(vertices, last));
  }

  for (int i = 0; i < size; ++i) {
    auto aid = vertices[4 * i];
    auto bid = vertices[4 * i + 1];
    auto cid = vertices[4 * i + 2];
    auto did = vertices[4 * i + 3];

    auto vertexIDs = utils::make_array(aid, bid, cid, did);
    PRECICE_CHECK(utils::unique_elements(vertexIDs), "The four vertex ID's of the quad nr {} are not unique. Please check that the vertices that form the quad are correct.", i);

    auto coords = mesh::coordsFor(mesh, vertexIDs);
    PRECICE_CHECK(utils::unique_elements(coords),
                  "The four vertices that form the quad nr {} are not unique. The resulting shape may be a point, line or triangle."
                  "Please check that the adapter sends the four unique vertices that form the quad, or that the mesh on the interface is composed of quads.",
                  i);

    auto convexity = math::geometry::isConvexQuad(coords);
    PRECICE_CHECK(convexity.convex, "The given quad nr {} is not convex. "
                                    "Please check that the adapter send the four correct vertices or that the interface is composed of quads.",
                  i);
    auto reordered = utils::reorder_array(convexity.vertexOrder, mesh::vertexPtrsFor(mesh, vertexIDs));

    // Use the shortest diagonal to split the quad into 2 triangles.
    // Vertices are now in V0-V1-V2-V3-V0 order. The new edge, e[4] is either 0-2 or 1-3
    double distance02 = (reordered[0]->getCoords() - reordered[2]->getCoords()).norm();
    double distance13 = (reordered[1]->getCoords() - reordered[3]->getCoords()).norm();

    if (distance02 <= distance13) {
      mesh.createTriangle(*reordered[0], *reordered[2], *reordered[1]);
      mesh.createTriangle(*reordered[0], *reordered[2], *reordered[3]);
    } else {
      mesh.createTriangle(*reordered[1], *reordered[3], *reordered[0]);
      mesh.createTriangle(*reordered[1], *reordered[3], *reordered[2]);
    }
  }
}

void SolverInterfaceImpl::setMeshTetrahedron(
    MeshID meshID,
    int    firstVertexID,
    int    secondVertexID,
    int    thirdVertexID,
    int    fourthVertexID)
{
  PRECICE_TRACE(meshID, firstVertexID, secondVertexID, thirdVertexID, fourthVertexID);
  PRECICE_REQUIRE_MESH_MODIFY(meshID);
  PRECICE_CHECK(_dimensions == 3, "setMeshTetrahedron is only possible for 3D cases."
                                  " Please set the dimension to 3 in the preCICE configuration file.");
  MeshContext &context = _accessor->usedMeshContext(meshID);
  if (context.meshRequirement == mapping::Mapping::MeshRequirement::FULL) {
    mesh::PtrMesh &mesh = context.mesh;
    using impl::errorInvalidVertexID;
    PRECICE_CHECK(mesh->isValidVertexID(firstVertexID), errorInvalidVertexID(firstVertexID));
    PRECICE_CHECK(mesh->isValidVertexID(secondVertexID), errorInvalidVertexID(secondVertexID));
    PRECICE_CHECK(mesh->isValidVertexID(thirdVertexID), errorInvalidVertexID(thirdVertexID));
    PRECICE_CHECK(mesh->isValidVertexID(fourthVertexID), errorInvalidVertexID(fourthVertexID));
    mesh::Vertex &A = mesh->vertices()[firstVertexID];
    mesh::Vertex &B = mesh->vertices()[secondVertexID];
    mesh::Vertex &C = mesh->vertices()[thirdVertexID];
    mesh::Vertex &D = mesh->vertices()[fourthVertexID];

    mesh->createTetrahedron(A, B, C, D);
  }
}

void SolverInterfaceImpl::setMeshTetrahedra(
    int        meshID,
    int        size,
    const int *vertices)
{
  PRECICE_TRACE(meshID, size);
  PRECICE_REQUIRE_MESH_MODIFY(meshID);
  MeshContext &context = _accessor->usedMeshContext(meshID);
  if (context.meshRequirement != mapping::Mapping::MeshRequirement::FULL) {
    return;
  }

  mesh::PtrMesh &mesh = context.mesh;
  {
    auto end           = std::next(vertices, size * 4);
    auto [first, last] = utils::find_first_range(vertices, end, [&mesh](VertexID vid) {
      return !mesh->isValidVertexID(vid);
    });
    PRECICE_CHECK(first == end,
                  impl::errorInvalidVertexIDRange,
                  std::distance(vertices, first),
                  std::distance(vertices, last));
  }

  for (int i = 0; i < size; ++i) {
    auto aid = vertices[4 * i];
    auto bid = vertices[4 * i + 1];
    auto cid = vertices[4 * i + 2];
    auto did = vertices[4 * i + 3];
    mesh->createTetrahedron(mesh->vertices()[aid],
                            mesh->vertices()[bid],
                            mesh->vertices()[cid],
                            mesh->vertices()[did]);
  }
}

void SolverInterfaceImpl::writeBlockVectorData(
    int           dataID,
    int           size,
    const int *   valueIndices,
    const double *values)
{
  PRECICE_TRACE(dataID, size);
  PRECICE_CHECK(_state != State::Finalized, "writeBlockVectorData(...) cannot be called after finalize().");
  PRECICE_REQUIRE_DATA_WRITE(dataID);
  if (size == 0)
    return;
  PRECICE_CHECK(valueIndices != nullptr, "writeBlockVectorData() was called with valueIndices == nullptr");
  PRECICE_CHECK(values != nullptr, "writeBlockVectorData() was called with values == nullptr");
  WriteDataContext &context = _accessor->writeDataContext(dataID);
  PRECICE_ASSERT(context.providedData() != nullptr);
  PRECICE_CHECK(context.getDataDimensions() == _dimensions,
                "You cannot call writeBlockVectorData on the scalar data type \"{0}\". Use writeBlockScalarData or change the data type for \"{0}\" to vector.",
                context.getDataName());
  PRECICE_VALIDATE_DATA(values, size * _dimensions);

  mesh::Data &data           = *context.providedData();
  auto &      valuesInternal = data.values();
  const auto  vertexCount    = valuesInternal.size() / context.getDataDimensions();
  for (int i = 0; i < size; i++) {
    const auto valueIndex = valueIndices[i];
    PRECICE_CHECK(0 <= valueIndex && valueIndex < vertexCount,
                  "Cannot write data \"{}\" to invalid Vertex ID ({}). Please make sure you only use the results from calls to setMeshVertex/Vertices().",
                  context.getDataName(), valueIndex);
    const int offsetInternal = valueIndex * _dimensions;
    const int offset         = i * _dimensions;
    for (int dim = 0; dim < _dimensions; dim++) {
      PRECICE_ASSERT(offset + dim < valuesInternal.size(),
                     offset + dim, valuesInternal.size());
      valuesInternal[offsetInternal + dim] = values[offset + dim];
    }
  }
}

void SolverInterfaceImpl::writeVectorData(
    int           dataID,
    int           valueIndex,
    const double *value)
{
  PRECICE_TRACE(dataID, valueIndex);
  PRECICE_CHECK(_state != State::Finalized, "writeVectorData(...) cannot be called after finalize().");
  PRECICE_REQUIRE_DATA_WRITE(dataID);
  PRECICE_DEBUG("value = {}", Eigen::Map<const Eigen::VectorXd>(value, _dimensions).format(utils::eigenio::debug()));
  WriteDataContext &context = _accessor->writeDataContext(dataID);
  PRECICE_ASSERT(context.providedData() != nullptr);
  PRECICE_CHECK(context.getDataDimensions() == _dimensions,
                "You cannot call writeVectorData on the scalar data type \"{0}\". Use writeScalarData or change the data type for \"{0}\" to vector.",
                context.getDataName());
  PRECICE_VALIDATE_DATA(value, _dimensions);

  mesh::Data &data        = *context.providedData();
  auto &      values      = data.values();
  const auto  vertexCount = values.size() / context.getDataDimensions();
  PRECICE_CHECK(0 <= valueIndex && valueIndex < vertexCount,
                "Cannot write data \"{}\" to invalid Vertex ID ({}). Please make sure you only use the results from calls to setMeshVertex/Vertices().",
                context.getDataName(), valueIndex);
  const int offset = valueIndex * _dimensions;
  for (int dim = 0; dim < _dimensions; dim++) {
    values[offset + dim] = value[dim];
  }
}

void SolverInterfaceImpl::writeGlobalVectorData(
    int dataID,
    // int           valueIndex,
    const double *value)
{
  // TODO: check which macros won't work for Global Data and create another implementation for them.
  PRECICE_TRACE(dataID);
  PRECICE_CHECK(_state != State::Finalized, "writeVectorData(...) cannot be called after finalize().");
  // PRECICE_REQUIRE_DATA_WRITE(dataID);
  // TODO: write an analog of this for global.
  PRECICE_DEBUG("value = {}", Eigen::Map<const Eigen::VectorXd>(value, _dimensions).format(utils::eigenio::debug()));
  GlobalDataContext &context = _accessor->globalDataContext(dataID);
  PRECICE_ASSERT(context.providedData() != nullptr);
  PRECICE_CHECK(context.getDataDimensions() == _dimensions,
                "You cannot call writeGlobalVectorData on the scalar data type \"{0}\". Use writeGlobalScalarData or change the data type for \"{0}\" to vector.",
                context.getDataName());
  PRECICE_VALIDATE_DATA(value, _dimensions);

  mesh::GlobalData &data           = *context.providedData();
  auto &            valuesInternal = data.values();
  const auto        vertexCount    = valuesInternal.size() / context.getDataDimensions(); // Should be 1. // TODO We can probably remove this line.
  PRECICE_CHECK(vertexCount == 1, "vertexCount = {} , should be 1", vertexCount);
  // PRECICE_CHECK(0 <= valueIndex && valueIndex < vertexCount,
  //               "Cannot write data \"{}\" to invalid Vertex ID ({}). Please make sure you only use the results from calls to setMeshVertex/Vertices().",
  //               context.getDataName(), valueIndex);
  // const int offset = valueIndex * _dimensions;
  for (int dim = 0; dim < _dimensions; dim++) {
    // values[offset + dim] = value[dim];
    valuesInternal[dim] = value[dim];
  }
}

void SolverInterfaceImpl::writeBlockScalarData(
    int           dataID,
    int           size,
    const int *   valueIndices,
    const double *values)
{
  PRECICE_TRACE(dataID, size);
  PRECICE_CHECK(_state != State::Finalized, "writeBlockScalarData(...) cannot be called after finalize().");
  PRECICE_REQUIRE_DATA_WRITE(dataID);
  if (size == 0)
    return;
  PRECICE_CHECK(valueIndices != nullptr, "writeBlockScalarData() was called with valueIndices == nullptr");
  PRECICE_CHECK(values != nullptr, "writeBlockScalarData() was called with values == nullptr");
  WriteDataContext &context = _accessor->writeDataContext(dataID);
  PRECICE_ASSERT(context.providedData() != nullptr);
  PRECICE_CHECK(context.getDataDimensions() == 1,
                "You cannot call writeBlockScalarData on the vector data type \"{}\". Use writeBlockVectorData or change the data type for \"{}\" to scalar.",
                context.getDataName(), context.getDataName());
  PRECICE_VALIDATE_DATA(values, size);

  mesh::Data &data           = *context.providedData();
  auto &      valuesInternal = data.values();
  const auto  vertexCount    = valuesInternal.size() / context.getDataDimensions();
  for (int i = 0; i < size; i++) {
    const auto valueIndex = valueIndices[i];
    PRECICE_CHECK(0 <= valueIndex && valueIndex < vertexCount,
                  "Cannot write data \"{}\" to invalid Vertex ID ({}). Please make sure you only use the results from calls to setMeshVertex/Vertices().",
                  context.getDataName(), valueIndex);
    valuesInternal[valueIndex] = values[i];
  }
}

void SolverInterfaceImpl::writeScalarData(
    int    dataID,
    int    valueIndex,
    double value)
{
  PRECICE_TRACE(dataID, valueIndex, value);
  PRECICE_CHECK(_state != State::Finalized, "writeScalarData(...) cannot be called after finalize().");
  PRECICE_REQUIRE_DATA_WRITE(dataID);
  WriteDataContext &context = _accessor->writeDataContext(dataID);
  PRECICE_ASSERT(context.providedData() != nullptr);
  PRECICE_CHECK(valueIndex >= -1,
                "Invalid value index ({}) when writing scalar data. Value index must be >= 0. "
                "Please check the value index for {}",
                valueIndex, context.getDataName());
  PRECICE_CHECK(context.getDataDimensions() == 1,
                "You cannot call writeScalarData on the vector data type \"{0}\". "
                "Use writeVectorData or change the data type for \"{0}\" to scalar.",
                context.getDataName());
  PRECICE_VALIDATE_DATA(static_cast<double *>(&value), 1);

  mesh::Data &data        = *context.providedData();
  auto &      values      = data.values();
  const auto  vertexCount = values.size() / context.getDataDimensions();
  PRECICE_CHECK(0 <= valueIndex && valueIndex < vertexCount,
                "Cannot write data \"{}\" to invalid Vertex ID ({}). "
                "Please make sure you only use the results from calls to setMeshVertex/Vertices().",
                context.getDataName(), valueIndex);
  values[valueIndex] = value;

  PRECICE_DEBUG("Written scalar value = {}", value);
}

void SolverInterfaceImpl::writeGlobalScalarData(
    int    dataID,
    double value)
{
  PRECICE_TRACE(dataID, value);
  PRECICE_CHECK(_state != State::Finalized, "writeGlobalScalarData(...) cannot be called after finalize().");
  // PRECICE_REQUIRE_DATA_WRITE(dataID); TODO write global data analog for this
  GlobalDataContext &context = _accessor->globalDataContext(dataID);
  PRECICE_ASSERT(context.providedData() != nullptr);
  // PRECICE_CHECK(valueIndex >= -1,
  //               "Invalid value index ({}) when writing scalar data. Value index must be >= 0. "
  //               "Please check the value index for {}",
  //               valueIndex, context.getDataName());
  PRECICE_CHECK(context.getDataDimensions() == 1,
                "You cannot call writeGlobalScalarData on the vector data type \"{0}\". "
                "Use writeGlobalVectorData or change the data type for \"{0}\" to scalar.",
                context.getDataName());
  PRECICE_VALIDATE_DATA(static_cast<double *>(&value), 1);

  // mesh::Data &data        = *context.providedData();
  mesh::GlobalData &data           = *context.providedData();
  auto &            valuesInternal = data.values();
  // PRECICE_CHECK(0 <= valueIndex && valueIndex < vertexCount,
  //               "Cannot write data \"{}\" to invalid Vertex ID ({}). "
  //               "Please make sure you only use the results from calls to setMeshVertex/Vertices().",
  //               context.getDataName(), valueIndex);
  valuesInternal[0] = value;

  PRECICE_DEBUG("Written scalar value = {}", value);
}

void SolverInterfaceImpl::writeScalarGradientData(
    int           dataID,
    int           valueIndex,
    const double *gradientValues)
{

  PRECICE_EXPERIMENTAL_API();

  PRECICE_TRACE(dataID, valueIndex);
  PRECICE_CHECK(_state != State::Finalized, "writeScalarGradientData(...) cannot be called after finalize().")
  PRECICE_REQUIRE_DATA_WRITE(dataID);

  if (requiresGradientDataFor(dataID)) {
    PRECICE_DEBUG("Gradient value = {}", Eigen::Map<const Eigen::VectorXd>(gradientValues, _dimensions).format(utils::eigenio::debug()));
    PRECICE_CHECK(gradientValues != nullptr, "writeScalarGradientData() was called with gradientValues == nullptr");

    WriteDataContext &context = _accessor->writeDataContext(dataID);
    PRECICE_ASSERT(context.providedData() != nullptr);
    mesh::Data &data = *context.providedData();

    // Check if data has been initialized to include gradient data
    PRECICE_CHECK(data.hasGradient(), "Data \"{}\" has no gradient values available. Please set the gradient flag to true under the data attribute in the configuration file.", data.getName())

    // Size of the gradient data input : must be spaceDimensions * dataDimensions -> here spaceDimensions (since for scalar: dataDimensions = 1)
    PRECICE_ASSERT(data.getSpatialDimensions() == _dimensions,
                   data.getSpatialDimensions(), _dimensions);

    PRECICE_VALIDATE_DATA(gradientValues, _dimensions);

    // Gets the gradientvalues matrix corresponding to the dataID
    auto &     gradientValuesInternal = data.gradientValues();
    const auto vertexCount            = gradientValuesInternal.cols() / context.getDataDimensions();

    // Check if the index and dimensions are valid
    PRECICE_CHECK(valueIndex >= -1,
                  "Invalid value index ({}) when writing gradient scalar data. Value index must be >= 0. "
                  "Please check the value index for {}",
                  valueIndex, data.getName());

    PRECICE_CHECK(0 <= valueIndex && valueIndex < vertexCount,
                  "Cannot write data \"{}\" to invalid vertex ID ({}). "
                  "Please make sure you only use the results from calls to setMeshVertex/Vertices().",
                  context.getDataName(), valueIndex);

    PRECICE_CHECK(data.getDimensions() == 1,
                  "You cannot call writeGradientScalarData on the vector data type \"{0}\". "
                  "Use writeVectorGradientData or change the data type for \"{0}\" to scalar.",
                  data.getName());

    // Values are entered derived in the spatial dimensions (#rows = #spatial dimensions)
    Eigen::Map<const Eigen::MatrixXd> gradient(gradientValues, _dimensions, 1);
    gradientValuesInternal.block(0, valueIndex, _dimensions, 1) = gradient;
  }
}

void SolverInterfaceImpl::writeBlockScalarGradientData(
    int           dataID,
    int           size,
    const int *   valueIndices,
    const double *gradientValues)
{

  PRECICE_EXPERIMENTAL_API();

  // Asserts and checks
  PRECICE_TRACE(dataID, size);
  PRECICE_CHECK(_state != State::Finalized, "writeBlockScalarGradientData(...) cannot be called after finalize().");
  PRECICE_REQUIRE_DATA_WRITE(dataID);
  if (size == 0)
    return;

  if (requiresGradientDataFor(dataID)) {

    PRECICE_CHECK(valueIndices != nullptr, "writeBlockScalarGradientData() was called with valueIndices == nullptr");
    PRECICE_CHECK(gradientValues != nullptr, "writeBlockScalarGradientData() was called with gradientValues == nullptr");

    // Get the data
    WriteDataContext &context = _accessor->writeDataContext(dataID);
    PRECICE_ASSERT(context.providedData() != nullptr);
    mesh::Data &data = *context.providedData();

    PRECICE_CHECK(data.hasGradient(), "Data \"{}\" has no gradient values available. Please set the gradient flag to true under the data attribute in the configuration file.", data.getName())

    PRECICE_CHECK(data.getDimensions() == 1,
                  "You cannot call writeBlockScalarGradientData on the vector data type \"{}\". Use writeBlockVectorGradientData or change the data type for \"{}\" to scalar.",
                  data.getName(), data.getName());

    PRECICE_ASSERT(data.getSpatialDimensions() == _dimensions,
                   data.getSpatialDimensions(), _dimensions);

    PRECICE_VALIDATE_DATA(gradientValues, size * _dimensions);

    // Get gradient data and check if initialized
    auto &     gradientValuesInternal = data.gradientValues();
    const auto vertexCount            = gradientValuesInternal.cols() / context.getDataDimensions();

    Eigen::Map<const Eigen::MatrixXd> gradients(gradientValues, _dimensions, size);

    for (auto i = 0; i < size; i++) {
      const auto valueIndex = valueIndices[i];
      PRECICE_CHECK(0 <= valueIndex && valueIndex < vertexCount,
                    "Cannot write gradient data \"{}\" to invalid Vertex ID ({}). Please make sure you only use the results from calls to setMeshVertex/Vertices().",
                    context.getDataName(), valueIndex);
      gradientValuesInternal.block(0, valueIndex, _dimensions, 1) = gradients.block(0, i, _dimensions, 1);
    }
  }
}

void SolverInterfaceImpl::writeVectorGradientData(
    int           dataID,
    int           valueIndex,
    const double *gradientValues)
{
  PRECICE_EXPERIMENTAL_API();

  PRECICE_TRACE(dataID, valueIndex);
  PRECICE_CHECK(_state != State::Finalized, "writeVectorGradientData(...) cannot be called after finalize().")
  PRECICE_REQUIRE_DATA_WRITE(dataID);

  if (requiresGradientDataFor(dataID)) {

    PRECICE_CHECK(gradientValues != nullptr, "writeVectorGradientData() was called with gradientValue == nullptr");

    WriteDataContext &context = _accessor->writeDataContext(dataID);
    PRECICE_ASSERT(context.providedData() != nullptr);
    mesh::Data &data = *context.providedData();

    // Check if Data object with ID dataID has been initialized with gradient data
    PRECICE_CHECK(data.hasGradient(), "Data \"{}\" has no gradient values available. Please set the gradient flag to true under the data attribute in the configuration file.", data.getName())

    // Check if the dimensions match
    PRECICE_CHECK(data.getDimensions() > 1,
                  "You cannot call writeVectorGradientData on the scalar data type \"{}\". Use writeScalarGradientData or change the data type for \"{}\" to vector.",
                  data.getName(), data.getName());

    PRECICE_ASSERT(data.getSpatialDimensions() == _dimensions,
                   data.getSpatialDimensions(), _dimensions);

    PRECICE_VALIDATE_DATA(gradientValues, _dimensions * _dimensions);

    auto &     gradientValuesInternal = data.gradientValues();
    const auto vertexCount            = gradientValuesInternal.cols() / data.getDimensions();

    // Check if the index is valid
    PRECICE_CHECK(0 <= valueIndex && valueIndex < vertexCount,
                  "Cannot write gradient data \"{}\" to invalid Vertex ID ({}). Please make sure you only use the results from calls to setMeshVertex/Vertices().",
                  data.getName(), valueIndex)

    const Eigen::Index                dims{_dimensions};
    Eigen::Map<const Eigen::MatrixXd> gradient(gradientValues, dims, dims);
    gradientValuesInternal.block(0, dims * valueIndex, dims, dims) = gradient;
  }
}

void SolverInterfaceImpl::writeBlockVectorGradientData(
    int           dataID,
    int           size,
    const int *   valueIndices,
    const double *gradientValues)
{

  PRECICE_EXPERIMENTAL_API();

  // Asserts and checks
  PRECICE_TRACE(dataID, size);
  PRECICE_CHECK(_state != State::Finalized, "writeBlockVectorGradientData(...) cannot be called after finalize().");
  PRECICE_REQUIRE_DATA_WRITE(dataID);
  if (size == 0)
    return;

  if (requiresGradientDataFor(dataID)) {

    PRECICE_CHECK(valueIndices != nullptr, "writeBlockVectorGradientData() was called with valueIndices == nullptr");
    PRECICE_CHECK(gradientValues != nullptr, "writeBlockVectorGradientData() was called with gradientValues == nullptr");

    // Get the data
    WriteDataContext &context = _accessor->writeDataContext(dataID);
    PRECICE_ASSERT(context.providedData() != nullptr);

    mesh::Data &data = *context.providedData();

    // Check if the Data object with ID dataID has been initialized with gradient data
    PRECICE_CHECK(data.hasGradient(), "Data \"{}\" has no gradient values available. Please set the gradient flag to true under the data attribute in the configuration file.", data.getName())

    // Check if the dimensions match
    PRECICE_CHECK(data.getDimensions() > 1,
                  "You cannot call writeBlockVectorGradientData on the scalar data type \"{}\". Use writeBlockScalarGradientData or change the data type for \"{}\" to vector.",
                  data.getName(), data.getName());

    PRECICE_ASSERT(data.getSpatialDimensions() == _dimensions,
                   data.getSpatialDimensions(), _dimensions);

    PRECICE_VALIDATE_DATA(gradientValues, size * _dimensions * _dimensions);

    // Get the gradient data and check if initialized
    auto &     gradientValuesInternal = data.gradientValues();
    const auto vertexCount            = gradientValuesInternal.cols() / data.getDimensions();

    const Eigen::Index                dims{_dimensions};
    Eigen::Map<const Eigen::MatrixXd> gradients(gradientValues, dims, dims * size);
    // gradient matrices input one after the other (read row-wise)
    for (auto i = 0; i < size; i++) {
      const auto valueIndex = valueIndices[i];
      PRECICE_CHECK(0 <= valueIndex && valueIndex < vertexCount,
                    "Cannot write gradient data \"{}\" to invalid Vertex ID ({}). Please make sure you only use the results from calls to setMeshVertex/Vertices().",
                    data.getName(), valueIndex);

      gradientValuesInternal.block(0, dims * valueIndex, dims, dims) = gradients.block(0, i * dims, dims, dims);
    }
  }
}

void SolverInterfaceImpl::readBlockVectorData(
    int        dataID,
    int        size,
    const int *valueIndices,
    double *   values) const
{
  PRECICE_TRACE(dataID, size);
  double relativeTimeWindowEndTime = _couplingScheme->getThisTimeWindowRemainder(); // samples at end of time window
  if (_accessor->readDataContext(dataID).getInterpolationOrder() != 0) {
    PRECICE_WARN("Interpolation order of read data named \"{}\" is set to \"{}\", but you are calling {} without providing a relativeReadTime. This looks like an error. You can fix this by providing a relativeReadTime to {} or by setting interpolation order to 0.",
                 _accessor->readDataContext(dataID).getDataName(), _accessor->readDataContext(dataID).getInterpolationOrder(), __func__, __func__);
  }
  readBlockVectorDataImpl(dataID, size, valueIndices, relativeTimeWindowEndTime, values);
}

void SolverInterfaceImpl::readBlockVectorData(
    int        dataID,
    int        size,
    const int *valueIndices,
    double     relativeReadTime,
    double *   values) const
{
  PRECICE_TRACE(dataID, size);
  PRECICE_EXPERIMENTAL_API();
  readBlockVectorDataImpl(dataID, size, valueIndices, relativeReadTime, values);
}

void SolverInterfaceImpl::readBlockVectorDataImpl(
    int        dataID,
    int        size,
    const int *valueIndices,
    double     relativeReadTime,
    double *   values) const
{
  PRECICE_CHECK(_state != State::Finalized, "readBlockVectorData(...) cannot be called after finalize().");
  PRECICE_CHECK(relativeReadTime <= _couplingScheme->getThisTimeWindowRemainder(), "readBlockVectorData(...) cannot sample data outside of current time window.");
  PRECICE_CHECK(relativeReadTime >= 0, "readBlockVectorData(...) cannot sample data before the current time.");
  double normalizedReadTime;
  if (_couplingScheme->hasTimeWindowSize()) {
    double timeStepStart = _couplingScheme->getTimeWindowSize() - _couplingScheme->getThisTimeWindowRemainder();
    double readTime      = timeStepStart + relativeReadTime;
    normalizedReadTime   = readTime / _couplingScheme->getTimeWindowSize(); //@todo might be moved into coupling scheme
  } else {                                                                  // if this participant defines time window size through participant-first method
    PRECICE_CHECK(relativeReadTime == _couplingScheme->getThisTimeWindowRemainder(), "Waveform relaxation is not allowed for solver that sets the time step size");
    normalizedReadTime = 1; // by default read at end of window.
  }
  PRECICE_REQUIRE_DATA_READ(dataID);
  if (size == 0)
    return;
  PRECICE_CHECK(valueIndices != nullptr, "readBlockVectorData() was called with valueIndices == nullptr");
  PRECICE_CHECK(values != nullptr, "readBlockVectorData() was called with values == nullptr");
  ReadDataContext &context = _accessor->readDataContext(dataID);
  PRECICE_CHECK(context.getDataDimensions() == _dimensions,
                "You cannot call readBlockVectorData on the scalar data type \"{0}\". "
                "Use readBlockScalarData or change the data type for \"{0}\" to vector.",
                context.getDataName());
  const auto valuesInternal = context.sampleWaveformAt(normalizedReadTime);
  const auto vertexCount    = valuesInternal.size() / context.getDataDimensions();
  for (int i = 0; i < size; i++) {
    const auto valueIndex = valueIndices[i];
    PRECICE_CHECK(0 <= valueIndex && valueIndex < vertexCount,
                  "Cannot read data \"{}\" to invalid Vertex ID ({}). "
                  "Please make sure you only use the results from calls to setMeshVertex/Vertices().",
                  context.getDataName(), valueIndex);
    int offsetInternal = valueIndex * _dimensions;
    int offset         = i * _dimensions;
    for (int dim = 0; dim < _dimensions; dim++) {
      values[offset + dim] = valuesInternal[offsetInternal + dim];
    }
  }
}

void SolverInterfaceImpl::readVectorData(
    int     dataID,
    int     valueIndex,
    double *value) const
{
  PRECICE_TRACE(dataID, valueIndex);
  double relativeTimeWindowEndTime = _couplingScheme->getThisTimeWindowRemainder(); // samples at end of time window
  if (_accessor->readDataContext(dataID).getInterpolationOrder() != 0) {
    PRECICE_WARN("Interpolation order of read data named \"{}\" is set to \"{}\", but you are calling {} without providing a relativeReadTime. This looks like an error. You can fix this by providing a relativeReadTime to {} or by setting interpolation order to 0.",
                 _accessor->readDataContext(dataID).getDataName(), _accessor->readDataContext(dataID).getInterpolationOrder(), __func__, __func__);
  }
  readVectorDataImpl(dataID, valueIndex, relativeTimeWindowEndTime, value);
}

void SolverInterfaceImpl::readVectorData(
    int     dataID,
    int     valueIndex,
    double  relativeReadTime,
    double *value) const
{
  PRECICE_TRACE(dataID, valueIndex);
  PRECICE_EXPERIMENTAL_API();
  readVectorDataImpl(dataID, valueIndex, relativeReadTime, value);
}

void SolverInterfaceImpl::readGlobalVectorData(
    int     dataID,
    double *value) const
{
  PRECICE_TRACE(dataID);
  double relativeTimeWindowEndTime = _couplingScheme->getThisTimeWindowRemainder(); // samples at end of time window
  if (_accessor->globalDataContext(dataID).getInterpolationOrder() != 0) {
    PRECICE_WARN("Interpolation order of read data named \"{}\" is set to \"{}\", but you are calling {} without providing a relativeReadTime. This looks like an error. You can fix this by providing a relativeReadTime to {} or by setting interpolation order to 0.",
                 _accessor->globalDataContext(dataID).getDataName(), _accessor->globalDataContext(dataID).getInterpolationOrder(), __func__, __func__);
  }
  readGlobalVectorDataImpl(dataID, relativeTimeWindowEndTime, value);
}

void SolverInterfaceImpl::readGlobalVectorData(
    int     dataID,
    double  relativeReadTime,
    double *value) const
{
  PRECICE_TRACE(dataID);
  PRECICE_EXPERIMENTAL_API();
  readGlobalVectorDataImpl(dataID, relativeReadTime, value);
}

void SolverInterfaceImpl::readVectorDataImpl(
    int     dataID,
    int     valueIndex,
    double  relativeReadTime,
    double *value) const
{
  PRECICE_CHECK(_state != State::Finalized, "readVectorData(...) cannot be called after finalize().");
  PRECICE_CHECK(relativeReadTime <= _couplingScheme->getThisTimeWindowRemainder(), "readVectorData(...) cannot sample data outside of current time window.");
  PRECICE_CHECK(relativeReadTime >= 0, "readVectorData(...) cannot sample data before the current time.");
  double normalizedReadTime;
  if (_couplingScheme->hasTimeWindowSize()) {
    double timeStepStart = _couplingScheme->getTimeWindowSize() - _couplingScheme->getThisTimeWindowRemainder();
    double readTime      = timeStepStart + relativeReadTime;
    normalizedReadTime   = readTime / _couplingScheme->getTimeWindowSize(); //@todo might be moved into coupling scheme
  } else {                                                                  // if this participant defines time window size through participant-first method
    PRECICE_CHECK(relativeReadTime == _couplingScheme->getThisTimeWindowRemainder(), "Waveform relaxation is not allowed for solver that sets the time step size");
    normalizedReadTime = 1; // by default read at end of window.
  }
  PRECICE_REQUIRE_DATA_READ(dataID);
  ReadDataContext &context = _accessor->readDataContext(dataID);
  PRECICE_CHECK(valueIndex >= -1,
                "Invalid value index ( {} ) when reading vector data. Value index must be >= 0. "
                "Please check the value index for {}",
                valueIndex, context.getDataName());
  PRECICE_CHECK(context.getDataDimensions() == _dimensions,
                "You cannot call readVectorData on the scalar data type \"{0}\". Use readScalarData or change the data type for \"{0}\" to vector.",
                context.getDataName());
  const auto values      = context.sampleWaveformAt(normalizedReadTime);
  const auto vertexCount = values.size() / context.getDataDimensions();
  PRECICE_CHECK(0 <= valueIndex && valueIndex < vertexCount,
                "Cannot read data \"{}\" to invalid Vertex ID ({}). "
                "Please make sure you only use the results from calls to setMeshVertex/Vertices().",
                context.getDataName(), valueIndex);
  int offset = valueIndex * _dimensions;
  for (int dim = 0; dim < _dimensions; dim++) {
    value[dim] = values[offset + dim];
  }
  PRECICE_DEBUG("read value = {}", Eigen::Map<const Eigen::VectorXd>(value, _dimensions).format(utils::eigenio::debug()));
}

void SolverInterfaceImpl::readGlobalVectorDataImpl(
    int     dataID,
    double  relativeReadTime,
    double *value) const
{
  PRECICE_CHECK(_state != State::Finalized, "readGlobalVectorData(...) cannot be called after finalize().");
  PRECICE_CHECK(relativeReadTime <= _couplingScheme->getThisTimeWindowRemainder(), "readGlobalVectorData(...) cannot sample data outside of current time window.");
  PRECICE_CHECK(relativeReadTime >= 0, "readGlobalVectorData(...) cannot sample data before the current time.");
  double normalizedReadTime;
  if (_couplingScheme->hasTimeWindowSize()) {
    double timeStepStart = _couplingScheme->getTimeWindowSize() - _couplingScheme->getThisTimeWindowRemainder();
    double readTime      = timeStepStart + relativeReadTime;
    normalizedReadTime   = readTime / _couplingScheme->getTimeWindowSize(); //@todo might be moved into coupling scheme
  } else {                                                                  // if this participant defines time window size through participant-first method
    PRECICE_CHECK(relativeReadTime == _couplingScheme->getThisTimeWindowRemainder(), "Waveform relaxation is not allowed for solver that sets the time step size");
    normalizedReadTime = 1; // by default read at end of window.
  }
  PRECICE_REQUIRE_DATA_READ(dataID);
  GlobalDataContext &context = _accessor->globalDataContext(dataID);
  // PRECICE_CHECK(valueIndex >= -1,
  //               "Invalid value index ( {} ) when reading vector data. Value index must be >= 0. "
  //               "Please check the value index for {}",
  //               valueIndex, context.getDataName());
  PRECICE_CHECK(context.getDataDimensions() == _dimensions,
                "You cannot call readGlobalVectorData on the scalar data type \"{0}\". Use readGlobalScalarData or change the data type for \"{0}\" to vector.",
                context.getDataName());
  const auto values      = context.sampleWaveformAt(normalizedReadTime);
  const auto vertexCount = values.size() / context.getDataDimensions();
  PRECICE_CHECK(vertexCount == 1, "vertexCount = {} , should be 1 for global vector data", vertexCount);
  // PRECICE_CHECK(0 <= valueIndex && valueIndex < vertexCount,
  //               "Cannot read data \"{}\" to invalid Vertex ID ({}). "
  //               "Please make sure you only use the results from calls to setMeshVertex/Vertices().",
  //               context.getDataName(), valueIndex);
  // int offset = valueIndex * _dimensions;
  for (int dim = 0; dim < _dimensions; dim++) {
    value[dim] = values[dim];
  }
  PRECICE_DEBUG("read value = {}", Eigen::Map<const Eigen::VectorXd>(value, _dimensions).format(utils::eigenio::debug()));
}

void SolverInterfaceImpl::readBlockScalarData(
    int        dataID,
    int        size,
    const int *valueIndices,
    double *   values) const
{
  PRECICE_TRACE(dataID, size);
  PRECICE_REQUIRE_DATA_READ(dataID);
  double relativeTimeWindowEndTime = _couplingScheme->getThisTimeWindowRemainder(); // samples at end of time window
  if (_accessor->readDataContext(dataID).getInterpolationOrder() != 0) {
    PRECICE_WARN("Interpolation order of read data named \"{}\" is set to \"{}\", but you are calling {} without providing a relativeReadTime. This looks like an error. You can fix this by providing a relativeReadTime to {} or by setting interpolation order to 0.",
                 _accessor->readDataContext(dataID).getDataName(), _accessor->readDataContext(dataID).getInterpolationOrder(), __func__, __func__);
  }
  readBlockScalarDataImpl(dataID, size, valueIndices, relativeTimeWindowEndTime, values);
}

void SolverInterfaceImpl::readBlockScalarData(
    int        dataID,
    int        size,
    const int *valueIndices,
    double     relativeReadTime,
    double *   values) const
{
  PRECICE_TRACE(dataID, size);
  PRECICE_EXPERIMENTAL_API();
  readBlockScalarDataImpl(dataID, size, valueIndices, relativeReadTime, values);
}

void SolverInterfaceImpl::readBlockScalarDataImpl(
    int        dataID,
    int        size,
    const int *valueIndices,
    double     relativeReadTime,
    double *   values) const
{
  PRECICE_CHECK(_state != State::Finalized, "readBlockScalarData(...) cannot be called after finalize().");
  PRECICE_CHECK(relativeReadTime <= _couplingScheme->getThisTimeWindowRemainder(), "readBlockScalarData(...) cannot sample data outside of current time window.");
  PRECICE_CHECK(relativeReadTime >= 0, "readBlockScalarData(...) cannot sample data before the current time.");
  double normalizedReadTime;
  if (_couplingScheme->hasTimeWindowSize()) {
    double timeStepStart = _couplingScheme->getTimeWindowSize() - _couplingScheme->getThisTimeWindowRemainder();
    double readTime      = timeStepStart + relativeReadTime;
    normalizedReadTime   = readTime / _couplingScheme->getTimeWindowSize(); //@todo might be moved into coupling scheme
  } else {                                                                  // if this participant defines time window size through participant-first method
    PRECICE_CHECK(relativeReadTime == _couplingScheme->getThisTimeWindowRemainder(), "Waveform relaxation is not allowed for solver that sets the time step size");
    normalizedReadTime = 1; // by default read at end of window.
  }
  PRECICE_REQUIRE_DATA_READ(dataID);
  if (size == 0)
    return;
  PRECICE_CHECK(valueIndices != nullptr, "readBlockScalarData() was called with valueIndices == nullptr");
  PRECICE_CHECK(values != nullptr, "readBlockScalarData() was called with values == nullptr");
  ReadDataContext &context = _accessor->readDataContext(dataID);
  PRECICE_CHECK(context.getDataDimensions() == 1,
                "You cannot call readBlockScalarData on the vector data type \"{0}\". "
                "Use readBlockVectorData or change the data type for \"{0}\" to scalar.",
                context.getDataName());
  const auto valuesInternal = context.sampleWaveformAt(normalizedReadTime);
  const auto vertexCount    = valuesInternal.size();

  for (int i = 0; i < size; i++) {
    const auto valueIndex = valueIndices[i];
    PRECICE_CHECK(0 <= valueIndex && valueIndex < vertexCount,
                  "Cannot read data \"{}\" to invalid Vertex ID ({}). "
                  "Please make sure you only use the results from calls to setMeshVertex/Vertices().",
                  context.getDataName(), valueIndex);
    values[i] = valuesInternal[valueIndex];
  }
}

void SolverInterfaceImpl::readScalarData(
    int     dataID,
    int     valueIndex,
    double &value) const
{
  PRECICE_TRACE(dataID, valueIndex);
  double relativeTimeWindowEndTime = _couplingScheme->getThisTimeWindowRemainder(); // samples at end of time window
  if (_accessor->readDataContext(dataID).getInterpolationOrder() != 0) {
    PRECICE_WARN("Interpolation order of read data named \"{}\" is set to \"{}\", but you are calling {} without providing a relativeReadTime. This looks like an error. You can fix this by providing a relativeReadTime to {} or by setting interpolation order to 0.",
                 _accessor->readDataContext(dataID).getDataName(), _accessor->readDataContext(dataID).getInterpolationOrder(), __func__, __func__);
  }
  readScalarDataImpl(dataID, valueIndex, relativeTimeWindowEndTime, value);
}

void SolverInterfaceImpl::readScalarData(
    int     dataID,
    int     valueIndex,
    double  relativeReadTime,
    double &value) const
{
  PRECICE_TRACE(dataID, valueIndex, value);
  PRECICE_EXPERIMENTAL_API();
  readScalarDataImpl(dataID, valueIndex, relativeReadTime, value);
}

void SolverInterfaceImpl::readGlobalScalarData(int     dataID,
                                               double &value) const
{
  PRECICE_TRACE(dataID);
  double relativeTimeWindowEndTime = _couplingScheme->getThisTimeWindowRemainder(); // samples at end of time window
  if (_accessor->globalDataContext(dataID).getInterpolationOrder() != 0) {
    PRECICE_WARN("Interpolation order of read data named \"{}\" is set to \"{}\", but you are calling {} without providing a relativeReadTime. This looks like an error. You can fix this by providing a relativeReadTime to {} or by setting interpolation order to 0.",
                 _accessor->globalDataContext(dataID).getDataName(), _accessor->globalDataContext(dataID).getInterpolationOrder(), __func__, __func__);
  }
  readGlobalScalarDataImpl(dataID, relativeTimeWindowEndTime, value);
}

void SolverInterfaceImpl::readGlobalScalarData(
    int     dataID,
    double  relativeReadTime,
    double &value) const
{
  PRECICE_TRACE(dataID, value);
  PRECICE_EXPERIMENTAL_API();
  readGlobalScalarDataImpl(dataID, relativeReadTime, value);
}

void SolverInterfaceImpl::readScalarDataImpl(
    int     dataID,
    int     valueIndex,
    double  relativeReadTime,
    double &value) const
{
  PRECICE_CHECK(_state != State::Finalized, "readScalarData(...) cannot be called after finalize().");
  PRECICE_CHECK(relativeReadTime <= _couplingScheme->getThisTimeWindowRemainder(), "readScalarData(...) cannot sample data outside of current time window.");
  PRECICE_CHECK(relativeReadTime >= 0, "readScalarData(...) cannot sample data before the current time.");
  double normalizedReadTime;
  if (_couplingScheme->hasTimeWindowSize()) {
    double timeStepStart = _couplingScheme->getTimeWindowSize() - _couplingScheme->getThisTimeWindowRemainder();
    double readTime      = timeStepStart + relativeReadTime;
    normalizedReadTime   = readTime / _couplingScheme->getTimeWindowSize(); //@todo might be moved into coupling scheme
  } else {                                                                  // if this participant defines time window size through participant-first method
    PRECICE_CHECK(relativeReadTime == _couplingScheme->getThisTimeWindowRemainder(), "Waveform relaxation is not allowed for solver that sets the time step size");
    normalizedReadTime = 1; // by default read at end of window.
  }
  PRECICE_REQUIRE_DATA_READ(dataID);
  ReadDataContext &context = _accessor->readDataContext(dataID);
  PRECICE_CHECK(valueIndex >= -1,
                "Invalid value index ( {} ) when reading scalar data. Value index must be >= 0. "
                "Please check the value index for {}",
                valueIndex, context.getDataName());
  PRECICE_CHECK(context.getDataDimensions() == 1,
                "You cannot call readScalarData on the vector data type \"{0}\". "
                "Use readVectorData or change the data type for \"{0}\" to scalar.",
                context.getDataName());

  const auto values      = context.sampleWaveformAt(normalizedReadTime);
  const auto vertexCount = values.size();
  PRECICE_CHECK(0 <= valueIndex && valueIndex < vertexCount,
                "Cannot read data \"{}\" from invalid Vertex ID ({}). "
                "Please make sure you only use the results from calls to setMeshVertex/Vertices().",
                context.getDataName(), valueIndex);
  value = values[valueIndex];
  PRECICE_DEBUG("Read value = {}", value);
}

void SolverInterfaceImpl::readGlobalScalarDataImpl(
    int     dataID,
    double  relativeReadTime,
    double &value) const
{
  PRECICE_CHECK(_state != State::Finalized, "readGlobalScalarData(...) cannot be called after finalize().");
  PRECICE_CHECK(relativeReadTime <= _couplingScheme->getThisTimeWindowRemainder(), "readGlobalScalarData(...) cannot sample data outside of current time window.");
  PRECICE_CHECK(relativeReadTime >= 0, "readGlobalScalarData(...) cannot sample data before the current time.");
  double normalizedReadTime;
  if (_couplingScheme->hasTimeWindowSize()) {
    double timeStepStart = _couplingScheme->getTimeWindowSize() - _couplingScheme->getThisTimeWindowRemainder();
    double readTime      = timeStepStart + relativeReadTime;
    normalizedReadTime   = readTime / _couplingScheme->getTimeWindowSize(); //@todo might be moved into coupling scheme
  } else {                                                                  // if this participant defines time window size through participant-first method
    PRECICE_CHECK(relativeReadTime == _couplingScheme->getThisTimeWindowRemainder(), "Waveform relaxation is not allowed for solver that sets the time step size");
    normalizedReadTime = 1; // by default read at end of window.
  }
  // PRECICE_REQUIRE_DATA_READ(dataID);
  // TODO: write an analog of this for global.
  GlobalDataContext &context = _accessor->globalDataContext(dataID);
  // PRECICE_CHECK(valueIndex >= -1,
  //               "Invalid value index ( {} ) when reading scalar data. Value index must be >= 0. "
  //               "Please check the value index for {}",
  //               valueIndex, context.getDataName());
  PRECICE_CHECK(context.getDataDimensions() == 1,
                "You cannot call readGlobalScalarData on the vector data type \"{0}\". "
                "Use readGlobalVectorData or change the data type for \"{0}\" to scalar.",
                context.getDataName());

  const auto values = context.sampleWaveformAt(normalizedReadTime);
  // const auto vertexCount = values.size();
  // PRECICE_CHECK(0 <= valueIndex && valueIndex < vertexCount,
  //               "Cannot read data \"{}\" from invalid Vertex ID ({}). "
  //               "Please make sure you only use the results from calls to setMeshVertex/Vertices().",
  //               context.getDataName(), valueIndex);
  value = values[0];
  PRECICE_DEBUG("Read value = {}", value);
}

void SolverInterfaceImpl::setMeshAccessRegion(
    const int     meshID,
    const double *boundingBox) const
{
  PRECICE_EXPERIMENTAL_API();
  PRECICE_TRACE(meshID);
  PRECICE_REQUIRE_MESH_USE(meshID);
  PRECICE_CHECK(_state != State::Finalized, "setMeshAccessRegion() cannot be called after finalize().")
  PRECICE_CHECK(_state != State::Initialized, "setMeshAccessRegion() needs to be called before initialize().");
  PRECICE_CHECK(!_accessRegionDefined, "setMeshAccessRegion may only be called once.");
  PRECICE_CHECK(boundingBox != nullptr, "setMeshAccessRegion was called with boundingBox == nullptr.");

  // Get the related mesh
  MeshContext & context = _accessor->meshContext(meshID);
  mesh::PtrMesh mesh(context.mesh);
  PRECICE_DEBUG("Define bounding box");
  // Transform bounds into a suitable format
  int                 dim = mesh->getDimensions();
  std::vector<double> bounds(dim * 2);

  for (int d = 0; d < dim; ++d) {
    // Check that min is lower or equal to max
    PRECICE_CHECK(boundingBox[2 * d] <= boundingBox[2 * d + 1], "Your bounding box is ill defined, i.e. it has a negative volume. The required format is [x_min, x_max...]");
    bounds[2 * d]     = boundingBox[2 * d];
    bounds[2 * d + 1] = boundingBox[2 * d + 1];
  }
  // Create a bounding box
  mesh::BoundingBox providedBoundingBox(bounds);
  // Expand the mesh associated bounding box
  mesh->expandBoundingBox(providedBoundingBox);
  // and set a flag so that we know the function was called
  _accessRegionDefined = true;
}

void SolverInterfaceImpl::getMeshVerticesAndIDs(
    const int meshID,
    const int size,
    int *     ids,
    double *  coordinates) const
{
  PRECICE_EXPERIMENTAL_API();
  PRECICE_TRACE(meshID, size);
  PRECICE_REQUIRE_MESH_USE(meshID);
  PRECICE_DEBUG("Get {} mesh vertices with IDs", size);

  // Check, if the requested mesh data has already been received. Otherwise, the function call doesn't make any sense
  PRECICE_CHECK((_state == State::Initialized) || _accessor->isMeshProvided(meshID), "initialize() has to be called before accessing"
                                                                                     " data of the received mesh \"{}\" on participant \"{}\".",
                _accessor->getMeshName(meshID), _accessor->getName());

  if (size == 0)
    return;

  const MeshContext & context = _accessor->meshContext(meshID);
  const mesh::PtrMesh mesh(context.mesh);

  PRECICE_CHECK(ids != nullptr, "getMeshVerticesAndIDs() was called with ids == nullptr");
  PRECICE_CHECK(coordinates != nullptr, "getMeshVerticesAndIDs() was called with coordinates == nullptr");

  const auto &vertices = mesh->vertices();
  PRECICE_CHECK(static_cast<unsigned int>(size) <= vertices.size(), "The queried size exceeds the number of available points.");

  Eigen::Map<Eigen::MatrixXd> posMatrix{
      coordinates, _dimensions, static_cast<EIGEN_DEFAULT_DENSE_INDEX_TYPE>(size)};

  for (size_t i = 0; i < static_cast<size_t>(size); i++) {
    PRECICE_ASSERT(i < vertices.size(), i, vertices.size());
    ids[i]           = vertices[i].getID();
    posMatrix.col(i) = vertices[i].getCoords();
  }
}

void SolverInterfaceImpl::configureM2Ns(
    const m2n::M2NConfiguration::SharedPointer &config)
{
  PRECICE_TRACE();
  for (const auto &m2nTuple : config->m2ns()) {
    std::string comPartner("");
    bool        isRequesting = false;
    if (std::get<1>(m2nTuple) == _accessorName) {
      comPartner   = std::get<2>(m2nTuple);
      isRequesting = true;
    } else if (std::get<2>(m2nTuple) == _accessorName) {
      comPartner = std::get<1>(m2nTuple);
    }
    if (not comPartner.empty()) {
      for (const impl::PtrParticipant &participant : _participants) {
        if (participant->getName() == comPartner) {
          PRECICE_ASSERT(not utils::contained(comPartner, _m2ns), comPartner);
          PRECICE_ASSERT(std::get<0>(m2nTuple));

          _m2ns[comPartner] = [&] {
            m2n::BoundM2N bound;
            bound.m2n          = std::get<0>(m2nTuple);
            bound.localName    = _accessorName;
            bound.remoteName   = comPartner;
            bound.isRequesting = isRequesting;
            return bound;
          }();
        }
      }
    }
  }
}

void SolverInterfaceImpl::configurePartitions(
    const m2n::M2NConfiguration::SharedPointer &m2nConfig)
{
  PRECICE_TRACE();
  for (MeshContext *context : _accessor->usedMeshContexts()) {

    if (context->provideMesh) { // Accessor provides mesh
      PRECICE_CHECK(context->receiveMeshFrom.empty(),
                    "Participant \"{}\" cannot provide and receive mesh {}!",
                    _accessorName, context->mesh->getName());

      context->partition = partition::PtrPartition(new partition::ProvidedPartition(context->mesh));

      for (auto &receiver : _participants) {
        for (auto &receiverContext : receiver->usedMeshContexts()) {
          if (receiverContext->receiveMeshFrom == _accessorName && receiverContext->mesh->getName() == context->mesh->getName()) {
            // meshRequirement has to be copied from "from" to provide", since
            // mapping are only defined at "provide"
            if (receiverContext->meshRequirement > context->meshRequirement) {
              context->meshRequirement = receiverContext->meshRequirement;
            }

            m2n::PtrM2N m2n = m2nConfig->getM2N(receiver->getName(), _accessorName);
            m2n->createDistributedCommunication(context->mesh);
            context->partition->addM2N(m2n);
          }
        }
      }

    } else { // Accessor receives mesh
      std::string receiver(_accessorName);
      std::string provider(context->receiveMeshFrom);

      PRECICE_DEBUG("Receiving mesh from {}", provider);

      context->partition = partition::PtrPartition(new partition::ReceivedPartition(context->mesh, context->geoFilter, context->safetyFactor, context->allowDirectAccess));

      m2n::PtrM2N m2n = m2nConfig->getM2N(receiver, provider);
      m2n->createDistributedCommunication(context->mesh);
      context->partition->addM2N(m2n);
      for (const MappingContext &mappingContext : context->fromMappingContexts) {
        context->partition->addFromMapping(mappingContext.mapping);
      }
      for (const MappingContext &mappingContext : context->toMappingContexts) {
        context->partition->addToMapping(mappingContext.mapping);
      }
    }
  }
}

void SolverInterfaceImpl::compareBoundingBoxes()
{
  // sort meshContexts by name, for communication in right order.
  std::sort(_accessor->usedMeshContexts().begin(), _accessor->usedMeshContexts().end(),
            [](MeshContext const *const lhs, MeshContext const *const rhs) -> bool {
              return lhs->mesh->getName() < rhs->mesh->getName();
            });

  for (MeshContext *meshContext : _accessor->usedMeshContexts()) {
    if (meshContext->provideMesh) // provided meshes need their bounding boxes already for the re-partitioning
      meshContext->mesh->computeBoundingBox();

    meshContext->clearMappings();
  }

  for (MeshContext *meshContext : _accessor->usedMeshContexts()) {
    meshContext->partition->compareBoundingBoxes();
  }
}

void SolverInterfaceImpl::computePartitions()
{
  // We need to do this in two loops: First, communicate the mesh and later compute the partition.
  // Originally, this was done in one loop. This however gave deadlock if two meshes needed to be communicated cross-wise.
  // Both loops need a different sorting

  auto &contexts = _accessor->usedMeshContexts();

  std::sort(contexts.begin(), contexts.end(),
            [](MeshContext const *const lhs, MeshContext const *const rhs) -> bool {
              return lhs->mesh->getName() < rhs->mesh->getName();
            });

  for (MeshContext *meshContext : contexts) {
    meshContext->partition->communicate();
  }

  // for two-level initialization, there is also still communication in partition::compute()
  // therefore, we cannot resort here.
  // @todo this hacky solution should be removed as part of #633
  bool resort = true;
  for (auto &m2nPair : _m2ns) {
    if (m2nPair.second.m2n->usesTwoLevelInitialization()) {
      resort = false;
      break;
    }
  }

  if (resort) {
    // pull provided meshes up front, to have them ready for the decomposition of the received meshes (for the mappings)
    std::stable_partition(contexts.begin(), contexts.end(),
                          [](MeshContext const *const meshContext) -> bool {
                            return meshContext->provideMesh;
                          });
  }

  for (MeshContext *meshContext : contexts) {
    meshContext->partition->compute();
    if (not meshContext->provideMesh) { // received mesh can only compute their bounding boxes here
      meshContext->mesh->computeBoundingBox();
    }

    // This allocates gradient values here too if available
    meshContext->mesh->allocateDataValues();
  }
}

void SolverInterfaceImpl::computeMappings(std::vector<MappingContext> &contexts, const std::string &mappingType)
{
  PRECICE_TRACE();
  using namespace mapping;
  for (impl::MappingContext &context : contexts) {
    if (not context.mapping->hasComputedMapping()) {
      PRECICE_INFO("Compute \"{}\" mapping from mesh \"{}\" to mesh \"{}\".",
                   mappingType, _accessor->meshContext(context.fromMeshID).mesh->getName(), _accessor->meshContext(context.toMeshID).mesh->getName());
      context.mapping->computeMapping();
    }
  }
}

void SolverInterfaceImpl::mapWrittenData()
{
  PRECICE_TRACE();
  computeMappings(_accessor->writeMappingContexts(), "write");
  for (auto &context : _accessor->writeDataContexts()) {
    if (context.hasMapping()) {
      PRECICE_DEBUG("Map write data \"{}\" from mesh \"{}\"", context.getDataName(), context.getMeshName());
      context.mapData();
    }
  }
}

void SolverInterfaceImpl::mapReadData()
{
  PRECICE_TRACE();
  computeMappings(_accessor->readMappingContexts(), "read");
  for (auto &context : _accessor->readDataContexts()) {
    if (context.hasMapping()) {
      PRECICE_DEBUG("Map read data \"{}\" to mesh \"{}\"", context.getDataName(), context.getMeshName());
      context.mapData();
    }
    context.storeDataInWaveform();
  }
}

void SolverInterfaceImpl::performDataActions(
    const std::set<action::Action::Timing> &timings,
    double                                  time)
{
  PRECICE_TRACE();
  for (action::PtrAction &action : _accessor->actions()) {
    if (timings.find(action->getTiming()) != timings.end()) {
      action->performAction(time);
    }
  }
}

void SolverInterfaceImpl::handleExports()
{
  PRECICE_TRACE();
  Participant::IntermediateExport exp;
  exp.timewindow = _couplingScheme->getTimeWindows() - 1;
  exp.iteration  = _numberAdvanceCalls;
  exp.complete   = _couplingScheme->isTimeWindowComplete();
  exp.time       = _couplingScheme->getTime();
  _accessor->exportIntermediate(exp);
}

void SolverInterfaceImpl::resetWrittenData()
{
  PRECICE_TRACE();
  for (auto &context : _accessor->writeDataContexts()) {
    context.resetData();
  }
}

PtrParticipant SolverInterfaceImpl::determineAccessingParticipant(
    const config::SolverInterfaceConfiguration &config)
{
  const auto &partConfig = config.getParticipantConfiguration();
  for (const PtrParticipant &participant : partConfig->getParticipants()) {
    if (participant->getName() == _accessorName) {
      return participant;
    }
  }
  PRECICE_ERROR("This participant's name, which was specified in the constructor of the preCICE interface as \"{}\", "
                "is not defined in the preCICE configuration. "
                "Please double-check the correct spelling.",
                _accessorName);
}

void SolverInterfaceImpl::initializeIntraCommunication()
{
  PRECICE_TRACE();

  Event e("com.initializeIntraCom", precice::syncMode);
  utils::IntraComm::getCommunication()->connectIntraComm(
      _accessorName, "IntraComm",
      _accessorProcessRank, _accessorCommunicatorSize);
}

void SolverInterfaceImpl::syncTimestep(double computedTimestepLength)
{
  PRECICE_ASSERT(utils::IntraComm::isParallel());
  if (utils::IntraComm::isSecondary()) {
    utils::IntraComm::getCommunication()->send(computedTimestepLength, 0);
  } else {
    PRECICE_ASSERT(utils::IntraComm::isPrimary());
    for (Rank secondaryRank : utils::IntraComm::allSecondaryRanks()) {
      double dt;
      utils::IntraComm::getCommunication()->receive(dt, secondaryRank);
      PRECICE_CHECK(math::equals(dt, computedTimestepLength),
                    "Found ambiguous values for the timestep length passed to preCICE in \"advance\". On rank {}, the value is {}, while on rank 0, the value is {}.",
                    secondaryRank, dt, computedTimestepLength);
    }
  }
}

void SolverInterfaceImpl::advanceCouplingScheme()
{
  PRECICE_DEBUG("Advance coupling scheme");
  // Orchestrate local and remote mesh changes
  std::vector<MeshID> localChanges;

  [[maybe_unused]] auto remoteChanges1 = _couplingScheme->firstSynchronization(localChanges);
  _couplingScheme->firstExchange();
  // Orchestrate remote mesh changes (local ones were handled in the first sync)
  [[maybe_unused]] auto remoteChanges2 = _couplingScheme->secondSynchronization();
  _couplingScheme->secondExchange();
}

void SolverInterfaceImpl::closeCommunicationChannels(CloseChannels close)
{
  // Apply some final ping-pong to sync solver that run e.g. with a uni-directional coupling only
  // afterwards close connections
  PRECICE_INFO("Synchronize participants and close {}communication channels",
               (close == CloseChannels::Distributed ? "distributed " : ""));
  std::string ping = "ping";
  std::string pong = "pong";
  for (auto &iter : _m2ns) {
    auto bm2n = iter.second;
    if (not utils::IntraComm::isSecondary()) {
      PRECICE_DEBUG("Synchronizing primary rank with {}", bm2n.remoteName);
      if (bm2n.isRequesting) {
        bm2n.m2n->getPrimaryRankCommunication()->send(ping, 0);
        std::string receive = "init";
        bm2n.m2n->getPrimaryRankCommunication()->receive(receive, 0);
        PRECICE_ASSERT(receive == pong);
      } else {
        std::string receive = "init";
        bm2n.m2n->getPrimaryRankCommunication()->receive(receive, 0);
        PRECICE_ASSERT(receive == ping);
        bm2n.m2n->getPrimaryRankCommunication()->send(pong, 0);
      }
    }
    if (close == CloseChannels::Distributed) {
      PRECICE_DEBUG("Closing distributed communication with {}", bm2n.remoteName);
      bm2n.m2n->closeDistributedConnections();
    } else {
      PRECICE_DEBUG("Closing communication with {}", bm2n.remoteName);
      bm2n.m2n->closeConnection();
    }
  }
}

const mesh::Mesh &SolverInterfaceImpl::mesh(const std::string &meshName) const
{
  PRECICE_TRACE(meshName);
  return *_accessor->usedMeshContext(meshName).mesh;
}

} // namespace impl
} // namespace precice
