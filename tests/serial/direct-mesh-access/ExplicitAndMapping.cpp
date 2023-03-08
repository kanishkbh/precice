#ifndef PRECICE_NO_MPI

#include "testing/Testing.hpp"

#include <precice/SolverInterface.hpp>
#include <vector>

BOOST_AUTO_TEST_SUITE(Integration)
BOOST_AUTO_TEST_SUITE(Serial)
BOOST_AUTO_TEST_SUITE(DirectMeshAccess)
// Test case for a direct mesh access on one participant to a mesh defined
// by another participant (see above). In addition to the direct mesh access
// and data writing in one direction, an additional mapping (NN) is defined
// in the other direction.
// TODO: This test would fail if we choose the bounding box smaller than
// the owned mesh(?) due to the current implementation of
// 'prepareBoundingBoxes' during the partitioning step in preCICE.
BOOST_AUTO_TEST_CASE(ExplicitAndMapping)
{
  PRECICE_TEST("SolverOne"_on(1_rank), "SolverTwo"_on(1_rank));

  // Set up Solverinterface
  precice::SolverInterface interface(context.name, context.config(), 0, 1);
  BOOST_TEST(interface.getDimensions() == 2);
  constexpr int dim = 2;

  if (context.isNamed("SolverOne")) {
    const int ownMeshID   = interface.getMeshID("MeshOne");
    const int otherMeshID = interface.getMeshID("MeshTwo");
    const int readDataID  = interface.getDataID("Forces", ownMeshID);
    const int writeDataID = interface.getDataID("Velocities", otherMeshID);

    std::vector<double> positions = {0.2, 0.2, 0.1, 0.6, 0.1, 0.0, 0.1, 0.0};
    std::vector<int>    ownIDs(4, -1);
    interface.setMeshVertices(ownMeshID, ownIDs.size(), positions.data(), ownIDs.data());

    std::array<double, dim * 2> boundingBox = {0.0, 1.0, 0.0, 1.0};
    // Define region of interest, where we could obtain direct write access
    interface.setMeshAccessRegion(otherMeshID, boundingBox.data());

    double dt = interface.initialize();
    // Get the size of the filtered mesh within the bounding box
    // (provided by the coupling participant)
    const int otherMeshSize = interface.getMeshVertexSize(otherMeshID);
    BOOST_TEST(otherMeshSize == 5);

    // Allocate a vector containing the vertices
    std::vector<double> solverTwoMesh(otherMeshSize * dim);
    std::vector<int>    otherIDs(otherMeshSize, -1);
    interface.getMeshVerticesAndIDs(otherMeshID, otherMeshSize, otherIDs.data(), solverTwoMesh.data());
    // Some dummy writeData
    std::array<double, 5> writeData({1, 2, 3, 4, 5});

    std::vector<double> readData(ownIDs.size(), -1);
    // Expected data = positions of the other participant's mesh
    const std::vector<double> expectedData = {0.0, 0.0, 0.0, 0.05, 0.1, 0.1, 0.1, 0.0, 0.5, 0.5};
    BOOST_TEST(solverTwoMesh == expectedData);

    while (interface.isCouplingOngoing()) {
      // Write data
      interface.writeBlockScalarData(writeDataID, otherMeshSize,
                                     otherIDs.data(), writeData.data());
      dt = interface.advance(dt);
      interface.readBlockScalarData(readDataID, ownIDs.size(),
                                    ownIDs.data(), readData.data());
      BOOST_TEST(readData == (std::vector<double>{2, 4, 3, 3}));
    }

  } else {
    BOOST_TEST(context.isNamed("SolverTwo"));
    std::vector<double> positions = {0.0, 0.0, 0.0, 0.05, 0.1, 0.1, 0.1, 0.0, 0.5, 0.5};
    std::vector<int>    ids(positions.size() / dim, -1);

    // Query IDs
    const int meshID      = interface.getMeshID("MeshTwo");
    const int writeDataID = interface.getDataID("Forces", meshID);
    const int readDataID  = interface.getDataID("Velocities", meshID);

    // Define the mesh
    interface.setMeshVertices(meshID, ids.size(), positions.data(), ids.data());
    // Allocate data to read
    std::vector<double> readData(ids.size(), -10);
    std::vector<double> writeData;
    for (unsigned int i = 0; i < ids.size(); ++i)
      writeData.emplace_back(i);

    // Initialize
    double dt = interface.initialize();
    while (interface.isCouplingOngoing()) {

      interface.writeBlockScalarData(writeDataID, ids.size(),
                                     ids.data(), writeData.data());
      dt = interface.advance(dt);
      interface.readBlockScalarData(readDataID, ids.size(),
                                    ids.data(), readData.data());
      // Expected data according to the writeData
      std::vector<double> expectedData({1, 2, 3, 4, 5});
      BOOST_TEST(precice::testing::equals(expectedData, readData));
    }
  }
}

BOOST_AUTO_TEST_SUITE_END() // Integration
BOOST_AUTO_TEST_SUITE_END() // Serial
BOOST_AUTO_TEST_SUITE_END() // AccessReceivedMesh

#endif // PRECICE_NO_MPI
