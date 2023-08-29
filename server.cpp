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
        cout << "Usage: ./server [username:password] [listen port] [working directory]";
        exit(1);
    }

    // Parse command line arguments
    string login = argv[1];
    string username, password;
    stringstream logstream(login);
    getline(logstream, username, ':');
    getline(logstream, password);
    int static_port = std::stoi(argv[2]);
    string working_dir = argv[3];

    // Set up address to bind to
    struct sockaddr* server;
    struct sockaddr_in ip_server;
    memset(&ip_server, 0, sizeof(ip_server));
    ip_server.sin_family = AF_INET;
    ip_server.sin_port = htons(static_port);
    ip_server.sin_addr.s_addr = htonl(INADDR_ANY);
    server = (struct sockaddr*) &ip_server;

    // Create socket
    int socket_s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_s == -1) {
        cout << "socket() call failed.\n";
        exit(1);
    }
    cout << "Socket initialized.\n";

    // Bind socket
    if (bind(socket_s, server, sizeof(ip_server)) == -1) {
        cout << "bind() call failed.\n";
        exit(1);
    }
    cout << "Socket bound to address.\n";

    // Set up client address
    struct sockaddr_in ip_client;
    struct sockaddr* client = (struct sockaddr*) &ip_client;
    socklen_t client_len = sizeof(ip_client);

    while (1) {
        cout << "Waiting for client...\n";

        // Recieve authentication packet
        if (eftp_recv(socket_s, client, &client_len) < 0 || LAST_OPCODE != AUTH) {
            continue;
        }

        // Validate login details
        string client_username(auth.username);
        string client_password(auth.password);
        if (username != client_username || password != client_password) {
            cout << "Login details not accepted.\n";
            eftp_send_err(socket_s, client, client_len, "Incorrect login details.");
            continue;
        }
        cout << "Login details accepted.\n";

        // Create a new socket for this client
        int socket_c = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_c == -1) {
            cout << "socket() call failed.\n";
            continue;
        }
        cout << "Socket initialized.\n";

        // Set up ACK packet
        srand(time(NULL));
        ack.session = rand() % 65535 + 1;

        // Configure 30 second timeout (assume client disconnected)
        struct timeval tv;
        tv.tv_sec = 30;
        tv.tv_usec = 0;
        setsockopt(socket_c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

        /*
            Set sockets to listen to in select() call.
            This is needed because if the ACK is dropped, the client will
            retransmit the auth packet to the socket on the original port, not
            the new one. So we need to listen on both sockets.
        */
        fd_set socks;
        FD_ZERO(&socks);
        FD_SET(socket_s, &socks);
        FD_SET(socket_c, &socks);

        // Try to send ACK and receive RRQ or WRQ packet
        bool e_flag = false;
        while (1) {
            // Send ACK
            if (sendto(socket_c, &ack, sizeof(ack), 0, client, client_len) < 0) {
                cout << "Send error\n";
                e_flag = true;
                break;
            }
            cout << "Sent ACK with session number: " << ack.session << '\n';
            cout << "Waiting for read/write request...\n";

            // Wait for response from either socket
            int nsocks = std::max(socket_s, socket_c) + 1;
            if (select(nsocks, &socks, (fd_set*)0, (fd_set*)0, &tv) > 0) {
                if (FD_ISSET(socket_s, &socks)) {
                    if (eftp_recv(socket_s, NULL, NULL) < 0) {
                        e_flag = true;
                        break;
                    }
                }
                if (FD_ISSET(socket_c, &socks)) {
                    if (eftp_recv(socket_c, NULL, NULL) < 0) {
                        e_flag = true;
                        break;
                    }
                }
            }
            else {
                cout << "Timeout\n";
                e_flag = true;
                break;
            }
            
            // Check that the expected packet was received
            // cout << "Last OPCODE: " << LAST_OPCODE << '\n';
            if (LAST_OPCODE == RRQ || LAST_OPCODE == WRQ) {
                break;
            }
            cout << "Received unexpected packet. Sending ACK again.\n";
        }
        if (e_flag) continue;

        // RRQ or WRQ received, handle each case
        if (LAST_OPCODE == RRQ) {
            cout << "Read request for file: " << req.filename << '\n';

            // Configure 5 second timeout for retransmission
            struct timeval tv;
            tv.tv_sec = 5;
            tv.tv_usec = 0;
            setsockopt(socket_c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

            // Open file in read mode
            string filepath = working_dir + '/' + req.filename;
            FILE* file = fopen(filepath.c_str(), "rb");

            if (file == NULL) {
                eftp_send_err(socket_c, client, client_len, "File doesn't exist.");
                continue;
            }

            // Send file
            eftp_send_file(socket_c, client, client_len, file, 0);
            fclose(file);
        }
        else if (LAST_OPCODE == WRQ) {
            cout << "Write request for file: " << req.filename << '\n';
            
            // Open file in write mode
            string filepath = working_dir + '/' + req.filename;
            FILE* file = fopen(filepath.c_str(), "wb");

            // Receive file
            eftp_recv_file(socket_c, client, client_len, file, false, 0);
            fclose(file);

            // If error, delete the file
            if (LAST_OPCODE == ERR) {
                remove(filepath.c_str());
            }
        }
    }
}