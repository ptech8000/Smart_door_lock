# 🔒 Smart Door Lock System

<p align="center">
  <img src="https://img.shields.io/badge/Platform-ESP32-blue?style=for-the-badge">
  <img src="https://img.shields.io/badge/Language-C%2B%2B%20(Arduino)-orange?style=for-the-badge">
  <img src="https://img.shields.io/badge/Authentication-Multi--Factor-red?style=for-the-badge">
  <img src="https://img.shields.io/badge/Status-Production%20Ready-success?style=for-the-badge">
  <img src="https://img.shields.io/badge/License-MIT-brightgreen?style=for-the-badge">
</p>

<p align="center">
  <b>A Secure Multi‑Factor IoT Door Lock with Remote Access & Real‑time Logging</b>
</p>

---

## 📌 Project Overview

The **Smart Door Lock System** is an advanced embedded security solution that replaces traditional keys with multiple authentication methods. Built around an **ESP32**, it integrates RFID, PIN keypad, fingerprint sensor, and remote controls via Wi-Fi/BLE.

### ✅ Key Capabilities

- Multi‑factor authentication (PIN + RFID + Fingerprint)
- Remote lock/unlock from mobile App or Web dashboard
- Real‑time access logs & tamper alerts
- Low‑power operation for battery backup
- Expandable user management (temporary access codes)

---

## 🧠 Core Architecture

The system is built on a **central ESP32 controller** that interfaces with multiple input/output modules:

| Module                     | Function                          |
|----------------------------|-----------------------------------|
| 🖥 ESP32 Main Board        | Decision logic & communication    |
| 🔢 4×4 Matrix Keypad       | PIN entry                         |
| 🆔 RFID Reader (MFRC522)   | Card / tag authentication         |
| 👆 Fingerprint Sensor (R307) | Biometric verification          |
| 🔐 Solenoid / Deadbolt     | Physical lock actuator            |
| 📡 Wi-Fi / BLE             | Remote access & notifications     |

---

## 📡 Communication Flow

```text
                    ┌──────────────────────────┐
                    │     User / Mobile App     │
                    └────────────┬─────────────┘
                                 │ (HTTPS / MQTT / BLE)
                    ┌────────────▼─────────────┐
                    │   ESP32 Main Controller  │
                    └────┬──────────────┬──────┘
                         │              │
    ┌────────────────────▼──┐      ┌────▼─────────────────┐
    │ Local Authentication  │      │ Remote Cloud / MQTT  │
    │ - Keypad              │      │ - AWS IoT / Firebase │
    │ - RFID                │      │ - Push notifications │
    │ - Fingerprint         │      │ - Web Dashboard      │
    └────────────────────┬──┘      └────┬─────────────────┘
                         │              │
                    ┌────▼──────────────▼────┐
                    │   Lock Actuator (GPIO) │
                    └────────────────────────┘
