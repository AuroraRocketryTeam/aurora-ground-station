// The ground station does not parse packets, if forwards bytes though the serial.

#define E220_900T22D
#define FREQUENCY_868
#include <LoRa_E220.h>

// Ground station: RX=D2(GPIO5), TX=D3(GPIO6), AUX=D4(GPIO7), M0=D5(GPIO8), M1=D6(GPIO9)
// Manny sends to ADDR 0x0005 CH 23


// set to 1 to parse payload as binary TelemetryPacket (sensor data),
// set to 0 to print payload as plain text (test packets).
#define BINARY_TELEMETRY 1

LoRa_E220 e220(&Serial1, D4, D5, D6, UART_BPS_RATE_9600);

// Packet header must match components/telemetry/protocol/src/Packet.hpp
#pragma pack(push, 1)
struct PacketHeader {
    uint16_t messageId;
    uint8_t  totalChunks;
    uint8_t  chunkIndex;
    uint8_t  payloadSize;
    uint8_t  flags;
    uint8_t  protocolVersion;
};

// TelemetryPacket layout must match TelemetryTask.hpp
// Total: 4+1+24+8+8+4+12+4+2 = 67 bytes
#pragma pack(push, 1)
struct TelemetryPacket {
    uint32_t timestamp;
    bool     dataValid;
    struct { float accel_x, accel_y, accel_z; float gyro_x, gyro_y, gyro_z; } imu;
    struct { float pressure, temperature; } baro1;
    struct { float pressure, temperature; } baro2;
    float    baro_altitude;  
    struct { float latitude, longitude, altitude; } gps;
    float    velocity;
    uint8_t  flight_phase;
    uint8_t  last_ack_command_id;
};
#pragma pack(pop)

static const size_t LORA_MAX_PAYLOAD_SIZE = sizeof(TelemetryPacket); // 67 bytes
struct Packet {
    PacketHeader header;
    uint8_t      payload[LORA_MAX_PAYLOAD_SIZE];
    uint16_t     crc;
};
#pragma pack(pop)
static const size_t PACKET_SIZE = sizeof(Packet);

// CommandPacket sent to the rocket via LoRa.
// Layout must match components/telemetry/protocol/src/Packet.hpp
#pragma pack(push, 1)
struct CommandPacket {
    uint8_t  command_id;
    uint8_t  payload[4];
};
#pragma pack(pop)

// Command from python backend:  0xCC 0xDD | command_id (1 byte)
static const uint8_t CMD_FRAME_MAGIC[2] = {0xCC, 0xDD};
static const uint8_t CMD_LORA_ADDH = 0x00; // Manny board address high byte
static const uint8_t CMD_LORA_ADDL = 0x03; // Manny board address low byte
static const uint8_t CMD_LORA_CH   = 23;   // Manny channel

//   0xAA 0x55 | length (1 byte) | payload (length bytes)
static const uint8_t FRAME_MAGIC[2] = {0xAA, 0x55};

// How many times and how long to wait if we don't receive command ACKs
static const unsigned long CMD_RETRY_INTERVAL_MS = 2500;
static const uint8_t CMD_MAX_RETRIES = 5;

struct PendingCommand {
    uint8_t       cmd_id;
    unsigned long sent_ms;
    uint8_t       retries;
} pendingCmd = {};

void sendFrame(const uint8_t* payload, uint8_t len) {
    Serial.write(FRAME_MAGIC, 2);
    Serial.write(len);
    Serial.write(payload, len);
}

void sendLoRaCommand(uint8_t cmd_id) {
    CommandPacket cmd = {};
    cmd.command_id = cmd_id;
    ResponseStatus rs = e220.sendFixedMessage(CMD_LORA_ADDH, CMD_LORA_ADDL, CMD_LORA_CH,
                                              &cmd, sizeof(CommandPacket));
    Serial.printf("[CMD TX] id=0x%02X attempt=%d status=%d\n",
                  cmd_id, pendingCmd.retries + 1, rs.code);
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.printf("=== Ground Station E220 (Packet size: %u bytes, mode: %s) ===\n",
                  (unsigned)PACKET_SIZE, BINARY_TELEMETRY ? "BINARY" : "TEXT");
    Serial.printf("[DBG] sizeof(PacketHeader)=%u sizeof(Packet)=%u sizeof(TelemetryPacket)=%u sizeof(CommandPacket)=%u\n",
                  (unsigned)sizeof(PacketHeader), (unsigned)PACKET_SIZE,
                  (unsigned)sizeof(TelemetryPacket), (unsigned)sizeof(CommandPacket));

    Serial1.begin(9600, SERIAL_8N1, D2, D3);
    e220.begin();

    ResponseStructContainer csc = e220.getConfiguration();
    if (csc.status.code != E220_SUCCESS) {
        Serial.printf("getConfiguration failed: %d\n", csc.status.code);
        csc.close();
        return;
    }
    Configuration config = *(Configuration*)csc.data;
    csc.close();

    config.ADDH = 0x00;
    config.ADDL = 0x05;
    config.CHAN  = 23;

    config.SPED.uartBaudRate = UART_BPS_9600;
    config.SPED.uartParity   = MODE_00_8N1;
    config.SPED.airDataRate  = AIR_DATA_RATE_100_96;

    config.OPTION.subPacketSetting  = SPS_200_00;
    config.OPTION.RSSIAmbientNoise  = RSSI_AMBIENT_NOISE_DISABLED;
    config.OPTION.transmissionPower = POWER_17;

    config.TRANSMISSION_MODE.fixedTransmission = FT_FIXED_TRANSMISSION;
    config.TRANSMISSION_MODE.enableRSSI        = RSSI_ENABLED;
    config.TRANSMISSION_MODE.enableLBT         = LBT_DISABLED;
    config.TRANSMISSION_MODE.WORPeriod         = WOR_2000_011;

    config.CRYPT.CRYPT_H = 0x00;
    config.CRYPT.CRYPT_L = 0x00;

    ResponseStatus rs = e220.setConfiguration(config, WRITE_CFG_PWR_DWN_SAVE);
    Serial.printf("setConfiguration: %d - %s\n", rs.code, rs.getResponseDescription().c_str());
    if (rs.code != E220_SUCCESS) { return; }

    Serial.println("Config saved. Waiting for packets from Manny...");
}

void printText(Packet* pkt) {
    char text[LORA_MAX_PAYLOAD_SIZE + 1] = {};
    memcpy(text, pkt->payload, pkt->header.payloadSize);
    for (int i = 0; i < pkt->header.payloadSize; i++) {
        if (text[i] == 0x17) { text[i] = '\0'; break; }
    }
    Serial.printf("[TEXT] msg=%u chunk=%u/%u flags=0x%02X: \"%s\"\n",
        pkt->header.messageId,
        pkt->header.chunkIndex + 1,
        pkt->header.totalChunks,
        pkt->header.flags,
        text);
}

void forwardBinary(Packet* pkt, int16_t rssi_dbm) {
    // discard if we don't have a full TelemetryPacket in the payload (shouldn't happen)
    if (pkt->header.payloadSize < sizeof(TelemetryPacket)) return;

    TelemetryPacket* tp = (TelemetryPacket*)pkt->payload;
    if (pendingCmd.cmd_id != 0 && tp->last_ack_command_id == pendingCmd.cmd_id) {
        Serial.printf("[ACK] cmd=0x%02X confirmed after %d attempt(s)\n",
                      pendingCmd.cmd_id, pendingCmd.retries + 1);
        pendingCmd = {};
    } else if (pendingCmd.cmd_id != 0) {
        // Manny just finished its TX cycle, buffer has been cleared: best time to send.
        pendingCmd.retries++;
        pendingCmd.sent_ms = millis();
        sendLoRaCommand(pendingCmd.cmd_id);
        if (pendingCmd.retries > CMD_MAX_RETRIES) {
            Serial.printf("[CMD] FAILED: no ACK for id=0x%02X after %d attempts\n",
                          pendingCmd.cmd_id, pendingCmd.retries);
            pendingCmd = {};
        }
    }

    // Frame: 0xAA 0x55 | len | rssi(int16 LE) | TelemetryPacket
    uint8_t buf[sizeof(int16_t) + sizeof(TelemetryPacket)];
    memcpy(buf, &rssi_dbm, sizeof(int16_t));
    memcpy(buf + sizeof(int16_t), pkt->payload, sizeof(TelemetryPacket));
    sendFrame(buf, sizeof(buf));
}

// Read and forward commands sent from dash.
// Frame: 0xCC 0xDD | command_id (1 byte)
void pollSerialForCommands() {
    static uint8_t buf[3];
    static uint8_t pos = 0;

    while (Serial.available()) {
        buf[pos++] = Serial.read();
        if (pos < 3) continue;
        pos = 0;

        if (buf[0] != 0xCC || buf[1] != 0xDD) continue;
        uint8_t cmd_id = buf[2];

        pendingCmd = {cmd_id, millis(), 0};
        sendLoRaCommand(cmd_id);
    }
}

void loop() {
    pollSerialForCommands();

    if (pendingCmd.cmd_id != 0 && millis() - pendingCmd.sent_ms > CMD_RETRY_INTERVAL_MS) {
        pendingCmd.retries++;
        pendingCmd.sent_ms = millis();
        sendLoRaCommand(pendingCmd.cmd_id);
    }

    int avail = e220.available();
    if (avail <= 0) return;

    ResponseStructContainer rsc = e220.receiveMessageRSSI(PACKET_SIZE);
    if (rsc.status.code != E220_SUCCESS) {
        Serial.printf("[RX ERR] code=%d desc=%s\n",
                      rsc.status.code, rsc.status.getResponseDescription().c_str());
        rsc.close();
        return;
    }

    Packet* pkt = (Packet*)rsc.data;
    int16_t rssi_dbm = (int16_t)(0 - (256 - rsc.rssi) / 2);

#if BINARY_TELEMETRY
    forwardBinary(pkt, rssi_dbm);
#else
    printText(pkt);
#endif

    rsc.close();
}
