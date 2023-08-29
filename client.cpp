#include <iostream>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cstring>
#include <string>
#include <sstream>
#include "eftp.h"

using std::cout;
using std::cin;
using std::string;
using std::stringstream;

int main(int argc, char** argv) {
    // Check command line arguments
    if (argc != 4) {
        cout << "Usage: ./client [username:password@ip:port] [upload/download] [filename]\n";
        exit(1);
    }

    // Parse command line arguments
    string login, connection, username, password, ip, port, action, filename, ext;
    bool is_text_file;
    int static_port;
    try {
        string log_con = argv[1];
        stringstream argstream(log_con);
        getline(argstream, login, '@');
        getline(argstream, connection);
        stringstream logstream(login);
        stringstream constream(connection);
        getline(logstream, username, ':');
        getline(logstream, password);
        getline(constream, ip, ':');
        getline(constream, port);
        action = argv[2];
        filename = argv[3];
        static_port = std::stoi(port);
    }
    catch (...) {
        cout << "Usage: ./client [username:password@ip:port] [upload/download] [filename]\n";
        exit(1);
    }

    if (action != "upload" && action != "download") {
        cout << "Usage: ./client [username:password@ip:port] [upload/download] [filename]\n";
        exit(1);
    }

    // Set up address to send to initially
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(static_port);
    address.sin_addr.s_addr = inet_addr(ip.c_str());

    // Create socket
    int socket_c = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_c == -1) {
        cout << "socket() call failed.\n";
        exit(1);
    }
    cout << "Socket initialized.\n";

    // Set up address for new server socket
    struct sockaddr_in ip_server;
    struct sockaddr* server = (struct sockaddr*) &ip_server;
    socklen_t server_len = sizeof(ip_server);

    // Set up auth packet
    strcpy(auth.username, username.c_str());
    strcpy(auth.password, password.c_str());

    // Configure 5 second timeout for retransmission
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(socket_c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    // Send auth packet
    cout << "Sending login details to server...\n";
    if (eftp_send(socket_c, &auth, sizeof(auth), (struct sockaddr*) &address,
    sizeof(address), server, &server_len, false) < 0) {
        exit(1);
    }

    uint16_t session = ack.session;
    cout << "Session number: " << session << '\n';
    cout << "New server port: " << ip_server.sin_port << '\n';

    // Set up req packet
    action == "download" ? req.OPCODE = (uint16_t) RRQ : req.OPCODE = (uint16_t) WRQ;
    req.session = session;
    strcpy(req.filename, filename.c_str());

    // Send req packet
    cout << "Sending " << action << " request to server...\n";
    int dlen = eftp_send(socket_c, &req, sizeof(req), server, server_len, NULL, NULL, false);
    if (dlen < 0) {
        exit(1);
    }
    
    // RRQ
    if (action == "download") {
        // Configure 30 second timeout (allow time for retransmissions)
        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        setsockopt(socket_c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

        // Open file in write mode
        FILE* file = fopen(filename.c_str(), "wb");

        // Handle first data packet received from server
        fwrite(data.data, sizeof(uint8_t), dlen - DATA_HEADER_SIZE, file);

        // Receive the rest of the file
        if (dlen - DATA_HEADER_SIZE == 1024) {
            eftp_recv_file(socket_c, server, server_len, file, true, session);
        }

        fclose(file);
    }
    // WRQ
    else if (action == "upload") {
        // Open file in read mode
        FILE* file = fopen(filename.c_str(), "rb");
        
        if (file == NULL) {
            eftp_send_err(socket_c, server, server_len, "File doesn't exist.");
            exit(1);
        }

        // Send file
        eftp_send_file(socket_c, server, server_len, file, session);
        fclose(file);
    }
}