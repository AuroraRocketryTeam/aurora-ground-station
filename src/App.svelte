<script>
  import { onMount, onDestroy } from 'svelte';

  // Navigation state
  let activePage = "dashboard"; 

  // Connection state
  let connectionType = "websocket"; // 'websocket' or 'serial'
  let status = "DISCONNECTED";
  let isConnected = false;
  
  // WebSocket State
  let socket;
  
  // Serial State
  let serialPort;
  let serialReader;
  let keepReadingSerial = false;
  let incomingBuffer = "";

  // Track if we have received the first data packet (for plotting initialization)
  let isInitialized = false; 

  // Dashboard data
  let currentStage = "Inactive";
  let imu = {};
  let baro1 = {};
  let baro2 = {};
  let accel = {};

  // Derived Data
  let velocity_z = 0;
  let position_z = 0;
  
  // Magnitude variables for display
  let accel_mag = 0;
  let velocity_mag = 0;

  // History Arrays
  const PLOTS_HISTORY_DURATION = 30; // Seconds
  const UPDATE_RATE = 20; // Hz
  const MAX_SAMPLES = PLOTS_HISTORY_DURATION * UPDATE_RATE; // = 600

  // History arrays (for plotting), initially filled with 0 (will be overwritten on first packet)
  let accelHistory = new Array(MAX_SAMPLES).fill(0);
  let pressureHistory = new Array(MAX_SAMPLES).fill(0);
  let velHistory = new Array(MAX_SAMPLES).fill(0);
  let posHistory = new Array(MAX_SAMPLES).fill(0);

  const STAGES = [
    "Inactive", "Calibrating", "Ready For Launch", "Launch", 
    "Accelerated Flight", "Ballistic Flight", "Apogee", 
    "Stabilization", "Landing", "Recovery"
  ];

  // Command Center State
  let commandInput = "";
  let commandHistory = []; 
  let commandTemplates = [];
  let newTemplateName = "";
  let newTemplateCmd = "";
  let fileInput; 

  // Lifecycle Hooks
  onMount(() => {
    // Load saved templates from localStorage
    const saved = localStorage.getItem('aurora_templates');
    if (saved) {
        try { commandTemplates = JSON.parse(saved); } catch(e) {}
    }
  });

  onDestroy(() => {
    disconnect();
  });

  // Connection Management
  async function toggleConnection() {
      if (isConnected) {
          disconnect();
      } else {
          if (connectionType === 'websocket') {
              connectWebSocket();
          } else {
              await connectSerial();
          }
      }
  }

  function disconnect() {
      // WS Disconnect
      if (socket) {
          socket.close();
          socket = null;
      }
      
      // Serial Disconnect
      keepReadingSerial = false;
      if (serialReader) {
          serialReader.cancel();
          serialReader = null;
      }
      if (serialPort) {
          serialPort.close();
          serialPort = null;
      }

      status = "DISCONNECTED";
      isConnected = false;
      isInitialized = false;
  }

  // WebSocket Logic
  function connectWebSocket() {
    status = "CONNECTING...";
    socket = new WebSocket('ws://localhost:8081');
    
    socket.onopen = () => { status = "WS CONNECTED"; isConnected = true; };
    
    socket.onclose = () => { 
        if (isConnected) {
            status = "DISCONNECTED"; 
            isConnected = false;
            isInitialized = false;
        }
    };
    
    socket.onmessage = (event) => {
      try {
        const data = JSON.parse(event.data);
        handleTelemetry(data);
      } catch (e) { }
    };
  }

  // Serial Logic
  async function connectSerial() {
      if (!("serial" in navigator)) {
          alert("Web Serial API not supported in this browser. Use Chrome/Edge.");
          return;
      }

      try {
          // Request user to select a port
          serialPort = await navigator.serial.requestPort();
          await serialPort.open({ baudRate: 115200 });
          
          status = "SERIAL CONNECTED";
          isConnected = true;
          keepReadingSerial = true;
          
          readSerialLoop();
      } catch (err) {
          console.error(err);
          status = "SERIAL ERROR";
          isConnected = false;
      }
  }

  async function readSerialLoop() {
      const textDecoder = new TextDecoderStream();
      const readableStreamClosed = serialPort.readable.pipeTo(textDecoder.writable);
      const reader = textDecoder.readable.getReader();
      serialReader = reader;

      try {
          while (keepReadingSerial) {
              const { value, done } = await reader.read();
              if (done) {
                  // Allow the serial port to be closed later.
                  reader.releaseLock();
                  break;
              }
              if (value) {
                  processSerialChunk(value);
              }
          }
      } catch (error) {
          console.error("Serial Read Error", error);
          disconnect();
      }
  }

  // Parses stream data that contains mixed Logs and JSON arrays
  function processSerialChunk(chunk) {
      incomingBuffer += chunk;

      // Simple bracket counting parser to handle streamed JSON arrays/objects
      let depth = 0;
      let startIndex = -1;
      let inString = false;
      let escape = false;

      // We loop through buffer to find complete JSON objects/arrays
      for (let i = 0; i < incomingBuffer.length; i++) {
          const char = incomingBuffer[i];

          // Handle String literals so brackets inside strings don't count
          if (char === '"' && !escape) { inString = !inString; }
          if (char === '\\' && inString) { escape = !escape; } else { escape = false; }

          if (!inString) {
              if (char === '{' || char === '[') {
                  if (depth === 0) startIndex = i; // Start of a potential JSON
                  depth++;
              } else if (char === '}' || char === ']') {
                  depth--;
                  if (depth === 0 && startIndex !== -1) {
                      // Found a complete block
                      const jsonStr = incomingBuffer.substring(startIndex, i + 1);
                      try {
                          const parsed = JSON.parse(jsonStr);
                          handleSerialJSON(parsed);
                      } catch (e) {
                          // Ignore parse errors (might be partial log text looking like JSON)
                      }
                      
                      // Remove processed part from buffer
                      incomingBuffer = incomingBuffer.substring(i + 1);
                      i = -1; // Reset loop for the remaining buffer
                      startIndex = -1;
                  }
              }
          }
      }

      // Cleanup: Limit buffer size if garbage data accumulates (Logs without JSON)
      if (incomingBuffer.length > 10000) {
          const lastStart = incomingBuffer.lastIndexOf('['); // Try to save latest array
          if (lastStart > -1) incomingBuffer = incomingBuffer.substring(lastStart);
          else incomingBuffer = ""; // Clear if no structure found
      }
  }

  // Data Adapters

  // Adapts the new Serial JSON format to the App's expected format
  function handleSerialJSON(data) {
      // The serial stream often sends an Array of objects
      const items = Array.isArray(data) ? data : [data];
      
      let unifiedUpdate = {};

      items.forEach(item => {
          if (item.type === 'SENSOR_DATA' && item.content) {
              const src = item.content.source;
              const pay = item.content.sensorData;
              
              if (src === 'BNO055') unifiedUpdate.imu = pay;
              else if (src === 'LIS3DHTR') unifiedUpdate.accelerometer = pay;
              else if (src === 'MS561101BA03_') unifiedUpdate.barometer1 = pay; // Use as Baro 1
              else if (src === 'GPS') {
                  unifiedUpdate.other = { ...(unifiedUpdate.other || {}), ...pay };
              }
          }
      });

      // Pass the mapped object to the main handler
      if (Object.keys(unifiedUpdate).length > 0) {
          handleTelemetry(unifiedUpdate);
      }
  }

  // Helper: Returns val if number, otherwise fallback.
  function sanitize(val, fallback = 0) {
      if (typeof val === 'number' && !isNaN(val)) return val;
      return fallback;
  }

  function handleTelemetry(data) {
      // Extract Standard Sensors (Merge with existing state to prevent flickering if partial updates come)
      if (data.imu) imu = { ...imu, ...data.imu };
      if (data.barometer1) baro1 = { ...baro1, ...data.barometer1 };
      if (data.barometer2) baro2 = { ...baro2, ...data.barometer2 };
      if (data.accelerometer) accel = { ...accel, ...data.accelerometer };

      // Extract Simulation Truth Data (Passed from CSV via "other")
      let vx = 0, vy = 0;

      if (data.other) {
          if (data.other.stage) currentStage = data.other.stage;
          
          velocity_z = sanitize(data.other.velocity_z, velocity_z);
          position_z = sanitize(data.other.altitude, position_z);
          
          // Attempt to get X and Y for magnitude calculation
          vx = sanitize(data.other.velocity_x, 0);
          vy = sanitize(data.other.velocity_y, 0);
      }

      // Update Visualization Histories
      
      const ax = sanitize(accel.acceleration_x, 0);
      const ay = sanitize(accel.acceleration_y, 0);
      const az = sanitize(accel.acceleration_z, 0);
      
      accel_mag = Math.sqrt((ax * ax) + (ay * ay) + (az * az));
      velocity_mag = Math.sqrt((vx * vx) + (vy * vy) + (velocity_z * velocity_z));
      const newPressure = sanitize(baro1.pressure, 101325);

      // Initialization Logic
      if (!isInitialized) {
          accelHistory.fill(accel_mag);
          velHistory.fill(velocity_mag);
          posHistory.fill(position_z);
          pressureHistory.fill(newPressure);
          isInitialized = true;
      }

      // Standard Update
      accelHistory.push(accel_mag);
      if (accelHistory.length > MAX_SAMPLES) accelHistory.shift();
      accelHistory = accelHistory;

      velHistory.push(velocity_mag);
      if (velHistory.length > MAX_SAMPLES) velHistory.shift();
      velHistory = velHistory;

      posHistory.push(position_z);
      if (posHistory.length > MAX_SAMPLES) posHistory.shift();
      posHistory = posHistory;

      pressureHistory.push(newPressure);
      if (pressureHistory.length > MAX_SAMPLES) pressureHistory.shift();
      pressureHistory = pressureHistory;
  }

  // Common Functions

  function sendCommand(str) {
      const time = new Date().toLocaleTimeString();
      let sent = false;

      // Send via WebSocket
      if (connectionType === 'websocket' && socket && socket.readyState === WebSocket.OPEN) {
          socket.send(str);
          sent = true;
      }
      // Send via Serial
      else if (connectionType === 'serial' && serialPort && serialPort.writable) {
         // Need a writer logic here for Serial
         const encoder = new TextEncoder();
         const writer = serialPort.writable.getWriter();
         writer.write(encoder.encode(str + "\n"));
         writer.releaseLock();
         sent = true;
      } else {
          alert("Not connected!");
          return;
      }

      if (sent) {
        commandHistory = [{ time, msg: str }, ...commandHistory].slice(0, 50); 
        commandInput = ""; 
      }
  }

  function loadTemplate(str) { commandInput = str; }

  function saveTemplate() {
      if (!newTemplateName || !newTemplateCmd) return;
      commandTemplates = [...commandTemplates, { name: newTemplateName, payload: newTemplateCmd }];
      newTemplateName = ""; newTemplateCmd = "";
      persistTemplates();
  }

  function deleteTemplate(index) {
      commandTemplates = commandTemplates.filter((_, i) => i !== index);
      persistTemplates();
  }

  function persistTemplates() {
      localStorage.setItem('aurora_templates', JSON.stringify(commandTemplates));
  }

  // File Import/Export

  function exportTemplates() {
      const dataStr = "data:text/json;charset=utf-8," + encodeURIComponent(JSON.stringify(commandTemplates));
      const downloadAnchorNode = document.createElement('a');
      downloadAnchorNode.setAttribute("href", dataStr);
      downloadAnchorNode.setAttribute("download", "aurora_commands.json");
      document.body.appendChild(downloadAnchorNode);
      downloadAnchorNode.click();
      downloadAnchorNode.remove();
  }

  function triggerImport() { fileInput.click(); }

  function handleFileImport(event) {
      const file = event.target.files[0];
      if (!file) return;
      const reader = new FileReader();
      reader.onload = (e) => {
          try {
              const parsed = JSON.parse(e.target.result);
              if (Array.isArray(parsed)) {
                  commandTemplates = parsed;
                  persistTemplates();
                  alert("Templates imported successfully!");
              } else { alert("Invalid JSON structure"); }
          } catch (err) { alert("Error parsing JSON file"); }
      };
      reader.readAsText(file);
  }

  // SVG Helper
  function getPolyline(data, height, minVal, maxVal) {
    if (!data || data.length < 2) return ""; 

    const width = 100;
    const step = width / (data.length - 1);

    let min = minVal;
    let max = maxVal;
    
    // Auto-scale if min/max not provided
    if (min === undefined || max === undefined) {
        const validData = data.filter(n => Number.isFinite(n));
        
        if (validData.length === 0) {
            min = 0; max = 100; 
        } else {
            min = Math.min(...validData);
            max = Math.max(...validData);
        }
        
        if (min === max) { max += 1; min -= 1; }
    }
    
    const range = max - min;
    
    return data.map((val, i) => {
      if (!Number.isFinite(val)) return `${i * step},${height}`; 
      
      const x = i * step;
      const clampedVal = Math.max(min, Math.min(max, val));
      const y = height - ((clampedVal - min) / range) * height; 
      return `${x},${y}`;
    }).join(" ");
  }
</script>

<main>
  <header>
    <div class="brand"><span>AURORA ROCKETRY</span> GROUND STATION</div>
    
    <nav class="nav-tabs">
        <button class:active={activePage === 'dashboard'} on:click={() => activePage = 'dashboard'}>
            DASHBOARD
        </button>
        <button class:active={activePage === 'commands'} on:click={() => activePage = 'commands'}>
            COMMAND CENTER
        </button>
    </nav>

    <div class="connection-ui">
        <select bind:value={connectionType} disabled={isConnected}>
            <option value="websocket">WebSocket</option>
            <option value="serial">USB / Serial</option>
        </select>
        <button 
            class="btn-connect" 
            class:connected={isConnected} 
            on:click={toggleConnection}
        >
            {isConnected ? 'DISCONNECT' : 'CONNECT'}
        </button>
        <div class="status-indicator" class:connected={isConnected}></div>
    </div>
  </header>

  <div class="container-fluid">
    
    {#if activePage === 'dashboard'}
        <div class="layout">
        
          <div class="sensors-col">
              
              <div class="plots-row">
              <div class="card plot-card">
                  <h3>ACCELERATION MAG (m/s²)</h3>
                  <div class="plot-container">
                  <svg viewBox="0 0 100 50" preserveAspectRatio="none">
                      <polyline points={getPolyline(accelHistory, 50, 0, 100)} fill="none" stroke="#4cc9f0" stroke-width="1" vector-effect="non-scaling-stroke" />
                  </svg>
                  <div class="live-val">{accel_mag?.toFixed(1) || 0}</div>
                  </div>
              </div>

              <div class="card plot-card">
                  <h3>VELOCITY MAG (m/s)</h3>
                  <div class="plot-container">
                  <svg viewBox="0 0 100 50" preserveAspectRatio="none">
                      <polyline points={getPolyline(velHistory, 50)} fill="none" stroke="#f72585" stroke-width="1" vector-effect="non-scaling-stroke" />
                  </svg>
                  <div class="live-val" style="color: #f72585;">{velocity_mag?.toFixed(1) || 0}</div>
                  </div>
              </div>

              <div class="card plot-card">
                  <h3>POSITION Z (m)</h3>
                  <div class="plot-container">
                  <svg viewBox="0 0 100 50" preserveAspectRatio="none">
                      <polyline points={getPolyline(posHistory, 50)} fill="none" stroke="#7209b7" stroke-width="1" vector-effect="non-scaling-stroke" />
                  </svg>
                  <div class="live-val" style="color: #7209b7;">{position_z?.toFixed(0) || 0}</div>
                  </div>
              </div>

              <div class="card plot-card">
                  <h3>PRESSURE (Pa)</h3>
                  <div class="plot-container">
                  <svg viewBox="0 0 100 50" preserveAspectRatio="none">
                      <polyline points={getPolyline(pressureHistory, 50)} fill="none" stroke="#e63946" stroke-width="1" vector-effect="non-scaling-stroke" />
                  </svg>
                  <div class="live-val text-red">{baro1.pressure?.toFixed(0) || 0}</div>
                  </div>
              </div>
              </div>

              <div class="mid-row">
              <div class="card">
                  <h3>BAROMETER 1</h3>
                  <div class="data-grid">
                  <div class="row"><span>Pressure</span> <b>{baro1.pressure?.toFixed(1)}</b></div>
                  <div class="row"><span>Temp</span> <b>{baro1.temperature?.toFixed(1)}°C</b></div>
                  <div class="row"><span>Time</span> <small>{baro1.timestamp?.toFixed(2)}</small></div>
                  </div>
              </div>
              <div class="card">
                  <h3>BAROMETER 2</h3>
                  <div class="data-grid">
                  <div class="row"><span>Pressure</span> <b>{baro2.pressure?.toFixed(1)}</b></div>
                  <div class="row"><span>Temp</span> <b>{baro2.temperature?.toFixed(1)}°C</b></div>
                  <div class="row"><span>Time</span> <small>{baro2.timestamp?.toFixed(2)}</small></div>
                  </div>
              </div>
              <div class="card">
                  <h3>ACCELEROMETER</h3>
                  <div class="data-grid">
                  <div class="row"><span>Acc X</span> <b>{accel.acceleration_x?.toFixed(2)}</b></div>
                  <div class="row"><span>Acc Y</span> <b>{accel.acceleration_y?.toFixed(2)}</b></div>
                  <div class="row"><span>Acc Z</span> <b>{accel.acceleration_z?.toFixed(2)}</b></div>
                  <div class="row"><span>Time</span> <small>{accel.timestamp?.toFixed(2)}</small></div>
                  </div>
              </div>
              </div>

              <div class="card imu-card">
              <h3>INERTIAL MEASUREMENT UNIT (IMU)</h3>
              <div class="imu-grid">
                  <div class="col">
                  <h4>CALIBRATION</h4>
                  <div class="row"><span>Sys</span> <b>{imu.calibration_sys}</b></div>
                  <div class="row"><span>Gyro</span> <b>{imu.calibration_gyro}</b></div>
                  <div class="row"><span>Accel</span> <b>{imu.calibration_accel}</b></div>
                  <div class="row"><span>Mag</span> <b>{imu.calibration_mag}</b></div>
                  </div>
                  <div class="col">
                  <h4>ORIENTATION</h4>
                  <div class="row"><span>X</span> <b>{imu.orientation_x?.toFixed(2)}</b></div>
                  <div class="row"><span>Y</span> <b>{imu.orientation_y?.toFixed(2)}</b></div>
                  <div class="row"><span>Z</span> <b>{imu.orientation_z?.toFixed(2)}</b></div>
                  </div>
                  <div class="col">
                  <h4>ANGULAR VEL</h4>
                  <div class="row"><span>X</span> <b>{imu.angular_velocity_x?.toFixed(2)}</b></div>
                  <div class="row"><span>Y</span> <b>{imu.angular_velocity_y?.toFixed(2)}</b></div>
                  <div class="row"><span>Z</span> <b>{imu.angular_velocity_z?.toFixed(2)}</b></div>
                  </div>
                  <div class="col">
                  <h4>LINEAR ACC</h4>
                  <div class="row"><span>X</span> <b>{imu.linear_acceleration_x?.toFixed(2)}</b></div>
                  <div class="row"><span>Y</span> <b>{imu.linear_acceleration_y?.toFixed(2)}</b></div>
                  <div class="row"><span>Z</span> <b>{imu.linear_acceleration_z?.toFixed(2)}</b></div>
                  </div>
                  <div class="col">
                  <h4>MAGNETOMETER</h4>
                  <div class="row"><span>X</span> <b>{imu.magnetometer_x?.toFixed(0)}</b></div>
                  <div class="row"><span>Y</span> <b>{imu.magnetometer_y?.toFixed(0)}</b></div>
                  <div class="row"><span>Z</span> <b>{imu.magnetometer_z?.toFixed(0)}</b></div>
                  </div>
              </div>
              <div class="imu-footer">
                  <span><b>Quats:</b> W:{imu.quaternion_w?.toFixed(3)} X:{imu.quaternion_x?.toFixed(3)} Y:{imu.quaternion_y?.toFixed(3)} Z:{imu.quaternion_z?.toFixed(3)}</span>
                  <span><b>Gravity:</b> X:{imu.gravity_x?.toFixed(2)} Y:{imu.gravity_y?.toFixed(2)} Z:{imu.gravity_z?.toFixed(2)}</span>
                  <span><b>Temp:</b> {imu.temperature?.toFixed(1)}°C</span>
              </div>
              </div>

          </div>

          <div class="stages-col">
              <h2>MISSION STAGE</h2>
              <div class="stage-list">
              {#each STAGES as stage}
                  <div class="stage-item" class:active={currentStage === stage}>
                  <div class="indicator"></div>
                  {stage}
                  </div>
              {/each}
              </div>
          </div>

        </div>

    {:else if activePage === 'commands'}
        <div class="command-layout">
            
            <div class="cmd-col">
                <div class="card">
                    <h3>MANUAL TRANSMISSION</h3>
                    <div class="input-group">
                        <input 
                            type="text" 
                            bind:value={commandInput} 
                            placeholder="Enter command string..." 
                            on:keydown={(e) => e.key === 'Enter' && sendCommand(commandInput)}
                        />
                        <button class="btn-send" on:click={() => sendCommand(commandInput)}>SEND</button>
                    </div>
                </div>

                <div class="card log-card">
                    <h3>TRANSMISSION LOG</h3>
                    <div class="cmd-log">
                        {#if commandHistory.length === 0}
                            <div class="log-empty">No commands sent yet.</div>
                        {/if}
                        {#each commandHistory as entry}
                            <div class="log-entry">
                                <span class="log-time">[{entry.time}]</span>
                                <span class="log-msg"> >> {entry.msg}</span>
                            </div>
                        {/each}
                    </div>
                </div>
            </div>

            <div class="template-col">
                <div class="card">
                    <div class="template-header">
                        <h3>SAVED TEMPLATES</h3>
                        <div class="file-actions">
                            <input type="file" bind:this={fileInput} on:change={handleFileImport} style="display:none" accept=".json"/>
                            <button class="btn-sm" on:click={triggerImport} title="Import JSON">Import</button>
                            <button class="btn-sm" on:click={exportTemplates} title="Export JSON">Export</button>
                        </div>
                    </div>

                    <div class="template-list">
                        {#each commandTemplates as tpl, i}
                            <div class="template-item">
                                <div class="tpl-info">
                                    <span class="tpl-name">{tpl.name}</span>
                                    <code class="tpl-cmd">{tpl.payload}</code>
                                </div>
                                <div class="tpl-actions">
                                    <button class="icon-btn btn-send-icon" on:click={() => sendCommand(tpl.payload)} title="Send Now">
                                        <svg viewBox="0 0 24 24"><path d="M2.01 21L23 12 2.01 3 2 10l15 2-15 2z"/></svg>
                                    </button>
                                    
                                    <button class="icon-btn btn-edit-icon" on:click={() => loadTemplate(tpl.payload)} title="Edit in Input Box">
                                        <svg viewBox="0 0 24 24"><path d="M3 17.25V21h3.75L17.81 9.94l-3.75-3.75L3 17.25zM20.71 7.04c.39-.39.39-1.02 0-1.41l-2.34-2.34c-.39-.39-1.02-.39-1.41 0l-1.83 1.83 3.75 3.75 1.83-1.83z"/></svg>
                                    </button>

                                    <button class="icon-btn btn-del-icon" on:click={() => deleteTemplate(i)} title="Delete">
                                        <svg viewBox="0 0 24 24"><path d="M6 19c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7H6v12zM19 4h-3.5l-1-1h-5l-1 1H5v2h14V4z"/></svg>
                                    </button>
                                </div>
                            </div>
                        {/each}
                        {#if commandTemplates.length === 0}
                            <div class="log-empty">No templates saved.</div>
                        {/if}
                    </div>

                    <div class="new-template">
                        <h4>CREATE NEW TEMPLATE</h4>
                        <input type="text" bind:value={newTemplateName} placeholder="Template Name" />
                        <input type="text" bind:value={newTemplateCmd} placeholder="Command String" />
                        <button class="btn-save" on:click={saveTemplate}>SAVE TEMPLATE</button>
                    </div>
                </div>
            </div>

        </div>
    {/if}
  </div>
</main>

<style>
  :global(body) { background: #0f0f13; color: #eee; margin: 0; font-family: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; overflow-x: hidden; }
  
  header { height: 60px; background: #18181f; display: flex; align-items: center; padding: 0 20px; border-bottom: 1px solid #333; justify-content: space-between; }
  .brand { font-weight: 800; font-size: 1.2rem; letter-spacing: 1px; min-width: 250px; }
  .brand span { color: red; }
  
  /* Connection UI Styles */
  .connection-ui { display: flex; align-items: center; gap: 10px; }
  .connection-ui select { background: #333; color: white; border: 1px solid #444; padding: 5px 10px; border-radius: 4px; outline: none; }
  .connection-ui select:disabled { opacity: 0.5; cursor: not-allowed; }
  
  .btn-connect {
      background: #444; color: white; border: none; padding: 6px 15px; border-radius: 4px; cursor: pointer; font-weight: bold; font-size: 0.8rem; letter-spacing: 0.5px;
      transition: background 0.2s;
  }
  .btn-connect:hover { background: #555; }
  .btn-connect.connected { background: #e63946; } /* Red for disconnect */
  
  .status-indicator { width: 10px; height: 10px; border-radius: 50%; background: #444; transition: background 0.3s; }
  .status-indicator.connected { background: #2a9d8f; box-shadow: 0 0 8px #2a9d8f; }

  /* --- NAVIGATION TABS --- */
  .nav-tabs { display: flex; gap: 10px; flex-grow: 1; justify-content: center; }
  .nav-tabs button {
      background: transparent; border: none; color: #666; font-weight: bold;
      padding: 8px 20px; cursor: pointer; border-bottom: 2px solid transparent;
      transition: all 0.2s; font-size: 0.9rem; letter-spacing: 1px;
  }
  .nav-tabs button:hover { color: #fff; }
  .nav-tabs button.active { color: #4cc9f0; border-bottom-color: #4cc9f0; }

  .container-fluid { width: 98%; margin: 0 auto; height: calc(100vh - 60px); }

  /* --- DASHBOARD STYLES --- */
  .layout { display: grid; grid-template-columns: 1fr 280px; height: 100%; }
  .sensors-col { padding: 20px; overflow-y: auto; display: flex; flex-direction: column; gap: 20px; }
  .plots-row { display: grid; grid-template-columns: repeat(4, 1fr); gap: 20px; height: 200px; }
  .mid-row { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 20px; }
  
  .card { background: #1e1e24; border: 1px solid #333; border-radius: 6px; padding: 15px; }
  h3 { margin: 0 0 10px 0; font-size: 0.8rem; color: #888; text-transform: uppercase; letter-spacing: 1px; border-bottom: 1px solid #333; padding-bottom: 5px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  
  .data-grid .row { display: flex; justify-content: space-between; font-size: 0.9rem; margin-bottom: 5px; border-bottom: 1px solid #2a2a30; padding-bottom: 2px; }
  .data-grid b { color: #4cc9f0; font-family: monospace; font-size: 1rem; }
  
  .imu-grid { display: grid; grid-template-columns: repeat(5, 1fr); gap: 15px; font-size: 0.8rem; }
  .imu-grid h4 { color: #555; font-size: 0.7rem; margin-bottom: 5px; }
  .imu-grid .row { display: flex; justify-content: space-between; margin-bottom: 4px; }
  .imu-grid b { color: #fff; font-family: monospace; }
  .imu-footer { margin-top: 15px; padding-top: 10px; border-top: 1px solid #333; display: flex; gap: 20px; font-size: 0.8rem; color: #888; flex-wrap: wrap; }
  .imu-footer b { color: #aaa; margin-right: 5px; }

  .plot-container { position: relative; height: 140px; width: 100%; background: #15151a; border-radius: 4px; overflow: hidden; }
  svg { width: 100%; height: 100%; }
  .live-val { position: absolute; top: 10px; right: 10px; font-family: monospace; font-size: 1.5rem; color: #4cc9f0; font-weight: bold; }
  .text-red { color: #e63946; }

  .stages-col { background: #131318; border-left: 1px solid #333; padding: 20px; padding-bottom: 20px; display: flex; flex-direction: column; overflow-y: auto; }
  .stage-list { display: flex; flex-direction: column; gap: 5px; flex-grow: 1; }
  .stage-item { padding: 12px 15px; background: #1e1e24; border-radius: 4px; color: #555; font-weight: bold; font-size: 0.9rem; display: flex; align-items: center; gap: 10px; border: 1px solid transparent; transition: all 0.2s; }
  .indicator { width: 10px; height: 10px; border-radius: 50%; background: #333; }
  .stage-item.active { background: #1c2b29; color: #4cc9f0; border-color: #2a9d8f; box-shadow: 0 0 10px rgba(76, 201, 240, 0.1); }
  .stage-item.active .indicator { background: #4cc9f0; box-shadow: 0 0 8px #4cc9f0; }

  /* --- COMMAND CENTER STYLES --- */
  .command-layout { 
      display: grid; 
      grid-template-columns: 2fr 1fr; 
      gap: 20px; 
      padding: 20px; 
      height: 100%; 
      box-sizing: border-box; /* Ensure padding doesn't overflow height */
  }

  .cmd-col, .template-col { 
      display: flex; 
      flex-direction: column; 
      gap: 20px; 
      height: 100%; 
      overflow: hidden; /* Prevent double scrollbars */
  }
  
  .input-group { display: flex; gap: 10px; }
  input[type="text"] { flex-grow: 1; background: #15151a; border: 1px solid #444; color: #fff; padding: 10px; border-radius: 4px; font-family: monospace; }
  input[type="text"]:focus { outline: none; border-color: #4cc9f0; }
  
  .btn-send { background: #2a9d8f; color: white; border: none; padding: 0 20px; border-radius: 4px; cursor: pointer; font-weight: bold; }
  .btn-send:hover { background: #228074; }
  
  /* Log Card - Fills remaining space */
  .log-card { 
      flex-grow: 1; 
      display: flex; 
      flex-direction: column; 
      min-height: 0; /* Important for flex child scrolling */
  }
  .cmd-log { 
      flex-grow: 1; 
      background: #000; 
      padding: 10px; 
      border-radius: 4px; 
      font-family: monospace; 
      overflow-y: auto; 
      color: #0f0; 
      border: 1px solid #333; 
  }
  .log-entry { margin-bottom: 4px; font-size: 0.9rem; }
  .log-time { color: #555; margin-right: 10px; }
  .log-empty { color: #444; font-style: italic; padding: 10px; text-align: center; }

  /* Template Card - Fills Full Height */
  .template-col .card {
      height: 100%;
      display: flex;
      flex-direction: column;
  }

  .template-header { display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid #333; padding-bottom: 5px; margin-bottom: 10px; }
  .template-header h3 { border: none; margin: 0; padding: 0; }
  .btn-sm { background: #333; border: none; color: #ccc; font-size: 0.75rem; padding: 4px 8px; border-radius: 3px; cursor: pointer; margin-left: 5px; }
  .btn-sm:hover { background: #555; color: #fff; }

  .template-list { 
      flex-grow: 1; 
      overflow-y: auto; 
      margin-bottom: 20px; 
      min-height: 0; /* Important for scrolling */
  }
  .template-item { background: #15151a; border: 1px solid #333; border-radius: 4px; padding: 10px; margin-bottom: 8px; display: flex; justify-content: space-between; align-items: center; }
  .tpl-info { display: flex; flex-direction: column; }
  .tpl-name { font-weight: bold; color: #ddd; font-size: 0.9rem; }
  .tpl-cmd { color: #666; font-size: 0.8rem; font-family: monospace; }
  
  .tpl-actions { display: flex; gap: 5px; }

  /* --- NEW ICON BUTTON STYLES --- */
  .icon-btn {
    border: none;
    border-radius: 4px;
    width: 30px;
    height: 30px;
    display: flex;
    align-items: center;
    justify-content: center;
    cursor: pointer;
    transition: all 0.2s;
    background: #25252b;
    color: #888;
  }
  .icon-btn svg {
    width: 18px;
    height: 18px;
    fill: currentColor;
  }
  
  /* Send Icon Hover */
  .btn-send-icon:hover { background: #2a9d8f; color: white; }
  
  /* Edit Icon Hover */
  .btn-edit-icon:hover { background: #ff9f1c; color: #121212; }
  
  /* Delete Icon Hover */
  .btn-del-icon:hover { background: #e63946; color: white; }

  .new-template { border-top: 1px solid #333; padding-top: 15px; display: flex; flex-direction: column; gap: 8px; }
  .new-template h4 { margin: 0; color: #666; font-size: 0.75rem; }
  .btn-save { background: #333; color: #ccc; border: 1px solid #444; padding: 8px; width: 100%; border-radius: 4px; cursor: pointer; font-weight: bold; transition: all 0.2s; }
  .btn-save:hover { background: #4cc9f0; color: #000; border-color: #4cc9f0; }

</style>