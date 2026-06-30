#include "simconnect_client.h"
#include <cstdio>

static void CALLBACK dispatchCallback(SIMCONNECT_RECV* pData, DWORD /*cbData*/, void* pContext) {
    auto* client = reinterpret_cast<SimConnectClient*>(pContext);
    switch (pData->dwID) {
    case SIMCONNECT_RECV_ID_SIMOBJECT_DATA: {
        auto* obj = reinterpret_cast<SIMCONNECT_RECV_SIMOBJECT_DATA*>(pData);
        if (obj->dwRequestID == REQUEST_ALL)
            client->onSimData(reinterpret_cast<const double*>(&obj->dwData),
                              client->poolSize());
        break;
    }
    case SIMCONNECT_RECV_ID_QUIT:
        client->onQuit();
        break;
    default:
        break;
    }
}

// ── startAsync ────────────────────────────────────────────────────────────────
void SimConnectClient::startAsync(const std::string& appName,
                                   const std::vector<ModuleConfig>& modules) {
    // SimVar-Pool vor Thread-Start aufbauen (nameToIdx danach read-only → kein Mutex nötig).
    m_pool.clear();
    m_poolUnits.clear();
    m_simData.nameToIdx.clear();
    m_simData.values.clear();
    m_simData.units.clear();

    for (const auto& mod : modules) {
        for (const auto& sv : mod.simvars) {
            if (sv.name.empty()) continue;
            if (!m_simData.nameToIdx.count(sv.name)) {
                m_simData.nameToIdx[sv.name] = m_pool.size();
                m_pool.push_back(sv.name);
                m_poolUnits.push_back(sv.unit);
                m_simData.values.push_back(0.0);
                m_simData.units.push_back(sv.unit);
            }
        }
    }

    m_appName    = appName;
    m_stopFlag   = false;
    m_quitFlag   = false;
    m_connected  = false;
    m_hasNewData = false;

    if (m_pool.empty()) return;  // keine SimVars → kein Thread nötig

    m_thread = std::thread(&SimConnectClient::threadMain, this);
}

// ── stopAsync ─────────────────────────────────────────────────────────────────
void SimConnectClient::stopAsync() {
    m_stopFlag = true;
    m_quitFlag = true;  // dispatch-Schleife sofort verlassen
    if (m_thread.joinable()) m_thread.join();
}

// ── threadMain ────────────────────────────────────────────────────────────────
// Läuft im Hintergrund. Verbindet sich mit MSFS, dispatcht Daten, reconnectet
// bei Verbindungsabbruch. Blockiert nie den Haupt-Thread.
void SimConnectClient::threadMain() {
    constexpr int RETRY_MS = 5000;
    constexpr int STEP_MS  =  100;  // Auflösung für m_stopFlag-Check während Wartezeit

    while (!m_stopFlag) {
        // SimConnect_Open kann 3-4 s blockieren wenn MSFS nicht läuft.
        // Das ist jetzt im Hintergrund-Thread — die Hauptschleife läuft weiter.
        HRESULT hr = SimConnect_Open(&m_hSim, m_appName.c_str(), nullptr, 0, nullptr, 0);
        if (m_stopFlag) break;

        if (FAILED(hr) || m_hSim == nullptr) {
            m_hSim = nullptr;
            for (int i = 0; i < RETRY_MS / STEP_MS && !m_stopFlag; ++i)
                Sleep(STEP_MS);
            continue;
        }

        if (!subscribeAll()) {
            SimConnect_Close(m_hSim);
            m_hSim = nullptr;
            for (int i = 0; i < RETRY_MS / STEP_MS && !m_stopFlag; ++i)
                Sleep(STEP_MS);
            continue;
        }

        m_connected = true;
        m_quitFlag  = false;

        // Dispatch-Schleife: läuft bis MSFS beendet (m_quitFlag) oder stopAsync() gerufen
        while (!m_stopFlag && !m_quitFlag) {
            SimConnect_CallDispatch(m_hSim, dispatchCallback, this);
        }

        // Verbindung sauber schließen
        m_connected = false;
        SimConnect_Close(m_hSim);
        m_hSim     = nullptr;
        m_quitFlag = false;

        // Kurze Pause vor Reconnect-Versuch (nur wenn nicht gestoppt)
        for (int i = 0; i < RETRY_MS / STEP_MS && !m_stopFlag; ++i)
            Sleep(STEP_MS);
    }

    m_connected = false;
}

// ── subscribeAll ──────────────────────────────────────────────────────────────
bool SimConnectClient::subscribeAll() {
    HRESULT hr = S_OK;
    for (size_t i = 0; i < m_pool.size(); ++i) {
        hr |= SimConnect_AddToDataDefinition(
            m_hSim, DEFINITION_ALL,
            m_pool[i].c_str(),
            m_poolUnits[i].c_str(),
            SIMCONNECT_DATATYPE_FLOAT64);
    }
    if (FAILED(hr)) return false;

    hr = SimConnect_RequestDataOnSimObject(
        m_hSim, REQUEST_ALL, DEFINITION_ALL,
        SIMCONNECT_OBJECT_ID_USER,
        SIMCONNECT_PERIOD_SIM_FRAME,
        SIMCONNECT_DATA_REQUEST_FLAG_CHANGED,
        0, 0, 0);
    return SUCCEEDED(hr);
}

// ── getData ───────────────────────────────────────────────────────────────────
SimData SimConnectClient::getData() {
    std::lock_guard<std::mutex> lk(m_dataMutex);
    m_hasNewData = false;
    return m_simData;
}

// ── onSimData (im Hintergrund-Thread) ────────────────────────────────────────
void SimConnectClient::onSimData(const double* raw, size_t count) {
    std::lock_guard<std::mutex> lk(m_dataMutex);
    for (size_t i = 0; i < count && i < m_simData.values.size(); ++i)
        m_simData.values[i] = raw[i];
    m_hasNewData = true;
}

// ── onQuit (im Hintergrund-Thread, aus dispatchCallback) ─────────────────────
void SimConnectClient::onQuit() {
    // Signalisiert der dispatch-Schleife in threadMain() den Austritt.
    // SimConnect_Close() erledigt threadMain() nach dem Schleifenaustritt.
    m_quitFlag  = true;
    m_connected = false;
}
