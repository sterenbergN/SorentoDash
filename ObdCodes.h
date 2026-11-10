// Plain-English descriptions for common generic OBD-II trouble codes.
// Anything not listed shows just the code (search it online for details).
#pragma once
#include <Arduino.h>

struct DtcInfo {
  const char *code;
  const char *text;
};

static const DtcInfo DTC_TABLE[] = {
    {"P0010", "Cam timing actuator circuit (B1)"},
    {"P0011", "Cam timing over-advanced (B1)"},
    {"P0016", "Crank/cam position correlation"},
    {"P0030", "O2 heater control circuit (B1S1)"},
    {"P0036", "O2 heater control circuit (B1S2)"},
    {"P0100", "Mass air flow circuit"},
    {"P0101", "Mass air flow range/performance"},
    {"P0102", "Mass air flow circuit low"},
    {"P0103", "Mass air flow circuit high"},
    {"P0106", "MAP sensor range/performance"},
    {"P0110", "Intake air temp circuit"},
    {"P0113", "Intake air temp circuit high"},
    {"P0115", "Coolant temp circuit"},
    {"P0116", "Coolant temp range/performance"},
    {"P0117", "Coolant temp circuit low"},
    {"P0118", "Coolant temp circuit high"},
    {"P0120", "Throttle position circuit"},
    {"P0121", "Throttle position range/performance"},
    {"P0125", "Coolant too cold for closed loop"},
    {"P0128", "Thermostat stuck open (runs cold)"},
    {"P0130", "O2 sensor circuit (B1S1)"},
    {"P0131", "O2 sensor low voltage (B1S1)"},
    {"P0133", "O2 sensor slow response (B1S1)"},
    {"P0134", "O2 sensor no activity (B1S1)"},
    {"P0135", "O2 heater circuit (B1S1)"},
    {"P0136", "O2 sensor circuit (B1S2)"},
    {"P0141", "O2 heater circuit (B1S2)"},
    {"P0150", "O2 sensor circuit (B2S1)"},
    {"P0155", "O2 heater circuit (B2S1)"},
    {"P0161", "O2 heater circuit (B2S2)"},
    {"P0171", "System too lean (bank 1)"},
    {"P0172", "System too rich (bank 1)"},
    {"P0174", "System too lean (bank 2)"},
    {"P0175", "System too rich (bank 2)"},
    {"P0300", "Random/multiple misfire"},
    {"P0301", "Cylinder 1 misfire"},
    {"P0302", "Cylinder 2 misfire"},
    {"P0303", "Cylinder 3 misfire"},
    {"P0304", "Cylinder 4 misfire"},
    {"P0305", "Cylinder 5 misfire"},
    {"P0306", "Cylinder 6 misfire"},
    {"P0325", "Knock sensor circuit"},
    {"P0335", "Crankshaft position sensor"},
    {"P0340", "Camshaft position sensor"},
    {"P0400", "EGR flow malfunction"},
    {"P0401", "EGR flow insufficient"},
    {"P0420", "Catalyst efficiency low (bank 1)"},
    {"P0430", "Catalyst efficiency low (bank 2)"},
    {"P0440", "EVAP system malfunction"},
    {"P0441", "EVAP purge flow incorrect"},
    {"P0442", "EVAP small leak (check gas cap)"},
    {"P0443", "EVAP purge valve circuit"},
    {"P0446", "EVAP vent control circuit"},
    {"P0449", "EVAP vent valve circuit"},
    {"P0451", "EVAP pressure sensor range"},
    {"P0455", "EVAP large leak (check gas cap)"},
    {"P0456", "EVAP very small leak"},
    {"P0457", "EVAP leak - fuel cap loose/off"},
    {"P0500", "Vehicle speed sensor"},
    {"P0505", "Idle control system"},
    {"P0506", "Idle speed lower than expected"},
    {"P0507", "Idle speed higher than expected"},
    {"P0562", "System voltage low"},
    {"P0563", "System voltage high"},
    {"P0600", "Engine computer communication"},
    {"P0700", "Transmission control fault"},
    {"P0705", "Transmission range sensor"},
    {"P0715", "Input/turbine speed sensor"},
    {"P0720", "Output speed sensor"},
    {"P0730", "Incorrect gear ratio"},
    {"P0741", "Torque converter clutch stuck off"},
    {"P0750", "Shift solenoid A"},
    {"P0755", "Shift solenoid B"},
    {"P1326", "Knock sensor detection (Hyundai/Kia)"},
    {"P2096", "Post-catalyst too lean (bank 1)"},
    {"P2097", "Post-catalyst too rich (bank 1)"},
};

static const char *dtcDescription(const String &code) {
  for (auto &d : DTC_TABLE)
    if (code == d.code) return d.text;
  return nullptr;
}
