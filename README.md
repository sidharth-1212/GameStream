# ⚡ GameStream: Low-Latency Streaming Infrastructure

> **Bare-metal optimization for the physical world.** <br>
> Engineered by [Architech Systems](https://architechsystems.vercel.app).

[![NVENC](https://img.shields.io/badge/Encoding-NVENC_Hardware-76B900.svg)]()
[![Audio](https://img.shields.io/badge/Audio-Opus_Codec-blue.svg)]()
[![Network](https://img.shields.io/badge/Network-Tailscale_Zero_Trust-black.svg)]()
[![License](https://img.shields.io/badge/License-Proprietary-red.svg)]()

## 🏗️ System Overview

Traditional remote desktop protocols and off-the-shelf streaming solutions collapse under high-framerate, low-latency demands. They introduce unacceptable input lag, audio desync, and resolution tearing when forced to operate outside their theoretical ideals.

**GameStream** is a custom-engineered host and client architecture built from the ground up for sub-millisecond local network streaming. By bypassing bloated streaming frameworks, it achieves bare-metal hardware acceleration, ensuring native-feel gameplay across secure network tunnels.

---

## ⚙️ Core Architecture

GameStream was built under the strict High-Tech Minimalist engineering doctrine: sub-second processing targets and zero-trust networking.

### Tech Stack
* **Video Encoding:** NVIDIA NVENC (Hardware-accelerated, zero-copy pipeline).
* **Audio Encoding:** Opus Codec (Ultra-low latency, high-fidelity spatial audio).
* **Network Infrastructure:** Tailscale (Encrypted, zero-trust peer-to-peer mesh networking).
* **Input Handling:** Raw input interception for absolute mouse positioning and peripheral passthrough.

### Feature Arsenal
* **Hardware-Accelerated Pipeline:** Offloads full encode/decode logic to the GPU, keeping CPU overhead strictly under 5%.
* **Absolute Mouse Positioning:** Custom logic to intercept and translate coordinates natively, eliminating standard remote-desktop cursor drift and resolution mismatch bugs.
* **Zero-Trust Tunnels:** All transmission data is securely encrypted over a Tailscale mesh network, preventing local snooping or unauthorized host access.
* **Dynamic Resolution Adapting:** Instantly corrects host-to-client resolution scaling without requiring manual monitor adjustments.
