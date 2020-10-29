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
/** @file Farm.h
 * @author Gav Wood <i@gavwood.com>
 * @date 2015
 */

#pragma once

#include <atomic>
#include <list>
#include <thread>

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/dll.hpp>
#include <boost/filesystem.hpp>
#include <boost/process.hpp>

#include <json/json.h>

#include <libdevcore/Common.h>
#include <libdevcore/Worker.h>
#include <libethcore/BlockHeader.h>
#include <libethcore/Miner.h>
#include <libhwmon/wrapadl.h>
#include <libhwmon/wrapnvml.h>
#if defined(__linux)
#include <libhwmon/wrapamdsysfs.h>
#include <sys/stat.h>
#endif

namespace dev
{
namespace eth
{
/**
 * @brief A collective of Miners.
 * Miners ask for work, then submit proofs
 * @threadsafe
 */
class Farm : public FarmFace
{
public:
    unsigned tstart = 0, tstop = 0;

    struct SealerDescriptor
    {
        std::function<unsigned()> instances;
        std::function<Miner*(FarmFace&, unsigned)> create;
    };

    Farm(boost::asio::io_service& io_service, bool hwmon, bool pwron) : m_io_strand(io_service), m_collectTimer(io_service)
    {
        m_hwmon = hwmon;
        m_pwron = pwron;

        // Init HWMON if needed
        if (m_hwmon)
        {
            adlh = wrap_adl_create();
#if defined(__linux)
            sysfsh = wrap_amdsysfs_create();
#endif
            nvmlh = wrap_nvml_create();
        }

        // Initialize nonce_scrambler
        shuffle();

        // Start data collector timer
        // It should work for the whole lifetime of Farm
        // regardless it's mining state
        m_collectTimer.expires_from_now(boost::posix_time::milliseconds(m_collectInterval));
        m_collectTimer.async_wait(m_io_strand.wrap(
            boost::bind(&Farm::collectData, this, boost::asio::placeholders::error)));
    }

    ~Farm()
    {
        // Deinit HWMON
        if (adlh)
            wrap_adl_destroy(adlh);
#if defined(__linux)
        if (sysfsh)
            wrap_amdsysfs_destroy(sysfsh);
#endif
        if (nvmlh)
            wrap_nvml_destroy(nvmlh);

        // Stop mining
        stop();

        // Stop data collector
        m_collectTimer.cancel();
    }

    /**
     * @brief Randomizes the nonce scrambler
     */
    void shuffle()
    {
        // Given that all nonces are equally likely to solve the problem
        // we could reasonably always start the nonce search ranges
        // at a fixed place, but that would be boring. Provide a once
        // per run randomized start place, without creating much overhead.
        random_device engine;
        m_nonce_scrambler = uniform_int_distribution<uint64_t>()(engine);
    }

    /**
     * @brief Sets the current mining mission.
     * @param _wp The work package we wish to be mining.
     */
    void setWork(WorkPackage const& _wp)
    {
        // Set work to each miner
        Guard l(x_minerWork);
        m_work = _wp;
        for (auto const& m : m_miners)
            m->setWork(m_work);
    }

    void setSealers(std::map<std::string, SealerDescriptor> const& _sealers)
    {
        m_sealers = _sealers;
    }

    /**
     * @brief Start a number of miners.
     */
    bool start(std::string const& _sealer, bool mixed)
    {
        Guard l(x_minerWork);
        if (!m_miners.empty() && m_lastSealer == _sealer)
            return true;
        if (!m_sealers.count(_sealer))
            return false;

        if (!mixed)
        {
            m_miners.clear();
        }
        auto ins = m_sealers[_sealer].instances();
        unsigned start = 0;
        if (!mixed)
        {
            m_miners.reserve(ins);
        }
        else
        {
            start = m_miners.size();
            ins += start;
            m_miners.reserve(ins);
        }
        for (unsigned i = start; i < ins; ++i)
        {
            // TODO: Improve miners creation, use unique_ptr.
            m_miners.push_back(std::shared_ptr<Miner>(m_sealers[_sealer].create(*this, i)));

            // Start miners' threads. They should pause waiting for new work
            // package.
            m_miners.back()->startWorking();
        }

        m_isMining.store(true, std::memory_order_relaxed);
        m_lastSealer = _sealer;

        return true;
    }

    /**
     * @brief Stop all mining activities.
     */
    void stop()
    {
        // Avoid re-entering if not actually mining.
        // This, in fact, is also called by destructor
        if (isMining())
        {
            {
                Guard l(x_minerWork);
                m_miners.clear();
                m_isMining.store(false, std::memory_order_relaxed);
            }
        }
    }

    /**
     * @brief Stop all mining activities and Starts them again
     */
    void restart()
    {
        if (m_onMinerRestart)
        {
            m_onMinerRestart();
        }
    }

    /**
     * @brief Stop all mining activities and Starts them again (async post)
     */
    void restart_async()
    {
        m_io_strand.get_io_service().post(m_io_strand.wrap(boost::bind(&Farm::restart, this)));
    }

    bool isMining() const { return m_isMining.load(std::memory_order_relaxed); }

    /**
     * @brief Spawn a reboot script (reboot.bat/reboot.sh)
     * @return false if no matching file was found
     */
    bool reboot(const std::vector<std::string>& args)
    {
#if defined(_WIN32)
        const char* filename = "reboot.bat";
#else
        const char* filename = "reboot.sh";
#endif

        return spawn_file_in_bin_dir(filename, args);
    }

    /**
     * @brief Get information on the progress of mining this work package.
     * @return The progress with mining so far.
     */
    WorkingProgress const& miningProgress() const
    {
        return m_progress;
    }

    std::shared_ptr<Miner> getMiner(unsigned index) { return m_miners[index]; }

    SolutionStats getSolutionStats() { return m_solutionStats; }

    void failedSolution() override { m_solutionStats.failed(); }

    void acceptedSolution(bool _stale)
    {
        if (!_stale)
        {
            m_solutionStats.accepted();
        }
        else
        {
            m_solutionStats.acceptedStale();
        }
    }

    void rejectedSolution() { m_solutionStats.rejected(); }

    using SolutionFound = std::function<void(const Solution&)>;
    using MinerRestart = std::function<void()>;

    /**
     * @brief Provides a valid header based upon that received previously with setWork().
     * @param _bi The now-valid header.
     * @return true if the header was good and that the Farm should pause until more work is
     * submitted.
     */
    void onSolutionFound(SolutionFound const& _handler) { m_onSolutionFound = _handler; }
    void onMinerRestart(MinerRestart const& _handler) { m_onMinerRestart = _handler; }

    WorkPackage work() const
    {
        Guard l(x_minerWork);
        return m_work;
    }

    std::chrono::steady_clock::time_point farmLaunched() { return m_farm_launched; }

    string farmLaunchedFormatted()
    {
        auto d = std::chrono::steady_clock::now() - m_farm_launched;
        int hsize = 3;
        auto hhh = std::chrono::duration_cast<std::chrono::hours>(d);
        if (hhh.count() < 100)
        {
            hsize = 2;
        }
        d -= hhh;
        auto mm = std::chrono::duration_cast<std::chrono::minutes>(d);
        std::ostringstream stream;
        stream << "Time: " << std::setfill('0') << std::setw(hsize) << hhh.count() << ':'
               << std::setfill('0') << std::setw(2) << mm.count();
        return stream.str();
    }

    void set_pool_addresses(string host, unsigned port)
    {
        stringstream ssPoolAddresses;
        ssPoolAddresses << host << ':' << port;
        m_pool_addresses = ssPoolAddresses.str();
    }

    string get_pool_addresses() { return m_pool_addresses; }

    uint64_t get_nonce_scrambler() override { return m_nonce_scrambler; }

    unsigned get_segment_width() override { return m_nonce_segment_with; }

    void set_nonce_scrambler(uint64_t n) { m_nonce_scrambler = n; }

    void set_nonce_segment_width(unsigned n) { m_nonce_segment_with = n; }

    /**
     * @brief Provides the description of segments each miner is working on
     * @return a JsonObject
     */
    Json::Value get_nonce_scrambler_json()
    {
        Json::Value jRes;
        jRes["noncescrambler"] = m_nonce_scrambler;
        jRes["segmentwidth"] = m_nonce_segment_with;

        for (size_t i = 0; i < m_miners.size(); i++)
        {
            Json::Value jSegment;
            uint64_t gpustartnonce =
                m_nonce_scrambler + ((uint64_t)pow(2, m_nonce_segment_with) * i);
            jSegment["gpu"] = (int)i;
            jSegment["start"] = gpustartnonce;
            jSegment["stop"] = uint64_t(gpustartnonce + (uint64_t)(pow(2, m_nonce_segment_with)));
            jRes["segments"].append(jSegment);
        }

        return jRes;
    }

    void setTStartTStop(unsigned tstart, unsigned tstop)
    {
        m_tstart = tstart;
        m_tstop = tstop;
    }

    unsigned get_tstart() override { return m_tstart; }

    unsigned get_tstop() override { return m_tstop; }

private:
    // Collects data about hashing and hardware status
    void collectData(const boost::system::error_code& ec)
    {
        if (ec)
            return;

        WorkingProgress progress;

        // Process miners
        for (auto const& miner : m_miners)
        {
            // Collect and reset hashrates
            if (!miner->is_mining_paused())
            {
                auto hr = miner->RetrieveHashRate();
                progress.hashRate += hr;
                progress.minersHashRates.push_back(hr);
                progress.miningIsPaused.push_back(false);
            }
            else
            {
                progress.minersHashRates.push_back(0.0);
                progress.miningIsPaused.push_back(true);
            }

            if (m_hwmon)
            {
                HwMonitorInfo hwInfo = miner->hwmonInfo();
                HwMonitor hw;
                unsigned int tempC = 0, fanpcnt = 0, powerW = 0;
                if (hwInfo.deviceIndex >= 0)
                {
                    if (hwInfo.deviceType == HwMonitorInfoType::NVIDIA && nvmlh)
                    {
                        int typeidx = 0;
                        if (hwInfo.indexSource == HwMonitorIndexSource::CUDA)
                        {
                            typeidx = nvmlh->cuda_nvml_device_id[hwInfo.deviceIndex];
                        }
                        else if (hwInfo.indexSource == HwMonitorIndexSource::OPENCL)
                        {
                            typeidx = nvmlh->opencl_nvml_device_id[hwInfo.deviceIndex];
                        }
                        else
                        {
                            // Unknown, don't map
                            typeidx = hwInfo.deviceIndex;
                        }
                        wrap_nvml_get_tempC(nvmlh, typeidx, &tempC);
                        wrap_nvml_get_fanpcnt(nvmlh, typeidx, &fanpcnt);
                        if (m_pwron)
                        {
                            wrap_nvml_get_power_usage(nvmlh, typeidx, &powerW);
                        }
                    }
                    else if (hwInfo.deviceType == HwMonitorInfoType::AMD && adlh)
                    {
                        int typeidx = 0;
                        if (hwInfo.indexSource == HwMonitorIndexSource::OPENCL)
                        {
                            typeidx = adlh->opencl_adl_device_id[hwInfo.deviceIndex];
                        }
                        else
                        {
                            // Unknown, don't map
                            typeidx = hwInfo.deviceIndex;
                        }
                        wrap_adl_get_tempC(adlh, typeidx, &tempC);
                        wrap_adl_get_fanpcnt(adlh, typeidx, &fanpcnt);
                        if (m_pwron)
                        {
                            wrap_adl_get_power_usage(adlh, typeidx, &powerW);
                        }
                    }
#if defined(__linux)
                    // Overwrite with sysfs data if present
                    if (hwInfo.deviceType == HwMonitorInfoType::AMD && sysfsh)
                    {
                        int typeidx = 0;
                        if (hwInfo.indexSource == HwMonitorIndexSource::OPENCL)
                        {
                            typeidx = sysfsh->opencl_sysfs_device_id[hwInfo.deviceIndex];
                        }
                        else
                        {
                            // Unknown, don't map
                            typeidx = hwInfo.deviceIndex;
                        }
                        wrap_amdsysfs_get_tempC(sysfsh, typeidx, &tempC);
                        wrap_amdsysfs_get_fanpcnt(sysfsh, typeidx, &fanpcnt);
                        if (m_pwron)
                        {
                            wrap_amdsysfs_get_power_usage(sysfsh, typeidx, &powerW);
                        }
                    }
#endif
                }

                miner->update_temperature(tempC);

                hw.tempC = tempC;
                hw.fanP = fanpcnt;
                hw.powerW = powerW / ((double)1000.0);
                progress.minerMonitors.push_back(hw);

            }
        }

        m_progress = progress;

        // Resubmit timer for another loop
        m_collectTimer.expires_from_now(boost::posix_time::milliseconds(m_collectInterval));
        m_collectTimer.async_wait(m_io_strand.wrap(
            boost::bind(&Farm::collectData, this, boost::asio::placeholders::error)));
    }

    /**
     * @brief Spawn a file - must be located in the directory of ethminer binary
     * @return false if file was not found or it is not executeable
     */
    bool spawn_file_in_bin_dir(const char* filename, const std::vector<std::string>& args)
    {
        std::string fn = boost::dll::program_location().parent_path().string() +
                         "/" +  // boost::filesystem::path::preferred_separator
                         filename;
        try
        {
            if (!boost::filesystem::exists(fn))
                return false;

            /* anything in the file */
            if (!boost::filesystem::file_size(fn))
                return false;

#if defined(__linux)
            struct stat sb;
            if (stat(fn.c_str(), &sb) != 0)
                return false;
            /* just check if any exec flag is set.
               still execution can fail (not the uid, not in the group, selinux, ...)
             */
            if ((sb.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
                return false;
#endif
            /* spawn it (no wait,...) - fire and forget! */
            boost::process::spawn(fn, args);
            return true;
        }
        catch (...)
        {
        }
        return false;
    }

    /**
     * @brief Called from a Miner to note a WorkPackage has a solution.
     * @param _p The solution.
     * @param _wp The WorkPackage that the Solution is for.
     * @return true iff the solution was good (implying that mining should be .
     */
    void submitProof(Solution const& _s) override
    {
        assert(m_onSolutionFound);
        m_onSolutionFound(_s);
    }

    mutable Mutex x_minerWork;
    std::vector<std::shared_ptr<Miner>> m_miners;
    WorkPackage m_work;

    std::atomic<bool> m_isMining = {false};

    mutable WorkingProgress m_progress;

    SolutionFound m_onSolutionFound;
    MinerRestart m_onMinerRestart;

    std::map<std::string, SealerDescriptor> m_sealers;
    std::string m_lastSealer;

    boost::asio::io_service::strand m_io_strand;
    boost::asio::deadline_timer m_collectTimer;
    int m_collectInterval = 5000;

    mutable SolutionStats m_solutionStats;
    std::chrono::steady_clock::time_point m_farm_launched = std::chrono::steady_clock::now();

    string m_pool_addresses;

    // StartNonce (non-NiceHash Mode) and
    // segment width assigned to each GPU as exponent of 2
    uint64_t m_nonce_scrambler;
    unsigned int m_nonce_segment_with = 40;

    // Switches for hw monitoring and power drain monitoring
    bool m_hwmon, m_pwron;

    // Hardware monitoring temperatures
    unsigned m_tstart = 0, m_tstop = 0;

    // Wrappers for hardware monitoring libraries
    wrap_nvml_handle* nvmlh = nullptr;
    wrap_adl_handle* adlh = nullptr;

#if defined(__linux)
    wrap_amdsysfs_handle* sysfsh = nullptr;
#endif
};

}  // namespace eth
}  // namespace dev
