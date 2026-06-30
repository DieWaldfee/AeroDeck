#include <Arduino.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <Wire.h>
#include <ACAN2517FD.h>
#include <SPI.h>
#include <algorithm>
#include <array>
#include <cmath>

// ─── Instrument Configuration ────────────────────────────────────────────────
// Mid-Continent 4300 series: four independently selectable variables.
// Set CFG to any DESIGN_x constant to switch the entire instrument look.

namespace {

enum class RollDialType    : uint8_t { Rotating, Fixed };
enum class PointerType     : uint8_t { Fixed, Rotating };
enum class HorizonDialType : uint8_t { Standard, HighResolution };
enum class AirplaneType    : uint8_t { Traditional, Delta };

struct InstrumentConfig {
    RollDialType    rollDial;
    PointerType     rollPointer;
    HorizonDialType horizonDial;
    AirplaneType    airplane;
};

} // namespace

static constexpr InstrumentConfig DESIGN_F = {
    RollDialType::Fixed,
    PointerType::Rotating,
    HorizonDialType::HighResolution,
    AirplaneType::Traditional
};

static constexpr InstrumentConfig CFG = DESIGN_F;

// ─── Display ─────────────────────────────────────────────────────────────────
static TFT_eSPI    tft;
static TFT_eSprite spr(&tft);
static TFT_eSprite numSpr(&tft);  // small sprite for rotated pitch labels

static constexpr int CX = 120;
static constexpr int CY = 120;

// RGB565 colors
static constexpr uint16_t C_SKY    = 0x04BF;  // #0094F8 cornflower blue
static constexpr uint16_t C_EARTH  = 0xA200;  // #A04000 earthy brown
static constexpr uint16_t C_ORANGE = 0xFD20;  // #F8A400 instrument orange

// pixels per degree of pitch on the horizon dial
static constexpr float PX_PER_DEG = 3.0f;

// ─── Math constants ───────────────────────────────────────────────────────────
static constexpr float PI_F         = 3.14159265358979323846f;
static constexpr float HALF_PI_F    = PI_F / 2.0f;
static constexpr float DEG_TO_RAD_F = PI_F / 180.0f;
static constexpr float RAD_TO_DEG_F = 180.0f / PI_F;

// ─── QMI8658 ─────────────────────────────────────────────────────────────────
static constexpr uint8_t IMU_SDA  = 6;
static constexpr uint8_t IMU_SCL  = 7;
static constexpr uint8_t QMI_ADDR = 0x6B;

static constexpr float ACC_SENS = 1.0f / 4096.0f;
static constexpr float GYR_SENS = 1.0f / 32.0f;

namespace {
struct Vec3f { float x, y, z; };
} // namespace

static void qmiWrite(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(QMI_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

static bool qmiBurst(uint8_t reg, uint8_t *buf, uint8_t n) {
    Wire.beginTransmission(QMI_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) { return false; }
    Wire.requestFrom(QMI_ADDR, n, static_cast<uint8_t>(1));
    for (uint8_t i = 0; i < n; i++) { buf[i] = Wire.read(); }
    return true;
}

static bool initIMU() {
    Wire.begin(IMU_SDA, IMU_SCL);
    Wire.setClock(400000);
    delay(20);
    uint8_t id = 0;
    if (!qmiBurst(0x00, &id, 1) || id != 0x05) { return false; }
    qmiWrite(0x02, 0x40);  // CTRL1: address auto-increment
    qmiWrite(0x03, 0x23);  // CTRL2: acc ±8 g, 1 kHz ODR
    qmiWrite(0x04, 0x63);  // CTRL3: gyro ±1024 dps, 1 kHz ODR
    qmiWrite(0x08, 0x03);  // CTRL7: acc + gyro enabled
    delay(50);
    return true;
}

static bool readIMU(Vec3f &acc, Vec3f &gyr) {
    std::array<uint8_t, 12> b{};
    if (!qmiBurst(0x35, b.data(), 12)) { return false; }
    auto s16 = [&](int i) -> int16_t { return static_cast<int16_t>((b[i + 1] << 8) | b[i]); };
    acc = {static_cast<float>(s16(0)) * ACC_SENS,
           static_cast<float>(s16(2)) * ACC_SENS,
           static_cast<float>(s16(4)) * ACC_SENS};
    gyr = {static_cast<float>(s16(6)) * GYR_SENS,
           static_cast<float>(s16(8)) * GYR_SENS,
           static_cast<float>(s16(10)) * GYR_SENS};
    return true;
}

// ─── CAN-FD (MCP2518FD via SPI3/HSPI) ────────────────────────────────────────
// SPI2/FSPI (Bus-ID 0) ist durch das Display belegt (TFT_eSPI, GPIO 8–12).
// Das CAN-Modul nutzt SPI3/HSPI (Bus-ID 1) — kein Konflikt.
//
// Pinbelegung MCP2518FD:
//   ESP32-S3 GPIO 13 → SDO  (MISO)
//   ESP32-S3 GPIO 14 → SCK
//   ESP32-S3 GPIO 15 → SDI  (MOSI)
//   ESP32-S3 GPIO 16 → NCS  (CS)
//   ESP32-S3 GPIO 17 → INT
static constexpr uint8_t  CAN_SCK     = 14;
static constexpr uint8_t  CAN_MISO    = 13;   // SDO am MCP2518FD
static constexpr uint8_t  CAN_MOSI    = 15;   // SDI am MCP2518FD
static constexpr uint8_t  CAN_CS      = 16;   // NCS am MCP2518FD
static constexpr uint8_t  CAN_INT     = 17;
static constexpr uint32_t CAN_RX_ID   = 0x200;
static constexpr uint32_t CAN_TX_ID   = 0x280;
// MUSS exakt mit bridge_ESP32/src/config.h::CAN_ARB_BPS übereinstimmen!
// Diagnose 2026-06-30 abgeschlossen: Eigentliche Ursache war msg.pad() fehlend (len=30
// ungültig für CAN-FD → isValid()=false → tryToSend() verwarf Frame lautlos).
// 20 kbit/s war nur Diagnose-Workaround — hier zurück auf 125 kbit/s.
// TODO Produktion: 500UL * 1000UL + TDC konfigurieren + verdrillte Leitung.
static constexpr uint32_t CAN_ARB_BPS = 125UL * 1000UL;
#define CAN_OSC_FREQ ACAN2517FDSettings::OSC_40MHz          // bestätigt: 40 MHz
static constexpr uint32_t CAN_OSC_MHZ = 40;               // Quarzfrequenz in MHz — muss mit CAN_OSC_FREQ übereinstimmen

// ─── Debug-Ausgabe-Level ─────────────────────────────────────────────────────
// Wird über Build-Flag HORIZON_DEBUG in platformio.ini gesetzt.
// 0 = nur kritische Fehler (Sprite, IMU, CAN-Init-Fehler)
// 1 = Boot-Banner + wichtige Ereignisse (CAN OK, IMU OK, 0xFC/0xFD, Zustandswechsel)
// 2 = verbose: HB-TX, alle CAN-RX-Frames, STATUS_REQUEST
#ifndef HORIZON_DEBUG
#  define HORIZON_DEBUG 1
#endif
static constexpr uint8_t DEBUG_LEVEL = HORIZON_DEBUG;

static SPIClass   g_canSpi(1);                              // SPI3/HSPI
static ACAN2517FD g_can(CAN_CS, g_canSpi, CAN_INT);        // ISR-Modus via GPIO CAN_INT (GPIO 17)

// ─── CAN-Frame-Format (bridge.exe Generic Assembler Output) ──────────────────
// CAN-ID: 0x200 — alle 200 ms von der bridge
// Byte  0      uint8    0x00 (Pakettyp Daten — immer fix)
// Byte  1– 4   float32  ATTITUDE INDICATOR BANK DEGREES  (radians)
// Byte  5– 8   float32  ATTITUDE INDICATOR PITCH DEGREES (radians)
// Byte  9–12   float32  INCIDENCE BETA                   (radians) → Libelle Modus 0
// Byte 13–16   float32  ACCELERATION BODY X              (ft/s²)   → Libelle Modus 1
// Byte 17–20   float32  ACCELERATION BODY Y              (ft/s²)   → Libelle Modus 1
// Byte 21–24   float32  PLANE BANK DEGREES               (radians) → Schwerkraft-Transform
// Byte 25–28   float32  PLANE PITCH DEGREES              (radians) → Schwerkraft-Transform
// Byte 29      uint8    PARTIAL PANEL ATTITUDE (0=ok, 1=warn, 2=fail)
static constexpr uint8_t CAN_FRAME_MIN_LEN = 30;

// ─── Bridge-Zustand ───────────────────────────────────────────────────────────
enum class BridgeState : uint8_t { LIVE, DEMO };

static BridgeState g_state       = BridgeState::LIVE;   // startet in LIVE (Pitch=0, Roll=0)
static uint32_t    g_lastFrameMs = 0;                   // elapsed ab Boot → nach 10 s → DEMO

static constexpr uint32_t TIMEOUT_LIVE_MS = 10000;      // LIVE→DEMO: 10 s ohne CAN-Frame

// ─── Empfangene CAN-Daten ────────────────────────────────────────────────────
static float   g_bank_rad      = 0.0f;
static float   g_pitch_rad     = 0.0f;
static float   g_incBeta_rad   = 0.0f;  // INCIDENCE BETA → Libelle Modus 0
static float   g_accelX        = 0.0f;  // ACCELERATION BODY X → Libelle Modus 1
static float   g_accelY        = 0.0f;  // ACCELERATION BODY Y → Libelle Modus 1
static float   g_planeBankRad  = 0.0f;  // PLANE BANK DEGREES → Schwerkraft-Transform
static float   g_planePitchRad = 0.0f;  // PLANE PITCH DEGREES → Schwerkraft-Transform
static uint8_t g_partPanel     = 0;     // PARTIAL PANEL ATTITUDE

// ─── Slip-Libelle: Berechnungszustand ─────────────────────────────────────────
static uint8_t  g_slipMode    = 0;      // 0=INCIDENCE_BETA, 1=ACCEL_DAMPED (via 0xFB Config)
static float    g_slipFiltDeg = 0.0f;   // Low-pass-Zustand für ACCEL_DAMPED
static uint32_t g_lastLoopMs  = 0;      // dt-Messung

// ─── Heartbeat + Init-Handshake ────────────────────────────────────────────────
static constexpr uint32_t BOOT_HB_INTERVAL_MS = 500;   // schneller Retry bis 0xFC bestätigt
static uint32_t  g_hbIntervalMs = 1000;   // Normalbetrieb; überschrieben per 0xFD
static uint32_t  g_lastHbMs     = 0;
static bool      g_initReceived = false;  // true sobald 0xFC Init von bridge_exe empfangen
static bool      g_canInitOk   = false;  // true nur wenn NormalFD-begin() fehlerfrei

// ─── NVS-Persistenz ───────────────────────────────────────────────────────────
static Preferences g_prefs;

// ─── Status-Dot ──────────────────────────────────────────────────────────────
static constexpr float STATUS_DOT_R         = 110.0f;
static constexpr float STATUS_DOT_ANGLE_DEG = 135.0f;
static constexpr int   STATUS_DOT_RADIUS    = 5;
static uint32_t        g_dotBlinkMs  = 0;
static bool            g_dotBlinkRed = true;

// ─── Drawing: Horizon background ─────────────────────────────────────────────
// pitchDeg is the raw sensor pitch (no sign flip).
// invertedFill = true when aircraft is inverted (|roll| > 90°): earth on top.
//
// The anchor (xC, yC) is the horizon centre on screen. Pitch shifts along the
// aircraft vertical axis, which rotates with roll:
//   xC = CX - pitchDeg * P * sinR
//   yC = CY + pitchDeg * P * cosR
// This matches the 0°-mark formula in drawPitchLadder exactly.
static void drawHorizonBg(float roll, float pitchDeg, bool invertedFill) {
    float sinR = sinf(roll);
    float cosR = cosf(roll);
    // tanf(±90°) → ±1.6e7; casting that to int is UB and corrupts the sprite.
    // Clamp to ±500 — any value beyond ±4 already covers the full 240-px column.
    float tanR = tanf(roll);
    if      (tanR >  500.0f) { tanR =  500.0f; }
    else if (tanR < -500.0f) { tanR = -500.0f; }

    float pitchPx = pitchDeg * PX_PER_DEG;
    float xC = CX - (pitchPx * sinR);
    float yC = CY + (pitchPx * cosR);

    if (!invertedFill) {
        spr.fillSprite(C_SKY);
        for (int x = 0; x < 240; x++) {
            int yH = static_cast<int>(yC + ((static_cast<float>(x) - xC) * tanR));
            if (yH < 240) {
                int y0 = yH < 0 ? 0 : yH;
                spr.drawFastVLine(x, y0, 240 - y0, C_EARTH);
            }
        }
    } else {
        spr.fillSprite(C_EARTH);
        for (int x = 0; x < 240; x++) {
            int yH = static_cast<int>(yC + ((static_cast<float>(x) - xC) * tanR));
            int y0 = std::max(0, std::min(240, yH));
            if (y0 < 240) { spr.drawFastVLine(x, y0, 240 - y0, C_SKY); }
        }
    }

    // Horizon line: extend 420 px along the direction vector (cosR, sinR).
    int hx0 = static_cast<int>(xC - (420.0f * cosR));
    int hy0 = static_cast<int>(yC - (420.0f * sinR));
    int hx1 = static_cast<int>(xC + (420.0f * cosR));
    int hy1 = static_cast<int>(yC + (420.0f * sinR));
    spr.drawLine(hx0, hy0, hx1, hy1, TFT_WHITE);
    // 2-px thickness: 1px offset along perpendicular direction (-sinR, cosR)
    int pnx = -static_cast<int>(roundf(sinR));
    int pny =  static_cast<int>(roundf(cosR));
    spr.drawLine(hx0 + pnx, hy0 + pny, hx1 + pnx, hy1 + pny, TFT_WHITE);
}

// ─── Drawing: Pitch ladder ────────────────────────────────────────────────────
// Graduation lines rotate with the horizon card.
// "aircraft up" direction on screen = (sinR, -cosR).
// Line direction (along graduation) = (cosR, sinR).
static void drawPitchLadder(float roll, float pitchDeg) {
    float cr = cosf(roll);
    float sr = sinf(roll);

    struct Mark { int8_t deg; uint8_t hw; };  // hw = half-width in pixels
    static constexpr std::array<Mark, 42> MARKS = {{
        {-90, 25}, {-85, 15}, {-80, 25}, {-75, 15}, {-70, 25}, {-65, 15},
        {-60, 25}, {-55, 15}, {-50, 25}, {-45, 15}, {-40, 25}, {-35, 15},
        {-30, 25}, {-25, 15}, {-20, 25}, {-15, 15}, {-10, 25},
        { -8, 20}, { -6, 15}, { -4, 10}, { -2,  5},
        {  2,  5}, {  4, 10}, {  6, 15}, {  8, 20},
        { 10, 25}, { 15, 15}, { 20, 25}, { 25, 15}, { 30, 25},
        { 35, 15}, { 40, 25}, { 45, 15}, { 50, 25}, { 55, 15}, { 60, 25},
        { 65, 15}, { 70, 25}, { 75, 15}, { 80, 25}, { 85, 15}, { 90, 25}
    }};

    for (const auto &m : MARKS) {
        float delta = (static_cast<float>(m.deg) - pitchDeg) * PX_PER_DEG;
        float mx    = CX + (delta * sr);
        float my    = CY - (delta * cr);
        int x1 = static_cast<int>(mx - (static_cast<float>(m.hw) * cr));
        int y1 = static_cast<int>(my - (static_cast<float>(m.hw) * sr));
        int x2 = static_cast<int>(mx + (static_cast<float>(m.hw) * cr));
        int y2 = static_cast<int>(my + (static_cast<float>(m.hw) * sr));

        // Skip marks whose endpoints extend outside the r=100 roll-arc circle
        float ex1 = x1 - CX, ey1 = y1 - CY;
        float ex2 = x2 - CX, ey2 = y2 - CY;
        if (((ex1*ex1) + (ey1*ey1)) > 100.0f*100.0f ||
            ((ex2*ex2) + (ey2*ey2)) > 100.0f*100.0f) { continue; }

        spr.drawLine(x1, y1, x2, y2, TFT_WHITE);
        // 2-px thickness via normal offset
        spr.drawLine(x1 - static_cast<int>(roundf(sr)), y1 + static_cast<int>(roundf(cr)),
                     x2 - static_cast<int>(roundf(sr)), y2 + static_cast<int>(roundf(cr)), TFT_WHITE);
    }
}

// ─── Drawing: Fixed roll scale arc + tick marks ───────────────────────────────
static void drawRollScale() {
    static constexpr int ARC_R = 100;

    // arc from -80° to +80° (2° steps)
    for (int deg = -80; deg <= 80; deg += 2) {
        float angleRad = (static_cast<float>(deg) * DEG_TO_RAD_F) - HALF_PI_F;
        spr.drawPixel(CX + static_cast<int>(ARC_R * cosf(angleRad)),
                      CY + static_cast<int>(ARC_R * sinf(angleRad)), TFT_WHITE);
    }

    static constexpr float R_LONG  = 120.0f;
    static constexpr float R_SHORT = 115.0f;
    static constexpr std::array<int8_t, 8> TICK_DEGS    = { 10,  20,  30,  60, -10, -20, -30, -60 };
    static constexpr std::array<bool,   8> TICK_IS_LONG = { false, false, true, true,
                                                             false, false, true, true };
    for (int i = 0; i < 8; i++) {
        float angleRad = (static_cast<float>(TICK_DEGS[i]) * DEG_TO_RAD_F) - HALF_PI_F;
        float ca = cosf(angleRad);
        float sa = sinf(angleRad);
        float r_outer = TICK_IS_LONG[i] ? R_LONG : R_SHORT;
        int x1 = CX + static_cast<int>(static_cast<float>(ARC_R) * ca);
        int y1 = CY + static_cast<int>(static_cast<float>(ARC_R) * sa);
        int x2 = CX + static_cast<int>(r_outer * ca);
        int y2 = CY + static_cast<int>(r_outer * sa);
        spr.drawLine(x1, y1, x2, y2, TFT_WHITE);
        spr.drawLine(x1 - static_cast<int>(roundf(sa)), y1 + static_cast<int>(roundf(ca)),
                     x2 - static_cast<int>(roundf(sa)), y2 + static_cast<int>(roundf(ca)), TFT_WHITE);
    }

    // ±45°: small white triangle
    for (int sign : {1, -1}) {
        float a   = (static_cast<float>(sign * 45) * DEG_TO_RAD_F) - HALF_PI_F;
        float ca  = cosf(a);
        float sa  = sinf(a);
        int   tip_x = CX + static_cast<int>((ARC_R + 1) * ca);
        int   tip_y = CY + static_cast<int>((ARC_R + 1) * sa);
        float bcx   = CX + ((ARC_R + 9) * ca);
        float bcy   = CY + ((ARC_R + 9) * sa);
        int b1x = static_cast<int>(bcx - (5.0f * sa));
        int b1y = static_cast<int>(bcy + (5.0f * ca));
        int b2x = static_cast<int>(bcx + (5.0f * sa));
        int b2y = static_cast<int>(bcy - (5.0f * ca));
        spr.fillTriangle(tip_x, tip_y, b1x, b1y, b2x, b2y, TFT_WHITE);
    }

    // 0° reference triangle: tip DOWN toward roll pointer
    spr.fillTriangle(CX,     CY - ARC_R -  1,
                     CX - 8, CY - ARC_R - 19,
                     CX + 8, CY - ARC_R - 19, TFT_WHITE);
}

// ─── Drawing: Rotating roll pointer ──────────────────────────────────────────
static void drawRollPointer(float roll) {
    float a  = roll - HALF_PI_F;
    float ca = cosf(a);
    float sa = sinf(a);

    int   ax  = CX + static_cast<int>(99.0f * ca);
    int   ay  = CY + static_cast<int>(99.0f * sa);
    float bcx = CX + (81.0f * ca);
    float bcy = CY + (81.0f * sa);

    int b1x = static_cast<int>(bcx - (8.0f * sa));
    int b1y = static_cast<int>(bcy + (8.0f * ca));
    int b2x = static_cast<int>(bcx + (8.0f * sa));
    int b2y = static_cast<int>(bcy - (8.0f * ca));
    spr.fillTriangle(ax, ay, b1x, b1y, b2x, b2y, C_ORANGE);
}

// ─── Drawing: Traditional symbolic airplane (fixed) ──────────────────────────
static void drawTraditionalAirplane() {
    spr.fillRect(CX - 42, CY - 2, 30, 5, C_ORANGE);  // left wing
    spr.fillRect(CX + 12, CY - 2, 30, 5, C_ORANGE);  // right wing
    spr.fillRect(CX - 17, CY + 3, 5, 8, C_ORANGE);   // left wing inner leg
    spr.fillRect(CX + 12, CY + 3, 5, 8, C_ORANGE);   // right wing inner leg
}

// ─── Drawing: Delta symbolic airplane ────────────────────────────────────────
static void drawDeltaAirplane() {
    spr.fillTriangle(CX, CY - 22, CX - 38, CY + 14, CX + 38, CY + 14, C_ORANGE);
}

// ─── Drawing: Rotated pitch-ladder labels ─────────────────────────────────────
static void drawPitchLabels(float roll, float pitchDeg) {
    float cr = cosf(roll);
    float sr = sinf(roll);
    auto rollDeg = static_cast<int16_t>(lroundf(roll * RAD_TO_DEG_F));

    struct LabelMark { int8_t deg; uint8_t hw; };
    static constexpr std::array<LabelMark, 18> PITCH_LABEL_MARKS = {{
        {-90, 25}, {-80, 25}, {-70, 25},
        {-60, 25}, {-50, 25}, {-40, 25},
        {-30, 25}, {-20, 25}, {-10, 25},
        { 10, 25}, { 20, 25}, { 30, 25},
        { 40, 25}, { 50, 25}, { 60, 25},
        { 70, 25}, { 80, 25}, { 90, 25}
    }};

    for (const auto &m : PITCH_LABEL_MARKS) {
        float delta  = (static_cast<float>(m.deg) - pitchDeg) * PX_PER_DEG;
        float mx     = CX + (delta * sr);
        float my     = CY - (delta * cr);

        float offset = static_cast<float>(m.hw) + 5.0f + 7.0f;

        float rx = mx + (offset * cr);
        float ry = my + (offset * sr);
        float lx = mx - (offset * cr);
        float ly = my - (offset * sr);

        std::array<char, 4> buf{};
        snprintf(buf.data(), buf.size(), "%d", static_cast<int>(abs(m.deg)));

        auto tryLabel = [&](float px, float py) {
            float dx = px - CX, dy = py - CY;
            if (((dx * dx) + (dy * dy)) > 100.0f * 100.0f) { return; }
            numSpr.fillSprite(TFT_BLACK);
            numSpr.drawString(buf.data(), 1, 1);
            numSpr.setPivot(7, 5);
            spr.setPivot(px, py);
            numSpr.pushRotated(&spr, rollDeg, TFT_BLACK);
        };

        tryLabel(rx, ry);
        tryLabel(lx, ly);
    }
}

// ─── Slip Indicator (Libelle) ─────────────────────────────────────────────────
// Curved tube at the bottom of the display, spanning ±25° from 6-o'clock.
// ballPos: normalised value −1.0…+1.0 (0 = centred)
//   LIVE-Modus:  ballPos = INCIDENCE BETA / SLIP_MAX_OFF
//   Demo-Modus:  ballPos = atan2(accX, -accY) / SLIP_MAX_OFF  (IMU-basiert)
static constexpr float SLIP_R       = 110.0f;
static constexpr int   SLIP_TUBE_R  =  10;
static constexpr int   SLIP_BALL_R  =   9;
static constexpr float SLIP_MAX_OFF =  20.0f * DEG_TO_RAD_F;  // ±20° Tube-Ende

static void drawSlipIndicator(float ballPos) {
    // 1. Tube (white): sweep filled circles along arc
    for (int da = -25; da <= 25; da++) {
        float a = HALF_PI_F + (static_cast<float>(da) * DEG_TO_RAD_F);
        spr.fillCircle(CX + static_cast<int>(SLIP_R * cosf(a)),
                       CY + static_cast<int>(SLIP_R * sinf(a)),
                       SLIP_TUBE_R, TFT_WHITE);
    }

    // 2. Centre reference marks: 3-px-wide radial black lines left & right
    float markOff = (static_cast<float>(SLIP_BALL_R) + 2.0f) / SLIP_R;
    for (int s : {-1, 1}) {
        float a  = HALF_PI_F + (static_cast<float>(s) * markOff);
        float ca = cosf(a);
        float sa = sinf(a);
        float ri = SLIP_R - static_cast<float>(SLIP_TUBE_R);
        float ro = SLIP_R + static_cast<float>(SLIP_TUBE_R);
        for (int t = -1; t <= 1; t++) {
            spr.drawLine(CX + static_cast<int>((ri * ca) - (static_cast<float>(t) * sa)),
                         CY + static_cast<int>((ri * sa) + (static_cast<float>(t) * ca)),
                         CX + static_cast<int>((ro * ca) - (static_cast<float>(t) * sa)),
                         CY + static_cast<int>((ro * sa) + (static_cast<float>(t) * ca)),
                         TFT_BLACK);
        }
    }

    // 3. Ball: angle from 6-o'clock derived from normalised ballPos
    float ballA = HALF_PI_F - (ballPos * SLIP_MAX_OFF);
    ballA = std::max(HALF_PI_F - SLIP_MAX_OFF, std::min(HALF_PI_F + SLIP_MAX_OFF, ballA));
    spr.fillCircle(CX + static_cast<int>(SLIP_R * cosf(ballA)),
                   CY + static_cast<int>(SLIP_R * sinf(ballA)),
                   SLIP_BALL_R, TFT_BLACK);
}

// ─── Main frame compositor ────────────────────────────────────────────────────
// invertedFill is derived internally from |roll| > 90°.
static void drawFrame(float roll, float pitchDeg, float ballPos) {
    bool invertedFill = (fabsf(roll) > HALF_PI_F);

    drawHorizonBg(roll, pitchDeg, invertedFill);
    drawPitchLadder(roll, pitchDeg);

    if (CFG.rollDial == RollDialType::Fixed) {
        drawRollScale();
    }

    if (CFG.rollPointer == PointerType::Rotating) {
        drawRollPointer(roll);
    } else {
        spr.fillTriangle(CX, CY - 93, CX - 5, CY - 85, CX + 5, CY - 85, TFT_WHITE);
    }

    if (CFG.airplane == AirplaneType::Traditional) {
        drawTraditionalAirplane();
    } else {
        drawDeltaAirplane();
    }

    drawSlipIndicator(ballPos);
    drawPitchLabels(roll, pitchDeg);

    spr.fillCircle(CX, CY, 3, C_ORANGE);
}

// ─── Overlays (ins Sprite, vor pushSprite) ───────────────────────────────────
// Position: 70° (Uhrzeigersinn ab 12 Uhr), r = STATUS_DOT_R = 110 px
//   x = 120 + round(110 · sin 70°) = 223
//   y = 120 − round(110 · cos 70°) =  82
static constexpr int OVERLAY_X = 223;
static constexpr int OVERLAY_Y =  82;

// Hilfsfunktion: Buchstabe bei 70° mit schwarzem Schlagschatten, transparent, Bold
static void drawOverlayChar(const char* ch) {
    spr.setTextDatum(MC_DATUM);
    spr.setTextFont(4);
    spr.setTextColor(TFT_BLACK);                          // Schlagschatten
    spr.drawString(ch, OVERLAY_X + 1, OVERLAY_Y + 1);
    spr.setTextColor(TFT_RED);                            // Hauptzeichen + Bold
    spr.drawString(ch, OVERLAY_X,     OVERLAY_Y);
    spr.drawString(ch, OVERLAY_X + 1, OVERLAY_Y);
}

// Rotes "D" bei 70° — zeigt Demo-/IMU-Modus an
static void drawDemoIndicator() { drawOverlayChar("D"); }

// Rotes "F" bei 70° — PARTIAL PANEL ATTITUDE = 1 (Instrumentenwarnung)
static void drawFailFlag()      { drawOverlayChar("F"); }

// Schwarzer Bildschirm mit rotem "Error" — PARTIAL PANEL ATTITUDE = 2
static void displayBlank() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_RED);
    tft.setTextFont(2);
    tft.drawString("Error", CX, CY);
}

// ─── Status-Dot auf dem Rollbogen ─────────────────────────────────────────────
// Position: 135° (Uhrzeigersinn ab 12 Uhr), r = 110 px → x ≈ 198, y ≈ 198
// Wird ins Sprite gezeichnet (vor pushSprite), nicht direkt auf das TFT.
static void drawStatusDot(uint16_t color) {
    constexpr float a = STATUS_DOT_ANGLE_DEG * DEG_TO_RAD_F;
    const int x = CX + static_cast<int>(STATUS_DOT_R * sinf(a));  // ≈ 198
    const int y = CY - static_cast<int>(STATUS_DOT_R * cosf(a));  // ≈ 198
    spr.fillCircle(x, y, STATUS_DOT_RADIUS, color);
}

// ─── CAN: ISR (muss vor begin() definiert sein, IRAM_ATTR für ISR-Kontext) ───
static void IRAM_ATTR canISR() { g_can.poll(); }

// ─── CAN: Initialisierung ────────────────────────────────────────────────────
static bool initCAN() {
    g_canSpi.begin(CAN_SCK, CAN_MISO, CAN_MOSI, -1);

    ACAN2517FDSettings settings(CAN_OSC_FREQ, CAN_ARB_BPS, DataBitRateFactor::x1);
    settings.mRequestedMode              = ACAN2517FDSettings::NormalFD;
    settings.mControllerTransmitFIFOSize = 8;   // Default=1; explizit für Konsistenz
    settings.mControllerReceiveFIFOSize  = 8;   // Default=27 → 1944B, knapp unter 2048B-Limit

    ACAN2517FDFilters filters;
    filters.appendPassAllFilter(nullptr);

    // ── Loopback-Selbsttest (vor NormalFD-Betrieb) ────────────────────────────
    // canISR nötig: mINT=17 (≠255) → Library setzt kISRIsNull (0x20) bei nullptr
    // und bricht vor dem SPI-Test ab. Im InternalLoopBack-Modus feuert INT intern,
    // canISR/poll() ist korrekt.
    {
        ACAN2517FDSettings lbSettings(CAN_OSC_FREQ, CAN_ARB_BPS, DataBitRateFactor::x1);
        lbSettings.mRequestedMode              = ACAN2517FDSettings::InternalLoopBack;
        lbSettings.mControllerTransmitFIFOSize = 8;
        lbSettings.mControllerReceiveFIFOSize  = 8;
        ACAN2517FDFilters lbFilters;
        lbFilters.appendPassAllFilter(nullptr);

        const uint32_t lbErr = g_can.begin(lbSettings, canISR, lbFilters);
        if (lbErr == 0) {
            CANFDMessage txMsg;
            txMsg.id      = 0x7FF;
            txMsg.ext     = false;
            txMsg.len     = 4;
            txMsg.type    = CANFDMessage::CANFD_WITH_BIT_RATE_SWITCH;
            txMsg.data[0] = 0xDE; txMsg.data[1] = 0xAD;
            txMsg.data[2] = 0xBE; txMsg.data[3] = 0xEF;
            g_can.tryToSend(txMsg);

            bool lbOk = false;
            for (int i = 0; i < 20 && !lbOk; ++i) {
                delay(2);
                CANFDMessage rx;
                bool got = g_can.receive(rx);
                if (!got) got = g_can.receive(rx);   // double-poll im Polling-Modus
                if (got && rx.id == txMsg.id && rx.len >= 4
                    && rx.data[0] == 0xDE && rx.data[1] == 0xAD
                    && rx.data[2] == 0xBE && rx.data[3] == 0xEF)
                    lbOk = true;
            }
            if (DEBUG_LEVEL >= 1)
                Serial.printf("CAN Loopback: %s  OSC=%lu MHz  Arb=%lu kbit/s\n",
                              lbOk ? "OK" : "FEHLER!",
                              (unsigned long)CAN_OSC_MHZ,
                              (unsigned long)(CAN_ARB_BPS / 1000UL));

            // end() stoppt den FreeRTOS-Task (myESP32Task, Priority 16) und die ISR des
            // Loopback-begin(). Ohne end() läuft der Task parallel zu NormalFD-begin() weiter
            // und korrumpiert den 800kHz-SPI-Readback-Test → kReadBackErrorWith1MHzSPIClock (0x02).
            // Nur aufrufen wenn begin() erfolgreich war — end() nach fehlgeschlagenem begin()
            // ruft detachInterrupt() auf einen nie registrierten ISR auf → abort().
            g_can.end();
        } else {
            Serial.printf("CAN Loopback: begin() fehlgeschlagen  Code:0x%08lX\n", (unsigned long)lbErr);
        }
    }

    // ── NormalFD-Betrieb mit ISR ──────────────────────────────────────────────
    const uint32_t err = g_can.begin(settings, canISR, filters);
    if (err != 0) {
        Serial.printf("CAN Init FEHLER: 0x%08lX\n", (unsigned long)err);  // immer
        g_canInitOk = false;
        return false;
    }
    g_canInitOk = true;
    if (DEBUG_LEVEL >= 1)
        Serial.printf("CAN: OK  OSC=%lu MHz  Arb=%lu kbit/s\n",
                      (unsigned long)CAN_OSC_MHZ,
                      (unsigned long)(CAN_ARB_BPS / 1000UL));
    return true;
}

// ─── NVS: Konfiguration laden / speichern ────────────────────────────────────
static void loadConfig() {
    g_prefs.begin("horizon", true);
    g_slipMode = g_prefs.getUChar("slip_mode", 0);
    g_prefs.end();
}

static void saveSlipMode(uint8_t mode) {
    g_prefs.begin("horizon", false);
    g_prefs.putUChar("slip_mode", mode);
    g_prefs.end();
}

// ─── CAN: Heartbeat senden (horizon → bridge) ────────────────────────────────
static void sendHeartbeat() {
    static uint32_t hbCount = 0;
    ++hbCount;
    const uint32_t uptime = millis();

    if (!g_canInitOk) {
        if (DEBUG_LEVEL >= 2)
            Serial.printf("HB #%lu TX:NO_CAN uptime=%lums init=%s\n",
                          (unsigned long)hbCount, (unsigned long)uptime,
                          g_initReceived ? "YES" : "no");
        return;
    }

    CANFDMessage hb;
    hb.id      = CAN_TX_ID;
    hb.ext     = false;
    hb.len     = 5;
    hb.type    = CANFDMessage::CANFD_WITH_BIT_RATE_SWITCH;
    hb.data[0] = 0x01;
    memcpy(hb.data + 1, &uptime, 4);
    const bool ok = g_can.tryToSend(hb);
    if (DEBUG_LEVEL >= 2)
        Serial.printf("HB #%lu TX:%s uptime=%lums init=%s\n",
                      (unsigned long)hbCount,
                      ok ? "ok" : "FAIL",
                      (unsigned long)uptime,
                      g_initReceived ? "YES" : "no");
}

// ─── CAN: STATUS_RESPONSE senden (Antwort auf 0xFE STATUS_REQUEST) ──────────
// Format: [0x04][seq][ts_echo 4B][simvars in INI-Reihenfolge]
// Gesamtlänge 35 Bytes → CAN-FD DLC rundet auf 48 Bytes (Padding ungenutzt).
static void sendStatusResponse(uint8_t seq, uint32_t tsEcho) {
    CANFDMessage resp;
    resp.id      = CAN_TX_ID;
    resp.ext     = false;
    resp.len     = 35;
    resp.type    = CANFDMessage::CANFD_WITH_BIT_RATE_SWITCH;
    resp.data[0] = 0x04;
    resp.data[1] = seq;
    memcpy(resp.data +  2, &tsEcho,          4);
    memcpy(resp.data +  6, &g_bank_rad,      4);
    memcpy(resp.data + 10, &g_pitch_rad,     4);
    memcpy(resp.data + 14, &g_incBeta_rad,   4);
    memcpy(resp.data + 18, &g_accelX,        4);
    memcpy(resp.data + 22, &g_accelY,        4);
    memcpy(resp.data + 26, &g_planeBankRad,  4);
    memcpy(resp.data + 30, &g_planePitchRad, 4);
    resp.data[34] = g_partPanel;
    g_can.tryToSend(resp);
}

// ─── CAN: Frame parsen ───────────────────────────────────────────────────────
// Erwartet: data[0] == 0x00 (bereits in tryReceiveFrame geprüft), len >= 30
static void parseFrame(const uint8_t* data) {
    memcpy(&g_bank_rad,      data +  1, 4);
    memcpy(&g_pitch_rad,     data +  5, 4);
    memcpy(&g_incBeta_rad,   data +  9, 4);
    memcpy(&g_accelX,        data + 13, 4);
    memcpy(&g_accelY,        data + 17, 4);
    memcpy(&g_planeBankRad,  data + 21, 4);
    memcpy(&g_planePitchRad, data + 25, 4);
    g_partPanel = data[29];
}

// ─── CAN: Frame empfangen ────────────────────────────────────────────────────
static bool tryReceiveFrame() {
    if (!g_canInitOk) return false;

    CANFDMessage msg;
    // ISR-Modus: canISR() hat poll() bereits ausgeführt → Frame liegt im Buffer.
    if (!g_can.receive(msg)) { return false; }

    if (DEBUG_LEVEL >= 2)
        Serial.printf("CAN-RX: ID=0x%03lX len=%u type=0x%02X\n",
                      (unsigned long)msg.id, msg.len,
                      msg.len > 0 ? msg.data[0] : 0xFF);

    if (msg.id != CAN_RX_ID) { return false; }
    if (msg.len < 1)          { return false; }

    const uint8_t pktType = msg.data[0];

    // 0xFC Init — bridge_exe hat Heartbeat gesehen und antwortet (analog COMMIT)
    if (pktType == 0xFC) {
        g_initReceived = true;
        if (DEBUG_LEVEL >= 1)
            Serial.println("CAN-RX: 0xFC INIT empfangen → g_initReceived=true");
        return false;
    }

    // 0xFB Config — slip_mode empfangen und in NVS speichern
    if (pktType == 0xFB && msg.len >= 2) {
        const uint8_t n = msg.data[1];
        for (uint8_t i = 0; i < n && (2u + i * 2u + 1u) < msg.len; ++i) {
            const uint8_t cfgId = msg.data[2 + i * 2];
            const uint8_t val   = msg.data[3 + i * 2];
            if (cfgId == 0x01) {   // slip_mode
                const uint8_t newMode = (val <= 1u) ? val : 0u;
                if (newMode != g_slipMode) {
                    g_slipMode    = newMode;
                    g_slipFiltDeg = 0.0f;
                    saveSlipMode(newMode);
                    if (DEBUG_LEVEL >= 1)
                        Serial.printf("CAN-RX: 0xFB slip_mode=%u gespeichert\n", newMode);
                }
            }
        }
        return false;
    }

    // 0xFD Heartbeat-Interval-Konfiguration
    if (pktType == 0xFD && msg.len >= 5) {
        uint32_t iv = 0;
        memcpy(&iv, msg.data + 1, 4);
        if (iv >= 100u && iv <= 60000u) {
            g_hbIntervalMs = iv;
            g_initReceived = true;   // bridge sendet 0xFD nur nach erstem HB → Verbindung steht
            if (DEBUG_LEVEL >= 1)
                Serial.printf("CAN-RX: 0xFD HB-Interval=%lums → g_initReceived=true\n",
                              (unsigned long)iv);
        }
        return false;
    }

    // 0xFE STATUS_REQUEST → sofort mit STATUS_RESPONSE antworten
    if (pktType == 0xFE && msg.len >= 6) {
        const uint8_t seq = msg.data[1];
        uint32_t tsEcho   = 0;
        memcpy(&tsEcho, msg.data + 2, 4);
        if (DEBUG_LEVEL >= 2)
            Serial.printf("CAN-RX: 0xFE STATUS_REQUEST seq=%u → antworte\n", seq);
        sendStatusResponse(seq, tsEcho);
        return false;
    }

    if (pktType != 0x00)              { return false; }  // nur Daten-Pakete
    if (msg.len < CAN_FRAME_MIN_LEN)  { return false; }
    parseFrame(msg.data);
    return true;
}

// ─── Slip-Libelle: physikalische Berechnung (Modus 1: ACCEL_DAMPED) ──────────
// Formel: netLat = ACCEL_BODY_X [ft/s²] + g·sin(bank)·cos(pitch)
//         Zielwinkel = atan2(netLat, g·cos(bank)·cos(pitch))
//         Low-pass (Öldämpfung): alpha = dt / (TAU + dt)
// Rückgabe: normierter Ausschlag -1..+1 (±SLIP_MAX_DEG = Vollausschlag)
static constexpr float SLIP_TAU_S   = 0.5f;   // Öldämpfung Zeitkonstante
static constexpr float SLIP_MAX_DEG = 25.0f;  // Vollausschlag in Grad

static float computeSlipAccel(float dt_s) {
    constexpr float G_FT_S2 = 32.174f;   // g in ft/s² (Einheit von ACCEL_BODY_X/Y)

    const float sinBank  = sinf(g_planeBankRad);
    const float cosBank  = cosf(g_planeBankRad);
    const float cosPitch = cosf(g_planePitchRad);

    const float gLat   = G_FT_S2 * sinBank * cosPitch;
    const float netLat = g_accelX + gLat;
    const float gNorm  = G_FT_S2 * cosBank * cosPitch;

    float targetDeg = 0.0f;
    if (fabsf(gNorm) > 0.1f)
        targetDeg = atan2f(netLat, gNorm) * RAD_TO_DEG_F;
    targetDeg = std::max(-SLIP_MAX_DEG, std::min(SLIP_MAX_DEG, targetDeg));

    const float alpha = dt_s / (SLIP_TAU_S + dt_s);
    g_slipFiltDeg += alpha * (targetDeg - g_slipFiltDeg);

    return std::max(-1.0f, std::min(1.0f, g_slipFiltDeg / SLIP_MAX_DEG));
}

// ─── IMU filter state ─────────────────────────────────────────────────────────
static bool  imuOk  = false;
static Vec3f filtAcc{0, -1, 0};
static Vec3f filtGyr{0,  0, 0};
static constexpr float ALPHA = 0.10f;

// ─── Setup ───────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    if (DEBUG_LEVEL >= 1)
        Serial.printf("\n=== horizon Boot ===  TX:0x%03lX  RX:0x%03lX  %lu kbit/s\n",
                      (unsigned long)CAN_TX_ID, (unsigned long)CAN_RX_ID,
                      (unsigned long)(CAN_ARB_BPS / 1000UL));

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, TFT_BACKLIGHT_ON);

    tft.init();
    tft.setRotation(0);
    tft.fillScreen(TFT_BLACK);

    numSpr.createSprite(14, 10);
    numSpr.setTextColor(TFT_WHITE);   // TFT_BLACK = transparency key
    numSpr.setTextFont(1);            // default 6×8 font

    if (spr.createSprite(240, 240) == nullptr) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_RED);
        tft.drawString("Sprite: kein RAM!", CX, CY);
        Serial.println("Sprite alloc failed");  // immer: fataler Fehler
        while (true) { delay(1000); }
    }

    loadConfig();   // Schicht 1: slip_mode sofort aus NVS (analog NVS-loadDecodeTable)
    if (DEBUG_LEVEL >= 1) Serial.printf("NVS: slip_mode=%u\n", g_slipMode);

    imuOk = initIMU();
    if (!imuOk) {
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_RED);
        tft.drawString("IMU nicht gefunden!", CX, CY - 8);
        tft.drawString("QMI8658 @ 0x6B?",    CX, CY + 8);
        Serial.println("IMU: nicht gefunden");          // immer: Hardwarefehler
    } else {
        if (DEBUG_LEVEL >= 1) Serial.println("IMU: OK");
    }

    if (!initCAN()) {
        // CAN-Initialisierungsfehler ist nicht fatal — Demo-Modus über IMU läuft weiter.
        // Fehlermeldung erscheint kurz auf dem Display und wird beim ersten drawFrame()
        // durch das Sprite überschrieben.
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_YELLOW);
        tft.drawString("CAN Init fehler!", CX, CY + 30);
    }

    if (DEBUG_LEVEL >= 1)
        Serial.printf("Setup fertig  canOk=%s  imuOk=%s\n",
                      g_canInitOk ? "ja" : "nein",
                      imuOk       ? "ja" : "nein");

    // Schicht 2: sofortige Anfrage an bridge_exe (analog sendMapRequest beim Boot)
    sendHeartbeat();
    g_lastHbMs    = millis();
    g_lastFrameMs = millis();   // LIVE→DEMO-Zähler ab boot-fertig, nicht ab t=0
}

// ─── Loop ────────────────────────────────────────────────────────────────────
void loop() {
    const uint32_t now = millis();
    const float dt_s = (g_lastLoopMs > 0)
                       ? static_cast<float>(now - g_lastLoopMs) * 0.001f
                       : 0.025f;
    g_lastLoopMs = now;

    // ── CAN empfangen → Zustand auf LIVE setzen ────────────────────────────────
    if (tryReceiveFrame()) {
        if (g_state == BridgeState::DEMO) {
            g_slipFiltDeg = 0.0f;
            if (DEBUG_LEVEL >= 1) Serial.println("State: DEMO→LIVE");
        }
        g_lastFrameMs = now;
        g_state = BridgeState::LIVE;
    }

    // ── Heartbeat: immer senden, vor allen Early-Return-Pfaden ───────────────
    {
        const uint32_t hbInterval = g_initReceived ? g_hbIntervalMs : BOOT_HB_INTERVAL_MS;
        if (now - g_lastHbMs >= hbInterval) {
            g_lastHbMs = now;
            sendHeartbeat();
        }
    }

    // ── Timeout-Übergang LIVE→DEMO ────────────────────────────────────────────
    const uint32_t elapsed = now - g_lastFrameMs;
    if (g_state == BridgeState::LIVE && elapsed >= TIMEOUT_LIVE_MS) {
        g_state = BridgeState::DEMO;
        if (DEBUG_LEVEL >= 1)
            Serial.printf("State: LIVE→DEMO (kein CAN-Frame seit %lus)\n",
                          (unsigned long)(elapsed / 1000UL));
    }

    // ── Totalausfall (PARTIAL PANEL = 2): schwarzer Bildschirm + "Error" ──────
    // Kein drawFrame(). Status-Dot direkt auf TFT (kein Sprite in diesem Pfad).
    if (g_state == BridgeState::LIVE && g_partPanel == 2) {
        displayBlank();
        constexpr float a = STATUS_DOT_ANGLE_DEG * DEG_TO_RAD_F;
        tft.fillCircle(CX + static_cast<int>(STATUS_DOT_R * sinf(a)),
                       CY - static_cast<int>(STATUS_DOT_R * cosf(a)),
                       STATUS_DOT_RADIUS, TFT_RED);
        delay(100);
        return;
    }

    // ── Datenquelle wählen ────────────────────────────────────────────────────
    float roll, pitchDeg, ballPos;

    if (g_state == BridgeState::LIVE) {
        roll     = -g_bank_rad;
        pitchDeg = -g_pitch_rad * RAD_TO_DEG_F;
        if (g_slipMode == 1) {
            ballPos = computeSlipAccel(dt_s);
        } else {
            ballPos = std::max(-1.0f, std::min(1.0f, g_incBeta_rad / SLIP_MAX_OFF));
        }

    } else {
        // DEMO → IMU
        if (!imuOk) { delay(20); return; }

        Vec3f acc{}, gyr{};
        if (!readIMU(acc, gyr)) { delay(20); return; }

        filtAcc.x += ALPHA * (acc.x - filtAcc.x);
        filtAcc.y += ALPHA * (acc.y - filtAcc.y);
        filtAcc.z += ALPHA * (acc.z - filtAcc.z);
        filtGyr.x += ALPHA * (gyr.x - filtGyr.x);
        filtGyr.y += ALPHA * (gyr.y - filtGyr.y);
        filtGyr.z += ALPHA * (gyr.z - filtGyr.z);

        roll = atan2f(filtAcc.x, -filtAcc.y);
        const float mag_xy = sqrtf((filtAcc.x * filtAcc.x) + (filtAcc.y * filtAcc.y));
        pitchDeg = atan2f(filtAcc.z, mag_xy) * RAD_TO_DEG_F;
        ballPos  = std::max(-1.0f, std::min(1.0f,
                       atan2f(filtAcc.x, -filtAcc.y) / SLIP_MAX_OFF));
    }

    // ── Horizont zeichnen ─────────────────────────────────────────────────────
    drawFrame(roll, pitchDeg, ballPos);

    // ── Overlays + Status-Dot ins Sprite, dann einmaliger Push ──────────────
    if (g_state == BridgeState::LIVE) {
        if (g_partPanel == 1) { drawFailFlag(); }
        drawStatusDot(g_partPanel != 0 ? TFT_RED : TFT_GREEN);
    } else if (g_state == BridgeState::DEMO) {
        drawDemoIndicator();
        if (now - g_dotBlinkMs >= 3000) {
            g_dotBlinkMs  = now;
            g_dotBlinkRed = !g_dotBlinkRed;
        }
        drawStatusDot(g_dotBlinkRed ? TFT_RED : TFT_GREEN);
    }

    spr.pushSprite(0, 0);

    delay(20);
}
