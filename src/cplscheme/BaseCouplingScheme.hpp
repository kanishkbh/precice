#pragma once

#include <Eigen/Core>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "Constants.hpp"
#include "CouplingData.hpp"
#include "CouplingScheme.hpp"
#include "SharedPointer.hpp"
#include "acceleration/SharedPointer.hpp"
#include "impl/ConvergenceMeasure.hpp"
#include "impl/SharedPointer.hpp"
#include "io/TXTTableWriter.hpp"
#include "logging/Logger.hpp"
#include "m2n/M2N.hpp"
#include "m2n/SharedPointer.hpp"
#include "mesh/SharedPointer.hpp"
#include "utils/assertion.hpp"

namespace precice {
namespace io {
class TXTTableWriter;
} // namespace io

namespace cplscheme {
class CouplingData;

/**
 * @brief Abstract base class for standard coupling schemes.
 *
 * ! General description
 * A coupling scheme computes the actions to be done by the coupled participants
 * (solvers) in time. It provides interface functions to setup, advance and
 * shutdown the coupling scheme and interface functions to query the state of
 * the coupling scheme and required actions of the participants.
 *
 * ! Usage
 * -# create an object of a concrete coupling scheme class
 *    (ExplicitCouplingScheme, e.g.)
 * -# add all meshes holding data to the coupling scheme by addMesh()
 * -# configure the object by adding subclass specific information
 * -# start the coupling scheme with initialize(), where the name of the local
 *    participant, i.e. the participant using the coupling scheme object, is
 *    needed
 * -# retrieve necessary information about sent/received data and the state of
 *    the coupled simulation
 * -# query actions and mark them as fulfilled
 * -# compute data to be sent (possibly taking into account received data from
 *    initialize())
 * -# advance the coupling scheme with advance(); where the maximum timestep
 *    length (= time window size) needs to be obeyed
 * -# ....
 * -# when the method isCouplingOngoing() returns false, call finalize() to
 *    stop the coupling scheme
 */
class BaseCouplingScheme : public CouplingScheme {
public:
  enum CouplingMode { Explicit,
                      Implicit,
                      Undefined };

  BaseCouplingScheme(
      double                        maxTime,
      int                           maxTimeWindows,
      double                        timeWindowSize,
      int                           validDigits,
      std::string                   localParticipant,
      int                           maxIterations,
      CouplingMode                  cplMode,
      constants::TimesteppingMethod dtMethod,
      int                           extrapolationOrder);

  /**
   * @brief Getter for _sendsInitializedData
   * @returns _sendsInitializedData
   */
  bool sendsInitializedData() const override final;

  /**
   * @brief getter for _isInitialized
   * @returns true, if initialize has been called.
   */
  bool isInitialized() const override final
  {
    return _isInitialized;
  }

  /**
   * @brief Adds newly computed time. Has to be called before every advance.
   * @param timeToAdd time to be added
   */
  void addComputedTime(double timeToAdd) override final;

  /**
   * @brief Returns true, if data will be exchanged when calling advance().
   *
   * Also returns true after the last call of advance() at the end of the
   * simulation.
   *
   * @param lastSolverTimestepLength [IN] The length of the last timestep
   *        computed by the solver calling willDataBeExchanged().
   */
  bool willDataBeExchanged(double lastSolverTimestepLength) const override final;

  /**
   * @brief getter for _hasDataBeenReceived
   * @returns true, if data has been received in last call of advance().
   */
  bool hasDataBeenReceived() const override final;

  /**
   * @brief getter for _time
   * @returns the currently computed time of the coupling scheme.
   */
  double getTime() const override final;

  /**
   * @brief getter for _timeWindows
   * @returns the number of currently computed time windows of the coupling scheme.
   */
  int getTimeWindows() const override final;

  /**
   * @brief Function to check whether time window size is defined by coupling scheme.
   *
   * There are two reasons why a scheme might have a time window size:
   * 1) a fixed time window size is given in the scheme
   * 2) the participant received the time window size from another participant in the scheme
   *
   * @returns true, if time window size is available.
   */
  bool hasTimeWindowSize() const override final;

  /**
   * @brief Returns the time window size, if one is given by the coupling scheme.
   *
   * An assertion is thrown, if no valid time window size is given. Check with
   * hasTimeWindowSize().
   */
  double getTimeWindowSize() const override final;

  /**
   * @brief Returns the remaining timestep length within the current time window.
   *
   * If no time window size is prescribed by the coupling scheme, always 0.0 is
   * returned.
   */
  double getThisTimeWindowRemainder() const override final;

  /**
   * @brief Returns the maximal length of the next timestep to be computed.
   *
   * If no time window size is prescribed by the coupling scheme, always the
   * maximal double accuracy floating point number value is returned.
   */
  double getNextTimestepMaxLength() const override final; // @todo mainly used in tests. Is this function actually needed or can we drop it and only use getThisTimeWindowRemainder()?

  /// Returns true, when the coupled simulation is still ongoing.
  bool isCouplingOngoing() const override final;

  /// Returns true, when the accessor can advance to the next time window.
  bool isTimeWindowComplete() const override final;

  /// Returns true, if the given action has to be performed by the accessor.
  bool isActionRequired(const std::string &actionName) const override final;

  /// Tells the coupling scheme that the accessor has performed the given action.
  void markActionFulfilled(const std::string &actionName) override final;

  /// Sets an action required to be performed by the accessor.
  void requireAction(const std::string &actionName) override final;

  /**
   * @brief Returns coupling state information.
   *
   * Includes current iteration, max iterations, time, time window and action.
   */
  std::string printCouplingState() const override;

  /// Finalizes the coupling scheme.
  void finalize() override final;

  /**
   * @brief Initializes the coupling scheme.
   *
   * @param[in] startTime starting time of coupling scheme
   * @param[in] startTimeWindow starting counter of time window, from which coupling scheme starts
   */
  void initialize(double startTime, int startTimeWindow) override final;

  /// Receives result of first advance, if this has to happen inside SolverInterface::initialize(), see CouplingScheme.hpp
  void receiveResultOfFirstAdvance() override final;

  /**
   * @brief Advances the coupling scheme.
   */
  void advance() override final;

  /// Adds a measure to determine the convergence of coupling iterations.
  void addConvergenceMeasure(
      int                         dataID,
      bool                        suffices,
      bool                        strict,
      impl::PtrConvergenceMeasure measure,
      bool                        doesLogging);

  /// Set an acceleration technique.
  void setAcceleration(const acceleration::PtrAcceleration &acceleration);

  /**
   * @brief Getter for _doesFirstStep
   * @returns _doesFirstStep
   */
  bool doesFirstStep() const
  {
    return _doesFirstStep;
  }

  /**
   * @returns true, if coupling scheme has any sendData
   */
  virtual bool hasAnySendData() = 0;

  /**
   * @brief Determines which data is initialized and therefore has to be exchanged during initialize.
   *
   * Calls determineInitialSend and determineInitialReceive for all send and receive data of this coupling scheme.
   */
  virtual void determineInitialDataExchange() = 0;

protected:
  /// Map that links DataID to CouplingData
  typedef std::map<int, PtrCouplingData> DataMap;

  /// Sends data sendDataIDs given in mapCouplingData with communication.
  void sendData(const m2n::PtrM2N &m2n, const DataMap &sendData);

  /// Receives data receiveDataIDs given in mapCouplingData with communication.
  void receiveData(const m2n::PtrM2N &m2n, const DataMap &receiveData);

  /// Map that links DataID to GlobalCouplingData
  typedef std::map<int, PtrGlobalCouplingData> GlobalDataMap;

  /// Sends global data sendDataIDs given in mapCouplingData with communication.
  void sendGlobalData(const m2n::PtrM2N &m2n, const GlobalDataMap &sendGlobalData);

  /// Receives global data receiveDataIDs given in mapCouplingData with communication.
  void receiveGlobalData(const m2n::PtrM2N &m2n, const GlobalDataMap &receiveGlobalData);

  /**
   * @brief interface to provide all CouplingData, depending on coupling scheme being used
   * @return DataMap containing all CouplingData
   */
  virtual const DataMap getAllData() = 0;

  /**
   * @brief Function to determine whether coupling scheme is an explicit coupling scheme
   * @returns true, if coupling scheme is explicit
   */
  bool isExplicitCouplingScheme()
  {
    PRECICE_ASSERT(_couplingMode != Undefined);
    return _couplingMode == Explicit;
  }

  /**
   * @brief Function to determine whether coupling scheme is an implicit coupling scheme
   * @returns true, if coupling scheme is implicit
   */
  bool isImplicitCouplingScheme()
  {
    PRECICE_ASSERT(_couplingMode != Undefined);
    return _couplingMode == Implicit;
  }

  /**
   * @brief Setter for _timeWindowSize
   * @param timeWindowSize
   */
  void setTimeWindowSize(double timeWindowSize);

  /**
   * @brief Getter for _computedTimeWindowPart
   * @returns _computedTimeWindowPart
   */
  double getComputedTimeWindowPart()
  {
    return _computedTimeWindowPart;
  }

  /**
   * @brief Setter for _doesFirstStep
   */
  void setDoesFirstStep(bool doesFirstStep)
  {
    _doesFirstStep = doesFirstStep;
  }

  /**
   * @brief Used to set flag after data has been received using receiveData().
   */
  void checkDataHasBeenReceived();

  /**
   * @brief Getter for _receivesInitializedData
   * @returns _receivesInitializedData
   */
  bool receivesInitializedData() const
  {
    return _receivesInitializedData;
  }

  /**
   * @brief Setter for _timeWindows
   *
   * Sets the computed time windows of the coupling scheme.
   * Used for testing to allow to advance in time without a coupling partner.
   *
   * @param timeWindows number of time windows
   */
  void setTimeWindows(int timeWindows)
  {
    _timeWindows = timeWindows;
  }

  /**
   * @brief Reserves memory to store data values from previous iterations and time windows in coupling data and acceleration, and initializes with zero.
   */
  void initializeStorages();

  /**
   * @brief sends convergence to other participant via m2n
   * @param m2n used for sending
   * @param convergence bool that is being sent
   */
  void sendConvergence(const m2n::PtrM2N &m2n, bool convergence);

  /**
   * @brief receives convergence from other participant via m2n
   * @param m2n used for receiving
   * @returns convergence bool
   */
  bool receiveConvergence(const m2n::PtrM2N &m2n);

  /**
   * @brief perform a coupling iteration
   * @returns whether this iteration has converged or not
   *
   * This function is called from the child classes
   */
  bool doImplicitStep();

  /**
   * @brief stores current data in buffer for extrapolation
   */
  void storeExtrapolationData();

  /**
   * @brief finalizes this window's data and initializes data for next window.
   */
  void moveToNextWindow();

  /**
   * @brief used for storing all Data at end of doImplicitStep for later reference.
   */
  void storeIteration()
  {
    PRECICE_ASSERT(isImplicitCouplingScheme());
    for (const DataMap::value_type &pair : getAllData()) {
      pair.second->storeIteration();
    }
  }

  /**
   * @brief Sets _sendsInitializedData, if sendData requires initialization
   * @param sendData CouplingData being checked
   */
  void determineInitialSend(DataMap &sendData);

  /**
   * @brief Sets _receivesInitializedData, if receiveData requires initialization
   * @param receiveData CouplingData being checked
   */
  void determineInitialReceive(DataMap &receiveData);

  /**
   * @brief getter for _extrapolationOrder
   */
  int getExtrapolationOrder();

private:
  /// Coupling mode used by coupling scheme.
  CouplingMode _couplingMode = Undefined;

  mutable logging::Logger _log{"cplscheme::BaseCouplingScheme"};

  /// Maximum time being computed. End of simulation is reached, if _time == _maxTime
  double _maxTime;

  /// current time; _time <= _maxTime
  double _time = 0;

  /// Number of time windows that have to be computed. End of simulation is reached, if _timeWindows == _maxTimeWindows
  int _maxTimeWindows;

  /// number of completed time windows; _timeWindows <= _maxTimeWindows
  int _timeWindows = 0;

  /// size of time window; _timeWindowSize <= _maxTime
  double _timeWindowSize;

  /// Part of the window that is already computed; _computedTimeWindowPart <= _timeWindowSize
  double _computedTimeWindowPart = 0;

  /// Limit of iterations during one time window. Continue to next time window, if _iterations == _maxIterations.
  int _maxIterations = -1;

  /// Number of iterations in current time window. _iterations <= _maxIterations
  int _iterations = -1;

  /// Number of total iterations performed.
  int _totalIterations = -1;

  /// True, if local participant is the one starting the explicit scheme.
  bool _doesFirstStep = false;

  /// True, if _computedTimeWindowPart == _timeWindowSize and (coupling has converged or _iterations == _maxIterations)
  bool _isTimeWindowComplete = false;

  /// Acceleration method to speedup iteration convergence.
  acceleration::PtrAcceleration _acceleration;

  /// True, if this participant has to send initialized data.
  bool _sendsInitializedData = false;

  /// True, if this participant has to receive initialized data.
  bool _receivesInitializedData = false;

  /// True, if data has been received from other participant. Flag is used to make sure that coupling scheme is implemented and used correctly.
  bool _hasDataBeenReceived = false;

  /// True, if coupling has been initialized.
  bool _isInitialized = false;

  std::set<std::string> _actions;

  /// Responsible for monitoring iteration count over time window.
  std::shared_ptr<io::TXTTableWriter> _iterationsWriter;

  /// Writes out coupling convergence within all time windows.
  std::shared_ptr<io::TXTTableWriter> _convergenceWriter;

  /// Local participant name.
  std::string _localParticipant = "unknown";

  /**
   * Order of predictor of interface values for first participant.
   *
   * The first participant in the implicit coupling scheme has to take some
   * initial guess for the interface values computed by the second participant.
   * In order to improve this initial guess, an extrapolation from previous
   * time windows can be performed.
   *
   * The standard predictor is of order zero, i.e., simply the converged values
   * of the last time windows are taken as initial guess for the coupling iterations.
   * Currently, an order 1 predictor (linear extrapolation) and order 2 predictor
   * (see https://doi.org/10.1016/j.compstruc.2008.11.013, p.796, Algorithm line 1 )
   * is implement besides that.
   */
  const int _extrapolationOrder;

  /// Smallest number, taking validDigits into account: eps = std::pow(10.0, -1 * validDigits)
  const double _eps;

  /**
   * @brief Holds meta information to perform a convergence measurement.
   * @param data Associated data field
   * @param couplingData Coupling data history
   * @param suffices Whether this measure already suffices for convergence
   * @param strict Whether non-convergence of this measure leads to a premature end of the simulation
   * @param measure Link to the actual convergence measure
   * @param doesLogging Whether this measure is logged in the convergence file
   */
  struct ConvergenceMeasureContext {
    PtrCouplingData             couplingData;
    bool                        suffices;
    bool                        strict;
    impl::PtrConvergenceMeasure measure;
    bool                        doesLogging;

    std::string logHeader() const
    {
      return "Res" + measure->getAbbreviation() + "(" + couplingData->getDataName() + ")";
    }
  };

  /**
   * @brief All convergence measures of coupling iterations.
   *
   * Before initialization, only dataID and measure variables are filled. Then,
   * the data is fetched from send and receive data assigned to the cpl scheme.
   */
  std::vector<ConvergenceMeasureContext> _convergenceMeasures;

  /// Functions needed for initialize()

  /**
   * @brief implements functionality for initialize in base class.
   */
  virtual void exchangeInitialData() = 0;

  /**
   * @brief implements functionality for receiveResultOfFirstAdvance
   */
  virtual void performReceiveOfFirstAdvance()
  {
    // noop by default. Will be overridden by child-coupling-schemes, if data has to be received here. See SerialCouplingScheme.
    return;
  }

  /// Functions needed for advance()

  /**
   * @brief implements functionality for advance in base class.
   * @returns true, if iteration converged
   */
  virtual bool exchangeDataAndAccelerate() = 0;

  /**
   * @brief interface to provide accelerated data, depending on coupling scheme being used
   * @return data being accelerated
   */
  virtual const DataMap getAccelerationData() = 0;

  /**
   * @brief If any required actions are open, an error message is issued.
   */
  void checkCompletenessRequiredActions();

  /**
   * @brief Function to check whether end of time window is reached. Does not check for convergence
   * @returns true if end time of time window is reached.
   */
  bool reachedEndOfTimeWindow();

  /**
   * @brief Initialize txt writers for iterations and convergence tracking
   */
  void initializeTXTWriters();

  /**
   * @brief Advance txt writers for iterations and convergence tracking
   */
  void advanceTXTWriters();

  /**
   * @brief Prints the coupling state
   *
   * @param timeWindows current number of completed time windows
   * @param time current time
   */
  std::string printBasicState(
      int    timeWindows,
      double time) const;

  /**
   * @brief Prints the action state
   * @returns a string representing the required actions.
   */
  std::string printActionsState() const;

  /**
   * @brief Measure whether coupling scheme has converged or not
   * @return Whether coupling scheme has converged
   */
  bool measureConvergence();

  /**
   * @brief Reset all convergence measurements after convergence
   */
  void newConvergenceMeasurements();

  /**
   * @brief Checks whether any CouplingData in dataMap requires initialization
   * @param dataMap map containing CouplingData
   * @return true, if any CouplingData in dataMap requires initialization
   */
  bool anyDataRequiresInitialization(DataMap &dataMap) const;
};
} // namespace cplscheme
} // namespace precice
