#include "mesh_utils/MeshTerminal.hpp"
#include "mesh_registry/NodeRegistry.hpp"
#include <Arduino.h>

namespace mesh {

MeshTerminal::MeshTerminal(Mesh& node) : node_(node) {}

void MeshTerminal::processSerial() {
    while (Serial.available() > 0) {
        char c = Serial.read();
        
        // Echo character back to user so they see they are typing
        if (c != '\n' && c != '\r') Serial.print(c);

        if (c == '\n' || c == '\r') {
            if (buffer_.length() > 0) {
                String input = buffer_;
                input.trim();
                buffer_ = ""; // Clear for next
                if (input.length() > 0) {
                    execute(input);
                }
            }
        } else {
            buffer_ += c;
            if (buffer_.length() > 256) buffer_ = ""; // Prevent overflow
        }
    }
}

String MeshTerminal::execute(const String& input, bool quiet) {
    if (!quiet) Serial.printf("\n> %s\n", input.c_str());

    if (input == "ls") {
        return handleLs(quiet);
    } else if (input.startsWith("msg ")) {
        return handleMsg(input.substring(4), quiet);
    } else if (input.startsWith("tcp ")) {
        return handleTcp(input.substring(4), quiet);
    } else if (input.startsWith("geo ")) {
        return handleGeo(input.substring(4), quiet);
    } else if (input.startsWith("broadcast ")) {
        return handleBroadcast(input.substring(10), quiet);
    } else if (input == "help") {
        String helpStr = "┌─────────────────────────────────────────┐\n";
        helpStr += "│         MESH TERMINAL COMMANDS          │\n";
        helpStr += "├─────────────────────────────────────────┤\n";
        helpStr += "│ ls                  List nearby nodes   │\n";
        helpStr += "│ msg <hash> <text>   Send UDP message    │\n";
        helpStr += "│ tcp <hash> <text>   Reliable TCP send   │\n";
        helpStr += "│ broadcast <text>    Send to all nodes   │\n";
        helpStr += "│ geo <lat> <lon> <text>  Geo-route msg   │\n";
        helpStr += "│ help                Show this menu      │\n";
        helpStr += "└─────────────────────────────────────────┘\n";
        if (!quiet) Serial.print(helpStr);
        return helpStr;
    } else {
        String err = "Unknown command. Type 'help'.\n";
        if (!quiet) Serial.print(err);
        return err;
    }
}

String MeshTerminal::handleLs(bool quiet) {
    String out = "--- DISCOVERED NODES ---\n";
    int maxNodes = node_.getNodeRegistry().capacity();
    Node* nodes = new Node[maxNodes];
    int count = node_.getNodeRegistry().getAll(nodes, maxNodes);
    for (int i = 0; i < count; i++) {
        char hashStr[33] = {0};
        for (int j = 0; j < 16; j++) sprintf(&hashStr[j * 2], "%02X", nodes[i].node_hash[j]);

        char buf[256];
        snprintf(buf, sizeof(buf), "[%02d] HASH:%s | MAC:%02X:%02X:%02X:%02X:%02X:%02X | POS:(%.6f, %.6f) | SEEN:%lus\n",
                      i, hashStr,
                      nodes[i].mac[0], nodes[i].mac[1], nodes[i].mac[2], nodes[i].mac[3], nodes[i].mac[4], nodes[i].mac[5],
                      nodes[i].lat, nodes[i].lon, (millis() - nodes[i].last_seen) / 1000);
        out += buf;
    }
    if (count == 0) out += "No peers found yet.\n";
    out += "------------------------\n";
    if (!quiet) Serial.print(out);
    delete[] nodes;
    return out;
}

String MeshTerminal::handleMsg(const String& args, bool quiet) {
    int firstSpace = args.indexOf(' ');
    if (firstSpace == -1) {
        String err = "Usage: msg <hash> <message>\n";
        if (!quiet) Serial.print(err);
        return err;
    }
    String hashStr = args.substring(0, firstSpace);
    String msg = args.substring(firstSpace + 1); // Greedily capture all remaining text

    uint8_t target[16] = {0};
    hexToBytes(hashStr, target, 16);
    if (node_.sendUDP(target, (const uint8_t*)msg.c_str(), msg.length())) {
        String out = "[CLI] UDP Sent to " + hashStr + ": " + msg + "\n";
        if (!quiet) Serial.print(out);
        return out;
    }
    return "";
}

String MeshTerminal::handleTcp(const String& args, bool quiet) {
    int firstSpace = args.indexOf(' ');
    if (firstSpace == -1) {
        String err = "Usage: tcp <hash> <message>\n";
        if (!quiet) Serial.print(err);
        return err;
    }
    String hashStr = args.substring(0, firstSpace);
    String msg = args.substring(firstSpace + 1); // Greedily capture all remaining text

    uint8_t target[16] = {0};
    hexToBytes(hashStr, target, 16);
    
    if (!quiet) Serial.printf("[CLI] TCP Session with %s: %s\n", hashStr.c_str(), msg.c_str());
    MeshResponse resp = node_.sendTCP(target, (const uint8_t*)msg.c_str(), msg.length());
    if (resp.success) {
        String reply = String((const char*)(resp.responsePacket.payload + 16));
        String out = "[CLI] TCP Result: " + reply + "\n";
        if (!quiet) Serial.print(out);
        return out;
    } else {
        String err = "[CLI] TCP Failed (Timeout)\n";
        if (!quiet) Serial.print(err);
        return err;
    }
}

String MeshTerminal::handleGeo(const String& args, bool quiet) {
    int firstSpace = args.indexOf(' ');
    int secondSpace = args.indexOf(' ', firstSpace + 1);
    if (firstSpace == -1 || secondSpace == -1) {
        String err = "Usage: geo <lat> <lon> <message>\n";
        if (!quiet) Serial.print(err);
        return err;
    }

    float lat = args.substring(0, firstSpace).toFloat();
    float lon = args.substring(firstSpace + 1, secondSpace).toFloat();
    String msg = args.substring(secondSpace + 1);

    uint8_t target[16] = {0};
    if (node_.sendUDP(target, (const uint8_t*)msg.c_str(), msg.length())) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[CLI] Geo-Routed to %.4f, %.4f: %s\n", lat, lon, msg.c_str());
        String out(buf);
        if (!quiet) Serial.print(out);
        return out;
    }
    return "";
}

String MeshTerminal::handleBroadcast(const String& args, bool quiet) {
    uint8_t target[16] = {0};
    if (node_.sendUDP(target, (const uint8_t*)args.c_str(), args.length())) {
        String out = "[CLI] Broadcast: " + args + "\n";
        if (!quiet) Serial.print(out);
        return out;
    }
    return "";
}

void MeshTerminal::hexToBytes(String hex, uint8_t* bytes, int maxLen) {
    hex.trim();
    String filtered = "";
    for (size_t i = 0; i < hex.length(); i++) {
        char c = tolower(hex[i]);
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
            filtered += c;
        }
    }
    
    int len = filtered.length() / 2;
    if (len > maxLen) len = maxLen;
    for (int i = 0; i < len; i++) {
        String part = filtered.substring(i * 2, i * 2 + 2);
        bytes[i] = (uint8_t)strtol(part.c_str(), NULL, 16);
    }
}

} // namespace mesh
