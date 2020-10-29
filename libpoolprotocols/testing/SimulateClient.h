#pragma once

#include <iostream>

#include <libdevcore/Worker.h>
#include <libethcore/EthashAux.h>
#include <libethcore/Farm.h>
#include <libethcore/Miner.h>

#include "../PoolClient.h"

using namespace std;
using namespace dev;
using namespace eth;

class SimulateClient : public PoolClient, Worker
{
public:
    SimulateClient(unsigned const& difficulty, unsigned const& block);
    ~SimulateClient() override;

    void connect() override;
    void disconnect() override;

    bool isConnected() override { return m_connected; }
    bool isPendingState() override { return false; }
    string ActiveEndPoint() override { return ""; };

    void submitHashrate(string const& rate, string const& id) override;
    void submitSolution(const Solution& solution) override;

private:
    void workLoop() override;

    bool m_uppDifficulty = false;
    unsigned m_difficulty;
    unsigned m_block;
    std::chrono::steady_clock::time_point m_time;
};
