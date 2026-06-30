#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <atomic>
#include "SimConnect.h"
#include "module.h"

// Flache SimConnect-Datentabelle: alle abonnierten Variablen als double-Array.
struct SimData {
    std::vector<double>                     values;
    std::vector<std::string>                units;
    std::unordered_map<std::string, size_t> nameToIdx;

    double get(const std::string& name, double def = 0.0) const {
        auto it = nameToIdx.find(name);
        return (it != nameToIdx.end()) ? values[it->second] : def;
    }
    bool has(const std::string& name) const { return nameToIdx.count(name) > 0; }
    bool empty() const { return values.empty(); }
};

enum DATA_DEFINE_ID  { DEFINITION_ALL = 0 };
enum DATA_REQUEST_ID { REQUEST_ALL    = 0 };

class SimConnectClient {
public:
    // Startet Hintergrund-Thread der SimConnect_Open + CallDispatch-Schleife ausführt.
    // Gibt sofort zurück — die Hauptschleife blockiert nicht mehr.
    void startAsync(const std::string& appName,
                    const std::vector<ModuleConfig>& modules);

    // Stoppt Hintergrund-Thread und wartet auf sauberes Ende.
    void stopAsync();

    // Non-blocking Status-Abfragen — Thread-safe (atomics):
    bool isConnected() const { return m_connected.load(); }
    bool hasNewData()  const { return m_hasNewData.load(); }

    // Aktuelle Daten abholen (mutex-geschützt, gibt Kopie zurück, löscht hasNewData):
    SimData getData();

    size_t poolSize() const { return m_pool.size(); }

    // Vom statischen Callback aufgerufen (läuft im Hintergrund-Thread):
    void onSimData(const double* raw, size_t count);
    void onQuit();

private:
    void threadMain();
    bool subscribeAll();

    // Nur vom Hintergrund-Thread verwendet — kein Mutex nötig:
    HANDLE                   m_hSim     = nullptr;
    std::string              m_appName;
    std::vector<std::string> m_pool;       // geordnete SimVar-Namen
    std::vector<std::string> m_poolUnits;  // zugehörige Einheiten

    // Thread-shared — atomics (lock-free):
    std::atomic<bool> m_connected {false};
    std::atomic<bool> m_hasNewData{false};
    std::atomic<bool> m_stopFlag  {false};
    std::atomic<bool> m_quitFlag  {false};  // MSFS-Quit-Signal → dispatch-Schleife verlassen

    // Thread-shared — mutex-geschützt:
    mutable std::mutex m_dataMutex;
    SimData            m_simData;  // nameToIdx vor Thread-Start geschrieben, danach read-only

    std::thread m_thread;
};
