# Aurora Ground Station

This project represents the ground station for the Aurora Rocketry Team. It is designed to visualize real-time telemetry data from the rocket during flight and simulations.

The application is built with **Svelte** (Frontend) and uses a **Python** bridge to handle data ingestion from various sources (Serial, CSV replay, or Random generation) and broadcast it via WebSockets.

## Features

*   **Real-time Dashboard**: Visualize critical flight data including:
    *   Acceleration, Velocity, and Altitude plots.
    *   Sensor data (IMU, Barometers, Accelerometer).
    *   Current flight stage status.
*   **Command Center**: Interface for sending commands to the rocket.
*   **Flexible Data Sources**:
    *   **Serial**: Connect directly to the ground station hardware via USB.
    *   **File Replay**: Replay flight data from CSV files for analysis and testing.
    *   **Simulation**: Generate random synthetic data for UI testing.
*   **Dual Connection Mode**: Supports both WebSocket connection (via Python bridge) and direct Web Serial connection.

## Project Structure

```
aurora-ground-station/
├── public/                 # Static assets
├── src/
│   ├── lib/                # Svelte components
│   │   ├── CommandCenter.svelte
│   │   ├── Counter.svelte
│   │   ├── Dashboard.svelte
│   │   ├── Header.svelte
│   │   └── utils.js
│   ├── assets/             # Project assets
│   ├── App.svelte          # Main application component
│   ├── bridge.py           # Python WebSocket telemetry bridge
│   ├── main.js             # Entry point
│   └── simulation_data.csv # Sample data for simulation
├── index.html              # HTML entry point
├── package.json            # Node.js dependencies and scripts
├── svelte.config.js        # Svelte configuration
└── vite.config.js          # Vite configuration
```

## Prerequisites

Before you begin, ensure you have the following installed:

*   **Node.js** (v16 or higher)
*   **npm** (usually comes with Node.js)
*   **Python 3.8+**

## Installation

1.  **Clone the repository:**
    ```bash
    git clone https://github.com/AuroraRocketryTeam/aurora-ground-station.git
    cd aurora-ground-station
    ```

2.  **Install Frontend Dependencies:**
    ```bash
    npm install
    ```

3.  **Install Python Dependencies:**
    You will need `websockets`, `pyserial`, and `pandas`.
    ```bash
    pip install websockets pyserial pandas
    ```

## Usage

To run the full ground station, you typically need to run both the Frontend (UI) and the Backend Bridge (Data Source).

### 1. Start the Frontend

Start the Vite development server:

```bash
npm run dev
```

Open your browser and navigate to the URL shown (usually `http://localhost:5173`).

### 2. Start the Telemetry Bridge

The `bridge.py` script acts as a server that feeds data to the frontend. It supports multiple modes:

**Mode A: File Replay (Default)**
Replays data from a CSV file. Useful for debriefing or testing with real flight data.
```bash
python src/bridge.py --source FILE --file src/simulation_data.csv
```

**Mode B: Random Simulation**
Generates random data to test the UI responsiveness.
```bash
python src/bridge.py --source RANDOM
```

**Mode C: Serial Connection (Real Hardware)**
Reads data from a connected USB device (e.g., LoRa receiver).
```bash
python src/bridge.py --source SERIAL --port COM3
```
*Note: Replace `COM3` with your actual serial port (e.g., `/dev/ttyUSB0` on Linux/Mac).*

## Connection Settings

In the Frontend UI:
1.  Select **Connection Type**:
    *   **WebSocket**: Connects to the `bridge.py` script (default `ws://localhost:8765`).
    *   **Serial**: Connects directly to a serial device using the Web Serial API (Chrome/Edge only).
2.  Click **Connect**.