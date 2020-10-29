/*
This file is part of ethminer.

ethminer is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

ethminer is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with ethminer.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "ethash_cuda_miner_kernel.h"

#include <libdevcore/Worker.h>
#include <libethcore/EthashAux.h>
#include <libethcore/Miner.h>

#include <functional>

namespace dev
{
namespace eth
{
class CUDAMiner : public Miner
{
public:
    CUDAMiner(unsigned _index);
    ~CUDAMiner() override;

    static unsigned getNumDevices();
    static void enumDevices(std::map<string, DeviceDescriptorType>& _DevicesCollection);
    static void configureGPU(unsigned _blockSize, unsigned _gridSize, unsigned _numStreams,
        unsigned _scheduleFlag, unsigned _dagLoadMode, unsigned _parallelHash);

    void search(
        uint8_t const* header, uint64_t target, uint64_t _startN, const dev::eth::WorkPackage& w);

    /* -- default values -- */
    /// Default value of the block size. Also known as workgroup size.
    static unsigned const c_defaultBlockSize;
    /// Default value of the grid size
    static unsigned const c_defaultGridSize;
    // default number of CUDA streams
    static unsigned const c_defaultNumStreams;

protected:
    bool initDevice() override;

    bool initEpoch_internal() override;

    void kick_miner() override;

private:
    atomic<bool> m_new_work = {false};

    boost::asio::io_service::strand m_io_strand;

    void workLoop() override;

    std::vector<volatile Search_results*> m_search_buf;
    std::vector<cudaStream_t> m_streams;
    uint64_t m_current_target = 0;

    /// The local work size for the search
    static unsigned s_blockSize;
    /// The initial global work size for the searches
    static unsigned s_gridSize;
    /// The number of CUDA streams
    static unsigned s_numStreams;
    /// CUDA schedule flag
    static unsigned s_scheduleFlag;
    /// CUDA parallel hashes
    static unsigned s_parallelHash;

    const uint32_t m_batch_size;
    const uint32_t m_streams_batch_size;

    uint64_t m_allocated_memory_dag = 0; // dag_size is a uint64_t in EpochContext struct
    size_t m_allocated_memory_light_cache = 0;
};


}  // namespace eth
}  // namespace dev
