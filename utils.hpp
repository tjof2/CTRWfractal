/***************************************************************************

  Copyright 2016-2020 Tom Furnival

  This file is part of CTRWfractal.

  CTRWfractal is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  CTRWfractal is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with CTRWfractal.  If not, see <http://www.gnu.org/licenses/>.

***************************************************************************/

#ifndef UTILS_HPP
#define UTILS_HPP

#include <thread>
#include <vector>

template <typename Function, typename Integer_Type>
void parallel(Function const &func,
              Integer_Type dimFirst,
              Integer_Type dimLast,
              int nJobs = -1,
              uint32_t threshold = 1)
{
    uint32_t const totalCores = (nJobs > 0) ? nJobs : std::thread::hardware_concurrency();

    if ((nJobs == 0) || (totalCores <= 1) || ((dimLast - dimFirst) <= threshold)) // No parallelization or small jobs
    {
        for (auto a = dimFirst; a != dimLast; ++a)
        {
            func(a);
        }
        return;
    }
    else // std::thread parallelization
    {
        std::vector<std::thread> threads;
        if (dimLast - dimFirst <= totalCores) // case of small job numbers
        {
            for (auto index = dimFirst; index != dimLast; ++index)
                threads.emplace_back(std::thread{[&func, index]() { func(index); }});
            for (auto &th : threads)
            {
                th.join();
            }
            return;
        }

        auto const &jobSlice = [&func](Integer_Type a, Integer_Type b) { // case of more jobs than CPU cores
            if (a >= b)
            {
                return;
            }
            while (a != b)
            {
                func(a++);
            }
        };

        threads.reserve(totalCores - 1);
        uint64_t tasksPerThread = (dimLast - dimFirst + totalCores - 1) / totalCores;

        for (auto index = 0UL; index != totalCores - 1; ++index)
        {
            Integer_Type first = tasksPerThread * index + dimFirst;
            first = std::min(first, dimLast);
            Integer_Type last = first + tasksPerThread;
            last = std::min(last, dimLast);
            threads.emplace_back(std::thread{jobSlice, first, last});
        }

        jobSlice(tasksPerThread * (totalCores - 1), dimLast);
        for (auto &th : threads)
        {
            th.join();
        }
    }
};

#endif