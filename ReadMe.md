# Smart EV Charging Station Optimizer

**ESP32 | MQTT | ThingsBoard | Edge AI**

A smart EV charging management system designed to optimize power distribution across multiple charging bays under limited grid capacity.

## Overview

The system simulates a multi-bay EV charging station where each bay reports its electrical parameters to an ESP32. Data is transmitted through MQTT to ThingsBoard for real-time monitoring and analysis.

An optimization layer dynamically manages charging power through load balancing, throttling, and charging deferral. Edge AI enables low-latency local decisions when cloud connectivity is unavailable.

## System Architecture

```text
EV Charging Bays
       |
       v
     ESP32
       |
      MQTT
       |
       v
  ThingsBoard
       |
       v
AI / Optimization
       |
       v
Load Management
```

## Key Features

- Real-time charging and load monitoring
- Multi-bay power management
- Dynamic load balancing
- Charging throttling and deferral
- Demand prediction
- Edge-based decision making
- Cloud-based monitoring through ThingsBoard
- ESP32-based IoT architecture

## Technologies

- ESP32
- Embedded C/C++
- MQTT
- ThingsBoard
- Edge AI
- IoT
- Optimization Algorithms
- ESP32 Simulation

## Example Scenario

If the station has **5 kW** available but three vehicles request a combined **8 kW**, the optimizer distributes the available 5 kW without exceeding the station's capacity.

## Project Goal

To develop an intelligent EV charging system that efficiently manages limited electrical capacity using **IoT, cloud monitoring, Edge AI, and optimization techniques**.
