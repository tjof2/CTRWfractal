/***************************************************************************

  Copyright 2016-2020 Tom Furnival

  This file is part of ctrwfractal.

  ctrwfractal is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  ctrwfractal is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with ctrwfractal.  If not, see <http://www.gnu.org/licenses/>.

  Percolation clusters developed from C code by Mark Newman.
  http://www-personal.umich.edu/~mejn/percolation/. See the paper:
  "A fast Monte Carlo algorithm for site or bond percolation"
  M. E. J. Newman and R. M. Ziff, Phys. Rev. E 64, 016706 (2001).

***************************************************************************/

#ifndef _CTRW_HPP
#define _CTRW_HPP

#include <cmath>
#include <cstdlib>
#include <random>
#include <armadillo>

#include "utils/pcg_random.hpp"
#include "utils/utils.hpp"

template <typename T>
class CTRWfractal
{
public:
  CTRWfractal(
      const uint64_t gridSize,
      const uint64_t latticeType,
      const double threshold,
      const uint64_t walkType,
      const uint64_t nWalks,
      const uint64_t nSteps,
      const double beta,
      const double tau0,
      const double noise,
      const int64_t randomSeed,
      const int64_t nJobs) : gridSize(gridSize),
                             latticeType(latticeType),
                             threshold(threshold),
                             walkType(walkType),
                             nWalks(nWalks),
                             nSteps(nSteps),
                             beta(beta),
                             tau0(tau0),
                             noise(noise),
                             randomSeed(randomSeed),
                             nJobs(nJobs)
  {
    includeWalks = ((nWalks > 0) && (nSteps > 0));

    if (includeWalks) // Set array sizes
    {
      simLength = (tau0 < 1.0) ? static_cast<uint64_t>(nSteps / tau0) : nSteps;

      walks.set_size(simLength);
      ctrwTimes.set_size(simLength);
      trueWalks.set_size(nSteps);
      eaMSD.set_size(nSteps);
      eaMSDall.set_size(nSteps - 1, nWalks);
      taMSD.set_size(nSteps - 1, nWalks);
      eataMSD.set_size(nSteps - 1);
      eataMSDall.set_size(nSteps - 1, nWalks);
      ergodicity.set_size(nSteps - 1);
    }
    else
    {
      simLength = 0;

      walks.set_size(0);
      ctrwTimes.set_size(0);
      trueWalks.set_size(0);
      eaMSD.set_size(0);
      eaMSDall.set_size(0, 0);
      taMSD.set_size(0, 0);
      eataMSD.set_size(0);
      eataMSDall.set_size(0, 0);
      ergodicity.set_size(0);
    }

    if (randomSeed < 0) // Seed with external entropy from std::random_device
    {
      RNG.seed(pcg_extras::seed_seq_from<std::random_device>());
    }
    else
    {
      RNG.seed(randomSeed);
    }
  };

  ~CTRWfractal()
  {
    walks.reset();
    ctrwTimes.reset();
    trueWalks.reset();
    eaMSD.reset();
    eaMSDall.reset();
    taMSD.reset();
    eataMSD.reset();
    eataMSDall.reset();
    ergodicity.reset();
    nn.reset();
    lattice.reset();
    occupation.reset();
    latticeCoords.reset();
    analysis.reset();
    walksCoords.reset();
  };

  void FindNeighbours()
  {
    t0 = GetTime();
    PrintFixed(0, "Searching neighbours...    ");

    switch (latticeType)
    {
    case 1:
      neighbourCount = 3;
      N = 4 * gridSize * gridSize;

      nn.set_size(neighbourCount, N);
      firstRow.set_size(2 * gridSize);
      lastRow.set_size(2 * gridSize);

      for (size_t i = 1; i <= 2 * gridSize; i++)
      {
        firstRow(i - 1) = 1 - 0.5 * (3 * gridSize) + 0.5 * (std::pow(-1, i) * gridSize) + 2 * i * gridSize - 1;
        lastRow(i - 1) = 0.5 * gridSize * (4 * i + std::pow(-1, i + 1) - 1) - 1;
      }
      BoundariesHoneycomb();
      break;
    case 0:
    default:
      neighbourCount = 4;
      N = gridSize * gridSize;

      nn.set_size(neighbourCount, N);
      firstRow.set_size(gridSize);
      lastRow.set_size(gridSize);

      for (size_t i = 1; i <= gridSize; i++)
      {
        firstRow(i - 1) = (i - 1) * gridSize - 1;
        lastRow(i - 1) = (i - 1) * gridSize;
      }
      BoundariesSquare();
      break;
    }

    EMPTY = (-1 * static_cast<int64_t>(N) - 1); // Define empty index
    lattice.set_size(N);                        // Set array sizes
    clusters.set_size(N);
    occupation.set_size(N);
    latticeCoords.set_size(2, N);

    if (includeWalks)
    {
      analysis.set_size(nSteps - 1, nWalks + 3);
      walksCoords.set_size(2, nSteps, nWalks);
    }
    else
    {
      analysis.set_size(0, 0);
      walksCoords.set_size(0, 0, 0);
      analysis.zeros();
      walksCoords.zeros();
    }

    t1 = GetTime();
    PrintFixed(6, ElapsedSeconds(t0, t1), " s\n");
  }

  void Permute()
  {
    PrintFixed(0, "Randomizing occupations... ");
    t0 = GetTime();

    int64_t j, t_;

    occupation = arma::regspace<arma::ivec>(0, N - 1);

    for (size_t i = 0; i < N; i++)
    {
      j = i + (N - i) * permConstant * UniformDistribution(RNG);
      t_ = occupation(i);
      occupation(i) = occupation(j);
      occupation(j) = t_;
    }

    t1 = GetTime();
    PrintFixed(6, ElapsedSeconds(t0, t1), " s\n");
  }

  void Percolate()
  {
    PrintFixed(0, "Running percolation...     ");
    t0 = GetTime();

    int64_t s1, s2;
    int64_t r1, r2;
    int64_t big = 0;

    lattice.fill(EMPTY);

    for (uint64_t i = 0; i < (threshold * N) - 1; i++)
    {
      r1 = s1 = occupation[i];
      lattice(s1) = -1;
      for (size_t j = 0; j < neighbourCount; j++)
      {
        s2 = nn(j, s1);
        if (lattice(s2) != EMPTY)
        {
          r2 = FindRoot(s2);
          if (r2 != r1)
          {
            if (lattice(r1) > lattice(r2))
            {
              lattice(r2) += lattice(r1);
              lattice(r1) = r2;
              r1 = r2;
            }
            else
            {
              lattice(r1) += lattice(r2);
              lattice(r2) = r1;
            }
            if (-lattice(r1) > big)
            {
              big = -lattice(r1);
            }
          }
        }
      }
    }

    t1 = GetTime();
    PrintFixed(6, ElapsedSeconds(t0, t1), " s\n");
  }

  void BuildLattice()
  {
    PrintFixed(0, "Building lattice...        ");
    t0 = GetTime();

    uint64_t count;

    switch (latticeType)
    {
    case 1: // Populate honeycomb lattice coordinates
      double xx, yy;
      uint64_t currentCol, xOffset, yOffset;
      count = 0;

      for (size_t i = 0; i < 4 * gridSize; i++)
      {
        for (size_t j = 0; j < gridSize; j++)
        {
          yOffset = (gridSize - j - 1); // Count from top to bottom
          xOffset = i / 4;              // divmod - remainder for free
          currentCol = i % 4;

          switch (currentCol)
          {
          case 0:
          default:
            xx = xOffset * 3;
            yy = yOffset * sqrt3 + sqrt3o2;
            break;
          case 1:
            xx = xOffset * 3 + 0.5;
            yy = yOffset * sqrt3;
            break;
          case 2:
            xx = xOffset * 3 + 1.5;
            yy = yOffset * sqrt3;
            break;
          case 3:
            xx = xOffset * 3 + 2.0;
            yy = yOffset * sqrt3 + sqrt3o2;
            break;
          }
          latticeCoords(0, count) = xx;
          latticeCoords(1, count) = yy;
          count++;
        }
      }

      unitCell = arma::max(latticeCoords, 1); // Get unit cell size
      unitCell(0) += 1.5;
      unitCell(1) += sqrt3o2;
      break;
    case 0: // Populate square lattice coordinates
    default:
      count = 0;

      for (size_t i = 0; i < gridSize; i++)
      {
        for (size_t j = 0; j < gridSize; j++)
        {
          latticeCoords(0, count) = i;
          latticeCoords(1, count) = j;
          count++;
        }
      }

      unitCell = arma::max(latticeCoords, 1); // Get unit cell size
      unitCell(0) += 1;
      unitCell(1) += 1;
      break;
    }

    t1 = GetTime();
    PrintFixed(6, ElapsedSeconds(t0, t1), " s\n");
  }

  void RandomWalks()
  {
    PrintFixed(0, "Simulating random walks... ");
    t0 = GetTime();

    PossibleStartPoints(); // Populate start points

    std::uniform_int_distribution<uint32_t> RandSample(0, static_cast<uint32_t>(latticeOnes.n_elem) - 1);

    arma::uvec boundaryDetect(simLength);
    arma::uvec boundaryTrue(simLength);
    int64_t boundary1 = static_cast<int64_t>(gridSize);
    int64_t boundary2 = static_cast<int64_t>(N) - boundary1;

    for (size_t i = 0; i < nWalks; i++) // Simulate a random walk on the lattice
    {
      uint64_t countLoop = 0;
      uint64_t countMax = std::min(N, static_cast<uint64_t>(1E6)); // Maximum attempts to find a starting site

      int64_t pos, posLast;
      bool okStart = false;
      arma::ivec neighbours;

      do // Search for a random start position
      {
        pos = latticeOnes(RandSample(RNG));
        neighbours = GetOccupiedNeighbours(pos);

        if (neighbours.n_elem > 0 || countLoop >= countMax) // Check start position has >= 1 occupied nearest neighbours
        {
          okStart = true;
        }
        else
        {
          countLoop++;
        }
      } while (!okStart);

      if (countLoop == countMax) // If no nearest neighbours, set the whole walk to that site
      {
        walks.fill(pos);
        boundaryDetect.zeros();
      }
      else
      {
        posLast = pos;
        walks(0) = pos;
        boundaryDetect(0) = 0;

        for (size_t j = 1; j < simLength; j++)
        {
          neighbours = GetOccupiedNeighbours(pos);
          std::uniform_int_distribution<uint32_t> RandChoice(0, static_cast<uint32_t>(neighbours.n_elem) - 1);
          pos = neighbours(RandChoice(RNG));
          walks(j) = pos;

          if (arma::any(firstRow == posLast) && arma::any(lastRow == pos)) // Walks that hit the top boundary
          {
            boundaryDetect(j) = 1;
          }
          else if (arma::any(lastRow == posLast) && arma::any(firstRow == pos)) // Walks that hit the bottom boundary
          {
            boundaryDetect(j) = 2;
          }
          else if (posLast >= boundary2 && pos < boundary1) // Walks that hit the right boundary
          {
            boundaryDetect(j) = 3;
          }
          else if (posLast < boundary1 && pos >= boundary2) // Walks that hit the left boundary
          {
            boundaryDetect(j) = 4;
          }
          else
          {
            boundaryDetect(j) = 0;
          }

          posLast = pos; // Update last position
        }
      }

      ctrwTimes.set_size(simLength);
      if (beta > 0.)
      {
        std::exponential_distribution<double> ExponentialDistribution(beta); // Create exponential distribution
        ctrwTimes.imbue([&]() { return ExponentialDistribution(RNG); });     // Draw CTRW random variates
        ctrwTimes = arma::cumsum(tau0 * arma::exp(ctrwTimes));               // Transform to Pareto distribution and accumulate
      }
      else
      {
        ctrwTimes = arma::linspace<arma::vec>(1, simLength, simLength);
      }

      arma::uvec boundaryTime_ = arma::find(ctrwTimes >= nSteps, 1, "first"); // Only keep times within range [0, nSteps]
      int64_t boundaryTime = boundaryTime_(0);
      ctrwTimes = ctrwTimes(arma::span(0, boundaryTime));
      ctrwTimes(boundaryTime) = nSteps;

      uint64_t counter = 0;
      boundaryTrue.zeros();

      for (size_t j = 0; j < nSteps; j++) // Subordinate fractal walk with CTRW
      {
        if (j > ctrwTimes(counter))
        {
          counter++;
          boundaryTrue(j) = boundaryDetect(counter);
        }
        trueWalks(j) = walks(counter);
      }

      int64_t nxCell = 0;
      int64_t nyCell = 0;
      for (size_t n = 0; n < nSteps; n++) // Convert the walk to the coordinate system
      {
        switch (boundaryTrue(n))
        {
        case 1:
          nyCell++;
          break;
        case 2:
          nyCell--;
          break;
        case 3:
          nxCell++;
          break;
        case 4:
          nxCell--;
          break;
        case 0:
        default:
          break;
        }
        walksCoords(0, n, i) = latticeCoords(0, trueWalks(n)) + nxCell * unitCell(0);
        walksCoords(1, n, i) = latticeCoords(1, trueWalks(n)) + nyCell * unitCell(1);
      }
    }

    t1 = GetTime();
    PrintFixed(6, ElapsedSeconds(t0, t1), " s\n");
  }

  void AnalyseWalks()
  {
    PrintFixed(0, "Analysing random walks...  ");
    t0 = GetTime();

    eaMSD.zeros(); // Zero the placeholders
    eaMSDall.zeros();
    taMSD.zeros();
    eataMSD.zeros();
    eataMSDall.zeros();
    ergodicity.zeros();

    // For long walks / lots of walks, the analysis is the bottleneck,
    // so we parallelize over nJobs using threading.

    auto &&func = [&](uint64_t i) {
      arma::vec::fixed<2> walkOrigin, walkStep;
      walkOrigin = walksCoords.slice(i).col(0);
      for (size_t j = 1; j < nSteps; j++)
      {
        walkStep = walksCoords.slice(i).col(j);
        eaMSDall(j - 1, i) = SquaredDist(walkStep(0), walkOrigin(0),
                                         walkStep(1), walkOrigin(1)); // Ensemble-average MSD
        taMSD(j - 1, i) = TAMSD(walksCoords.slice(i), nSteps, j);     // Time-average MSD
        eataMSDall(j - 1, i) = TAMSD(walksCoords.slice(i), j, 1);     // Ensemble-time-average MSD
      }
    };

    parallel(func, static_cast<uint64_t>(0), static_cast<uint64_t>(nWalks), nJobs);

    eaMSD.elem(arma::find_nonfinite(eaMSD)).zeros(); // Check for NaNs
    taMSD.elem(arma::find_nonfinite(taMSD)).zeros();
    eaMSDall.elem(arma::find_nonfinite(eataMSDall)).zeros();

    eaMSD = arma::mean(eaMSDall, 1); // Take means
    eataMSD = arma::mean(eataMSDall, 1);

    eataMSD.elem(arma::find_nonfinite(eataMSD)).zeros(); //  Check for NaNs

    arma::mat meanTAMSD = arma::square(arma::mean(taMSD, 1));
    arma::mat meanTAMSD2 = arma::mean(arma::square(taMSD), 1);

    ergodicity = (meanTAMSD2 - meanTAMSD) / meanTAMSD; // Ergodicity breaking over s
    ergodicity.elem(arma::find_nonfinite(ergodicity)).zeros();
    ergodicity /= arma::regspace<arma::vec>(1, nSteps - 1);
    ergodicity.elem(arma::find_nonfinite(ergodicity)).zeros();

    analysis.col(0) = eaMSD;
    analysis.col(1) = eataMSD;
    analysis.col(2) = ergodicity;
    analysis.cols(3, nWalks + 2) = taMSD;

    t1 = GetTime();
    PrintFixed(6, ElapsedSeconds(t0, t1), " s\n");
  }

  void AddNoise()
  {
    if (noise > 0.0)
    {
      PrintFixed(0, "Adding noise...            ");
      t0 = GetTime();

      arma::cube noiseCube(size(walksCoords));
      std::normal_distribution<double> NormalDistribution(0, noise);
      noiseCube.imbue([&]() { return NormalDistribution(RNG); });
      walksCoords += noiseCube;

      t1 = GetTime();
      PrintFixed(6, ElapsedSeconds(t0, t1), " s\n");
    }
  }

  void GroupClusters()
  {
    clusters = lattice;
    int64_t j;
    for (size_t i = 0; i < N; i++)
    {
      //PrintFixed(0, i, " ", clusters(i), " ");
      j = GroupRoot(i);
      if (clusters(i) >= 0)
      {
        clusters(i) = clusters(j);
      }
      //PrintFixed(0, j, " ", clusters(i), " ", clusters(j), "\n");
    }
  }

  bool includeWalks;
  arma::Col<int64_t> lattice, clusters;
  arma::Mat<T> latticeCoords, analysis;
  arma::Cube<T> walksCoords;

private:
  uint64_t gridSize, latticeType;
  double threshold;
  uint64_t walkType, nWalks, nSteps;
  double beta, tau0, noise;
  int64_t randomSeed, nJobs;

  uint64_t N, simLength;
  int64_t EMPTY;
  uint8_t neighbourCount;

  const double sqrt3 = 1.7320508075688772;
  const double sqrt3o2 = 0.8660254037844386;

  const uint32_t maxSites = 4294967294;      // Max uint32_t
  const double permConstant = 2.3283064e-10; // Equal to 1 / maxSites (max uint32_t)

  arma::ivec occupation, walks, trueWalks, firstRow, lastRow, latticeOnes;
  arma::imat nn;
  arma::Col<T> unitCell, ctrwTimes, eaMSD, eataMSD, ergodicity;
  arma::Mat<T> eaMSDall, eataMSDall, taMSD;

  pcg64 RNG;
  std::uniform_int_distribution<uint32_t> UniformDistribution{0, maxSites};
  std::chrono::high_resolution_clock::time_point t0, t1;

  inline int64_t FindRoot(const int64_t i)
  {
    return (lattice(i) < 0) ? i : lattice(i) = FindRoot(lattice(i));
  };

  inline int64_t GroupRoot(const int64_t i)
  {
    return (clusters(i) < 0) ? i : clusters(i) = GroupRoot(clusters(i));
  };

  void PossibleStartPoints()
  {
    latticeOnes = arma::regspace<arma::ivec>(0, N - 1);

    // Set up selection of random start point
    //  - walkType = 1 : on largest cluster, or
    //  - walkType = 0 : on ALL clusters
    if (walkType == 1)
    {
      int64_t latticeMin = lattice.elem(find(lattice > EMPTY)).min();
      arma::uvec idxMin = arma::find(lattice == latticeMin);
      arma::uvec largestCluster = arma::find(lattice == idxMin(0));
      uint64_t largestClusterSize = largestCluster.n_elem + 1;
      largestCluster.resize(largestClusterSize);
      largestCluster(largestClusterSize - 1) = idxMin(0);
      latticeOnes = latticeOnes.elem(largestCluster);
    }
    else
    {
      latticeOnes = latticeOnes.elem(find(lattice != EMPTY));
    }
  };

  arma::ivec GetOccupiedNeighbours(const int64_t pos)
  {
    arma::Col<uint8_t> checkNeighbour(neighbourCount, arma::fill::zeros);
    arma::ivec neighbours = nn.col(pos);

    for (size_t k = 0; k < neighbourCount; k++)
    {
      checkNeighbour(k) = (lattice(neighbours(k)) == EMPTY) ? 0 : 1;
    }

    return neighbours.elem(find(checkNeighbour == 1));
  };

  void BoundariesHoneycomb()
  {
    // Honeycomb lattice nearest neighbours with periodic boundary conditions
    uint64_t currentCol = 0;
    uint64_t count = 0;

    for (size_t i = 0; i < N; i++)
    {
      if (i == 0) // First site
      {
        nn(0, i) = i + gridSize;
        nn(1, i) = i + 2 * gridSize - 1;
        nn(2, i) = i + N - gridSize;
      }
      else if (i == N - gridSize) // Top right-hand corner
      {
        nn(0, i) = i - 1;
        nn(1, i) = i - gridSize;
        nn(2, i) = i - N + gridSize;
      }
      else if (i == N - gridSize - 1) // Bottom right-hand corner
      {
        nn(0, i) = i - gridSize;
        nn(1, i) = i + gridSize;
        nn(2, i) = i + 1;
      }
      else if (i < gridSize) // First column
      {
        nn(0, i) = i + gridSize - 1;
        nn(1, i) = i + gridSize;
        nn(2, i) = i + N - gridSize;
      }
      else if (i > (N - gridSize)) // Last column
      {
        nn(0, i) = i - gridSize - 1;
        nn(1, i) = i - gridSize;
        nn(2, i) = i - N + gridSize;
      }
      else // Run through the rest of the tests
      {
        switch (currentCol)
        {
        case 0:
          if (arma::any(firstRow == i)) // First row
          {
            nn(0, i) = i - gridSize;
            nn(1, i) = i + gridSize;
            nn(2, i) = i + 2 * gridSize - 1;
          }
          else
          {
            nn(0, i) = i - gridSize;
            nn(1, i) = i + gridSize - 1;
            nn(2, i) = i + gridSize;
          }
          break;
        case 1:
          if (arma::any(lastRow == i)) // Last row
          {
            nn(0, i) = i - gridSize;
            nn(1, i) = i + gridSize;
            nn(2, i) = i - 2 * gridSize + 1;
          }
          else
          {
            nn(0, i) = i - gridSize;
            nn(1, i) = i - gridSize + 1;
            nn(2, i) = i + gridSize;
          }
          break;
        case 2:
          if (arma::any(lastRow == i)) // Last row
          {
            nn(0, i) = i - gridSize;
            nn(1, i) = i + gridSize;
            nn(2, i) = i + 1;
          }
          else
          {
            nn(0, i) = i - gridSize;
            nn(1, i) = i + gridSize;
            nn(2, i) = i + gridSize + 1;
          }
          break;
        case 3:
          if (arma::any(firstRow == i)) // First row
          {
            nn(0, i) = i - 1;
            nn(1, i) = i - gridSize;
            nn(2, i) = i + gridSize;
          }
          else
          {
            nn(0, i) = i - gridSize - 1;
            nn(1, i) = i - gridSize;
            nn(2, i) = i + gridSize;
          }
          break;
        }
      }

      if ((i + 1) % gridSize == 0) // Update current column
      {
        count++;
        currentCol = count % 4;
      }
    }
  };

  void BoundariesSquare()
  {
    // Square lattice nearest neighbours with periodic boundary conditions
    for (size_t i = 0; i < N; i++)
    {
      nn(0, i) = (i + 1) % N;
      nn(1, i) = (i + N - 1) % N;
      nn(2, i) = (i + gridSize) % N;
      nn(3, i) = (i + N - gridSize) % N;
      if (i % gridSize == 0)
      {
        nn(1, i) = i + gridSize - 1;
      }
      if ((i + 1) % gridSize == 0)
      {
        nn(0, i) = i - gridSize + 1;
      }
    }
  };
};

template <typename T>
uint64_t CTRWwrapper(
    arma::Col<int64_t> &clusters,
    arma::Mat<T> &lattice,
    arma::Mat<T> &analysis,
    arma::Cube<T> &walks,
    const uint64_t gridSize,
    const uint64_t latticeType,
    const double threshold,
    const uint64_t walkType,
    const uint64_t nWalks,
    const uint64_t nSteps,
    const double beta,
    const double tau0,
    const double noise,
    const int64_t randomSeed,
    const int64_t nJobs)
{
  CTRWfractal<T> *sim = new CTRWfractal<T>(
      gridSize,
      latticeType,
      threshold,
      walkType,
      nWalks,
      nSteps,
      beta,
      tau0,
      noise,
      randomSeed,
      nJobs);

  sim->FindNeighbours(); // Identify neighbouring sites
  sim->Permute();        // Randomize the order in which the sites are occupied
  sim->Percolate();      // Run the percolation algorithm
  sim->BuildLattice();   // Build the lattice coordinates
  sim->GroupClusters();  // Group clusters by root

  if (sim->includeWalks)
  {
    sim->RandomWalks();  // Run the random walks
    sim->AddNoise();     // Add noise to walks
    sim->AnalyseWalks(); // Calculate statistics for walks
  }

  lattice = sim->latticeCoords;
  //clusters = sim->lattice;
  clusters = sim->clusters;
  analysis = sim->analysis;
  walks = sim->walksCoords;

  if (sim->includeWalks) // Armadillo is Fortran-contiguous, numpy is C-contiguous
  {
    arma::inplace_trans(lattice);
    arma::inplace_trans(analysis);
  }

  delete sim;
  return 0;
};

#endif
