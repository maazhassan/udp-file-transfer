/*
    This header file contains common EFTP types and functions that both the
    client and server use. Buffers for sending and receiving packets are also
    located in this file.
*/

#include <iostream>
#include <stdint.h>
#include <cstring>
#include <sys/socket.h>
#include <cerrno>

using std::cout;

#define AUTH 1
#define RRQ 2
#define WRQ 3
#define DATA 4
#define ACK 5
#define ERR 6
#define DATA_HEADER_SIZE 7

// Flag containing opcode of last received packet
uint16_t LAST_OPCODE = 0;

// Struct describing an auth packet
struct auth_packet {
    uint16_t OPCODE = AUTH;
    char username[33];
    char password[33];
};

// Struct describing a read/write request packet
struct req_packet {
    uint16_t OPCODE; // RRQ = 02, WRQ = 03
    uint16_t session;
    char filename[256];
};

// Struct describing a data packet
struct data_packet {
    uint16_t OPCODE = DATA;
    uint16_t session;
    uint16_t block;
    uint8_t segment;
    uint8_t data[1024];
};

// Struct describing an ACK packet
struct ack_packet {
    uint16_t OPCODE = ACK;
    uint16_t session;
    uint16_t block;
    uint8_t segment;
};

// Struct describing an error packet
struct err_packet {
    uint16_t OPCODE = ERR;
    char message[513];
};

// Initialize packets for buffers
struct auth_packet auth;
struct req_packet req;
struct data_packet data;
struct ack_packet ack;
struct err_packet err;

// Receive buffer for all messages
uint8_t recv_buf[1050];

/*
    Function to receive into the buffer, check for errors, and send info to packet struct.
    Assumes sockfd is already configured to timeout.
*/ 
int eftp_recv(int sockfd, struct sockaddr* src_addr, socklen_t* addrlen) {
    // Clear receive buffer
    memset(recv_buf, 0, 1050);

    // Receive data from socket
    int num = recvfrom(sockfd, recv_buf, 1050, 0, src_addr, addrlen);
    if (num < 0) {
        if (errno = EAGAIN) cout << "Timeout\n";
        else cout << "Read error\n";
        return -1;
    }

    // Get opcode from packet and set flag
    uint16_t OPCODE = recv_buf[0];
    LAST_OPCODE = OPCODE;

    // Check for error
    if (OPCODE == ERR) {
        memcpy(&err, recv_buf, sizeof(err));
        cout << "Error received:\n\t" << err.message << '\n';
        return -1;
    }

    // Populate the correct packet struct based on the OPCODE
    switch (OPCODE) {
        case AUTH:
            memcpy(&auth, recv_buf, sizeof(auth));
            break;
        case RRQ:
        case WRQ:
            memcpy(&req, recv_buf, sizeof(req));
            break;
        case DATA:
            memcpy(&data, recv_buf, sizeof(data));
            break;
        case ACK:
            memcpy(&ack, recv_buf, sizeof(ack));
            break;
        default:
            // Never reached
            break;
    }
    return num;
}

// Function to send error
void eftp_send_err(int sockfd, struct sockaddr* d_addr, socklen_t d_addrlen, 
const char* message) {
    strcpy(err.message, message);
    sendto(sockfd, &err, sizeof(err), 0, d_addr, d_addrlen);
    cout << "Sent error: " << message << '\n';
}

/*
    Function to send a packet and wait for a response, retransmitting if necessary.
    If the response is a data packet (RRQ), return the size of the packet.
    Assumes sockfd is already configured to timeout.
*/
int eftp_send(int sockfd, void* p, size_t p_len, struct sockaddr* d_addr, 
socklen_t d_addrlen, struct sockaddr* s_addr, socklen_t* s_addrlen, bool is_data) {
    for (int i = 0; i < 4; i++) {
        // Send packet
        cout << "Sending packet...\n";
        int slen = sendto(sockfd, p, p_len, 0, d_addr, d_addrlen);
        if (slen < 0) {
            cout << "Send error\n";
            return -1;
        }

        if (is_data) {
            cout << "Sent DATA ( " << data.session << ' ' << data.block <<
                ' ' << unsigned(data.segment) << " )\n";
        }

        // Wait for response
        int rlen = eftp_recv(sockfd, s_addr, s_addrlen);
        if (rlen < 0) {
            // timeout  
            if (errno == EAGAIN) {
                continue;
            } 
            else return -1; // error packet
        }

        if (LAST_OPCODE == ACK) {
            cout << "Received ACK ( " << ack.session << ' ' << ack.block << ' ' << 
                unsigned(ack.segment) << " )\n";
        }

        if (LAST_OPCODE == DATA) {
            cout << "Received DATA ( " << data.session << ' ' << data.block <<
                ' ' << unsigned(data.segment) << " )\n";
            return rlen;
        }
        return slen;
    }
    // Timed out 4 times
    cout << "Timed out 4 times. Aborting.\n";
    eftp_send_err(sockfd, d_addr, d_addrlen, "Too many timeouts.");
    return -1;
}

/*
    Function to send an ACK and wait for a DATA packet. ACK packet should already be
    configured before calling. Retransmits ACK on receiving duplicate packet.
    Assumes sockfd is already configured to timeout (should be 30s).
*/
int eftp_send_ack_recv_data(int sockfd, struct sockaddr* d_addr, socklen_t d_addrlen,
uint16_t block, uint8_t segment) {
    bool e_flag = false;
    int num;
    while (1) {
        // Send ACK
        if (sendto(sockfd, &ack, sizeof(ack), 0, d_addr, d_addrlen) < 0) {
            cout << "Send error\n";
            e_flag = true;
            break;
        }
        cout << "Sent ACK ( " << ack.session << ' ' << ack.block << ' ' << 
            unsigned(ack.segment) << " )\n";

        // Wait for response
        num = eftp_recv(sockfd, NULL, NULL);
        if (num < 0) {
            e_flag = true;
            break;
        }

        // Check that the expected packet was received
        if (LAST_OPCODE == DATA && data.block == block && data.segment == segment) {
            cout << "Received DATA ( " << data.session << ' ' << data.block <<
                ' ' << unsigned(data.segment) << " )\n";
            break;
        }
        cout << "Received unexpected packet. Sending ACK again.\n";
    }
    if (e_flag) return -1;
    else return num;
}

void eftp_recv_file(int sockfd, struct sockaddr* d_addr, socklen_t d_addrlen,
FILE* file, bool isClient, uint16_t session) {
    // Initial setup
    ack.session = session;
    int next_block = 1;
    int next_segment = isClient ? 1 : 0;
    bool e_flag = false;

    while (1) {
        ack.block = next_block;
        ack.segment = next_segment;

        // Increment expected block and segment
        if (next_segment == 8) {
            next_block += 1;
            next_segment = 1;
        }
        else {
            next_segment += 1;
        }

        // Send ACK and receive new data
        int dlen = eftp_send_ack_recv_data(sockfd, d_addr, d_addrlen, next_block, 
        next_segment);
        if (dlen < 0) {
            e_flag = true;
            break;
        }
        
        // Write data to file
        fwrite(data.data, sizeof(uint8_t), dlen - DATA_HEADER_SIZE, file);

        // Check if transmission finished
        if (dlen - DATA_HEADER_SIZE < 1024) break;
    }
    if (e_flag) return;

    // Send final ACK
    ack.block = next_block;
    ack.segment = next_segment;
    sendto(sockfd, &ack, sizeof(ack), 0, d_addr, d_addrlen);
    cout << "Sent ACK ( " << ack.session << ' ' << ack.block << ' ' << 
        unsigned(ack.segment) << " )\n";
}

void eftp_send_file(int sockfd, struct sockaddr* d_addr, socklen_t d_addrlen,
FILE* file, uint16_t session) {
    // Initial setup
    data.session = session;
    data.block = 1;
    data.segment = 1;
    bool e_flag = false;

    // Loop through file
    size_t n_read;
    while ((n_read = fread(data.data, sizeof(uint8_t), 1024, file)) == 1024) {
        // Send packet
        if (eftp_send(sockfd, &data, 1024 + DATA_HEADER_SIZE, d_addr, d_addrlen,
        NULL, NULL, true) < 0) {
            e_flag = true;
            break;
        }

        // Increment block and segment
        if (data.segment == 8) {
            data.block += 1;
            data.segment = 1;
        }
        else {
            data.segment += 1;
        }
    }
    if (e_flag) return;

    // Reached end of file, send last data packet
    eftp_send(sockfd, &data, DATA_HEADER_SIZE + n_read, d_addr, d_addrlen, NULL, 
    NULL, true);
}