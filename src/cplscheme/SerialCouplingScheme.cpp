#include "SerialCouplingScheme.hpp"
#include <cmath>
#include <memory>
#include <ostream>
#include <utility>

#include <vector>
#include "acceleration/Acceleration.hpp"
#include "acceleration/SharedPointer.hpp"
#include "cplscheme/BaseCouplingScheme.hpp"
#include "cplscheme/BiCouplingScheme.hpp"
#include "cplscheme/CouplingScheme.hpp"
#include "logging/LogMacros.hpp"
#include "m2n/M2N.hpp"
#include "math/differences.hpp"
#include "utils/assertion.hpp"

namespace precice::cplscheme {

SerialCouplingScheme::SerialCouplingScheme(
    double                        maxTime,
    int                           maxTimeWindows,
    double                        timeWindowSize,
    int                           validDigits,
    const std::string &           firstParticipant,
    const std::string &           secondParticipant,
    const std::string &           localParticipant,
    m2n::PtrM2N                   m2n,
    constants::TimesteppingMethod dtMethod,
    CouplingMode                  cplMode,
    int                           maxIterations,
    int                           extrapolationOrder)
    : BiCouplingScheme(maxTime, maxTimeWindows, timeWindowSize, validDigits, firstParticipant,
                       secondParticipant, localParticipant, std::move(m2n), maxIterations, cplMode, dtMethod, extrapolationOrder)
{
  if (dtMethod == constants::FIRST_PARTICIPANT_SETS_TIME_WINDOW_SIZE) {
    if (doesFirstStep()) {
      PRECICE_ASSERT(not _participantReceivesTimeWindowSize);
      setTimeWindowSize(UNDEFINED_TIME_WINDOW_SIZE);
      _participantSetsTimeWindowSize = true; // not allowed to call setTimeWindowSize anymore.
      PRECICE_ASSERT(not hasTimeWindowSize());
    } else {
      _participantReceivesTimeWindowSize = true;
      PRECICE_ASSERT(not _participantSetsTimeWindowSize);
    }
  }
}

void SerialCouplingScheme::setTimeWindowSize(double timeWindowSize)
{
  PRECICE_ASSERT(not _participantSetsTimeWindowSize);
  BaseCouplingScheme::setTimeWindowSize(timeWindowSize);
}

void SerialCouplingScheme::sendTimeWindowSize()
{
  PRECICE_TRACE();
  if (_participantSetsTimeWindowSize) {
    PRECICE_DEBUG("sending time window size of {}", getComputedTimeWindowPart());
    getM2N()->send(getComputedTimeWindowPart());
  }
}

void SerialCouplingScheme::receiveAndSetTimeWindowSize()
{
  PRECICE_TRACE();
  if (_participantReceivesTimeWindowSize) {
    double dt = UNDEFINED_TIME_WINDOW_SIZE;
    getM2N()->receive(dt);
    PRECICE_DEBUG("Received time window size of {}.", dt);
    PRECICE_ASSERT(not _participantSetsTimeWindowSize);
    PRECICE_ASSERT(not math::equals(dt, UNDEFINED_TIME_WINDOW_SIZE));
    PRECICE_ASSERT(not doesFirstStep(), "Only second participant can receive time window size.");
    setTimeWindowSize(dt);
  }
}

void SerialCouplingScheme::performReceiveOfFirstAdvance()
{
  if (doesFirstStep()) {
    // do nothing
  } else { // second participant
    receiveAndSetTimeWindowSize();
    PRECICE_DEBUG("Receiving data...");
    receiveData(getM2N(), getReceiveData());
    checkDataHasBeenReceived();
  }
}

bool SerialCouplingScheme::exchangeDataAndAccelerate()
{
  bool convergence = true;

  if (doesFirstStep()) { // first participant
    PRECICE_DEBUG("Sending data...");
    sendTimeWindowSize();
    sendData(getM2N(), getSendData());
    sendGlobalData(getM2N(), getSendGlobalData());
    PRECICE_DEBUG("Receiving data...");
    if (isImplicitCouplingScheme()) {
      convergence = receiveConvergence(getM2N());
    }
    receiveData(getM2N(), getReceiveData());
    receiveGlobalData(getM2N(), getReceiveGlobalData());
    checkDataHasBeenReceived();
  } else { // second participant
    if (isImplicitCouplingScheme()) {
      // TODO: global data stuff here?
      PRECICE_DEBUG("Test Convergence and accelerate...");
      convergence = doImplicitStep();
      sendConvergence(getM2N(), convergence);
    }
    PRECICE_DEBUG("Sending data...");
    sendData(getM2N(), getSendData());
    sendGlobalData(getM2N(), getSendGlobalData());
    // the second participant does not want new data in the last iteration of the last time window
    if (isCouplingOngoing() || (isImplicitCouplingScheme() && not convergence)) {
      receiveAndSetTimeWindowSize();
      PRECICE_DEBUG("Receiving data...");
      receiveData(getM2N(), getReceiveData());
      receiveGlobalData(getM2N(), getReceiveGlobalData());
      checkDataHasBeenReceived();
    }
  }

  return convergence;
}

} // namespace precice::cplscheme
